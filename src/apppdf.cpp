// PDF documents in a tab of their own: the pages one under another, fitted
// to the tab's width, scrolled and zoomed. A thread of its own reads the
// document (here or on another machine) and draws pages as they come into
// view (picture.h: the system's CoreGraphics), at the size they are shown:
// a page is drawn whole up to 4096 pixels on a side; zoomed in further, the
// part of it on show is drawn again, sharp, over that. A page shows blank
// (white) until it is drawn, and a drawing made at another zoom stands in,
// stretched, until the new one comes. Pages far from the view are let go.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#include "app.hpp"
#include "fsio.hpp"
#include "look.hpp"
#include "picture.h"
#include "remote.hpp"
#include "textbox.hpp"

namespace blit {

namespace {

constexpr u64 kMostBytes = 512ull << 20;  // a file larger than this is not read
constexpr double kGap = 10;               // points between pages, and around them
constexpr double kMostSide = 4096;        // a page drawn whole is at most this many pixels on a side
constexpr int kKeep = 3;                  // pages kept drawn either side of those on show

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 18;
    SDL_PushEvent(&ev);
}

i64 stamp_of(const std::string& path) {
    SDL_PathInfo info;
    return !is_remote(path) && SDL_GetPathInfo(path.c_str(), &info) ? static_cast<i64>(info.modify_time) : 0;
}

std::string leaf_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string size_text(u64 bytes) {
    char b[32];
    if (bytes < 1024 * 1024) std::snprintf(b, sizeof b, "%.1f KB", static_cast<double>(bytes) / 1024);
    else std::snprintf(b, sizeof b, "%.1f MB", static_cast<double>(bytes) / (1024 * 1024));
    return b;
}

}  // namespace

// A document as the reader has it (only its thread uses `pdf`).
struct PdfFile {
    BlitPdf* pdf = nullptr;
    ~PdfFile() { blit_pdf_close(pdf); }
};

// What the reader is asked to do: read a document, or draw (part of) a page.
struct PdfJob {
    u64 doc = 0;  // the tab's document
    std::string path;
    std::shared_ptr<PdfFile> file;  // null: read it
    int page = 0;
    double scale = 0;
    int x = 0, y = 0, w = 0, h = 0;  // the part of the page, in its pixels at that scale
    bool detail = false;
    bool operator==(const PdfJob& o) const {
        return doc == o.doc && file == o.file && page == o.page && scale == o.scale && x == o.x && y == o.y && w == o.w && h == o.h && detail == o.detail;
    }
};

PdfJob read_job(u64 doc, const std::string& path) {
    PdfJob j;
    j.doc = doc;
    j.path = path;
    return j;
}

PdfJob draw_job(u64 doc, const std::shared_ptr<PdfFile>& file, int page, double scale, int x, int y, int w, int h, bool detail) {
    PdfJob j;
    j.doc = doc, j.file = file, j.page = page, j.scale = scale, j.x = x, j.y = y, j.w = w, j.h = h, j.detail = detail;
    return j;
}

struct App::Pdfs {
    struct Done {
        PdfJob job;
        std::shared_ptr<PdfFile> file;                 // a document read
        std::vector<std::pair<double, double>> sizes;  // its pages' sizes (points)
        u64 bytes = 0;
        i64 stamp = 0;
        BlitPicture pic{};  // a page drawn
        std::string error;
    };
    std::mutex mu;
    std::condition_variable_any cv;
    std::deque<PdfJob> reads;    // documents to read (before any drawing)
    std::vector<PdfJob> draws;   // what to draw, the most wanted first: replaced whole as the view changes
    PdfJob drawing;              // what is being drawn now (doc 0: nothing)
    std::vector<Done> done;
    u32 blank = 0;               // a white pixel, stretched over a page not drawn yet
    std::jthread worker;

    Pdfs() {
        worker = std::jthread([this](std::stop_token st) { run(st); });
    }
    ~Pdfs() {
        worker.request_stop();
        cv.notify_all();
        if (worker.joinable()) worker.join();
        for (Done& d : done) blit_picture_free(&d.pic);
    }
    void run(std::stop_token st) {
        for (;;) {
            PdfJob job;
            {
                std::unique_lock l(mu);
                if (!cv.wait(l, st, [&] { return !reads.empty() || !draws.empty(); })) return;
                if (!reads.empty()) {
                    job = std::move(reads.front());
                    reads.pop_front();
                } else {
                    job = std::move(draws.front());
                    draws.erase(draws.begin());
                    drawing = job;
                }
            }
            Done d;
            if (!job.file) {  // a document to read
                std::string bytes;
                d.stamp = stamp_of(job.path);
                if (fs_read_whole(job.path, &bytes, &d.error, kMostBytes, st)) {
                    char why[160] = "";
                    if (BlitPdf* pdf = blit_pdf_open(bytes.data(), bytes.size(), why, sizeof why)) {
                        d.file = std::make_shared<PdfFile>();
                        d.file->pdf = pdf;
                        const int n = blit_pdf_pages(pdf);
                        for (int k = 0; k < n; ++k) {
                            double w = 0, h = 0;
                            blit_pdf_page_size(pdf, k, &w, &h);
                            d.sizes.emplace_back(std::max(w, 1.0), std::max(h, 1.0));
                        }
                    } else {
                        d.error = why;
                    }
                }
                d.bytes = bytes.size();
            } else if (!blit_pdf_render(job.file->pdf, job.page, job.scale, job.x, job.y, job.w, job.h, &d.pic)) {
                d.error = "the page could not be drawn";
            }
            if (st.stop_requested()) {
                blit_picture_free(&d.pic);
                return;
            }
            d.job = std::move(job);
            {
                std::lock_guard l(mu);
                drawing = PdfJob{};
                done.push_back(std::move(d));
            }
            wake();
        }
    }
};

// A PDF's tab.
struct PdfView {
    std::string path;
    Renderer* gpu = nullptr;  // (to let go of its pages' drawings when it goes)
    u8 state = PictureInfo::kReading;
    std::string error;
    std::shared_ptr<PdfFile> file;
    u64 bytes = 0;
    i64 stamp = 0;
    bool again = false;  // being read again (its file changed)
    struct Page {
        double w = 0, h = 0, top = 0;  // its size, and where its top is, in points
        u32 whole = 0;                 // drawn whole, at whole_scale
        double whole_scale = 0;
        u32 part = 0;                  // drawn in part (zoomed in far), at part_scale: part_rect of its pixels
        double part_scale = 0;
        int part_rect[4] = {};
    };
    std::vector<Page> pages;
    double doc_w = 0, doc_h = 0;  // all of it, in points (the gaps too)
    // The view: pixels to a point (0: fitted to the width), and the point of
    // the document at the area's top left.
    double zoom = 0, scale = 1;
    double left = 0, top = 0;
    bool dragging = false;
    float drag_x = 0, drag_y = 0;
    // As drawn: the tab, and the area the pages are in.
    int x = 0, y = 0, w = 0, h = 0, ax = 0, ay = 0, aw = 0, ah = 0;
    TextBox box;
    u64 key = 0;

    void drop(Page& p) {
        if (p.whole && gpu) gpu->image_drop(p.whole);
        if (p.part && gpu) gpu->image_drop(p.part);
        p.whole = p.part = 0;
        p.whole_scale = p.part_scale = 0;
    }
    ~PdfView() {
        for (Page& p : pages) drop(p);
    }
    // The page whose middle is nearest the middle of the area.
    int current() const {
        const double mid = top + ah / 2.0 / scale;
        int best = 0;
        for (size_t k = 0; k < pages.size(); ++k)
            if (pages[k].top <= mid) best = static_cast<int>(k);
        return best;
    }
    void clamp() {
        const double vw = aw / scale, vh = ah / scale;
        left = doc_w <= vw ? (doc_w - vw) / 2 : std::clamp(left, 0.0, doc_w - vw);
        top = std::clamp(top, 0.0, std::max(0.0, doc_h - vh));
    }
    // Zoomed to `next` pixels a point, the point at (px, py) in the window staying where it is.
    void zoom_to(double next, double px, double py) {
        next = std::clamp(next, 0.05, 40.0);
        const double dx = left + (px - ax) / scale, dy = top + (py - ay) / scale;
        scale = zoom = next;
        left = dx - (px - ax) / scale;
        top = dy - (py - ay) / scale;
        clamp();
    }
};

bool App::is_pdf_path(std::string_view path) {
    if (path.size() < 4) return false;
    std::string ext(path.substr(path.size() - 4));
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".pdf";
}

// A PDF in a tab of its own (open_path() sends it here).
bool App::pdf_open(const std::string& full) {
    if (!pdfs_) pdfs_ = std::make_shared<Pdfs>();
    auto fresh = std::make_unique<Doc>();
    fresh->ed.path = full;
    fresh->read_only = true;
    fresh->pdf = std::make_shared<PdfView>();
    fresh->pdf->path = full;
    fresh->pdf->gpu = &gpu_;
    fresh->id = next_doc_id_++;
    if (!is_remote(full)) {
        fresh->disk_stamp = stamp_of(full);
        recent_add(full, false);
    }
    {
        std::lock_guard l(pdfs_->mu);
        pdfs_->reads.push_back(read_job(fresh->id, full));
    }
    pdfs_->cv.notify_all();
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

// What the reader has done: documents read, pages drawn.
void App::pdf_sync() {
    if (!pdfs_) return;
    std::vector<Pdfs::Done> done;
    {
        std::lock_guard l(pdfs_->mu);
        done.swap(pdfs_->done);
    }
    for (Pdfs::Done& d : done) {
        Doc* doc = find_doc(d.job.doc);
        PdfView* v = doc ? doc->pdf.get() : nullptr;
        if (v && !d.job.file) {  // the document
            v->again = false;
            v->stamp = d.stamp;
            if (!d.file) {
                if (v->state != PictureInfo::kReady) v->state = PictureInfo::kFailed, v->error = d.error;
            } else {
                // Its pages, one under another; the view kept on the same page (if it was read again).
                const bool first = v->pages.empty();
                const int was = first ? 0 : v->current();
                const double into = v->pages.empty() ? 0 : v->top - v->pages[static_cast<size_t>(was)].top;
                for (PdfView::Page& p : v->pages) v->drop(p);
                v->pages.assign(d.sizes.size(), {});
                v->doc_w = 0;
                double y = kGap;
                for (size_t k = 0; k < d.sizes.size(); ++k) {
                    PdfView::Page& p = v->pages[k];
                    p.w = d.sizes[k].first, p.h = d.sizes[k].second, p.top = y;
                    y += p.h + kGap;
                    v->doc_w = std::max(v->doc_w, p.w + 2 * kGap);
                }
                v->doc_h = y;
                v->file = std::move(d.file);
                v->bytes = d.bytes;
                v->state = PictureInfo::kReady;
                v->error.clear();
                const size_t back = std::min(static_cast<size_t>(was), v->pages.size() - 1);
                v->top = first ? 0 : v->pages[back].top + into;
            }
        } else if (v && d.job.file == v->file && d.job.page < static_cast<int>(v->pages.size()) && d.pic.rgba) {  // a page
            PdfView::Page& p = v->pages[static_cast<size_t>(d.job.page)];
            if (const u32 id = gpu_.image_add(d.pic.rgba, d.pic.w, d.pic.h)) {
                if (d.job.detail) {
                    if (p.part) gpu_.image_drop(p.part);
                    p.part = id;
                    p.part_scale = d.job.scale;
                    p.part_rect[0] = d.job.x, p.part_rect[1] = d.job.y, p.part_rect[2] = d.job.x + d.job.w, p.part_rect[3] = d.job.y + d.job.h;
                } else {
                    if (p.whole) gpu_.image_drop(p.whole);
                    p.whole = id;
                    p.whole_scale = d.job.scale;
                }
            }
        }
        blit_picture_free(&d.pic);
        dirty_ = true;
    }
}

// PDFs whose files have changed are read again.
void App::pdf_recheck() {
    if (!pdfs_) return;
    for (const std::unique_ptr<Pane>& pane : panes_)
        for (const std::unique_ptr<Doc>& d : pane->docs) {
            if (!d->pdf || d->pdf->again || d->pdf->state == PictureInfo::kReading || is_remote(d->pdf->path)) continue;
            if (stamp_of(d->pdf->path) == d->pdf->stamp) continue;
            d->pdf->again = true;
            {
                std::lock_guard l(pdfs_->mu);
                pdfs_->reads.push_back(read_job(d->id, d->pdf->path));
            }
            pdfs_->cv.notify_all();
        }
}

void App::pdf_layout(FrameDesc& fd) {
    if (!pdfs_) return;
    Font& font = *font_;
    const int cw = font.cell_w(), ch = font.cell_h();
    std::vector<PdfJob> want;  // what should be drawn, the most wanted first
    std::vector<PdfJob> later;  // (pages either side of those on show)
    for (const std::unique_ptr<Pane>& pane : panes_) {
        if (!pane->doc || !pane->doc->pdf || fd.popup_count >= std::size(fd.popups)) continue;
        PdfView& v = *pane->doc->pdf;
        const int x = pane->x0, y = pane->text_y, w = pane->x1 - pane->x0, h = pane->y1 - pane->text_y;
        if (w < cw * 4 || h < ch * 3) continue;
        v.x = x, v.y = y, v.w = w, v.h = h;
        const u32 cols = static_cast<u32>(w / cw), rows = static_cast<u32>(h / ch);
        const int pad_x = (w - static_cast<int>(cols) * cw) / 2, pad_y = (h - static_cast<int>(rows) * ch) / 2;
        v.ax = x, v.ay = y, v.aw = std::max(1, w), v.ah = std::max(1, h - pad_y - ch);
        if (v.state == PictureInfo::kReady) {
            if (v.zoom <= 0) v.scale = v.aw / v.doc_w;  // (fitted to the width)
            v.clamp();
        }
        const int now = v.state == PictureInfo::kReady ? v.current() : 0;
        const int percent = static_cast<int>(std::lround(v.scale / density_ * 100));
        u64 key = 1469598103934665603ull;
        auto mix = [&](u64 k) { key = (key ^ k) * 1099511628211ull; };
        mix(cols), mix(rows), mix(font.generation()), mix(v.state), mix(v.pages.size()), mix(static_cast<u64>(now)), mix(static_cast<u64>(percent));
        if (key != v.key) {
            v.key = key;
            std::string info, keys;
            u32 info_ink = kInkMuted;
            if (v.state == PictureInfo::kReading) {
                info = "Reading…";
            } else if (v.state == PictureInfo::kFailed) {
                info = "Cannot show it: " + v.error;
                info_ink = kInkDiagnostic;
            } else {
                info = "Page " + std::to_string(now + 1) + " of " + std::to_string(v.pages.size()) + "   " + size_text(v.bytes) + "   " + std::to_string(percent) + "%";
                keys = "1";
            }
            v.box.resize(cols, rows);
            const u32 r = rows - 1;
            // (From the text edge, as every bar: the name, where in it, and on the right the keys.)
            const u32 edge = static_cast<u32>(std::max(1, (look().edge - pad_x + cw - 1) / cw));
            u32 c = put(v.box, font, r, edge, leaf_of(v.path), ink::kStrong);
            c = v.box.put(font, r, c + 3, info, info_ink);
            static constexpr Hint kHints[] = {{"n p", "page"}, {"+ -", "zoom"}, {"0", "width"}, {"9", "whole page"}};
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
        if (v.state != PictureInfo::kReady) continue;
        if (!pdfs_->blank) {
            const u8 white[4] = {255, 255, 255, 255};
            pdfs_->blank = gpu_.image_add(white, 1, 1);
        }
        // The pages on show: drawn (or blank, or stretched), and what they want drawn.
        const double vy0 = v.top, vy1 = v.top + v.ah / v.scale;
        int first = -1, last = -1;
        for (size_t k = 0; k < v.pages.size(); ++k) {
            PdfView::Page& p = v.pages[k];
            if (p.top + p.h < vy0 || p.top > vy1) continue;
            if (first < 0) first = static_cast<int>(k);
            last = static_cast<int>(k);
            const double left = std::round(v.ax + ((v.doc_w - p.w) / 2 - v.left) * v.scale), top = std::round(v.ay + (p.top - v.top) * v.scale);
            const double pw = std::round(p.w * v.scale), ph = std::round(p.h * v.scale);
            auto draw = [&](u32 image, double x0, double y0, double x1, double y1) {
                if (!image || fd.picture_count >= std::size(fd.pictures)) return;
                PictureDraw& d = fd.pictures[fd.picture_count++];
                d = PictureDraw{};
                d.image = image;
                d.rect[0] = static_cast<float>(x0), d.rect[1] = static_cast<float>(y0), d.rect[2] = static_cast<float>(x1), d.rect[3] = static_cast<float>(y1);
                d.clip[0] = v.ax, d.clip[1] = v.ay, d.clip[2] = v.ax + v.aw, d.clip[3] = v.ay + v.ah;
                d.after = fd.popup_count;
            };
            draw(p.whole ? p.whole : pdfs_->blank, left, top, left + pw, top + ph);
            // Drawn whole at this zoom (or as near as 4096 pixels a side allow).
            const double whole = std::min(v.scale, kMostSide / std::max(p.w, p.h));
            if (std::abs(p.whole_scale - whole) > whole * 0.005) want.push_back(draw_job(pane->doc->id, v.file, static_cast<int>(k), whole, 0, 0, static_cast<int>(std::lround(p.w * whole)), static_cast<int>(std::lround(p.h * whole)), false));
            // Zoomed past that: the part on show drawn sharp over it (with a margin, for moving a little).
            if (v.scale > whole * 1.001) {
                const int sx0 = static_cast<int>(std::floor((std::max<double>(v.ax, left) - left))), sy0 = static_cast<int>(std::floor(std::max<double>(v.ay, top) - top));
                const int sx1 = static_cast<int>(std::ceil(std::min<double>(v.ax + v.aw, left + pw) - left)), sy1 = static_cast<int>(std::ceil(std::min<double>(v.ay + v.ah, top + ph) - top));
                const bool covered = p.part && p.part_scale == v.scale && p.part_rect[0] <= sx0 && p.part_rect[1] <= sy0 && p.part_rect[2] >= sx1 && p.part_rect[3] >= sy1;
                if (!covered && sx1 > sx0 && sy1 > sy0) {
                    const int mx = v.aw / 4, my = v.ah / 4;
                    const int rx0 = std::max(0, sx0 - mx), ry0 = std::max(0, sy0 - my);
                    const int rx1 = std::min(static_cast<int>(pw), sx1 + mx), ry1 = std::min(static_cast<int>(ph), sy1 + my);
                    want.push_back(draw_job(pane->doc->id, v.file, static_cast<int>(k), v.scale, rx0, ry0, rx1 - rx0, ry1 - ry0, true));
                }
                if (p.part) {
                    const double r = v.scale / p.part_scale;
                    draw(p.part, left + p.part_rect[0] * r, top + p.part_rect[1] * r, left + p.part_rect[2] * r, top + p.part_rect[3] * r);
                }
            } else if (p.part) {  // (not zoomed in so far any more)
                gpu_.image_drop(p.part);
                p.part = 0;
                p.part_scale = 0;
            }
        }
        // Pages either side drawn ahead; those further away let go.
        for (int k = 0; k < static_cast<int>(v.pages.size()); ++k) {
            PdfView::Page& p = v.pages[static_cast<size_t>(k)];
            if (first >= 0 && (k >= first - kKeep && k <= last + kKeep)) {
                if (k >= first && k <= last) continue;
                const double whole = std::min(v.scale, kMostSide / std::max(p.w, p.h));
                if (std::abs(p.whole_scale - whole) > whole * 0.005 && std::abs(k - (k < first ? first : last)) <= 1)
                    later.push_back(draw_job(pane->doc->id, v.file, k, whole, 0, 0, static_cast<int>(std::lround(p.w * whole)), static_cast<int>(std::lround(p.h * whole)), false));
                if (p.part) gpu_.image_drop(p.part), p.part = 0, p.part_scale = 0;
            } else if (p.whole || p.part) {
                v.drop(p);
            }
        }
    }
    want.insert(want.end(), later.begin(), later.end());
    // The reader's list, if it has changed (what it is drawing now is not asked for again).
    std::lock_guard l(pdfs_->mu);
    std::erase(want, pdfs_->drawing);
    if (want != pdfs_->draws) {
        pdfs_->draws = std::move(want);
        pdfs_->cv.notify_all();
    }
}

bool App::pdf_hit(int px, int py, Pane** pane) {
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->pdf) continue;
        const PdfView& v = *p->doc->pdf;
        if (px < v.x || py < v.y || px >= v.x + v.w || py >= v.y + v.h) continue;
        *pane = p.get();
        return true;
    }
    return false;
}

bool App::pdf_key(const SDL_KeyboardEvent& k) {
    if (!pane_->doc->pdf || (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) return false;
    PdfView& v = *pane_->doc->pdf;
    if (v.state != PictureInfo::kReady) return false;
    const bool cmd = (k.mod & SDL_KMOD_GUI) != 0, shift = (k.mod & SDL_KMOD_SHIFT) != 0;
    const double mx = v.ax + v.aw / 2.0, my = v.ay + v.ah / 2.0;
    const double line = cfg_.scroll_lines * font_->cell_h() / v.scale, screen = std::max(line, v.ah / v.scale - 3 * line);
    const int now = v.current();
    auto go_page = [&](int k2) {
        k2 = std::clamp(k2, 0, static_cast<int>(v.pages.size()) - 1);
        v.top = v.pages[static_cast<size_t>(k2)].top - kGap;
    };
    switch (k.key) {
        case SDLK_EQUALS:
        case SDLK_PLUS:
        case SDLK_KP_PLUS: v.zoom_to(v.scale * 1.25, mx, my); break;
        case SDLK_MINUS:
        case SDLK_KP_MINUS: v.zoom_to(v.scale * 0.8, mx, my); break;
        case SDLK_0:
        case SDLK_KP_0: v.zoom = 0; break;
        default:
            if (cmd) return false;
            switch (k.key) {
                case SDLK_1:
                case SDLK_KP_1: v.zoom_to(density_, mx, my); break;
                case SDLK_9: {  // the page on show, all of it
                    const PdfView::Page& p = v.pages[static_cast<size_t>(now)];
                    v.scale = v.zoom = std::min(v.aw / (p.w + 2 * kGap), v.ah / (p.h + 2 * kGap));
                    go_page(now);
                    break;
                }
                case SDLK_DOWN:
                case SDLK_J: v.top += line; break;
                case SDLK_UP:
                case SDLK_K: v.top -= line; break;
                case SDLK_LEFT:
                case SDLK_H: v.left -= line; break;
                case SDLK_RIGHT:
                case SDLK_L: v.left += line; break;
                case SDLK_SPACE:
                case SDLK_PAGEDOWN: v.top += shift && k.key == SDLK_SPACE ? -screen : screen; break;
                case SDLK_B:
                case SDLK_PAGEUP: v.top -= screen; break;
                case SDLK_N: go_page(now + 1); break;
                case SDLK_P: go_page(now - 1); break;
                case SDLK_HOME: v.top = 0; break;
                case SDLK_END: v.top = v.doc_h; break;
                case SDLK_G: v.top = shift ? v.doc_h : 0; break;
                default: return false;
            }
    }
    v.clamp();
    term_drop_text_ = true;  // (a key's text is not typed after it)
    dirty_ = true;
    return true;
}

// The wheel scrolls (as the text does); with Cmd (or Ctrl) it zooms, about the mouse.
bool App::pdf_wheel(const SDL_MouseWheelEvent& w) {
    Pane* pane = nullptr;
    const float mx = mouse_x_ * density_, my = mouse_y_ * density_;
    if (!pdf_hit(static_cast<int>(mx), static_cast<int>(my), &pane)) return false;
    PdfView& v = *pane->doc->pdf;
    if (v.state != PictureInfo::kReady) return true;
    if (SDL_GetModState() & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) {
        v.zoom_to(v.scale * std::pow(1.1, w.y), mx, my);
    } else {
        const bool precise = w.y != std::floor(w.y) || w.x != std::floor(w.x);
        const double step = (precise ? 10.0 * scale_ : cfg_.scroll_lines * font_->cell_h()) / v.scale;
        v.left += w.x * step;
        v.top -= w.y * step;
        v.clamp();
    }
    dirty_ = true;
    return true;
}

bool App::pdf_pinch(float by) {
    Pane* pane = nullptr;
    const float mx = mouse_x_ * density_, my = mouse_y_ * density_;
    if (!pdf_hit(static_cast<int>(mx), static_cast<int>(my), &pane)) return false;
    PdfView& v = *pane->doc->pdf;
    if (v.state == PictureInfo::kReady && by > 0) v.zoom_to(v.scale * by, mx, my);
    dirty_ = true;
    return true;
}

// A click gives the tab the keyboard and starts a drag that moves the pages.
bool App::pdf_mouse_down(const SDL_MouseButtonEvent& b) {
    Pane* pane = nullptr;
    const float px = b.x * density_, py = b.y * density_;
    if (!pdf_hit(static_cast<int>(px), static_cast<int>(py), &pane)) return false;
    focus_pane(pane);
    dirty_ = true;
    if (b.button != SDL_BUTTON_LEFT) return true;
    PdfView& v = *pane->doc->pdf;
    v.dragging = true;
    v.drag_x = px, v.drag_y = py;
    return true;
}

bool App::pdf_mouse_motion(const SDL_MouseMotionEvent& m) {
    for (const std::unique_ptr<Pane>& pane : panes_) {
        if (!pane->doc || !pane->doc->pdf || !pane->doc->pdf->dragging) continue;
        PdfView& v = *pane->doc->pdf;
        const float px = m.x * density_, py = m.y * density_;
        if (v.state == PictureInfo::kReady) {
            v.left -= (px - v.drag_x) / v.scale;
            v.top -= (py - v.drag_y) / v.scale;
            v.clamp();
        }
        v.drag_x = px, v.drag_y = py;
        dirty_ = true;
        return true;
    }
    return false;
}

void App::pdf_mouse_up() {
    for (const std::unique_ptr<Pane>& pane : panes_)
        if (pane->doc && pane->doc->pdf) pane->doc->pdf->dragging = false;
}

bool App::pdfs_busy() const {
    if (!pdfs_) return false;
    for (const std::unique_ptr<Pane>& pane : panes_)
        for (const std::unique_ptr<Doc>& d : pane->docs)
            if (d->pdf && (d->pdf->state == PictureInfo::kReading || d->pdf->again)) return true;
    std::lock_guard l(pdfs_->mu);
    return !pdfs_->reads.empty() || !pdfs_->draws.empty() || pdfs_->drawing.doc || !pdfs_->done.empty();
}

// The PDF tab in front (the first there is), as the tests see it.
std::string App::pdf_dump() {
    for (const std::unique_ptr<Pane>& pane : panes_) {
        if (!pane->doc || !pane->doc->pdf) continue;
        const PdfView& v = *pane->doc->pdf;
        if (v.state == PictureInfo::kReading) return leaf_of(v.path) + ": reading";
        if (v.state == PictureInfo::kFailed) return leaf_of(v.path) + ": failed (" + v.error + ")";
        std::string drawn;
        for (size_t k = 0; k < v.pages.size(); ++k)
            if (v.pages[k].whole) drawn += (drawn.empty() ? "" : ",") + std::to_string(k + 1) + (v.pages[k].part ? "+" : "");
        char b[256];
        std::snprintf(b, sizeof b, "ready, %zu pages, page %d, zoom %d%%, drawn %s, top %.0f", v.pages.size(), v.current() + 1, static_cast<int>(std::lround(v.scale / density_ * 100)),
                      drawn.empty() ? "none" : drawn.c_str(), v.top);
        return leaf_of(v.path) + ": " + b;
    }
    return "no PDF tab";
}

// Where page `page` (from 1) of the PDF tab in front is drawn: x0,y0,x1,y1 (for the tests).
std::string App::pdf_page_rect(int page) {
    for (const std::unique_ptr<Pane>& pane : panes_) {
        if (!pane->doc || !pane->doc->pdf) continue;
        const PdfView& v = *pane->doc->pdf;
        if (page < 1 || page > static_cast<int>(v.pages.size())) return "none";
        const PdfView::Page& p = v.pages[static_cast<size_t>(page - 1)];
        const long left = std::lround(v.ax + ((v.doc_w - p.w) / 2 - v.left) * v.scale), top = std::lround(v.ay + (p.top - v.top) * v.scale);
        return std::to_string(left) + "," + std::to_string(top) + "," + std::to_string(left + std::lround(p.w * v.scale)) + "," + std::to_string(top + std::lround(p.h * v.scale));
    }
    return "none";
}

// Waits until the PDFs have been read and their pages on show drawn (20 s at most).
void App::pdf_wait() {
    const u64 until = now_ns() + 20'000'000'000ull;
    while (now_ns() < until) {
        pdf_sync();
        frame();
        if (!pdfs_busy()) break;
        SDL_Delay(2);
    }
    pdf_sync();
}

}  // namespace blit
