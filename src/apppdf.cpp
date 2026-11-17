// PDF documents in a tab of their own: the pages one under another, fitted
// to the tab's width, scrolled and zoomed. A thread of its own reads the
// document (here or on another machine) and draws pages as they come into
// view (picture.h: the system's CoreGraphics), at the size they are shown:
// a page is drawn whole up to 4096 pixels on a side; zoomed in further, the
// part of it on show is drawn again, sharp, over that. A page shows blank
// (white) until it is drawn, and a drawing made at another zoom stands in,
// stretched, until the new one comes. Pages far from the view are let go.
//
// The same thread reads each page's text as it comes into view (PDFKit's:
// where each character and line is), and its links: text is chosen by
// dragging over it and copied with Cmd+C; a link is followed by a click.
// Cmd+F finds text in all of it, on that thread, a slice at a time between
// the drawings, and marks what it found on the pages on show. The outline,
// if the document has one, is a list in a box at the side (o). A document
// locked with a password asks for it in the tab; the password goes to the
// thread to open it with, and is wiped as soon as it has been tried.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
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
#include "utf8.hpp"

namespace blit {

namespace {

constexpr u64 kMostBytes = 512ull << 20;  // a file larger than this is not read
constexpr double kGap = 10;               // points between pages, and around them
constexpr double kMostSide = 4096;        // a page drawn whole is at most this many pixels on a side
constexpr int kKeep = 3;                  // pages kept drawn either side of those on show
constexpr u64 kFindSliceNs = 8'000'000;   // the reader finds for this long at a time, and draws pages between
constexpr size_t kMostFound = 10000;      // matches found, at most
constexpr size_t kMostPassword = 255;     // bytes of a password typed, at most
// How strongly what is chosen, what was found and the match that is the
// current one are washed over the page (in the theme's colours for them).
constexpr float kChosenWash = 0.4f, kFoundWash = 0.35f, kCurrentWash = 0.55f;

// Scripted runs (no window): what Cmd+C copied and the web address a link
// would have opened, here and not in the system's clipboard or browser.
std::string g_pdf_clipboard, g_pdf_opened;

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

// A password as it is typed and tried: its bytes in one block that is
// never copied (only moved, which hands the block over), and wiped before
// it goes. Room is kept for the longest, so that typing never moves it.
using Secret = std::vector<char>;

void wipe(Secret& s) {
    s.resize(s.capacity());
    volatile char* p = s.data();
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
}

}  // namespace

// A page's text, as the reader read it: UTF-8, where each character (a
// UTF-16 unit, as PDFKit counts them) starts in it, and, read for a page
// on show, where each character and each line is on the page (points from
// its top left) and the links on it. Never changed once made.
struct PdfLink {
    float box[4] = {};
    std::string url;  // a web address, or "":
    int page = -1;    // a page of the document,
    float y = 0;      // and how far down it
};

// An entry of the outline: what it says, how deep it is (0: at the top), and where it goes.
struct PdfHeading {
    std::string title;
    int depth = 0, page = -1;
    float y = 0;
};

struct PdfText {
    std::string text, folded;  // (folded: in small letters, for finding)
    std::vector<u32> at;       // characters + 1
    std::vector<float> box;    // 4 a character (empty: where they are was not read)
    std::vector<int> line;     // a character's line
    std::vector<float> lines;  // 4 a line
    std::vector<u32> line_from, line_to;  // each line's characters, [from, to)
    std::vector<PdfLink> links;
    u32 count() const { return at.empty() ? 0 : static_cast<u32>(at.size() - 1); }
    bool boxed() const { return !box.empty(); }
    // The character a byte of the text is in.
    u32 char_at(size_t byte) const { return static_cast<u32>(std::upper_bound(at.begin(), at.end(), static_cast<u32>(byte)) - at.begin()) - 1; }
};

namespace {

std::shared_ptr<const PdfText> take_text(BlitPdfText& t) {
    auto out = std::make_shared<PdfText>();
    const size_t n = static_cast<size_t>(std::max(0, t.count));
    out->text.assign(t.text ? t.text : "");
    if (t.at) out->at.assign(t.at, t.at + n + 1);
    else out->at.assign(1, 0);
    out->folded = out->text;
    for (char& c : out->folded) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (t.box && t.line && t.line_box && t.lines > 0) {
        out->box.assign(t.box, t.box + n * 4);
        out->line.assign(t.line, t.line + n);
        out->lines.assign(t.line_box, t.line_box + static_cast<size_t>(t.lines) * 4);
        out->line_from.assign(static_cast<size_t>(t.lines), ~0u);
        out->line_to.assign(static_cast<size_t>(t.lines), 0);
        for (u32 i = 0; i < n; ++i) {
            const size_t l = static_cast<size_t>(std::clamp(out->line[i], 0, t.lines - 1));
            out->line[i] = static_cast<int>(l);
            out->line_from[l] = std::min(out->line_from[l], i);
            out->line_to[l] = std::max(out->line_to[l], i + 1);
        }
        for (size_t l = 0; l < out->line_from.size(); ++l)
            if (out->line_from[l] == ~0u) out->line_from[l] = out->line_to[l] = 0;
    }
    blit_pdf_text_free(&t);
    return out;
}

}  // namespace

// A match found: its page, and its characters there.
struct PdfMatch {
    int page = 0;
    u32 at = 0, len = 0;
};

// A document as the reader has it (only its thread uses `pdf`, and the
// pages' text it has read, for finding and copying).
struct PdfFile {
    BlitPdf* pdf = nullptr;
    std::vector<std::shared_ptr<const PdfText>> text;
    ~PdfFile() { blit_pdf_close(pdf); }
    const PdfText& text_of(int page) {
        if (text.size() < static_cast<size_t>(blit_pdf_pages(pdf))) text.resize(static_cast<size_t>(blit_pdf_pages(pdf)));
        std::shared_ptr<const PdfText>& t = text[static_cast<size_t>(page)];
        if (!t) {
            BlitPdfText raw{};
            blit_pdf_text(pdf, page, 0, &raw);
            t = take_text(raw);
        }
        return *t;
    }
};

// What the reader is asked to do.
struct PdfJob {
    enum Kind : u8 {
        kRead,     // read a document
        kDraw,     // draw (part of) a page
        kText,     // read a page's text, where it is, and its links
        kFind,     // find text: from `page` on (a slice at a time)
        kCopy,     // the text from (page, at) to (page2, at2)
        kOutline,  // read the outline
        kUnlock,   // try a password
    };
    u8 kind = kRead;
    u64 doc = 0;  // the tab's document
    std::string path;
    std::shared_ptr<PdfFile> file;
    int page = 0;
    double scale = 0;
    int x = 0, y = 0, w = 0, h = 0;  // the part of the page, in its pixels at that scale
    bool detail = false;
    std::string what;  // what to find
    int page2 = 0;
    u32 at = 0, at2 = 0;
    u64 serial = 0;  // (a find: which; a newer one makes it stale)
    std::vector<PdfMatch> found;  // (a find: what it has found so far)
    Secret password;
    bool operator==(const PdfJob& o) const {
        return kind == o.kind && doc == o.doc && file == o.file && page == o.page && scale == o.scale && x == o.x && y == o.y && w == o.w && h == o.h && detail == o.detail;
    }
};

namespace {

PdfJob read_job(u64 doc, const std::string& path) {
    PdfJob j;
    j.doc = doc;
    j.path = path;
    return j;
}

PdfJob draw_job(u64 doc, const std::shared_ptr<PdfFile>& file, int page, double scale, int x, int y, int w, int h, bool detail) {
    PdfJob j;
    j.kind = PdfJob::kDraw;
    j.doc = doc, j.file = file, j.page = page, j.scale = scale, j.x = x, j.y = y, j.w = w, j.h = h, j.detail = detail;
    return j;
}

PdfJob page_job(u8 kind, u64 doc, const std::shared_ptr<PdfFile>& file, int page) {
    PdfJob j;
    j.kind = kind;
    j.doc = doc, j.file = file, j.page = page;
    return j;
}

}  // namespace

struct App::Pdfs {
    struct Done {
        PdfJob job;
        std::shared_ptr<PdfFile> file;                 // a document read
        std::vector<std::pair<double, double>> sizes;  // its pages' sizes (points)
        bool locked = false;                           // (it wants a password)
        u64 bytes = 0;
        i64 stamp = 0;
        BlitPicture pic{};  // a page drawn
        std::shared_ptr<const PdfText> text;  // a page's text read
        std::vector<PdfMatch> found;          // what a find found
        std::string copied;                   // the text copied
        std::vector<PdfHeading> outline;
        std::string error;
    };
    std::mutex mu;
    std::condition_variable_any cv;
    std::deque<PdfJob> reads;    // documents to read, passwords to try (before any drawing)
    std::vector<PdfJob> draws;   // what to draw and read on the pages on show, the most wanted first: replaced whole as the view changes
    std::deque<PdfJob> finds;    // finds (a slice at a time, taking turns with the drawing), copies and outlines
    std::map<u64, u64> find_serial;  // each document's newest find
    PdfJob drawing;              // what is being drawn now (doc 0: nothing)
    bool working = false;
    bool finds_turn = false;
    std::vector<Done> done;
    u32 blank = 0;               // a white pixel, stretched over a page not drawn yet
    std::map<u32, u32> washes;   // a pixel of a colour (by its rgb), washed over what is chosen and found
    std::jthread worker;

    Pdfs() {
        worker = std::jthread([this](std::stop_token st) { run(st); });
    }
    ~Pdfs() {
        worker.request_stop();
        cv.notify_all();
        if (worker.joinable()) worker.join();
        for (Done& d : done) blit_picture_free(&d.pic);
        for (PdfJob& j : reads) wipe(j.password);
    }
    void post_find(PdfJob j) {
        {
            std::lock_guard l(mu);
            std::erase_if(finds, [&](const PdfJob& f) { return f.kind == PdfJob::kFind && f.doc == j.doc; });
            find_serial[j.doc] = j.serial;
            if (!j.what.empty()) finds.push_back(std::move(j));
        }
        cv.notify_all();
    }
    void post(std::deque<PdfJob>& q, PdfJob j) {
        {
            std::lock_guard l(mu);
            q.push_back(std::move(j));
        }
        cv.notify_all();
    }
    // A slice of a find: pages searched until its time is up. True when it has searched them all.
    static bool find_slice(PdfJob& job) {
        const u64 until = now_ns() + kFindSliceNs;
        const int n = blit_pdf_pages(job.file->pdf);
        std::string what = job.what;
        for (char& c : what) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        while (job.page < n && job.found.size() < kMostFound) {
            const PdfText& t = job.file->text_of(job.page);
            for (size_t b = t.folded.find(what); b != std::string::npos && job.found.size() < kMostFound; b = t.folded.find(what, b + what.size())) {
                const u32 from = t.char_at(b), to = t.char_at(b + what.size() - 1) + 1;
                job.found.push_back({job.page, from, to - from});
            }
            ++job.page;
            if (now_ns() >= until) break;
        }
        return job.page >= n || job.found.size() >= kMostFound;
    }
    void run(std::stop_token st) {
        for (;;) {
            PdfJob job;
            {
                std::unique_lock l(mu);
                if (!cv.wait(l, st, [&] { return !reads.empty() || !draws.empty() || !finds.empty(); })) return;
                // Reads first; then draws and finds take turns (a find in slices, so that pages come while it goes on).
                if (!reads.empty()) {
                    job = std::move(reads.front());
                    reads.pop_front();
                } else if (!finds.empty() && (draws.empty() || finds_turn)) {
                    job = std::move(finds.front());
                    finds.pop_front();
                    finds_turn = false;
                } else {
                    job = std::move(draws.front());
                    draws.erase(draws.begin());
                    drawing = job;
                    finds_turn = true;
                }
                working = true;
            }
            Done d;
            switch (job.kind) {
                case PdfJob::kRead: {
                    std::string bytes;
                    d.stamp = stamp_of(job.path);
                    if (fs_read_whole(job.path, &bytes, &d.error, kMostBytes, st)) {
                        char why[160] = "";
                        if (BlitPdf* pdf = blit_pdf_open(bytes.data(), bytes.size(), why, sizeof why)) {
                            d.file = std::make_shared<PdfFile>();
                            d.file->pdf = pdf;
                            d.locked = blit_pdf_locked(pdf);
                        } else {
                            d.error = why;
                        }
                    }
                    d.bytes = bytes.size();
                    break;
                }
                case PdfJob::kUnlock:
                    if (blit_pdf_unlock(job.file->pdf, job.password.data())) d.file = job.file;
                    else d.error = "That password does not open it";
                    wipe(job.password);
                    break;
                case PdfJob::kDraw:
                    if (!blit_pdf_render(job.file->pdf, job.page, job.scale, job.x, job.y, job.w, job.h, &d.pic)) d.error = "the page could not be drawn";
                    break;
                case PdfJob::kText: {
                    BlitPdfText raw{};
                    blit_pdf_text(job.file->pdf, job.page, 1, &raw);
                    auto t = std::const_pointer_cast<PdfText>(take_text(raw));
                    BlitPdfLink* links = nullptr;
                    const int n = blit_pdf_links(job.file->pdf, job.page, &links);
                    for (int k = 0; k < n; ++k) {
                        PdfLink l;
                        std::copy(links[k].box, links[k].box + 4, l.box);
                        l.url = links[k].url ? links[k].url : "";
                        l.page = links[k].page, l.y = links[k].y;
                        t->links.push_back(std::move(l));
                    }
                    blit_pdf_links_free(links, n);
                    d.text = std::move(t);
                    break;
                }
                case PdfJob::kFind: {
                    if (!find_slice(job)) {  // (more to search: it goes on later, unless a newer one has come)
                        std::lock_guard l(mu);
                        working = false;
                        if (find_serial[job.doc] == job.serial) finds.push_front(std::move(job));
                        continue;
                    }
                    d.found = std::move(job.found);
                    break;
                }
                case PdfJob::kCopy:
                    for (int k = job.page; k <= job.page2 && k < blit_pdf_pages(job.file->pdf); ++k) {
                        const PdfText& t = job.file->text_of(k);
                        const u32 a = k == job.page ? std::min(job.at, t.count()) : 0, b = k == job.page2 ? std::min(job.at2, t.count()) : t.count();
                        if (k > job.page && !d.copied.empty() && d.copied.back() != '\n') d.copied += '\n';
                        if (b > a) d.copied.append(t.text, t.at[a], t.at[b] - t.at[a]);
                    }
                    break;
                case PdfJob::kOutline: {
                    BlitPdfHeading* h = nullptr;
                    const int n = blit_pdf_outline(job.file->pdf, &h);
                    for (int k = 0; k < n; ++k) d.outline.push_back({h[k].title ? h[k].title : "", h[k].depth, h[k].page, h[k].y});
                    blit_pdf_outline_free(h, n);
                    break;
                }
                default: break;
            }
            if (d.file && !d.locked) {  // a document open: its pages' sizes
                const int n = blit_pdf_pages(d.file->pdf);
                for (int k = 0; k < n; ++k) {
                    double w = 0, h = 0;
                    blit_pdf_page_size(d.file->pdf, k, &w, &h);
                    d.sizes.emplace_back(std::max(w, 1.0), std::max(h, 1.0));
                }
            }
            if (st.stop_requested()) {
                blit_picture_free(&d.pic);
                return;
            }
            job.found.clear();
            d.job = std::move(job);
            {
                std::lock_guard l(mu);
                drawing = PdfJob{};
                working = false;
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
    // Locked with a password: what is typed (wiped once it has been tried),
    // whether it is being tried, and what the last try said.
    bool locked = false, unlocking = false;
    Secret password;
    std::string lock_error;
    struct Page {
        double w = 0, h = 0, top = 0;  // its size, and where its top is, in points
        u32 whole = 0;                 // drawn whole, at whole_scale
        double whole_scale = 0;
        u32 part = 0;                  // drawn in part (zoomed in far), at part_scale: part_rect of its pixels
        double part_scale = 0;
        int part_rect[4] = {};
        std::shared_ptr<const PdfText> text;  // its text and links (read while it is near the view)
    };
    std::vector<Page> pages;
    double doc_w = 0, doc_h = 0;  // all of it, in points (the gaps too)
    // The view: pixels to a point (0: fitted to the width), and the point of
    // the document at the area's top left.
    double zoom = 0, scale = 1;
    double left = 0, top = 0;
    bool dragging = false;
    float drag_x = 0, drag_y = 0;
    // What is chosen: from `anchor` to `head`, each a page and a character
    // on it (at ~0u: the end of the page). The same: nothing.
    struct Place {
        int page = -1;
        u32 at = 0;
        bool operator==(const Place&) const = default;
        bool operator<(const Place& o) const { return page != o.page ? page < o.page : at < o.at; }
    };
    Place anchor, head;
    bool selecting = false;
    // A press on a link (its page and which): followed if the mouse is let go where it was.
    int press_page = -1, press_link = -1;
    float press_x = 0, press_y = 0;
    bool moved = false;
    // Finding (Cmd+F): the field, what was found (in order), which is the
    // current one, which find is newest, whether one is going on, and
    // whether the view still has to be moved to the current one (when its
    // page's text comes).
    bool finding = false, find_busy = false, reveal = false;
    std::string find;
    std::vector<PdfMatch> found;
    size_t current = 0;
    u64 find_serial = 0;
    // The outline: its entries, whether its box shows, the first row on
    // show, and the box (its cells, and where it is).
    std::vector<PdfHeading> outline;
    bool outline_open = false;
    u32 outline_top = 0;
    int ox = 0, oy = 0, ow = 0, oh = 0, opad_x = 0, opad_y = 0;
    TextBox obox;
    u64 okey = 0;
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
        wipe(password);
    }
    // The page whose middle is nearest the middle of the area.
    int current_page() const {
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
    // Where a page's top left is in the window.
    double page_x(const Page& p) const { return ax + ((doc_w - p.w) / 2 - left) * scale; }
    double page_y(const Page& p) const { return ay + (p.top - top) * scale; }
    // A place on a page shown at the top of the view, a third of the way down it.
    void go_to(int page, double down) {
        if (pages.empty()) return;
        page = std::clamp(page, 0, static_cast<int>(pages.size()) - 1);
        top = pages[static_cast<size_t>(page)].top + down - kGap;
        clamp();
    }
    // The place in the text nearest the point (px, py) of the window: on the
    // page under it (or the nearest), the character whose box holds it, or
    // the nearest on the nearest line; after it, if the point is past its
    // middle. Strict: only on a line of text (where a drag chooses text
    // rather than moving the pages). False where there is no text read.
    bool place_at(double px, double py, Place* out, bool strict) const {
        if (pages.empty()) return false;
        const double dy = top + (py - ay) / scale;
        int k = 0;
        for (size_t i = 0; i < pages.size(); ++i)
            if (pages[i].top - kGap / 2 <= dy) k = static_cast<int>(i);
        const Page& p = pages[static_cast<size_t>(k)];
        if (!p.text || !p.text->boxed()) return false;
        const PdfText& t = *p.text;
        const float u = static_cast<float>((px - page_x(p)) / scale), v = static_cast<float>((py - page_y(p)) / scale);
        int best = -1;
        float far = 1e30f;
        for (size_t l = 0; l < t.line_from.size(); ++l) {
            if (t.line_to[l] <= t.line_from[l]) continue;
            const float* b = &t.lines[l * 4];
            const float d = v < b[1] ? b[1] - v : v > b[3] ? v - b[3] : 0;
            if (d < far) far = d, best = static_cast<int>(l);
        }
        if (best < 0) return false;
        const float* lb = &t.lines[static_cast<size_t>(best) * 4];
        if (strict && (far > 2 || u < lb[0] - 4 || u > lb[2] + 4)) return false;
        u32 at = t.line_to[static_cast<size_t>(best)];
        for (u32 i = t.line_from[static_cast<size_t>(best)]; i < t.line_to[static_cast<size_t>(best)]; ++i)
            if (u < (t.box[i * 4] + t.box[i * 4 + 2]) / 2) {
                at = i;
                break;
            }
        // (Past the end of a line: before its line break, if it has one.)
        if (at == t.line_to[static_cast<size_t>(best)] && at > 0 && at <= t.count() && (t.text[t.at[at - 1]] == '\n' || t.text[t.at[at - 1]] == '\r')) --at;
        *out = {k, at};
        return true;
    }
    bool chosen() const { return anchor.page >= 0 && head.page >= 0 && anchor != head; }
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
    fresh->pdf->password.reserve(kMostPassword + 1);
    fresh->id = next_doc_id_++;
    if (!is_remote(full)) {
        fresh->disk_stamp = stamp_of(full);
        recent_add(full, false);
    }
    pdfs_->post(pdfs_->reads, read_job(fresh->id, full));
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

// The view moved so that the current match is on show (a third of the way
// down, if it was not); if its page's text has not come yet, its page, and
// then again when it comes.
void reveal_current(PdfView& v) {
    if (v.found.empty() || v.pages.empty()) return;
    const PdfMatch& m = v.found[std::min(v.current, v.found.size() - 1)];
    if (m.page >= static_cast<int>(v.pages.size())) return;
    const PdfView::Page& p = v.pages[static_cast<size_t>(m.page)];
    if (!p.text || !p.text->boxed() || m.at >= p.text->count()) {
        if (p.top + p.h < v.top || p.top > v.top + v.ah / v.scale) v.go_to(m.page, 0);
        v.reveal = true;
        return;
    }
    v.reveal = false;
    const PdfText& t = *p.text;
    const float* lb = &t.lines[static_cast<size_t>(t.line[m.at]) * 4];
    const double y0 = p.top + lb[1], y1 = p.top + lb[3];
    if (y0 < v.top || y1 > v.top + v.ah / v.scale) {
        v.top = y0 - v.ah / v.scale / 3;
        v.clamp();
    }
    // (Sideways too, when it is wider than the view.)
    const double x0 = (v.doc_w - p.w) / 2 + t.box[m.at * 4], vw = v.aw / v.scale;
    if (x0 < v.left || x0 > v.left + vw) {
        v.left = x0 - vw / 3;
        v.clamp();
    }
}

}  // namespace

// What the reader has done: documents read, pages drawn, text read and found.
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
        if (v && (d.job.kind == PdfJob::kRead || d.job.kind == PdfJob::kUnlock)) {  // the document
            if (d.job.kind == PdfJob::kRead) {
                v->again = false;
                v->stamp = d.stamp;
                v->bytes = d.bytes;
            } else {
                v->unlocking = false;
                v->lock_error = d.error;
            }
            if (d.file && d.locked) {  // (it asks for the password: the field shows)
                v->file = std::move(d.file);
                v->locked = true;
                v->state = PictureInfo::kReady;
                for (PdfView::Page& p : v->pages) v->drop(p);
                v->pages.clear();
            } else if (!d.file) {
                if (v->state != PictureInfo::kReady && d.job.kind == PdfJob::kRead) v->state = PictureInfo::kFailed, v->error = d.error;
            } else {
                // Its pages, one under another; the view kept on the same page (if it was read again).
                const bool first = v->pages.empty();
                const int was = first ? 0 : v->current_page();
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
                v->state = PictureInfo::kReady;
                v->locked = false;
                v->error.clear();
                v->found.clear();
                v->anchor = v->head = {};
                const size_t back = std::min(static_cast<size_t>(was), v->pages.size() - 1);
                v->top = first ? 0 : v->pages[back].top + into;
                // Its outline, and what was being looked for, in it.
                pdfs_->post(pdfs_->finds, page_job(PdfJob::kOutline, d.job.doc, v->file, 0));
                if (v->finding && !v->find.empty()) {
                    PdfJob j = page_job(PdfJob::kFind, d.job.doc, v->file, 0);
                    j.what = v->find;
                    j.serial = ++v->find_serial;
                    v->find_busy = true;
                    pdfs_->post_find(std::move(j));
                }
            }
        } else if (v && d.job.file != v->file) {
            // (from the document as it was before it was read again)
        } else if (v && d.job.kind == PdfJob::kDraw && d.job.page < static_cast<int>(v->pages.size()) && d.pic.rgba) {  // a page
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
        } else if (v && d.job.kind == PdfJob::kText && d.job.page < static_cast<int>(v->pages.size()) && d.text) {
            v->pages[static_cast<size_t>(d.job.page)].text = std::move(d.text);
            if (v->reveal && !v->found.empty() && v->found[std::min(v->current, v->found.size() - 1)].page == d.job.page) reveal_current(*v);
        } else if (v && d.job.kind == PdfJob::kFind && d.job.serial == v->find_serial) {
            v->find_busy = false;
            v->found = std::move(d.found);
            // The current one: the first at or after the page in the middle of the view.
            const int now = v->current_page();
            v->current = 0;
            while (v->current < v->found.size() && v->found[v->current].page < now) ++v->current;
            if (v->current == v->found.size()) v->current = 0;
            reveal_current(*v);
        } else if (v && d.job.kind == PdfJob::kCopy) {
            if (win_) SDL_SetClipboardText(d.copied.c_str());
            else g_pdf_clipboard = d.copied;
        } else if (v && d.job.kind == PdfJob::kOutline) {
            v->outline = std::move(d.outline);
            v->outline_top = 0;
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
            pdfs_->post(pdfs_->reads, read_job(d->id, d->pdf->path));
        }
}

namespace {

// The colours the tab washes over the page with: what is chosen (the
// theme's selection), what was found (its warnings'), and the current
// match (its accent), each a pixel of its own on the GPU.
u32 wash_of(Renderer& gpu, std::map<u32, u32>& washes, const float* rgb) {
    u8 px[4];
    for (int c = 0; c < 3; ++c) px[c] = static_cast<u8>(std::lround(std::clamp(rgb[c], 0.0f, 1.0f) * 255));
    px[3] = 255;
    const u32 key = static_cast<u32>(px[0]) << 16 | static_cast<u32>(px[1]) << 8 | px[2];
    auto it = washes.find(key);
    if (it == washes.end()) it = washes.emplace(key, gpu.image_add(px, 1, 1)).first;
    return it->second;
}

}  // namespace

void App::pdf_layout(FrameDesc& fd) {
    if (!pdfs_) return;
    Font& font = *font_;
    const int cw = font.cell_w(), ch = font.cell_h();
    const Look lk = look();
    std::vector<PdfJob> want;   // what should be drawn, the most wanted first
    std::vector<PdfJob> texts;  // (the text of the pages on show)
    std::vector<PdfJob> later;  // (pages either side of those on show)
    for (const std::unique_ptr<Pane>& pane : panes_) {
        if (!pane->doc || !pane->doc->pdf || fd.popup_count + 2 >= std::size(fd.popups)) continue;
        PdfView& v = *pane->doc->pdf;
        const u64 doc_id = pane->doc->id;
        const int x = pane->x0, y = pane->text_y, w = pane->x1 - pane->x0, h = pane->y1 - pane->text_y;
        if (w < cw * 4 || h < ch * 3) continue;
        v.x = x, v.y = y, v.w = w, v.h = h;
        const u32 cols = static_cast<u32>(w / cw), rows = static_cast<u32>(h / ch);
        const int pad_x = (w - static_cast<int>(cols) * cw) / 2, pad_y = (h - static_cast<int>(rows) * ch) / 2;
        const bool ready = v.state == PictureInfo::kReady && !v.locked;
        // The outline's box, at the left, when it shows: the pages beside it.
        const bool side = ready && v.outline_open && !v.outline.empty() && w > 40 * cw;
        v.ow = side ? std::clamp(w / 4, 24 * cw, 44 * cw) : 0;
        v.ax = x + v.ow, v.ay = y, v.aw = std::max(1, w - v.ow), v.ah = std::max(1, h - pad_y - ch);
        if (ready) {
            if (v.zoom <= 0) v.scale = v.aw / v.doc_w;  // (fitted to the width)
            v.clamp();
        }
        const int now = ready ? v.current_page() : 0;
        const int percent = static_cast<int>(std::lround(v.scale / density_ * 100));
        u64 key = 1469598103934665603ull;
        auto mix = [&](u64 k) { key = (key ^ k) * 1099511628211ull; };
        mix(cols), mix(rows), mix(font.generation()), mix(v.state), mix(v.pages.size()), mix(static_cast<u64>(now)), mix(static_cast<u64>(percent));
        mix(v.locked), mix(v.unlocking), mix(v.password.size()), mix(std::hash<std::string>{}(v.lock_error)), mix(v.outline.size()), mix(v.outline_open);
        mix(v.finding), mix(std::hash<std::string>{}(v.find)), mix(v.found.size()), mix(v.current), mix(v.find_busy);
        if (key != v.key) {
            v.key = key;
            std::string info;
            std::span<const Hint> hints;
            u32 info_ink = kInkMuted;
            v.box.resize(cols, rows);
            const u32 r = rows - 1;
            // (From the text edge, as every bar: the name, where in it, and on the right the keys.)
            const u32 edge = static_cast<u32>(std::max(1, (lk.edge - pad_x + cw - 1) / cw));
            u32 c = put(v.box, font, r, edge, leaf_of(v.path), ink::kStrong);
            static constexpr Hint kHints[] = {{"n p", "page"}, {"+ -", "zoom"}, {"0", "width"}, {"9", "whole page"}};
            static constexpr Hint kHintsOutline[] = {{"o", "outline"}, {"n p", "page"}, {"+ -", "zoom"}, {"0", "width"}, {"9", "whole page"}};
            static constexpr Hint kHintsFind[] = {{"Enter", "next"}, {"Shift+Enter", "previous"}};
            static constexpr Hint kHintsLocked[] = {{"Enter", "open"}};
            if (v.state == PictureInfo::kReading) {
                info = "Reading…";
            } else if (v.state == PictureInfo::kFailed) {
                info = "Cannot show it: " + v.error;
                info_ink = kInkDiagnostic;
            } else if (v.locked) {
                info = v.unlocking ? "Opening…" : "Locked with a password";
                hints = kHintsLocked;
                // The field, in the middle of the tab: what is typed, a dot a letter.
                const u32 mid = rows > 4 ? (rows - 1) / 2 : 0;
                const std::string title = leaf_of(v.path) + " is locked with a password";
                put(v.box, font, mid - (mid > 0 ? 1 : 0), cols > width_of(title) ? (cols - width_of(title)) / 2 : 0, fit_text(title, cols), ink::kHeading);
                std::string dots;
                for (const char b : v.password)
                    if (!is_cont(static_cast<u8>(b))) dots += font.has(0x2022) ? "\xE2\x80\xA2" : "*";
                const u32 fw = std::min<u32>(cols, 40);
                const u32 f0 = (cols - fw) / 2;
                u32 fc = put(v.box, font, mid + 1, f0, "Password", ink::kHeading);
                fc = put(v.box, font, mid + 1, fc + 2, fit_text(dots, fw > 12 ? fw - 12 : 1), ink::kText);
                if (fc < cols) put(v.box, font, mid + 1, fc, font.can_draw(0x258F) ? "\xE2\x96\x8F" : "_", ink::kAccent);
                if (!v.lock_error.empty() && mid + 3 < r) {
                    const std::string said = fit_text(v.lock_error, cols);
                    put(v.box, font, mid + 3, (cols - std::min(cols, width_of(said))) / 2, said, ink::kError);
                }
            } else if (v.finding) {
                // The field: what is looked for, the caret, and how many there are.
                c = put(v.box, font, r, c + 3, "Find", ink::kHeading);
                c = put(v.box, font, r, c + 2, fit_text(v.find, cols > c + 30 ? cols - c - 30 : 4), ink::kText);
                if (c < cols) c = put(v.box, font, r, c, font.can_draw(0x258F) ? "\xE2\x96\x8F" : "_", ink::kAccent);
                std::string count;
                Ink count_ink = ink::kMuted;
                if (v.find.empty()) count = "";
                else if (v.find_busy) count = "Finding…";
                else if (v.found.empty()) count = "No matches", count_ink = ink::kError;
                else count = std::to_string(v.current + 1) + " of " + std::to_string(v.found.size()) + (v.found.size() >= kMostFound ? "+" : "");
                c = put(v.box, font, r, c + 2, count, count_ink);
                hints = kHintsFind;
            } else {
                info = "Page " + std::to_string(now + 1) + " of " + std::to_string(v.pages.size()) + "   " + size_text(v.bytes) + "   " + std::to_string(percent) + "%";
                hints = v.outline.empty() ? std::span<const Hint>(kHints) : std::span<const Hint>(kHintsOutline);
            }
            if (!info.empty()) c = v.box.put(font, r, c + 3, info, info_ink);
            const u32 kw = hints.empty() ? 0 : hints_width(hints, font);
            if (!hints.empty() && c + 3 + kw + edge < cols) put_hints(v.box, font, r, cols - edge - kw, hints);
        }
        Popup& q = fd.popups[fd.popup_count++];
        q = Popup{};
        q.x = x, q.y = y, q.w = w, q.h = h;
        q.pad_x = pad_x, q.pad_y = pad_y;
        q.cols = v.box.cols, q.rows = v.box.rows;
        q.cells = v.box.cells.data();
        q.border_px = 0;
        q.bg = theme_.bg;
        if (!ready) continue;
        // The outline: its entries a row each, indented by how deep they
        // are, the one the view is in lit.
        int here = -1;
        if (side) {
            const double at_top = v.top + kGap;
            for (size_t k = 0; k < v.outline.size(); ++k) {
                const PdfHeading& o = v.outline[k];
                if (o.page < 0 || o.page >= static_cast<int>(v.pages.size())) continue;
                if (v.pages[static_cast<size_t>(o.page)].top + o.y <= at_top + 1) here = static_cast<int>(k);
            }
            v.ox = x, v.oy = y, v.oh = v.ah;
            v.opad_x = lk.edge, v.opad_y = lk.pad_y;
            const u32 ocols = static_cast<u32>(std::max(1, (v.ow - 2 * v.opad_x) / cw)), orows = static_cast<u32>(std::max(1, (v.oh - 2 * v.opad_y) / ch));
            v.outline_top = std::min<u32>(v.outline_top, v.outline.size() + 1 > orows ? static_cast<u32>(v.outline.size()) + 1 - orows : 0);
            u64 ok = 1469598103934665603ull;
            auto omix = [&](u64 k) { ok = (ok ^ k) * 1099511628211ull; };
            omix(ocols), omix(orows), omix(font.generation()), omix(v.outline.size()), omix(v.outline_top), omix(static_cast<u64>(here + 1));
            if (ok != v.okey) {
                v.okey = ok;
                v.obox.resize(ocols, orows);
                put(v.obox, font, 0, 0, "Outline", ink::kHeading);
                for (u32 r = 1; r < orows && v.outline_top + r - 1 < v.outline.size(); ++r) {
                    const size_t k = v.outline_top + r - 1;
                    const u32 indent = std::min<u32>(static_cast<u32>(v.outline[k].depth) * 2, ocols / 2);
                    put(v.obox, font, r, indent, fit_text(v.outline[k].title, ocols - indent), static_cast<int>(k) == here ? ink::kAccent : ink::kText);
                    if (static_cast<int>(k) == here) v.obox.select_row(r);
                }
            }
            Popup& o = fd.popups[fd.popup_count++];
            o = Popup{};
            o.x = v.ox, o.y = v.oy, o.w = v.ow, o.h = v.oh;
            o.pad_x = v.opad_x, o.pad_y = v.opad_y;
            o.cols = v.obox.cols, o.rows = v.obox.rows;
            o.cells = v.obox.cells.data();
            o.border_px = 0;
            if (lk.pill > 0) {
                o.pill = lk.pill;
                o.pill_dx = static_cast<float>(lk.hair);
                o.pill_dy = std::round(0.5f * scale_);
                o.pill_out = std::max(0.0f, static_cast<float>(v.opad_x - lk.inset));
            }
        }
        if (!pdfs_->blank) {
            const u8 white[4] = {255, 255, 255, 255};
            pdfs_->blank = gpu_.image_add(white, 1, 1);
        }
        auto draw = [&](u32 image, double x0, double y0, double x1, double y1, float opacity = 1) {
            if (!image || fd.picture_count >= std::size(fd.pictures)) return;
            PictureDraw& d = fd.pictures[fd.picture_count++];
            d = PictureDraw{};
            d.image = image;
            d.rect[0] = static_cast<float>(x0), d.rect[1] = static_cast<float>(y0), d.rect[2] = static_cast<float>(x1), d.rect[3] = static_cast<float>(y1);
            d.clip[0] = v.ax, d.clip[1] = v.ay, d.clip[2] = v.ax + v.aw, d.clip[3] = v.ay + v.ah;
            d.after = fd.popup_count;
            d.opacity = opacity;
        };
        // The pages on show: drawn (or blank, or stretched), and what they want drawn.
        const double vy0 = v.top, vy1 = v.top + v.ah / v.scale;
        int first = -1, last = -1;
        for (size_t k = 0; k < v.pages.size(); ++k) {
            PdfView::Page& p = v.pages[k];
            if (p.top + p.h < vy0 || p.top > vy1) continue;
            if (first < 0) first = static_cast<int>(k);
            last = static_cast<int>(k);
            const double left = std::round(v.page_x(p)), top = std::round(v.page_y(p));
            const double pw = std::round(p.w * v.scale), ph = std::round(p.h * v.scale);
            draw(p.whole ? p.whole : pdfs_->blank, left, top, left + pw, top + ph);
            // Drawn whole at this zoom (or as near as 4096 pixels a side allow).
            const double whole = std::min(v.scale, kMostSide / std::max(p.w, p.h));
            if (std::abs(p.whole_scale - whole) > whole * 0.005) want.push_back(draw_job(doc_id, v.file, static_cast<int>(k), whole, 0, 0, static_cast<int>(std::lround(p.w * whole)), static_cast<int>(std::lround(p.h * whole)), false));
            if (!p.text) texts.push_back(page_job(PdfJob::kText, doc_id, v.file, static_cast<int>(k)));
            // Zoomed past that: the part on show drawn sharp over it (with a margin, for moving a little).
            if (v.scale > whole * 1.001) {
                const int sx0 = static_cast<int>(std::floor((std::max<double>(v.ax, left) - left))), sy0 = static_cast<int>(std::floor(std::max<double>(v.ay, top) - top));
                const int sx1 = static_cast<int>(std::ceil(std::min<double>(v.ax + v.aw, left + pw) - left)), sy1 = static_cast<int>(std::ceil(std::min<double>(v.ay + v.ah, top + ph) - top));
                const bool covered = p.part && p.part_scale == v.scale && p.part_rect[0] <= sx0 && p.part_rect[1] <= sy0 && p.part_rect[2] >= sx1 && p.part_rect[3] >= sy1;
                if (!covered && sx1 > sx0 && sy1 > sy0) {
                    const int mx = v.aw / 4, my = v.ah / 4;
                    const int rx0 = std::max(0, sx0 - mx), ry0 = std::max(0, sy0 - my);
                    const int rx1 = std::min(static_cast<int>(pw), sx1 + mx), ry1 = std::min(static_cast<int>(ph), sy1 + my);
                    want.push_back(draw_job(doc_id, v.file, static_cast<int>(k), v.scale, rx0, ry0, rx1 - rx0, ry1 - ry0, true));
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
        // Washed over the pages on show: what was found (the current one
        // stronger), and what is chosen. A run of characters is a box a
        // line, from the first character's left to the last one's right.
        auto wash = [&](int page, u32 a, u32 b, const float* rgb, float opacity) {
            const PdfView::Page& p = v.pages[static_cast<size_t>(page)];
            if (!p.text || !p.text->boxed()) return;
            const PdfText& t = *p.text;
            b = std::min(b, t.count());
            if (a >= b) return;
            const u32 image = wash_of(gpu_, pdfs_->washes, rgb);
            const double left = v.page_x(p), top = v.page_y(p);
            const int l0 = t.line[a], l1 = t.line[b - 1];
            for (int l = l0; l <= l1; ++l) {
                const float* lb = &t.lines[static_cast<size_t>(l) * 4];
                const float x0 = l == l0 ? t.box[a * 4] : lb[0], x1 = l == l1 ? t.box[(b - 1) * 4 + 2] : lb[2];
                if (x1 <= x0) continue;
                draw(image, std::round(left + x0 * v.scale), std::round(top + lb[1] * v.scale), std::round(left + x1 * v.scale), std::round(top + lb[3] * v.scale), opacity);
            }
        };
        if (first >= 0 && !v.found.empty()) {
            const float* found_rgb = theme_.palette[kInkDiagnostic + 1];
            const float* current_rgb = theme_.palette[kInkAccent];
            auto from = std::ranges::lower_bound(v.found, first, {}, &PdfMatch::page);
            for (auto it = from; it != v.found.end() && it->page <= last; ++it) {
                const bool cur = static_cast<size_t>(it - v.found.begin()) == v.current;
                wash(it->page, it->at, it->at + it->len, cur ? current_rgb : found_rgb, cur ? kCurrentWash : kFoundWash);
            }
        }
        if (first >= 0 && v.chosen()) {
            const PdfView::Place a = std::min(v.anchor, v.head), b = std::max(v.anchor, v.head);
            for (int k = std::max(first, a.page); k <= std::min(last, b.page); ++k) wash(k, k == a.page ? a.at : 0, k == b.page ? b.at : ~0u, theme_.sel, kChosenWash);
        }
        // Pages either side drawn ahead; those further away let go.
        for (int k = 0; k < static_cast<int>(v.pages.size()); ++k) {
            PdfView::Page& p = v.pages[static_cast<size_t>(k)];
            if (first >= 0 && (k >= first - kKeep && k <= last + kKeep)) {
                if (k >= first && k <= last) continue;
                const double whole = std::min(v.scale, kMostSide / std::max(p.w, p.h));
                if (std::abs(p.whole_scale - whole) > whole * 0.005 && std::abs(k - (k < first ? first : last)) <= 1)
                    later.push_back(draw_job(doc_id, v.file, k, whole, 0, 0, static_cast<int>(std::lround(p.w * whole)), static_cast<int>(std::lround(p.h * whole)), false));
                if (p.part) gpu_.image_drop(p.part), p.part = 0, p.part_scale = 0;
            } else {
                if (p.whole || p.part) v.drop(p);
                p.text.reset();
            }
        }
    }
    want.insert(want.end(), texts.begin(), texts.end());
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

namespace {

// The last character typed into a field, taken off (a whole character; a
// password's bytes are wiped as they go).
template <typename S>
void erase_last(S& s) {
    auto off = [&] {
        s.back() = 0;
        s.pop_back();
    };
    while (!s.empty() && is_cont(static_cast<u8>(s.back()))) off();
    if (!s.empty()) off();
}

}  // namespace

// What is chosen in the PDF in front, copied (by the reader, which has the
// text of every page; the clipboard gets it when it is done).
void App::pdf_copy(Doc& doc) {
    PdfView& v = *doc.pdf;
    if (!v.chosen() || !v.file) return;
    const PdfView::Place a = std::min(v.anchor, v.head), b = std::max(v.anchor, v.head);
    PdfJob j = page_job(PdfJob::kCopy, doc.id, v.file, a.page);
    j.at = a.at, j.page2 = b.page, j.at2 = b.at;
    pdfs_->post(pdfs_->finds, std::move(j));
}

// What is typed in the field looked for, from the start (a newer find makes an older one stale).
void App::pdf_find(Doc& doc) {
    PdfView& v = *doc.pdf;
    PdfJob j = page_job(PdfJob::kFind, doc.id, v.file, 0);
    j.what = v.find;
    j.serial = ++v.find_serial;
    v.find_busy = !v.find.empty();
    if (v.find.empty()) v.found.clear();
    pdfs_->post_find(std::move(j));
}

// Text typed into the PDF in front: its password, or what to find.
bool App::pdf_text(const char* text) {
    if (!pane_->doc->pdf) return false;
    PdfView& v = *pane_->doc->pdf;
    const size_t n = std::strlen(text);
    if (v.locked) {
        if (!v.unlocking && v.password.size() + n <= kMostPassword) v.password.insert(v.password.end(), text, text + n);
    } else if (v.finding) {
        v.find += text;
        pdf_find(*pane_->doc);
    } else {
        return false;
    }
    dirty_ = true;
    return true;
}

bool App::pdf_key(const SDL_KeyboardEvent& k) {
    // (The editor's key is still the PDF's with some keys: Cmd on a Mac, Ctrl elsewhere.)
    if (!pane_->doc->pdf || (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI) & ~kPrimaryMod)) return false;
    Doc& doc = *pane_->doc;
    PdfView& v = *doc.pdf;
    const bool cmd = (k.mod & kPrimaryMod) != 0, shift = (k.mod & SDL_KMOD_SHIFT) != 0;
    if (v.locked) {  // the password field has the keyboard
        if (cmd) return false;
        switch (k.key) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (!v.unlocking && v.file) {
                    PdfJob j = page_job(PdfJob::kUnlock, doc.id, v.file, 0);
                    v.password.push_back(0);
                    j.password = std::move(v.password);  // (handed over, not copied)
                    v.password = Secret();
                    v.password.reserve(kMostPassword + 1);
                    v.unlocking = true;
                    v.lock_error.clear();
                    pdfs_->post(pdfs_->reads, std::move(j));
                }
                break;
            case SDLK_BACKSPACE: erase_last(v.password); break;
            case SDLK_ESCAPE: wipe(v.password); break;
            default: return true;  // (its text comes to pdf_text)
        }
        dirty_ = true;  // (these keys bring no text)
        return true;
    }
    if (v.state != PictureInfo::kReady) return false;
    const double mx = v.ax + v.aw / 2.0, my = v.ay + v.ah / 2.0;
    const double line = cfg_.scroll_lines * font_->cell_h() / v.scale, screen = std::max(line, v.ah / v.scale - 3 * line);
    const int now = v.current_page();
    auto step_found = [&](bool back) {
        if (v.found.empty()) return;
        v.current = back ? (v.current + v.found.size() - 1) % v.found.size() : (v.current + 1) % v.found.size();
        reveal_current(v);
    };
    // Cmd+F finds, Cmd+G and Cmd+Shift+G go to the next and the one
    // before, Cmd+C copies what is chosen, Cmd+A chooses all of it.
    if (cmd && (k.key == SDLK_F || k.key == SDLK_G || k.key == SDLK_C || k.key == SDLK_A)) {
        if (k.key == SDLK_F) v.finding = true;
        else if (k.key == SDLK_G) step_found(shift);
        else if (k.key == SDLK_C) pdf_copy(doc);
        else v.anchor = {0, 0}, v.head = {static_cast<int>(v.pages.size()) - 1, ~0u};
        dirty_ = true;  // (a key with Cmd brings no text)
        return true;
    }
    if (v.finding && !cmd) {  // the field has the keyboard, but for the keys that move the view
        switch (k.key) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER: step_found(shift); break;
            case SDLK_ESCAPE:
                v.finding = v.find_busy = false;
                v.find.clear();
                v.found.clear();
                ++v.find_serial;
                pdf_find(doc);
                break;
            case SDLK_BACKSPACE:
                erase_last(v.find);
                pdf_find(doc);
                break;
            case SDLK_UP:
            case SDLK_DOWN:
            case SDLK_PAGEUP:
            case SDLK_PAGEDOWN:
            case SDLK_HOME:
            case SDLK_END:
                goto move;
            default: return true;  // (its text comes to pdf_text)
        }
        dirty_ = true;
        return true;
    }
    if (k.key == SDLK_ESCAPE && !cmd && v.chosen()) {
        v.anchor = v.head = {};
        term_drop_text_ = true;
        dirty_ = true;
        return true;
    }
move:
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
                    v.go_to(now, 0);
                    break;
                }
                case SDLK_O:
                    if (v.outline.empty()) return false;
                    v.outline_open = !v.outline_open;
                    break;
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
                case SDLK_N: v.go_to(now + 1, 0); break;
                case SDLK_P: v.go_to(now - 1, 0); break;
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

// The wheel scrolls (as the text does); with Cmd (or Ctrl) it zooms, about
// the mouse. Over the outline, it scrolls the outline.
bool App::pdf_wheel(const SDL_MouseWheelEvent& w) {
    Pane* pane = nullptr;
    const float mx = mouse_x_ * density_, my = mouse_y_ * density_;
    if (!pdf_hit(static_cast<int>(mx), static_cast<int>(my), &pane)) return false;
    PdfView& v = *pane->doc->pdf;
    if (v.state != PictureInfo::kReady || v.locked) return true;
    if (v.ow > 0 && mx < v.ox + v.ow) {
        const int step = static_cast<int>(-w.y * (w.y != std::floor(w.y) ? 1 : 3));
        v.outline_top = static_cast<u32>(std::max<i64>(0, static_cast<i64>(v.outline_top) + step));
    } else if (SDL_GetModState() & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) {
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
    if (v.state == PictureInfo::kReady && !v.locked && by > 0) v.zoom_to(v.scale * by, mx, my);
    dirty_ = true;
    return true;
}

// A click gives the tab the keyboard. On text, a drag chooses it (two
// clicks a word, three a line); elsewhere it moves the pages. A click on a
// link follows it when the mouse is let go, if it has not moved; one on an
// entry of the outline goes there.
bool App::pdf_mouse_down(const SDL_MouseButtonEvent& b) {
    Pane* pane = nullptr;
    const float px = b.x * density_, py = b.y * density_;
    if (!pdf_hit(static_cast<int>(px), static_cast<int>(py), &pane)) return false;
    focus_pane(pane);
    dirty_ = true;
    if (b.button != SDL_BUTTON_LEFT) return true;
    PdfView& v = *pane->doc->pdf;
    if (v.state != PictureInfo::kReady || v.locked) return true;
    if (v.ow > 0 && px < v.ox + v.ow) {
        const int r = (static_cast<int>(py) - v.oy - v.opad_y) / font_->cell_h();
        const size_t k = v.outline_top + static_cast<size_t>(std::max(0, r)) - 1;
        if (r >= 1 && k < v.outline.size() && v.outline[k].page >= 0) v.go_to(v.outline[k].page, v.outline[k].y);
        return true;
    }
    if (py >= v.ay + v.ah) return true;  // (the row at the bottom)
    // A link under it (on a page whose links have been read).
    v.press_page = v.press_link = -1;
    v.press_x = px, v.press_y = py;
    v.moved = false;
    for (size_t k = 0; k < v.pages.size() && v.press_link < 0; ++k) {
        const PdfView::Page& p = v.pages[k];
        if (!p.text) continue;
        const double u = (px - v.page_x(p)) / v.scale, w = (py - v.page_y(p)) / v.scale;
        for (size_t i = 0; i < p.text->links.size(); ++i) {
            const float* bx = p.text->links[i].box;
            if (u >= bx[0] && u < bx[2] && w >= bx[1] && w < bx[3]) {
                v.press_page = static_cast<int>(k), v.press_link = static_cast<int>(i);
                break;
            }
        }
    }
    PdfView::Place at;
    if (v.place_at(px, py, &at, true)) {
        const PdfText& t = *v.pages[static_cast<size_t>(at.page)].text;
        if (b.clicks >= 3 && t.count() > 0) {  // its line
            const size_t l = static_cast<size_t>(t.line[std::min(at.at, t.count() - 1)]);
            v.anchor = {at.page, t.line_from[l]};
            v.head = {at.page, t.line_to[l]};
        } else if (b.clicks == 2 && at.at < t.count()) {  // its word
            auto word = [&](u32 i) {
                const u8 c = static_cast<u8>(t.text[t.at[i]]);
                return c >= 0x80 || std::isalnum(c) || c == '_' || c == '\'';
            };
            u32 a = at.at, e = at.at;
            if (!word(a) && a > 0 && word(a - 1)) a = e = a - 1;
            while (a > 0 && word(a - 1)) --a;
            while (e < t.count() && word(e)) ++e;
            v.anchor = {at.page, a};
            v.head = {at.page, std::max(e, a + 1)};
        } else {
            v.anchor = v.head = at;
            v.selecting = true;
        }
        return true;
    }
    v.anchor = v.head = {};
    v.dragging = true;
    v.drag_x = px, v.drag_y = py;
    return true;
}

bool App::pdf_mouse_motion(const SDL_MouseMotionEvent& m) {
    for (const std::unique_ptr<Pane>& pane : panes_) {
        if (!pane->doc || !pane->doc->pdf) continue;
        PdfView& v = *pane->doc->pdf;
        if (!v.dragging && !v.selecting) continue;
        const float px = m.x * density_, py = m.y * density_;
        if (std::abs(px - v.press_x) > 3 * scale_ || std::abs(py - v.press_y) > 3 * scale_) v.moved = true;
        if (v.selecting) {
            PdfView::Place at;
            if (v.place_at(px, py, &at, false)) v.head = at;
        } else if (v.state == PictureInfo::kReady) {
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
    for (const std::unique_ptr<Pane>& pane : panes_) {
        if (!pane->doc || !pane->doc->pdf) continue;
        PdfView& v = *pane->doc->pdf;
        if (v.press_link >= 0 && !v.moved && v.press_page < static_cast<int>(v.pages.size())) {
            const PdfView::Page& p = v.pages[static_cast<size_t>(v.press_page)];
            if (p.text && static_cast<size_t>(v.press_link) < p.text->links.size()) {
                const PdfLink& l = p.text->links[static_cast<size_t>(v.press_link)];
                if (!l.url.empty()) {  // (the web, through the system's opener)
                    if (win_) SDL_OpenURL(l.url.c_str());
                    else g_pdf_opened = l.url;
                } else {
                    v.go_to(l.page, l.y);
                }
                v.anchor = v.head = {};
                dirty_ = true;
            }
        }
        v.press_page = v.press_link = -1;
        v.dragging = v.selecting = false;
    }
}

bool App::pdfs_busy() const {
    if (!pdfs_) return false;
    for (const std::unique_ptr<Pane>& pane : panes_)
        for (const std::unique_ptr<Doc>& d : pane->docs)
            if (d->pdf && (d->pdf->state == PictureInfo::kReading || d->pdf->again)) return true;
    std::lock_guard l(pdfs_->mu);
    return !pdfs_->reads.empty() || !pdfs_->draws.empty() || !pdfs_->finds.empty() || pdfs_->working || !pdfs_->done.empty();
}

// The PDF tab in front (the first there is), as the tests see it.
std::string App::pdf_dump() {
    for (const std::unique_ptr<Pane>& pane : panes_) {
        if (!pane->doc || !pane->doc->pdf) continue;
        const PdfView& v = *pane->doc->pdf;
        if (v.state == PictureInfo::kReading) return leaf_of(v.path) + ": reading";
        if (v.state == PictureInfo::kFailed) return leaf_of(v.path) + ": failed (" + v.error + ")";
        if (v.locked)  // (never the password: how many letters it has)
            return leaf_of(v.path) + ": locked, " + std::to_string(v.password.size()) + " typed" + (v.lock_error.empty() ? "" : " (" + v.lock_error + ")");
        std::string drawn;
        for (size_t k = 0; k < v.pages.size(); ++k)
            if (v.pages[k].whole) drawn += (drawn.empty() ? "" : ",") + std::to_string(k + 1) + (v.pages[k].part ? "+" : "");
        char b[256];
        std::snprintf(b, sizeof b, "ready, %zu pages, page %d, zoom %d%%, drawn %s, top %.0f", v.pages.size(), v.current_page() + 1, static_cast<int>(std::lround(v.scale / density_ * 100)),
                      drawn.empty() ? "none" : drawn.c_str(), v.top);
        std::string out = leaf_of(v.path) + ": " + b;
        if (v.finding) {
            out += ", find \"" + v.find + "\": ";
            if (v.found.empty()) out += "none";
            for (size_t k = 0; k < v.found.size(); ++k)
                out += (k ? " " : "") + std::string(k == v.current ? "*" : "") + "p" + std::to_string(v.found[k].page + 1) + "@" + std::to_string(v.found[k].at);
        }
        if (v.chosen()) {
            const PdfView::Place a = std::min(v.anchor, v.head), e = std::max(v.anchor, v.head);
            out += ", chosen p" + std::to_string(a.page + 1) + "@" + std::to_string(a.at) + " to p" + std::to_string(e.page + 1) + "@" + (e.at == ~0u ? "end" : std::to_string(e.at));
        }
        return out;
    }
    return "no PDF tab";
}

// What the tests ask of a PDF tab: what Cmd+C copied and the web address a
// link opened (a scripted run has neither clipboard nor browser), its
// outline (> the entry the view is in, + if its box shows), and where a
// page's text is.
std::string App::pdf_dump_more(std::string_view what) {
    if (what == "clip") return "copied: " + g_pdf_clipboard;
    if (what == "opened") return "opened: " + (g_pdf_opened.empty() ? std::string("nothing") : g_pdf_opened);
    for (const std::unique_ptr<Pane>& pane : panes_) {
        if (!pane->doc || !pane->doc->pdf) continue;
        const PdfView& v = *pane->doc->pdf;
        if (what == "outline") {
            if (v.outline.empty()) return "no outline";
            std::string out = v.outline_open ? "+" : "";
            for (const PdfHeading& o : v.outline) out += std::string(static_cast<size_t>(o.depth) * 2, ' ') + o.title + " (page " + std::to_string(o.page + 1) + ") ;; ";
            return out;
        }
        // "N:TEXT": where TEXT is on page N, in window pixels x0,y0,x1,y1 (its first line's box).
        const size_t colon = what.find(':');
        const int page = std::atoi(std::string(what.substr(0, colon)).c_str()) - 1;
        if (colon == std::string_view::npos || page < 0 || page >= static_cast<int>(v.pages.size())) return "none";
        const PdfView::Page& p = v.pages[static_cast<size_t>(page)];
        if (!p.text || !p.text->boxed()) return "not read";
        const PdfText& t = *p.text;
        const size_t b = t.text.find(what.substr(colon + 1));
        if (b == std::string::npos || colon + 1 == what.size()) return "not there";
        const u32 a = t.char_at(b), e = t.char_at(b + what.size() - colon - 2);
        const double left = v.page_x(p), top = v.page_y(p);
        const float* lb = &t.lines[static_cast<size_t>(t.line[a]) * 4];
        char out[128];
        std::snprintf(out, sizeof out, "%ld,%ld,%ld,%ld", std::lround(left + t.box[a * 4] * v.scale), std::lround(top + lb[1] * v.scale), std::lround(left + t.box[e * 4 + 2] * v.scale),
                      std::lround(top + lb[3] * v.scale));
        return out;
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
        const long left = std::lround(v.page_x(p)), top = std::lround(v.page_y(p));
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
