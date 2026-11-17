// Pictures: image files read on a thread of their own (picture.h: the
// system's reader) and put on the GPU (Renderer::image_add), for an image
// file's tab and for the pictures in the Markdown preview. A picture not
// drawn for a minute is let go; one whose file changes is read again.
// An animation's frames are read together, and go up to the GPU as each is
// first shown; a timer wakes the main loop when the next frame is due, and
// only while one is on show (the frame drawn then is the only one drawn).
// A picture drawn from shapes (SVG) is drawn again at the size it is shown.
// Pictures on the web (the Markdown preview's, when markdown.web_pictures
// is on) are fetched by a reader of their own and kept in blit's cache.
//
// An image file opens in a tab of its own: the picture fitted to the tab
// (never made larger than it is), zoomed with + and - (Cmd too), a pinch or
// Cmd+wheel, moved by dragging it, the wheel, or the arrow keys; 0 fits it
// again and 1 shows it at its own size. The row at the bottom says what it is.
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
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
#include "look.hpp"
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
extern "C" int blit_picture_draw(const void*, size_t, int, int, BlitPicture* out, char* err, size_t err_size) { return blit_picture_read(nullptr, 0, 0, out, err, err_size); }
extern "C" void blit_picture_free(BlitPicture* p) {
    if (p) std::free(p->rgba), p->rgba = nullptr;
}
extern "C" int blit_frames_read(const void*, size_t, int, size_t, BlitFrames* out) {
    *out = BlitFrames{};
    return 0;
}
extern "C" void blit_frames_free(BlitFrames* f) {
    if (f) std::free(f->rgba), std::free(f->ms), f->rgba = nullptr, f->ms = nullptr;
}
extern "C" int blit_fetch(const char*, int, size_t, char** data, size_t* size, char* err, size_t err_size) {
    *data = nullptr, *size = 0;
    if (err && err_size) std::snprintf(err, err_size, "the web is not read on this system yet");
    return 0;
}
extern "C" BlitPdf* blit_pdf_open(const void*, size_t, char* err, size_t err_size) {
    if (err && err_size) std::snprintf(err, err_size, "PDFs are not read on this system yet");
    return nullptr;
}
extern "C" void blit_pdf_close(BlitPdf*) {}
extern "C" int blit_pdf_locked(const BlitPdf*) { return 0; }
extern "C" int blit_pdf_unlock(BlitPdf*, const char*) { return 0; }
extern "C" int blit_pdf_pages(const BlitPdf*) { return 0; }
extern "C" void blit_pdf_page_size(const BlitPdf*, int, double* w, double* h) { *w = 612, *h = 792; }
extern "C" int blit_pdf_render(const BlitPdf*, int, double, int, int, int, int, BlitPicture*) { return 0; }
extern "C" int blit_pdf_text(BlitPdf*, int, int, BlitPdfText* out) {
    *out = BlitPdfText{};
    return 0;
}
extern "C" void blit_pdf_text_free(BlitPdfText*) {}
extern "C" int blit_pdf_links(BlitPdf*, int, BlitPdfLink** out) {
    *out = nullptr;
    return 0;
}
extern "C" void blit_pdf_links_free(BlitPdfLink*, int) {}
extern "C" int blit_pdf_outline(BlitPdf*, BlitPdfHeading** out) {
    *out = nullptr;
    return 0;
}
extern "C" void blit_pdf_outline_free(BlitPdfHeading*, int) {}
#endif

namespace blit {

namespace {

constexpr u64 kMostBytes = 256ull << 20;    // a file larger than this is not read
constexpr int kMostSide = 4096;             // a picture larger than this is read smaller
constexpr u64 kMostFrameBytes = 128ull << 20;  // an animation's frames, all together: one larger is read smaller
constexpr u64 kForgetNs = 60'000'000'000ull;  // a picture not drawn for this long is let go
// An animation's frame drawn this long after it was due was not on show
// meanwhile: it goes on from where it was, rather than catching up.
constexpr u64 kLateNs = 250'000'000ull;
constexpr int kWebMs = 20000;           // a picture on the web: how long it may take to come
constexpr i64 kWebKeepNs = 86'400'000'000'000ll;  // and how long it is kept before it is fetched again

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 17;
    SDL_PushEvent(&ev);
}

// The animation timer that has fired, by its number (0: none). Its
// callback runs on SDL's timer thread, and only says so.
std::atomic<u64> g_frame_fired{0};

Uint64 SDLCALL frame_due(void* number, SDL_TimerID, Uint64) {
    g_frame_fired.store(reinterpret_cast<uintptr_t>(number));
    wake();
    return 0;
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

std::string leaf_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool is_web(std::string_view path) { return path.starts_with("http://") || path.starts_with("https://"); }

// blit's cache folder (where the GPU's compiled pipelines are kept too).
std::string cache_dir() {
    if (const char* d = std::getenv("BLIT_CACHE_DIR"); d && *d) return d;
#if defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home) return std::string(home) + "/Library/Caches/blit";
#else
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return std::string(x) + "/blit";
    if (const char* home = std::getenv("HOME"); home && *home) return std::string(home) + "/.cache/blit";
#endif
    return {};
}

// A picture on the web: the copy in blit's cache, if it was fetched in the
// last day; else fetched (by the system) and kept there. One that cannot
// be fetched now is shown from an older copy, if there is one.
bool web_read(const std::string& url, std::string* bytes, std::string* why, std::stop_token st) {
    u64 h = 1469598103934665603ull;
    for (const char c : url) h = (h ^ static_cast<u8>(c)) * 1099511628211ull;
    char name[40];
    std::snprintf(name, sizeof name, "%016llx", static_cast<unsigned long long>(h));
    const std::string dir = cache_dir(), kept = dir.empty() ? std::string() : dir + "/pictures/" + name;
    SDL_PathInfo info;
    const bool have = !kept.empty() && SDL_GetPathInfo(kept.c_str(), &info) && info.type == SDL_PATHTYPE_FILE;
    SDL_Time now = 0;
    SDL_GetCurrentTime(&now);
    if (have && now - info.modify_time < kWebKeepNs && fs_read_whole(kept, bytes, why, kMostBytes, st)) return true;
    char* data = nullptr;
    size_t size = 0;
    char err[200] = "";
    if (blit_fetch(url.c_str(), kWebMs, kMostBytes, &data, &size, err, sizeof err)) {
        bytes->assign(data, size);
        std::free(data);
        std::string ignored;
        if (!kept.empty() && fs_mkdirs(dir + "/pictures", &ignored)) fs_write(kept, *bytes, &ignored);
        return true;
    }
    if (have && fs_read_whole(kept, bytes, why, kMostBytes, st)) return true;
    *why = err;
    return false;
}

}  // namespace

struct App::Pictures {
    std::map<std::string, PictureInfo, std::less<>> known;
    // What a reader is asked to do: read a picture's file, or draw a
    // picture made from shapes again at another size.
    struct Job {
        std::string path;
        int w = 0, h = 0;  // the size to draw it at (0: read the file)
        std::shared_ptr<const std::string> source;
    };
    struct Done {
        std::string path;
        BlitPicture pic{};
        BlitFrames frames{};  // an animation's
        std::shared_ptr<const std::string> source;  // a picture drawn from shapes: its file
        std::string error;
        u64 bytes = 0;
        i64 stamp = 0;
        bool clear = false;
        bool redraw = false;  // drawn again at another size
    };
    std::mutex mu;
    std::condition_variable_any cv;
    // Three readers: files here, files on other machines (which may wait
    // for a connection), and pictures on the web (which may wait for a
    // server): neither of the slow ones holds up the others.
    std::deque<Job> todo[3];
    std::vector<Done> done;
    u64 tidied_ns = 0;
    // The timer for the next frame of an animation on show (one at a time:
    // the soonest), and its number.
    SDL_TimerID timer = 0;
    u64 timer_due = 0, timer_number = 0;
    u64 shown_in = ~0ull;  // the frame an animation was last drawn in (when it fires, the frame after that is drawn only if one was on show)
    std::jthread workers[3];

    Pictures() {
        for (int k = 0; k < 3; ++k) workers[k] = std::jthread([this, k](std::stop_token st) { run(st, todo[k]); });
    }
    ~Pictures() {
        if (timer) SDL_RemoveTimer(timer);
        for (std::jthread& w : workers) w.request_stop();
        cv.notify_all();
        for (std::jthread& w : workers)
            if (w.joinable()) w.join();
        for (Done& d : done) blit_picture_free(&d.pic), blit_frames_free(&d.frames);
    }
    void want(Job job) {
        {
            std::lock_guard l(mu);
            std::deque<Job>& q = todo[is_web(job.path) ? 2 : is_remote(job.path) ? 1 : 0];
            // (A drawing at another size not begun yet is not wanted any more: this one is.)
            auto same = std::ranges::find_if(q, [&](const Job& j) { return j.path == job.path && j.w > 0; });
            if (job.w > 0 && same != q.end()) *same = std::move(job);
            else q.push_back(std::move(job));
        }
        cv.notify_all();
    }
    void run(std::stop_token st, std::deque<Job>& queue) {
        for (;;) {
            Job job;
            {
                std::unique_lock l(mu);
                if (!cv.wait(l, st, [&] { return !queue.empty(); })) return;
                job = std::move(queue.front());
                queue.pop_front();
            }
            Done d;
            d.path = job.path;
            char why[200] = "";
            if (job.w > 0) {
                d.redraw = true;
                if (!blit_picture_draw(job.source->data(), job.source->size(), job.w, job.h, &d.pic, why, sizeof why)) d.error = why;
            } else {
                d.stamp = stamp_of(job.path);
                std::string bytes;
                if (is_web(job.path) ? web_read(job.path, &bytes, &d.error, st) : fs_read_whole(job.path, &bytes, &d.error, kMostBytes, st)) {
                    if (!blit_picture_read(bytes.data(), bytes.size(), kMostSide, &d.pic, why, sizeof why)) d.error = why;
                    else if (d.pic.frames > 1) blit_frames_read(bytes.data(), bytes.size(), kMostSide, kMostFrameBytes, &d.frames);
                }
                d.bytes = bytes.size();
                if (d.pic.vector) d.source = std::make_shared<const std::string>(std::move(bytes));
            }
            // (See-through anywhere: in its first frame, or any of an animation's.)
            for (size_t k = 3; d.pic.rgba && k < static_cast<size_t>(d.pic.w) * static_cast<size_t>(d.pic.h) * 4 && !d.clear; k += 4) d.clear = d.pic.rgba[k] != 255;
            for (size_t k = 3; d.frames.rgba && k < static_cast<size_t>(d.frames.w) * static_cast<size_t>(d.frames.h) * 4 * static_cast<size_t>(d.frames.frames) && !d.clear; k += 4)
                d.clear = d.frames.rgba[k] != 255;
            if (st.stop_requested()) {
                blit_picture_free(&d.pic);
                blit_frames_free(&d.frames);
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
    u64 source = 0;             // the text tab this shows the picture of (an SVG's, Cmd+Shift+V), or 0
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
    // (Not SVG: it is text, and opens as text; Cmd+Shift+V shows its picture
    // beside it, as it does a Markdown file's preview.)
    static const char* const kKinds[] = {"png", "jpg", "jpeg", "jpe", "gif", "bmp", "webp", "heic", "heif", "avif", "tif", "tiff", "ico", "icns", "tga", "psd", "jp2", "exr", "hdr", "apng"};
    return std::ranges::any_of(kKinds, [&](const char* k) { return ext == k; });
}

bool App::is_svg_path(std::string_view path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || path.size() - dot != 4) return false;
    std::string ext(path.substr(dot + 1));
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "svg";
}

// Cmd+Shift+V on an SVG: its picture in a tab beside it, drawn again each
// time the file is saved (the picture store watches the file). The tab has
// no path of its own, as a Markdown preview has none: going to the file, the
// session and the scripts see only the text.
void App::svg_preview() {
    Doc& src = *pane_->doc;
    if (src.image && src.image->source) {  // (from the picture: back to its text)
        Pane* p = nullptr;
        size_t at = 0;
        if (find_doc(src.image->source, &p, &at)) {
            focus_pane(p);
            tab_show(at);
        }
        return;
    }
    for (const std::unique_ptr<Pane>& p : panes_)
        for (size_t i = 0; i < p->docs.size(); ++i)
            if (p->docs[i]->image && p->docs[i]->image->source == src.id) {
                Pane* const editing = pane_;
                pane_ = p.get();
                tab_show(i);
                focus_pane(editing);
                dirty_ = true;
                return;
            }
    const std::string full = full_path(src.ed.path);
    const u64 id = src.id;
    Pane* const editing = pane_;
    Pane* into = split_pane(editing, kEdgeRight);
    if (!into) into = editing;
    pane_ = into;
    Doc* d = add_doc(pane_->tab + 1);
    d->title = "Preview: " + full.substr(full.find_last_of('/') + 1);
    d->read_only = true;
    d->image = std::make_shared<ImageView>();
    d->image->path = full;
    d->image->source = id;
    picture(full);
    for (size_t i = 0; i < pane_->docs.size(); ++i)
        if (pane_->docs[i].get() == d) tab_show(i);
    focus_pane(editing);
    dirty_ = true;
}

// Cmd+Shift+V: an SVG's picture, or a Markdown preview.
void App::preview_open() {
    const Doc& d = *pane_->doc;
    if ((d.image && d.image->source) || (!d.image && !d.md && is_svg_path(d.ed.path))) svg_preview();
    else md_open();
}

// The picture of this file: being read (asked for now, the first time),
// read, or not readable. The pointer is good until the next event.
const PictureInfo* App::picture(const std::string& path) {
    if (!pictures_) pictures_ = std::make_shared<Pictures>();
    auto it = pictures_->known.find(path);
    if (it == pictures_->known.end()) {
        it = pictures_->known.emplace(path, PictureInfo{}).first;
        pictures_->want({path});
    }
    it->second.used_ns = now_ns();
    return &it->second;
}

namespace {

// An animation's frames, let go of on the GPU.
void drop_frames(Renderer& gpu, PictureInfo& p) {
    for (const u32 id : p.frame_gpu)
        if (id) gpu.image_drop(id);
    p.frame_gpu.clear();
    p.frame_ms.clear();
    p.frames_waiting.reset();
    p.frame = p.played = p.loops = 0;
    p.frame_due_ns = 0;
    p.finished = false;
}

}  // namespace

// Pictures that have been read go onto the GPU; an animation's timer that
// has fired asks for a frame.
void App::picture_sync() {
    if (!pictures_) return;
    if (const u64 fired = g_frame_fired.exchange(0); fired && fired == pictures_->timer_number) {
        pictures_->timer = 0;
        pictures_->timer_due = 0;
        if (pictures_->shown_in == frames_) dirty_ = true;  // (not if the last frame drawn had none on show: it has gone since)
    }
    std::vector<Pictures::Done> done;
    {
        std::lock_guard l(pictures_->mu);
        done.swap(pictures_->done);
    }
    for (Pictures::Done& d : done) {
        auto it = pictures_->known.find(d.path);
        if (it != pictures_->known.end() && d.redraw) {  // drawn again at the size it is shown
            PictureInfo& p = it->second;
            p.redrawing = false;
            if (d.pic.rgba && p.state == PictureInfo::kReady && p.vector)
                if (const u32 id = gpu_.image_add(d.pic.rgba, d.pic.w, d.pic.h)) {
                    if (p.gpu) gpu_.image_drop(p.gpu);
                    p.gpu = id;
                    p.w = d.pic.w, p.h = d.pic.h;
                    p.clear = d.clear;
                }
            dirty_ = true;
        } else if (it != pictures_->known.end()) {
            PictureInfo& p = it->second;
            p.stamp = d.stamp;
            if (!d.error.empty()) {
                if (p.state != PictureInfo::kReady) {  // (one that was there stays, if reading it again fails: it may be being written)
                    p.state = PictureInfo::kFailed;
                    p.error = d.error;
                }
            } else if (const u32 id = gpu_.image_add(d.pic.rgba, d.pic.w, d.pic.h)) {
                if (p.gpu) gpu_.image_drop(p.gpu);
                drop_frames(gpu_, p);
                p.gpu = id;
                p.state = PictureInfo::kReady;
                p.w = d.pic.w, p.h = d.pic.h, p.full_w = d.pic.full_w, p.full_h = d.pic.full_h;
                p.frames = d.pic.frames;
                p.kind = d.pic.kind;
                p.bytes = d.bytes;
                p.clear = d.clear;
                p.vector = d.pic.vector;
                p.redrawing = false;
                p.source = std::move(d.source);
                p.error.clear();
                if (d.frames.frames > 1) {  // an animation: its frames go up as they are shown
                    p.frame_gpu.assign(static_cast<size_t>(d.frames.frames), 0);
                    p.frame_ms.assign(d.frames.ms, d.frames.ms + d.frames.frames);
                    p.frames_waiting = std::shared_ptr<u8>(d.frames.rgba, std::free);
                    d.frames.rgba = nullptr;
                    p.frames_w = d.frames.w, p.frames_h = d.frames.h;
                    p.loops = static_cast<u32>(std::max(0, d.frames.loops));
                }
            } else if (p.state != PictureInfo::kReady) {
                p.state = PictureInfo::kFailed;
                p.error = "the GPU would not take it";
            }
            p.again = false;
            ++pictures_gen_;
            dirty_ = true;
        }
        blit_picture_free(&d.pic);
        blit_frames_free(&d.frames);
    }
}

// What to draw of a picture now (picture() has been asked for it): an
// animation's frame, played as the time goes while it is drawn, and paused
// while it is not (its next frame drawn when it is due, by the timer); a
// picture drawn from shapes drawn again when it is shown at another size.
u32 App::picture_gpu(const std::string& path, double w, double h) {
    auto it = pictures_ ? pictures_->known.find(path) : decltype(pictures_->known)::iterator{};
    if (!pictures_ || it == pictures_->known.end() || it->second.state != PictureInfo::kReady) return 0;
    PictureInfo& p = it->second;
    if (p.vector && p.source && w >= 1 && h >= 1) {
        // Drawn as large as the largest it is shown at (at most 4096 pixels a
        // side, as any picture), when that is larger than the drawing by
        // more than a little; smaller again only when it has been shown at
        // under half the size for two frames (two views of it at two sizes
        // share the larger drawing, and do not ask for each in turn).
        const double k = std::min(1.0, kMostSide / std::max(w, h));
        const int want_w = static_cast<int>(std::max(1.0, std::round(w * k))), want_h = static_cast<int>(std::max(1.0, std::round(h * k)));
        if (p.shown_frame != frames_) {
            p.before_w = p.shown_w, p.before_h = p.shown_h;
            p.shown_w = p.shown_h = 0;
            p.shown_frame = frames_;
        }
        p.shown_w = std::max(p.shown_w, want_w), p.shown_h = std::max(p.shown_h, want_h);
        const int most_w = std::max(p.shown_w, p.before_w), most_h = std::max(p.shown_h, p.before_h);
        if (p.redrawing) {
        } else if (p.shown_w > p.w + std::max(2, p.w / 50) || p.shown_h > p.h + std::max(2, p.h / 50)) {
            p.redrawing = true;
            pictures_->want({path, p.shown_w, p.shown_h, p.source});
        } else if (most_w * 2 < p.w && most_h * 2 < p.h) {
            p.redrawing = true;
            pictures_->want({path, most_w, most_h, p.source});
        }
    }
    const size_t n = p.frame_gpu.size();
    if (n < 2) return p.gpu;
    pictures_->shown_in = frames_;
    const u64 now = now_ns();
    if (!p.finished) {
        if (!p.frame_due_ns) {
            p.frame_due_ns = now + p.frame_ms[p.frame] * 1'000'000ull;  // (it starts, or goes on, from the frame it is at)
        } else if (now + 2'000'000 >= p.frame_due_ns) {  // (a timer a hair early is on time)
            if (now > p.frame_due_ns + kLateNs) {
                p.frame_due_ns = now + p.frame_ms[p.frame] * 1'000'000ull;
            } else {
                while (p.frame_due_ns && now + 2'000'000 >= p.frame_due_ns) {
                    if (p.frame + 1 < n) {
                        ++p.frame;
                    } else if (++p.played, p.loops && p.played >= p.loops) {
                        p.finished = true;  // (it stays on its last frame)
                        p.frame_due_ns = 0;
                        break;
                    } else {
                        p.frame = 0;
                    }
                    p.frame_due_ns += p.frame_ms[p.frame] * 1'000'000ull;
                }
            }
        }
        if (p.frame_due_ns) picture_due(p.frame_due_ns);
    }
    // The frame, up to the GPU the first time it is shown (the frames let go of once all are up).
    u32& id = p.frame_gpu[p.frame];
    if (!id && p.frames_waiting) {
        const size_t one = static_cast<size_t>(p.frames_w) * static_cast<size_t>(p.frames_h) * 4;
        id = gpu_.image_add(p.frames_waiting.get() + one * p.frame, p.frames_w, p.frames_h);
        if (std::ranges::all_of(p.frame_gpu, [](u32 g) { return g != 0; })) p.frames_waiting.reset();
    }
    return id ? id : p.gpu;
}

// The main loop woken when the soonest frame of an animation on show is due.
void App::picture_due(u64 at_ns) {
    Pictures& s = *pictures_;
    if (s.timer && s.timer_due <= at_ns) return;  // (one as soon, or sooner, is set)
    if (s.timer) SDL_RemoveTimer(s.timer);
    const u64 now = now_ns();
    s.timer_due = at_ns;
    s.timer_number = s.timer_number + 1;
    s.timer = SDL_AddTimerNS(std::max<u64>(at_ns > now ? at_ns - now : 0, 1'000'000), frame_due, reinterpret_cast<void*>(static_cast<uintptr_t>(s.timer_number)));
}

// Pictures whose files have changed (here: on another machine they are not watched) are read again.
void App::picture_recheck() {
    if (!pictures_) return;
    for (auto& [path, p] : pictures_->known) {
        if (p.state == PictureInfo::kReading || p.again || is_remote(path)) continue;
        if (stamp_of(path) == p.stamp) continue;
        p.again = true;
        pictures_->want({path});
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
        if (p.state == PictureInfo::kReading || p.again || p.redrawing || now - p.used_ns < kForgetNs) return false;
        if (p.gpu) gpu_.image_drop(p.gpu);
        drop_frames(gpu_, p);
        return true;
    });
}

bool App::pictures_busy() const {
    if (!pictures_) return false;
    for (const auto& [path, p] : pictures_->known)
        if (p.state == PictureInfo::kReading || p.again || p.redrawing) return true;
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
        forget_doc(pane_->doc);  // (the empty file it takes the place of: nothing may still point at it)
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
// The scale that fits the picture into the area. One that is small (an
// icon, pixel art) is drawn larger by whole steps, so that its pixels stay
// sharp squares, until it takes about half the room; one larger than the
// room is made smaller to fit.
double fit_scale(const PictureInfo& p, int aw, int ah, double density) {
    if (p.full_w <= 0 || p.full_h <= 0) return 1;
    const double fit = std::min(static_cast<double>(aw) / p.full_w, static_cast<double>(ah) / p.full_h);
    if (fit < density) return fit;
    const double half = std::min(aw / 2.0 / p.full_w, ah / 2.0 / p.full_h);
    return density * std::max(1.0, std::floor(half / density));
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
                info = p->kind + "   " + std::to_string(p->full_w) + " × " + std::to_string(p->full_h) + "   " + size_text(p->bytes);
                if (p->frame_gpu.size() > 1) info += "   " + std::to_string(p->frames) + " frames";
                else if (p->frames > 1) info += "   " + std::to_string(p->frames) + " pictures, the first shown";
                info += "   " + std::to_string(percent) + "%";
                keys = "1";
            }
            v.box.resize(cols, rows);
            const u32 r = rows - 1;
            // (From the text edge, as every bar: the name, what it is, and on the right the keys.)
            const u32 edge = static_cast<u32>(std::max(1, (look().edge - pad_x + cw - 1) / cw));
            u32 c = put(v.box, font, r, edge, leaf_of(v.path), ink::kStrong);
            c = v.box.put(font, r, c + 3, info, info_ink);
            static constexpr Hint kHints[] = {{"+ -", "zoom"}, {"0", "fit"}, {"1", "actual size"}};
            const u32 kw = hints_width(kHints, font);
            if (!keys.empty() && c + 3 + kw + edge < cols) put_hints(v.box, font, r, cols - edge - kw, kHints);
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
        const double pw = p->full_w * v.scale, ph = p->full_h * v.scale;
        const u32 image = picture_gpu(v.path, pw, ph);
        PictureDraw& d = fd.pictures[fd.picture_count++];
        d = PictureDraw{};
        d.image = image;
        const double left = v.ax + v.aw / 2.0 - v.cx * pw, top = v.ay + v.ah / 2.0 - v.cy * ph;
        d.rect[0] = static_cast<float>(std::round(left)), d.rect[1] = static_cast<float>(std::round(top));
        d.rect[2] = static_cast<float>(std::round(left + pw)), d.rect[3] = static_cast<float>(std::round(top + ph));
        d.clip[0] = v.ax, d.clip[1] = v.ay, d.clip[2] = v.ax + v.aw, d.clip[3] = v.ay + v.ah;
        d.after = fd.popup_count;
        d.sharp = !p->vector && pw / p->w >= 3;
        d.clear = p->clear;
        std::copy(d.rect, d.rect + 4, v.rect);
        if (p->clear) {  // (a checkerboard behind what is see-through)
            d.checker = std::round(8 * density_);
            for (int c = 0; c < 3; ++c) {
                const float fg = c == 0 ? tc.foreground.r : c == 1 ? tc.foreground.g : tc.foreground.b;
                d.check_a[c] = theme_.bg[c] + (fg - theme_.bg[c]) * 0.08f;  // (two tones far enough apart that the picture's edge shows)
                d.check_b[c] = theme_.bg[c] + (fg - theme_.bg[c]) * 0.18f;
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
        // (As the text scrolls: a touchpad's fractions 10 points a unit, a wheel's notches some lines.)
        const bool precise = w.y != std::floor(w.y) || w.x != std::floor(w.x);
        const double step = precise ? 10.0 * scale_ : cfg_.scroll_lines * font_->cell_h();
        v.cx += w.x * step / (p->full_w * v.scale);
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
        char b[320], anim[96] = "";
        if (p->frame_gpu.size() > 1)
            std::snprintf(anim, sizeof anim, " frame %u of %zu%s", p->frame + 1, p->frame_gpu.size(), p->finished ? " (played)" : p->frame_due_ns ? " playing" : "");
        std::snprintf(b, sizeof b, "ready %s %dx%d read %dx%d%s%s zoom %d%% middle %.2f,%.2f at %d,%d,%d,%d", p->kind.c_str(), p->full_w, p->full_h, p->w, p->h, p->clear ? " clear" : "", anim,
                      static_cast<int>(std::lround(v.scale / density_ * 100)), v.cx, v.cy, static_cast<int>(v.rect[0]), static_cast<int>(v.rect[1]), static_cast<int>(v.rect[2]),
                      static_cast<int>(v.rect[3]));
        return leaf_of(v.path) + ": " + b;
    }
    return "no image tab";
}

// For the tests: `ms` milliseconds of what the main loop does (events
// taken in, a frame drawn whenever one is asked for), and how many frames
// were drawn in them, and how many times something woke it. An animation
// on show draws one for each of its frames, and nothing else draws any.
std::string App::picture_frames(int ms) {
    const u64 until = now_ns() + static_cast<u64>(std::max(0, ms)) * 1'000'000ull;
    const u64 before = frames_;
    u64 woken = 0;
    while (now_ns() < until) {
        SDL_Event e;
        if (SDL_PollEvent(&e)) {
            ++woken;
            handle(e);
            while (SDL_PollEvent(&e)) handle(e);
        }
        if (dirty_) frame();
        SDL_Delay(1);
    }
    return std::to_string(frames_ - before) + " frames drawn, woken " + std::to_string(woken) + " times";
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
