// Pictures: image files read on a thread of their own (picture.h: the
// system's reader) and put on the GPU (Renderer::image_add), for an image
// file's tab and for the pictures in the Markdown preview. A picture not
// drawn for a minute is let go; one whose file changes is read again.
//
// An image file opens in a tab of its own: the picture fitted to the tab
// (never made larger than it is), zoomed with + and - (Cmd too), a pinch or
// Cmd+wheel, moved by dragging it, the wheel, or the arrow keys; 0 fits it
// again and 1 shows it at its own size. The row at the bottom says what it is.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "app.hpp"
#include "fsio.hpp"
#include "picture.h"
#include "remote.hpp"
#include "textbox.hpp"

#if !defined(__APPLE__)
// (No reader on other systems yet.)
extern "C" int blit_picture_read(const void*, size_t, int, BlitPicture* out, char* err, size_t err_size) {
    *out = BlitPicture{};
    if (err && err_size) std::snprintf(err, err_size, "pictures are not read on this system yet");
    return 0;
}
extern "C" void blit_picture_free(BlitPicture* p) {
    if (p) std::free(p->rgba), p->rgba = nullptr;
}
#endif

namespace blit {

namespace {

constexpr u64 kMostBytes = 256ull << 20;    // a file larger than this is not read
constexpr int kMostSide = 4096;             // a picture larger than this is read smaller
constexpr u64 kForgetNs = 60'000'000'000ull;  // a picture not drawn for this long is let go

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 17;
    SDL_PushEvent(&ev);
}

i64 stamp_of(const std::string& path) {
    SDL_PathInfo info;
    return !is_remote(path) && SDL_GetPathInfo(path.c_str(), &info) ? static_cast<i64>(info.modify_time) : 0;
}

std::string size_text(u64 bytes) {
    char b[32];
    if (bytes < 1024) std::snprintf(b, sizeof b, "%llu bytes", static_cast<unsigned long long>(bytes));
    else if (bytes < 1024 * 1024) std::snprintf(b, sizeof b, "%.1f KB", static_cast<double>(bytes) / 1024);
    else std::snprintf(b, sizeof b, "%.1f MB", static_cast<double>(bytes) / (1024 * 1024));
    return b;
}

// The file's bytes, here or on another machine (the connection to it
// waited for a while: it may be coming up; not past `st`, so that quitting
// does not wait). False, and why, if not.
bool read_file(const std::string& path, std::string* bytes, std::string* why, std::stop_token st) {
    if (is_remote(path)) {
        std::string there;
        std::shared_ptr<Remote> r;
        for (int waited = 0; !r && waited < 30000 && !st.stop_requested(); waited += 200) r = remote_for(path, &there, 200);
        if (!r) return *why = remote_error(path), false;
        switch (r->read(there, kMostBytes, bytes)) {
            case 0: return true;
            case 1: *why = "there is no such file"; break;
            case 2: *why = "it is not a file"; break;
            case 3: *why = "the file is too big (over " + size_text(kMostBytes) + ")"; break;
            default: *why = remote_error(path); break;
        }
        return false;
    }
    if (!fs_read(path, bytes, kMostBytes)) return *why = fs_kind(path) == 0 ? "there is no such file" : "the file could not be read", false;
    if (bytes->size() >= kMostBytes) return *why = "the file is too big (over " + size_text(kMostBytes) + ")", false;
    return true;
}

std::string leaf_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

struct App::Pictures {
    std::map<std::string, PictureInfo, std::less<>> known;
    struct Done {
        std::string path;
        BlitPicture pic{};
        std::string error;
        u64 bytes = 0;
        i64 stamp = 0;
        bool clear = false;
    };
    std::mutex mu;
    std::condition_variable_any cv;
    // Two readers: files here, and files on other machines (which may wait
    // for a connection, and should not hold up the others).
    std::deque<std::string> todo[2];
    std::vector<Done> done;
    u64 tidied_ns = 0;
    std::jthread workers[2];

    Pictures() {
        for (int k = 0; k < 2; ++k) workers[k] = std::jthread([this, k](std::stop_token st) { run(st, todo[k]); });
    }
    ~Pictures() {
        for (std::jthread& w : workers) w.request_stop();
        cv.notify_all();
        for (std::jthread& w : workers)
            if (w.joinable()) w.join();
        for (Done& d : done) blit_picture_free(&d.pic);
    }
    void want(const std::string& path) {
        {
            std::lock_guard l(mu);
            todo[is_remote(path) ? 1 : 0].push_back(path);
        }
        cv.notify_all();
    }
    void run(std::stop_token st, std::deque<std::string>& queue) {
        for (;;) {
            std::string path;
            {
                std::unique_lock l(mu);
                if (!cv.wait(l, st, [&] { return !queue.empty(); })) return;
                path = std::move(queue.front());
                queue.pop_front();
            }
            Done d;
            d.path = path;
            d.stamp = stamp_of(path);
            std::string bytes;
            if (read_file(path, &bytes, &d.error, st)) {
                char why[160] = "";
                if (!blit_picture_read(bytes.data(), bytes.size(), kMostSide, &d.pic, why, sizeof why)) d.error = why;
                for (size_t k = 3; d.pic.rgba && k < static_cast<size_t>(d.pic.w) * static_cast<size_t>(d.pic.h) * 4 && !d.clear; k += 4) d.clear = d.pic.rgba[k] != 255;
            }
            d.bytes = bytes.size();
            if (st.stop_requested()) {
                blit_picture_free(&d.pic);
                return;
            }
            {
                std::lock_guard l(mu);
                done.push_back(std::move(d));
            }
            wake();
        }
    }
};

// An image file's tab.
struct ImageView {
    std::string path;
    double zoom = 0;            // pixels of the window to a pixel of the picture; 0: fitted to the tab
    double cx = 0.5, cy = 0.5;  // the point of the picture at the middle of the tab (0 to 1 across it)
    // As drawn: where the picture may be, its size there, and the bottom row.
    int ax = 0, ay = 0, aw = 0, ah = 0;
    double scale = 1;
    int x = 0, y = 0, w = 0, h = 0;  // the tab
    float rect[4] = {};              // where all of the picture was drawn
    TextBox box;
    u64 key = 0;
    bool dragging = false;
    float drag_x = 0, drag_y = 0;
};

bool App::is_picture_path(std::string_view path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || path.find('/', dot) != std::string_view::npos) return false;
    std::string ext(path.substr(dot + 1));
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* const kKinds[] = {"png", "jpg", "jpeg", "jpe", "gif", "bmp", "webp", "heic", "heif", "avif", "tif", "tiff", "ico", "icns", "tga", "psd", "jp2", "exr", "hdr"};
    return std::ranges::any_of(kKinds, [&](const char* k) { return ext == k; });
}

// The picture of this file: being read (asked for now, the first time),
// read, or not readable. The pointer is good until the next event.
const PictureInfo* App::picture(const std::string& path) {
    if (!pictures_) pictures_ = std::make_shared<Pictures>();
    auto it = pictures_->known.find(path);
    if (it == pictures_->known.end()) {
        it = pictures_->known.emplace(path, PictureInfo{}).first;
        pictures_->want(path);
    }
    it->second.used_ns = now_ns();
    return &it->second;
}

// Pictures that have been read go onto the GPU.
void App::picture_sync() {
    if (!pictures_) return;
    std::vector<Pictures::Done> done;
    {
        std::lock_guard l(pictures_->mu);
        done.swap(pictures_->done);
    }
    for (Pictures::Done& d : done) {
        auto it = pictures_->known.find(d.path);
        if (it != pictures_->known.end()) {
            PictureInfo& p = it->second;
            p.stamp = d.stamp;
            if (!d.error.empty()) {
                if (p.state != PictureInfo::kReady) {  // (one that was there stays, if reading it again fails: it may be being written)
                    p.state = PictureInfo::kFailed;
                    p.error = d.error;
                }
            } else if (const u32 id = gpu_.image_add(d.pic.rgba, d.pic.w, d.pic.h)) {
                if (p.gpu) gpu_.image_drop(p.gpu);
                p.gpu = id;
                p.state = PictureInfo::kReady;
                p.w = d.pic.w, p.h = d.pic.h, p.full_w = d.pic.full_w, p.full_h = d.pic.full_h;
                p.frames = d.pic.frames;
                p.kind = d.pic.kind;
                p.bytes = d.bytes;
                p.clear = d.clear;
                p.error.clear();
            } else if (p.state != PictureInfo::kReady) {
                p.state = PictureInfo::kFailed;
                p.error = "the GPU would not take it";
            }
            p.again = false;
            ++pictures_gen_;
            dirty_ = true;
        }
        blit_picture_free(&d.pic);
    }
}

// Pictures whose files have changed (here: on another machine they are not watched) are read again.
void App::picture_recheck() {
    if (!pictures_) return;
    for (auto& [path, p] : pictures_->known) {
        if (p.state == PictureInfo::kReading || p.again || is_remote(path)) continue;
        if (stamp_of(path) == p.stamp) continue;
        p.again = true;
        pictures_->want(path);
    }
}

// Pictures nobody has drawn for a minute are let go (those of open image tabs are kept).
void App::picture_tidy() {
    if (!pictures_) return;
    const u64 now = now_ns();
    if (now - pictures_->tidied_ns < 5'000'000'000ull) return;
    pictures_->tidied_ns = now;
    for (const std::unique_ptr<Pane>& pane : panes_)
        for (const std::unique_ptr<Doc>& d : pane->docs)
            if (d->image)
                if (auto it = pictures_->known.find(d->image->path); it != pictures_->known.end()) it->second.used_ns = now;
    std::erase_if(pictures_->known, [&](auto& kv) {
        PictureInfo& p = kv.second;
        if (p.state == PictureInfo::kReading || p.again || now - p.used_ns < kForgetNs) return false;
        if (p.gpu) gpu_.image_drop(p.gpu);
        return true;
    });
}

bool App::pictures_busy() const {
    if (!pictures_) return false;
    for (const auto& [path, p] : pictures_->known)
        if (p.state == PictureInfo::kReading || p.again) return true;
    return false;
}

// An image file in a tab of its own (open_path() sends it here). It stays
// a document with the file's path, for the tab and for finding it again,
// with no text.
bool App::image_open(const std::string& full) {
    auto fresh = std::make_unique<Doc>();
    fresh->ed.path = full;
    fresh->read_only = true;
    fresh->image = std::make_shared<ImageView>();
    fresh->image->path = full;
    fresh->id = next_doc_id_++;
    if (!is_remote(full)) {
        fresh->disk_stamp = stamp_of(full);
        recent_add(full, false);
    }
    picture(full);
    const bool reuse = pane_->doc->ed.path.empty() && !pane_->doc->ed.modified() && pane_->doc->ed.buf.size() == 0 && !pane_->doc->read_only;
    size_t at = pane_->tab + 1;
    if (reuse) {
        stop_lsp();
        at = pane_->tab;
        pane_->docs[pane_->tab] = std::move(fresh);
        pane_->doc = pane_->docs[at].get();
        pane_->tab = at;
        pane_->scroll_x = pane_->scroll_y = pane_->target_x = pane_->target_y = 0;
        pane_->cache_rev = ~0ull;
    } else {
        pane_->docs.insert(pane_->docs.begin() + static_cast<std::ptrdiff_t>(at), std::move(fresh));
    }
    tab_show(at);
    pick_theme();
    dirty_ = true;
    return true;
}

namespace {

// The scale that fits the picture into the area, never larger than one
// pixel of it to a point.
double fit_scale(const PictureInfo& p, int aw, int ah, double density) {
    if (p.full_w <= 0 || p.full_h <= 0) return 1;
    return std::min({density, static_cast<double>(aw) / p.full_w, static_cast<double>(ah) / p.full_h});
}

// The middle kept where the picture still covers the area (or in the middle, where it does not).
void keep_in(ImageView& v, const PictureInfo& p, double scale) {
    const double w = p.full_w * scale, h = p.full_h * scale;
    v.cx = w <= v.aw ? 0.5 : std::clamp(v.cx, v.aw / (2 * w), 1 - v.aw / (2 * w));
    v.cy = h <= v.ah ? 0.5 : std::clamp(v.cy, v.ah / (2 * h), 1 - v.ah / (2 * h));
}

// Zoomed by `by`, the point of the picture at (px, py) in the window staying where it is.
void zoom_at(ImageView& v, const PictureInfo& p, double by, double px, double py) {
    if (p.full_w <= 0 || p.full_h <= 0) return;
    const double now = v.scale, next = std::clamp(now * by, 0.01, 64.0);
    const double mx = v.ax + v.aw / 2.0, my = v.ay + v.ah / 2.0;
    const double u = v.cx + (px - mx) / (p.full_w * now), w = v.cy + (py - my) / (p.full_h * now);
    v.cx = u - (px - mx) / (p.full_w * next);
    v.cy = w - (py - my) / (p.full_h * next);
    v.zoom = next;
    v.scale = next;
    keep_in(v, p, next);
}

}  // namespace

void App::image_layout(FrameDesc& fd) {
    picture_tidy();
    Font& font = *font_;
    const int cw = font.cell_w(), ch = font.cell_h();
    const ThemeColors& tc = cfg_.theme(SDL_GetSystemTheme() != SDL_SYSTEM_THEME_LIGHT);
    for (const std::unique_ptr<Pane>& pane : panes_) {
        if (!pane->doc || !pane->doc->image || fd.popup_count >= std::size(fd.popups)) continue;
        ImageView& v = *pane->doc->image;
        const int x = pane->x0, y = pane->text_y, w = pane->x1 - pane->x0, h = pane->y1 - pane->text_y;
        if (w < cw * 4 || h < ch * 3) continue;
        v.x = x, v.y = y, v.w = w, v.h = h;
        const u32 cols = static_cast<u32>(w / cw), rows = static_cast<u32>(h / ch);
        const int pad_x = (w - static_cast<int>(cols) * cw) / 2, pad_y = (h - static_cast<int>(rows) * ch) / 2;
        // The picture's room: all but the bottom row, a little in from the edges.
        const int margin = std::max(cw, static_cast<int>(8 * density_));
        v.ax = x + margin, v.ay = y + margin;
        v.aw = std::max(1, w - 2 * margin), v.ah = std::max(1, h - pad_y - ch - 2 * margin);
        const PictureInfo* p = picture(v.path);
        if (p->state == PictureInfo::kReady) {
            v.scale = v.zoom > 0 ? v.zoom : fit_scale(*p, v.aw, v.ah, density_);
            keep_in(v, *p, v.scale);
        }
        // The bottom row: what it is, how large, how far zoomed, and the keys
        // (made again only when one of those changes).
        const int percent = static_cast<int>(std::lround(v.scale / density_ * 100));
        u64 key = 1469598103934665603ull;
        auto mix = [&](u64 k) { key = (key ^ k) * 1099511628211ull; };
        mix(cols), mix(rows), mix(font.generation()), mix(p->state), mix(p->gpu), mix(static_cast<u64>(percent));
        if (key != v.key) {
            v.key = key;
            std::string info, keys;
            u32 info_ink = kInkMuted;
            if (p->state == PictureInfo::kReading) {
                info = "Reading…";
            } else if (p->state == PictureInfo::kFailed) {
                info = "Cannot show it: " + p->error;
                info_ink = kInkDiagnostic;
            } else {
                info = p->kind + " · " + std::to_string(p->full_w) + " × " + std::to_string(p->full_h) + " · " + size_text(p->bytes);
                if (p->frames > 1) info += " · " + std::to_string(p->frames) + " frames (the first shown)";
                info += " · " + std::to_string(percent) + "%";
                keys = "+ − zoom   0 fit   1 its own size";
            }
            v.box.resize(cols, rows);
            const u32 r = rows - 1;
            u32 c = v.box.put(font, r, 1, leaf_of(v.path), 0, Font::kBold);
            c = v.box.put(font, r, c + 2, info, info_ink);
            const u32 kw = width_of(keys);
            if (!keys.empty() && c + 3 + kw < cols) v.box.put(font, r, cols - 1 - kw, keys, kInkMuted);
        }
        Popup& q = fd.popups[fd.popup_count++];
        q = Popup{};
        q.x = x, q.y = y, q.w = w, q.h = h;
        q.pad_x = pad_x, q.pad_y = pad_y;
        q.cols = v.box.cols, q.rows = v.box.rows;
        q.cells = v.box.cells.data();
        q.border_px = 0;
        q.bg = theme_.bg;
        if (p->state != PictureInfo::kReady || fd.picture_count >= std::size(fd.pictures)) continue;
        PictureDraw& d = fd.pictures[fd.picture_count++];
        d = PictureDraw{};
        d.image = p->gpu;
        const double pw = p->full_w * v.scale, ph = p->full_h * v.scale;
        const double left = v.ax + v.aw / 2.0 - v.cx * pw, top = v.ay + v.ah / 2.0 - v.cy * ph;
        d.rect[0] = static_cast<float>(std::round(left)), d.rect[1] = static_cast<float>(std::round(top));
        d.rect[2] = static_cast<float>(std::round(left + pw)), d.rect[3] = static_cast<float>(std::round(top + ph));
        d.clip[0] = v.ax, d.clip[1] = v.ay, d.clip[2] = v.ax + v.aw, d.clip[3] = v.ay + v.ah;
        d.after = fd.popup_count;
        d.sharp = pw / p->w >= 3;
        d.clear = p->clear;
        std::copy(d.rect, d.rect + 4, v.rect);
        if (p->clear) {  // (a checkerboard behind what is see-through)
            d.checker = std::round(8 * density_);
            for (int c = 0; c < 3; ++c) {
                const float fg = c == 0 ? tc.foreground.r : c == 1 ? tc.foreground.g : tc.foreground.b;
                d.check_a[c] = theme_.bg[c] + (fg - theme_.bg[c]) * 0.06f;
                d.check_b[c] = theme_.bg[c] + (fg - theme_.bg[c]) * 0.12f;
            }
            d.check_a[3] = d.check_b[3] = 1;
        }
    }
}

bool App::image_hit(int px, int py, Pane** pane) {
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->image) continue;
        const ImageView& v = *p->doc->image;
        if (px < v.x || py < v.y || px >= v.x + v.w || py >= v.y + v.h) continue;
        *pane = p.get();
        return true;
    }
    return false;
}

bool App::image_key(const SDL_KeyboardEvent& k) {
    if (!pane_->doc->image || (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) return false;
    ImageView& v = *pane_->doc->image;
    const PictureInfo* p = picture(v.path);
    if (p->state != PictureInfo::kReady) return false;
    const bool cmd = (k.mod & SDL_KMOD_GUI) != 0;
    const double mx = v.ax + v.aw / 2.0, my = v.ay + v.ah / 2.0;
    const double step_x = std::max(1.0, v.aw * 0.1) / (p->full_w * v.scale), step_y = std::max(1.0, v.ah * 0.1) / (p->full_h * v.scale);
    switch (k.key) {
        case SDLK_EQUALS:
        case SDLK_PLUS:
        case SDLK_KP_PLUS: zoom_at(v, *p, 1.25, mx, my); break;
        case SDLK_MINUS:
        case SDLK_KP_MINUS: zoom_at(v, *p, 0.8, mx, my); break;
        case SDLK_0:
        case SDLK_KP_0: v.zoom = 0, v.cx = v.cy = 0.5; break;
        case SDLK_1:
        case SDLK_KP_1:
            if (cmd) return false;
            zoom_at(v, *p, density_ / v.scale, mx, my);
            break;
        default:
            if (cmd) return false;
            switch (k.key) {
                case SDLK_LEFT:
                case SDLK_H: v.cx -= step_x; break;
                case SDLK_RIGHT:
                case SDLK_L: v.cx += step_x; break;
                case SDLK_UP:
                case SDLK_K: v.cy -= step_y; break;
                case SDLK_DOWN:
                case SDLK_J: v.cy += step_y; break;
                default: return false;
            }
            keep_in(v, *p, v.scale);
    }
    term_drop_text_ = true;  // (a key's text is not typed after it)
    dirty_ = true;
    return true;
}

// The wheel moves a zoomed picture; with Cmd (or Ctrl), it zooms, about the mouse.
bool App::image_wheel(const SDL_MouseWheelEvent& w) {
    Pane* pane = nullptr;
    const float mx = mouse_x_ * density_, my = mouse_y_ * density_;
    if (!image_hit(static_cast<int>(mx), static_cast<int>(my), &pane)) return false;
    ImageView& v = *pane->doc->image;
    const PictureInfo* p = picture(v.path);
    if (p->state != PictureInfo::kReady) return true;
    if (SDL_GetModState() & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) {
        zoom_at(v, *p, std::pow(1.1, w.y), mx, my);
    } else {
        const bool precise = w.y != std::floor(w.y) || w.x != std::floor(w.x);
        const double step = precise ? 1.0 : static_cast<double>(font_->cell_h()) * 3;
        v.cx -= w.x * step / (p->full_w * v.scale);
        v.cy -= w.y * step / (p->full_h * v.scale);
        keep_in(v, *p, v.scale);
    }
    dirty_ = true;
    return true;
}

bool App::image_pinch(float by) {
    Pane* pane = nullptr;
    const float mx = mouse_x_ * density_, my = mouse_y_ * density_;
    if (!image_hit(static_cast<int>(mx), static_cast<int>(my), &pane)) return false;
    ImageView& v = *pane->doc->image;
    const PictureInfo* p = picture(v.path);
    if (p->state == PictureInfo::kReady && by > 0) zoom_at(v, *p, by, mx, my);
    dirty_ = true;
    return true;
}

// A click gives the tab the keyboard and starts a drag that moves the
// picture; two clicks switch between fitted and its own size.
bool App::image_mouse_down(const SDL_MouseButtonEvent& b) {
    Pane* pane = nullptr;
    const float px = b.x * density_, py = b.y * density_;
    if (!image_hit(static_cast<int>(px), static_cast<int>(py), &pane)) return false;
    focus_pane(pane);
    dirty_ = true;
    if (b.button != SDL_BUTTON_LEFT) return true;
    ImageView& v = *pane->doc->image;
    const PictureInfo* p = picture(v.path);
    if (p->state != PictureInfo::kReady) return true;
    if (b.clicks == 2) {
        if (v.zoom > 0) v.zoom = 0, v.cx = v.cy = 0.5;
        else zoom_at(v, *p, density_ / v.scale, px, py);
        return true;
    }
    v.dragging = true;
    v.drag_x = px, v.drag_y = py;
    return true;
}

bool App::image_mouse_motion(const SDL_MouseMotionEvent& m) {
    for (const std::unique_ptr<Pane>& pane : panes_) {
        if (!pane->doc || !pane->doc->image || !pane->doc->image->dragging) continue;
        ImageView& v = *pane->doc->image;
        const PictureInfo* p = picture(v.path);
        const float px = m.x * density_, py = m.y * density_;
        if (p->state == PictureInfo::kReady) {
            v.cx -= (px - v.drag_x) / (p->full_w * v.scale);
            v.cy -= (py - v.drag_y) / (p->full_h * v.scale);
            keep_in(v, *p, v.scale);
        }
        v.drag_x = px, v.drag_y = py;
        dirty_ = true;
        return true;
    }
    return false;
}

void App::image_mouse_up() {
    for (const std::unique_ptr<Pane>& pane : panes_)
        if (pane->doc && pane->doc->image) pane->doc->image->dragging = false;
}

// The image tab in front (the first there is), as the tests see it.
std::string App::image_dump() {
    for (const std::unique_ptr<Pane>& pane : panes_) {
        if (!pane->doc || !pane->doc->image) continue;
        const ImageView& v = *pane->doc->image;
        const PictureInfo* p = picture(v.path);
        if (p->state == PictureInfo::kReading) return leaf_of(v.path) + ": reading";
        if (p->state == PictureInfo::kFailed) return leaf_of(v.path) + ": failed (" + p->error + ")";
        char b[320];
        std::snprintf(b, sizeof b, "ready %s %dx%d read %dx%d%s zoom %d%% middle %.2f,%.2f at %d,%d,%d,%d", p->kind.c_str(), p->full_w, p->full_h, p->w, p->h, p->clear ? " clear" : "",
                      static_cast<int>(std::lround(v.scale / density_ * 100)), v.cx, v.cy, static_cast<int>(v.rect[0]), static_cast<int>(v.rect[1]), static_cast<int>(v.rect[2]),
                      static_cast<int>(v.rect[3]));
        return leaf_of(v.path) + ": " + b;
    }
    return "no image tab";
}

// Waits until every picture asked for has been read (40 s at most: one on
// another machine may wait for the connection).
void App::picture_wait() {
    const u64 until = now_ns() + 40'000'000'000ull;
    while (pictures_busy() && now_ns() < until) {
        picture_sync();
        SDL_Delay(2);
    }
    picture_sync();
}

}  // namespace blit
