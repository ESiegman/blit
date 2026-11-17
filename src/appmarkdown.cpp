// The Markdown preview: a tab beside a Markdown file that shows it drawn
// (headings, emphasis, lists, tables, code, Mermaid diagrams: markdown.cpp,
// mermaid.cpp; pictures: appimage.cpp), kept up to date as it is typed in,
// and scrolled to where the file's cursor is. Cmd+Shift+V opens it; it is
// read only. Pictures are read from files beside it (or anywhere on its
// machine), never from the web: those stay a link.
//
// The text is drawn into rows on a thread of its own (MdWorker): a key
// typed in the file only copies the text and hands it over, and the
// preview takes the new rows on a frame soon after. (Drawn in the frame, a
// README of 1,600 lines made each key cost 1.7 ms instead of 41 µs.) Code
// blocks are coloured there too, by the editor's grammars, the theme's
// colours of a few kinds of token standing for all of them.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "app.hpp"
#include "markdown.hpp"
#include "remote.hpp"
#include "syntax.hpp"
#include "textbox.hpp"
#include "utf8.hpp"

namespace blit {

struct MdView {
    u64 source = 0;  // the document it shows
    std::vector<MdRow> rows;
    // What the text was last handed to the worker at: its revision, the
    // width and height, and the pictures' generation (asked again when one
    // of them changes); how many times rows have come; whether they have
    // just come.
    u64 asked_rev = ~0ull, asked_pictures = ~0ull;
    u32 asked_width = 0, asked_tall = 0;
    u64 renders = 0;
    bool fresh = false;
    std::string base;               // the folder its pictures are found from
    std::vector<std::string> pics;  // the pictures its text names (their files)
    const ThemeColors* coloured = nullptr;  // the theme its colours were taken from
    float coloured_bg[3] = {-1, -1, -1};
    u32 top = 0;           // the first row shown
    u64 followed = ~0ull;  // the source's line it was last scrolled to
    double wheel = 0;      // a touchpad's fractions of a row, kept
    // As drawn: the cells, where, and the links in them.
    TextBox box;
    u64 key = 0;
    int x = 0, y = 0, pad_x = 0, pad_y = 0;
    u32 cols = 0, shown = 0;
    float palette[32][4] = {};
    float tones[8][4] = {};
    struct Hit {
        u32 row, c0, c1;
        std::string link;
    };
    std::vector<Hit> hits;
};

namespace {

constexpr u32 kMargin = 2;  // columns kept clear at either side
constexpr u32 kTop = 1;     // rows kept clear above the text (it does not start against the tab bar)

// The file a picture in the text is (relative to the folder of the file
// the text is in, `base`), or "" if it is not one to read: on the web, or
// in the text itself (data:).
std::string picture_source(const std::string& base, std::string_view src) {
    if (src.starts_with("http://") || src.starts_with("https://") || src.starts_with("data:") || src.starts_with("//")) return {};
    if (src.starts_with("file://")) src.remove_prefix(7);
    src = src.substr(0, src.find_first_of("?#"));
    std::string path;
    for (size_t k = 0; k < src.size(); ++k) {  // (%20 and the like)
        if (src[k] == '%' && k + 2 < src.size() && std::isxdigit(static_cast<unsigned char>(src[k + 1])) && std::isxdigit(static_cast<unsigned char>(src[k + 2]))) {
            path += static_cast<char>(std::stoi(std::string(src.substr(k + 1, 2)), nullptr, 16));
            k += 2;
        } else {
            path += src[k];
        }
    }
    if (path.empty()) return {};
    std::string host, there;
    const bool remote = split_remote(base, &host, &there);
    if (path.starts_with('/')) return remote ? remote_path(host, path) : path;
    if (base.empty()) return {};
    while (path.starts_with("./")) path.erase(0, 2);
    return base + "/" + path;
}

// The theme's colour for the first of these captures it gives one to (else `otherwise`).
Rgb pick(const ThemeColors& tc, std::initializer_list<const char*> captures, Rgb otherwise) {
    for (const char* c : captures) {
        const Rgb col = tc.syntax_style(c).color;
        if (col.r != tc.foreground.r || col.g != tc.foreground.g || col.b != tc.foreground.b) return col;
    }
    return otherwise;
}

void colours(const ThemeColors& tc, const float* bg, MdView& v) {
    auto set = [&](u32 i, Rgb c) {
        v.palette[i][0] = c.r, v.palette[i][1] = c.g, v.palette[i][2] = c.b, v.palette[i][3] = 1;
    };
    set(kMdText, tc.foreground);
    set(kMdHeading1, pick(tc, {"markup.heading.1", "markup.heading", "keyword"}, tc.accent));
    set(kMdHeading2, pick(tc, {"markup.heading.2", "function", "type"}, tc.accent));
    set(kMdHeading3, pick(tc, {"markup.heading.3", "type", "string"}, tc.accent));
    set(kMdHeading, pick(tc, {"markup.heading", "keyword"}, tc.foreground));
    set(kMdCode, pick(tc, {"markup.raw", "string"}, tc.foreground));
    set(kMdLink, pick(tc, {"markup.link.url", "markup.link", "string.special.url", "function"}, tc.info));
    set(kMdQuote, pick(tc, {"markup.quote", "comment"}, tc.muted));
    set(kMdMuted, tc.muted);
    set(kMdAccent, tc.accent);
    set(kMdBorder, pick(tc, {"type", "function"}, tc.accent));
    set(kMdEdge, tc.muted);
    set(kMdLabel, pick(tc, {"string", "constant"}, tc.hint));
    set(kMdError, tc.error);
    set(kMdSlice0, pick(tc, {"function"}, tc.info));
    set(kMdSlice1, pick(tc, {"string"}, tc.warning));
    set(kMdSlice2, pick(tc, {"type"}, tc.hint));
    set(kMdSlice3, pick(tc, {"keyword"}, tc.error));
    set(kMdCodeKeyword, pick(tc, {"keyword"}, tc.accent));
    set(kMdCodeString, pick(tc, {"string"}, tc.foreground));
    set(kMdCodeComment, pick(tc, {"comment"}, tc.muted));
    set(kMdCodeFunction, pick(tc, {"function"}, tc.foreground));
    set(kMdCodeType, pick(tc, {"type"}, tc.foreground));
    set(kMdCodeConstant, pick(tc, {"number", "constant", "boolean"}, tc.foreground));
    set(kMdCodeOperator, pick(tc, {"operator", "punctuation"}, tc.foreground));
    set(kMdCodePlain, tc.foreground);
    // Backgrounds: the text's own, a little towards its colour.
    auto tone = [&](u32 i, float k) {
        for (int c = 0; c < 3; ++c) v.tones[i][c] = bg[c] + (v.palette[kMdText][c] - bg[c]) * k;
        v.tones[i][3] = 1;
    };
    tone(kMdCodeTone, 0.07f);
    tone(kMdHeaderTone, 0.12f);
    tone(kMdNodeTone, 0.05f);
}

// The preview's ink for a capture of a highlight query: by the first part
// of its name, the few kinds of token it tells apart.
u8 code_ink(std::string_view capture) {
    const std::string_view head = capture.substr(0, capture.find('.'));
    static const struct {
        std::string_view name;
        u8 ink;
    } kinds[] = {
        {"keyword", kMdCodeKeyword}, {"conditional", kMdCodeKeyword}, {"repeat", kMdCodeKeyword}, {"include", kMdCodeKeyword},
        {"exception", kMdCodeKeyword}, {"preproc", kMdCodeKeyword}, {"storageclass", kMdCodeKeyword},
        {"string", kMdCodeString}, {"character", kMdCodeString}, {"escape", kMdCodeString},
        {"comment", kMdCodeComment},
        {"function", kMdCodeFunction}, {"method", kMdCodeFunction}, {"constructor", kMdCodeFunction},
        {"type", kMdCodeType}, {"module", kMdCodeType}, {"namespace", kMdCodeType}, {"attribute", kMdCodeType},
        {"number", kMdCodeConstant}, {"float", kMdCodeConstant}, {"boolean", kMdCodeConstant}, {"constant", kMdCodeConstant},
        {"operator", kMdCodeOperator}, {"punctuation", kMdCodeOperator}, {"delimiter", kMdCodeOperator},
    };
    for (const auto& k : kinds)
        if (head == k.name) return k.ink;
    return kMdCodePlain;
}

// The size of a picture known to the worker: read (and its own size), or not to be shown.
struct PicSize {
    bool ready = false;
    int w = 0, h = 0;
};

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 20;
    SDL_PushEvent(&ev);
}

}  // namespace

struct App::MdWorker {
    struct Job {
        u64 view = 0;  // the preview's document
        u64 rev = 0, pictures = 0;
        u32 width = 0, tall = 0;
        std::string text, base;
        double density = 1;
        int cw = 1, ch = 1;
        std::map<std::string, PicSize> known;  // pictures asked about before
        // Code blocks coloured (not the first time: then it is drawn in
        // the frame, and comes again coloured), and where grammars are.
        bool colour = false;
        SyntaxOptions syntax;
        std::vector<GrammarSource> grammars;
    };
    struct Done {
        u64 view = 0, rev = 0, pictures = 0;
        u32 width = 0, tall = 0;
        std::vector<MdRow> rows;
        std::vector<std::string> wanted;  // pictures it names that were not known yet
    };
    std::mutex mu;
    std::condition_variable_any cv;
    std::map<u64, Job> todo;  // the newest for each preview (an older one not drawn yet is not drawn at all)
    u64 busy = 0;             // the preview being drawn (0: none)
    u64 posted = 0;           // how many have been handed over (for md_wait)
    std::vector<Done> done;
    // Code blocks coloured lately, by language and text, with their inks:
    // typing in a file redraws all of it, and its code blocks are mostly
    // as they were. Only the worker uses it.
    std::map<std::string, std::vector<u8>> coloured;
    std::jthread worker;

    MdWorker() {
        worker = std::jthread([this](std::stop_token st) { run(st); });
    }
    ~MdWorker() {
        worker.request_stop();
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }
    // The rows of a job's text. A picture's size: one of its pixels to a
    // point (or the size the text asks for), made smaller to fit across,
    // and to fit in the view.
    static Done draw(Job& j, std::map<std::string, std::vector<u8>>* coloured = nullptr) {
        Done d;
        d.view = j.view, d.rev = j.rev, d.pictures = j.pictures, d.width = j.width, d.tall = j.tall;
        const MdPictureFn size = [&](const std::string& s, u32 room, u32 want_w, u32 want_h, MdPic* out) {
            const std::string path = picture_source(j.base, s);
            if (path.empty()) return false;
            const auto it = j.known.find(path);
            if (it == j.known.end()) {
                d.wanted.push_back(path);
                j.known[path] = {};
                return false;
            }
            if (!it->second.ready || it->second.w <= 0 || it->second.h <= 0) return false;
            double pw = it->second.w, ph = it->second.h;
            if (want_w && want_h) pw = want_w, ph = want_h;
            else if (want_w) ph = ph * want_w / pw, pw = want_w;
            else if (want_h) pw = pw * want_h / ph, ph = want_h;
            pw *= j.density, ph *= j.density;
            const double k = std::min({1.0, static_cast<double>(room) * j.cw / pw, static_cast<double>(std::max<u32>(j.tall, 4) - 2) * j.ch / ph});
            out->w = static_cast<u32>(std::max(1.0, std::round(pw * k)));
            out->h = static_cast<u32>(std::max(1.0, std::round(ph * k)));
            out->cols = (out->w + static_cast<u32>(j.cw) - 1) / static_cast<u32>(j.cw);
            out->rows = (out->h + static_cast<u32>(j.ch) - 1) / static_cast<u32>(j.ch);
            return true;
        };
        const MdCodeFn colour = [&](std::string_view word, std::string_view code, std::vector<u8>* inks) {
            const std::string lang = Syntax::language_named(word, j.grammars, j.syntax);
            if (lang.empty()) return false;
            std::string key = lang + '\n';
            key += code;
            if (const auto it = coloured->find(key); it != coloured->end()) {
                *inks = it->second;
                return true;
            }
            std::vector<u8> styles;
            std::vector<std::string> names;
            if (!Syntax::highlight_text(lang, code, j.syntax, &styles, &names)) return false;
            std::vector<u8> by_style(names.size(), kMdCodePlain);
            for (size_t s = 1; s < names.size(); ++s) by_style[s] = code_ink(names[s]);
            inks->resize(styles.size());
            for (size_t k = 0; k < styles.size(); ++k) (*inks)[k] = styles[k] < by_style.size() ? by_style[styles[k]] : static_cast<u8>(kMdCodePlain);
            if (coloured->size() >= 256) coloured->clear();
            coloured->emplace(std::move(key), *inks);
            return true;
        };
        d.rows = md_render(j.text, j.width, size, j.colour && coloured ? colour : MdCodeFn{});
        return d;
    }
    void run(std::stop_token st) {
        for (;;) {
            Job j;
            {
                std::unique_lock l(mu);
                if (!cv.wait(l, st, [&] { return !todo.empty(); })) return;
                const auto it = todo.begin();
                j = std::move(it->second);
                todo.erase(it);
                busy = j.view;
            }
            Done d = draw(j, &coloured);
            {
                std::lock_guard l(mu);
                busy = 0;
                done.push_back(std::move(d));
            }
            wake();
        }
    }
};

// Rows the worker has drawn, into their previews; pictures they name, asked for.
void App::md_sync() {
    if (!md_worker_) return;
    std::vector<MdWorker::Done> done;
    {
        std::lock_guard l(md_worker_->mu);
        done.swap(md_worker_->done);
    }
    for (MdWorker::Done& d : done) {
        Doc* doc = find_doc(d.view);
        if (!doc || !doc->md) continue;
        MdView& v = *doc->md;
        v.rows = std::move(d.rows);
        ++v.renders;
        v.fresh = true;
        for (const std::string& path : d.wanted) {
            if (std::ranges::find(v.pics, path) != v.pics.end()) continue;
            v.pics.push_back(path);
            // (one read already, by another preview or a tab: drawn again with it now)
            if (picture(path)->state != PictureInfo::kReading) v.asked_pictures = ~0ull;
        }
        dirty_ = true;
    }
}

// For the tests: frames until every preview's text has been drawn as it is now (10 s at most).
void App::md_wait() {
    const u64 until = now_ns() + 10'000'000'000ull;
    while (now_ns() < until) {
        const u64 posted = md_worker_ ? md_worker_->posted : 0;
        frame();
        bool idle = true;
        if (md_worker_) {
            for (;;) {
                {
                    std::lock_guard l(md_worker_->mu);
                    idle = md_worker_->todo.empty() && !md_worker_->busy;
                }
                if (idle || now_ns() >= until) break;
                SDL_Delay(1);
            }
        }
        md_sync();
        if (!md_worker_ || md_worker_->posted == posted) break;
    }
    frame();  // (the rows that came, put in the cells: what a click lands on)
}

// The front file shown drawn, in a tab in a split to its right (or that
// tab brought to the front, if there is one); from the preview, back to its file.
void App::md_open() {
    Doc& src = *pane_->doc;
    if (src.md) {
        Pane* p = nullptr;
        size_t at = 0;
        if (find_doc(src.md->source, &p, &at)) {
            focus_pane(p);
            tab_show(at);
        }
        return;
    }
    for (const std::unique_ptr<Pane>& p : panes_)
        for (size_t i = 0; i < p->docs.size(); ++i)
            if (p->docs[i]->md && p->docs[i]->md->source == src.id) {
                Pane* const editing = pane_;
                pane_ = p.get();
                tab_show(i);
                focus_pane(editing);
                dirty_ = true;
                return;
            }
    const u64 id = src.id;
    const std::string name = src.ed.path.empty() ? std::string("untitled") : src.ed.path.substr(src.ed.path.find_last_of('/') + 1);
    Pane* const editing = pane_;
    Pane* into = split_pane(editing, kEdgeRight);
    if (!into) into = editing;
    pane_ = into;
    Doc* d = add_doc(pane_->tab + 1);
    d->title = "Preview: " + name;
    d->read_only = true;
    d->md = std::make_shared<MdView>();
    d->md->source = id;
    for (size_t i = 0; i < pane_->docs.size(); ++i)
        if (pane_->docs[i].get() == d) tab_show(i);
    focus_pane(editing);
    dirty_ = true;
}

void App::md_layout(FrameDesc& fd) {
    Font& font = *font_;
    const int cw = font.cell_w(), ch = font.cell_h();
    const ThemeColors& tc = cfg_.theme(SDL_GetSystemTheme() != SDL_SYSTEM_THEME_LIGHT);
    for (const std::unique_ptr<Pane>& pane : panes_) {
        if (!pane->doc || !pane->doc->md || fd.popup_count >= std::size(fd.popups)) continue;
        MdView& v = *pane->doc->md;
        const int x = pane->x0, y = pane->text_y, w = pane->x1 - pane->x0, h = pane->y1 - pane->text_y;
        if (w < cw * 8 || h < ch * 2) continue;
        const u32 cols = static_cast<u32>(w / cw), all_rows = static_cast<u32>(h / ch);
        const u32 rows = all_rows > kTop + 1 ? all_rows - kTop : all_rows;  // (the text's, under the clear row)
        const u32 top_row = all_rows - rows;
        v.x = x, v.y = y, v.cols = cols, v.shown = rows;
        v.pad_x = (w - static_cast<int>(cols) * cw) / 2;
        v.pad_y = (h - static_cast<int>(all_rows) * ch) / 2;
        if (v.coloured != &tc || !std::equal(theme_.bg, theme_.bg + 3, v.coloured_bg)) {  // (the theme's colours, looked up when it changes)
            colours(tc, theme_.bg, v);
            v.coloured = &tc;
            std::copy(theme_.bg, theme_.bg + 3, v.coloured_bg);
        }
        // Drawn again when the text or the width changes.
        Pane* src_pane = nullptr;
        size_t src_at = 0;
        Doc* src = find_doc(v.source, &src_pane, &src_at);
        const u32 width = cols > 2 * kMargin + 12 ? cols - 2 * kMargin : cols;
        // Its text handed to the worker when it, the width or the height change, or a picture comes.
        if (src && (src->ed.revision() != v.asked_rev || width != v.asked_width || rows != v.asked_tall || pictures_gen_ != v.asked_pictures)) {
            if (!md_worker_) md_worker_ = std::make_shared<MdWorker>();
            v.base = !src->ed.path.empty() ? parent_of(full_path(src->ed.path)) : std::string();
            MdWorker::Job j;
            j.view = pane->doc->id;
            j.rev = src->ed.revision(), j.pictures = pictures_gen_, j.width = width, j.tall = rows;
            j.text = src->ed.buf.text(0, src->ed.buf.size());
            j.base = v.base;
            j.density = density_, j.cw = cw, j.ch = ch;
            for (const std::string& path : v.pics) {
                const PictureInfo* p = picture(path);
                j.known[path] = {p->state == PictureInfo::kReady, p->full_w, p->full_h};
            }
            j.colour = cfg_.syntax;
            j.syntax.parser_dirs = cfg_.parser_dirs;
            j.syntax.query_dirs = cfg_.query_dirs;
            j.grammars = cfg_.grammars;
            v.asked_rev = j.rev, v.asked_width = width, v.asked_tall = rows, v.asked_pictures = j.pictures;
            const bool first = v.renders == 0 && v.rows.empty();
            if (first) {  // (the first time: at once, so that it opens drawn; its code is coloured on the worker)
                MdWorker::Done d = MdWorker::draw(j);
                std::lock_guard l(md_worker_->mu);
                md_worker_->done.push_back(std::move(d));
            }
            if (!first || (j.colour && (j.text.find("```") != std::string::npos || j.text.find("~~~") != std::string::npos))) {
                std::lock_guard l(md_worker_->mu);
                md_worker_->todo[j.view] = std::move(j);
                ++md_worker_->posted;
                md_worker_->cv.notify_all();
            }
            if (v.renders == 0) md_sync();
        }
        const bool fresh = v.fresh;
        v.fresh = false;
        // Scrolled to where the file's cursor is, while the file is being worked on (not the preview).
        if (src && src_pane && src_pane->doc == src && pane.get() != pane_) {
            const u64 line = src->ed.buf.line_of(src->ed.cursor());
            if (line != v.followed || fresh) {
                const bool moved = line != v.followed;
                v.followed = line;
                u32 row = 0;
                for (u32 k = 0; k < v.rows.size(); ++k)
                    if (v.rows[k].line <= line) row = k;
                if (moved || row < v.top || row >= v.top + rows) v.top = row > rows / 3 ? row - rows / 3 : 0;
            }
        }
        const u32 max_top = v.rows.size() > rows / 2 ? static_cast<u32>(v.rows.size()) - rows / 2 : 0;
        v.top = std::min(v.top, max_top);

        u64 key = 1469598103934665603ull;
        auto mix = [&](u64 k) { key = (key ^ k) * 1099511628211ull; };
        mix(cols), mix(rows), mix(font.generation()), mix(v.top), mix(v.renders), mix(src ? 1 : 0);
        for (int c = 0; c < 3; ++c) mix(static_cast<u64>(v.palette[kMdText][c] * 1000));
        if (key != v.key) {
            v.key = key;
            v.box.resize(cols, all_rows);
            v.hits.clear();
            if (!src) v.box.put(font, top_row, kMargin, "The file this showed has been closed.", kMdMuted, 0);
            for (u32 k = 0; src && k < rows && v.top + k < v.rows.size(); ++k) {
                const MdRow& row = v.rows[v.top + k];
                const u32 r = k + top_row;  // (its row in the box)
                u32* line = v.box.cells.data() + static_cast<size_t>(r) * cols;
                if (row.fill)
                    for (u32 c = kMargin + row.fill_from; c < std::min(cols, kMargin + row.fill_to); ++c) line[c] |= kCellSelected | static_cast<u32>(row.fill) << kCellDecoShift;
                u32 c = kMargin;
                for (const MdRun& run : row.runs) {
                    const u32 style = (run.style & kMdBold ? Font::kBold : 0) | (run.style & kMdItalic ? Font::kItalic : 0);
                    const u32 end = v.box.put(font, r, c, run.text, run.ink, style);
                    if (run.tone)
                        for (u32 cell = c; cell < end; ++cell) line[cell] = (line[cell] & ~kCellDecoMask) | kCellSelected | static_cast<u32>(run.tone) << kCellDecoShift;
                    if (!run.link.empty()) v.hits.push_back({r, c, end, run.link});
                    c = end;
                }
                for (const MdPic& pic : row.pics)
                    if (!pic.link.empty()) v.hits.push_back({r, kMargin + pic.col, kMargin + pic.col + pic.cols, pic.link});
            }
        }
        Popup& q = fd.popups[fd.popup_count++];
        q = Popup{};
        q.x = x, q.y = y, q.w = w, q.h = h;
        q.pad_x = v.pad_x, q.pad_y = v.pad_y;
        q.cols = v.box.cols, q.rows = v.box.rows;
        q.cells = v.box.cells.data();
        q.border_px = 0;
        q.bg = theme_.bg;
        q.palette = &v.palette[0][0];
        q.tones = &v.tones[0][0];
        // The pictures on show, over the rows kept for them (each from its
        // first row, or the first row shown if that is above the view).
        for (u32 k = 0; src && k < rows && v.top + k < v.rows.size(); ++k)
            for (const MdPic& pic : v.rows[v.top + k].pics) {
                if ((pic.part != 0 && k != 0) || fd.picture_count >= std::size(fd.pictures)) continue;
                const u32 r = k + top_row;
                const std::string path = picture_source(v.base, pic.src);
                if (path.empty()) continue;
                const PictureInfo* p = picture(path);
                if (p->state != PictureInfo::kReady) continue;
                PictureDraw& d = fd.pictures[fd.picture_count++];
                d = PictureDraw{};
                d.image = p->gpu;
                const int left = x + v.pad_x + static_cast<int>(kMargin + pic.col) * cw;
                const int top = y + v.pad_y + (static_cast<int>(r) - static_cast<int>(pic.part)) * ch;
                d.rect[0] = static_cast<float>(left), d.rect[1] = static_cast<float>(top);
                d.rect[2] = static_cast<float>(left + static_cast<int>(pic.w)), d.rect[3] = static_cast<float>(top + static_cast<int>(pic.h));
                d.clip[0] = x, d.clip[1] = y + v.pad_y + static_cast<int>(top_row) * ch, d.clip[2] = x + w, d.clip[3] = y + v.pad_y + static_cast<int>(all_rows) * ch;
                d.after = fd.popup_count;
                d.sharp = static_cast<double>(pic.w) / p->w >= 3;
                d.clear = p->clear;
            }
    }
}

bool App::md_hit(int px, int py, Pane** pane, u32* row, u32* col) {
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->md) continue;
        const MdView& v = *p->doc->md;
        if (px < v.x || py < v.y || px >= p->x1 || py >= p->y1 || !v.cols) continue;
        const int c = (px - v.x - v.pad_x) / font_->cell_w(), r = (py - v.y - v.pad_y) / font_->cell_h();
        *pane = p.get();
        *row = static_cast<u32>(std::max(0, r));
        *col = static_cast<u32>(std::max(0, c));
        return true;
    }
    return false;
}

bool App::md_key(const SDL_KeyboardEvent& k) {
    if (!pane_->doc->md || (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_ALT))) return false;
    MdView& v = *pane_->doc->md;
    const u32 page = v.shown > 3 ? v.shown - 2 : 1;
    const u32 last = static_cast<u32>(v.rows.size());
    switch (k.key) {
        case SDLK_UP:
        case SDLK_K: v.top = v.top ? v.top - 1 : 0; break;
        case SDLK_DOWN:
        case SDLK_J: v.top = std::min(v.top + 1, last); break;
        case SDLK_PAGEUP:
        case SDLK_B: v.top = v.top > page ? v.top - page : 0; break;
        case SDLK_PAGEDOWN:
        case SDLK_SPACE: v.top = std::min(v.top + page, last); break;
        case SDLK_HOME: v.top = 0; break;
        case SDLK_G:
            v.top = k.mod & SDL_KMOD_SHIFT ? last : 0;
            break;
        case SDLK_END: v.top = last; break;
        case SDLK_ESCAPE: {  // back to its file
            Pane* p = nullptr;
            size_t at = 0;
            if (!find_doc(v.source, &p, &at)) return false;
            focus_pane(p);
            tab_show(at);
            break;
        }
        default: return false;
    }
    term_drop_text_ = true;  // (a letter's text is not typed after it)
    dirty_ = true;
    return true;
}

bool App::md_wheel(const SDL_MouseWheelEvent& w) {
    Pane* pane = nullptr;
    u32 row = 0, col = 0;
    if (!md_hit(static_cast<int>(mouse_x_ * density_), static_cast<int>(mouse_y_ * density_), &pane, &row, &col)) return false;
    MdView& v = *pane->doc->md;
    const bool precise = w.y != std::floor(w.y);
    v.wheel -= w.y * (precise ? 1.0 : 3.0);
    const int step = static_cast<int>(v.wheel);
    v.wheel -= step;
    v.top = static_cast<u32>(std::clamp(static_cast<i64>(v.top) + step, i64{0}, static_cast<i64>(v.rows.size())));
    dirty_ = true;
    return true;
}

// A click on the preview gives it the keyboard; on a link, follows it: a
// web address in the browser, a file beside the source in a tab.
bool App::md_mouse_down(const SDL_MouseButtonEvent& b) {
    Pane* pane = nullptr;
    u32 row = 0, col = 0;
    if (!md_hit(static_cast<int>(b.x * density_), static_cast<int>(b.y * density_), &pane, &row, &col)) return false;
    focus_pane(pane);
    MdView& v = *pane->doc->md;
    dirty_ = true;
    if (b.button != SDL_BUTTON_LEFT) return true;
    for (const MdView::Hit& h : v.hits) {
        if (h.row != row || col < h.c0 || col >= h.c1) continue;
        const std::string& link = h.link;
        if (link.starts_with("http://") || link.starts_with("https://") || link.starts_with("mailto:")) {
            SDL_OpenURL(link.c_str());
        } else if (link.starts_with('#')) {  // a heading in it: its row to the top
            std::string want = link.substr(1);
            for (u32 k = 0; k < v.rows.size(); ++k) {
                std::string slug;
                for (const char ch : md_row_text(v.rows[k])) {
                    if (std::isalnum(static_cast<unsigned char>(ch))) slug += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    else if (ch == ' ' || ch == '-') slug += '-';
                }
                if (slug == want) {
                    v.top = k;
                    break;
                }
            }
        } else if (Doc* src = find_doc(v.source); src && !src->ed.path.empty()) {
            std::string path = link.substr(0, link.find('#'));
            if (!path.starts_with('/')) path = std::string(parent_of(full_path(src->ed.path))) + "/" + path;
            open_path(path.c_str());
        }
        break;
    }
    return true;
}

// The preview in front (the first there is), its rows as text (for the
// tests); with `inks`, each run of a row in the colour it is drawn in
// ("[#rrggbb]text").
std::string App::md_dump(bool inks) {
    md_wait();
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->md) continue;
        const MdView& v = *p->doc->md;
        std::string out;
        for (const MdRow& r : v.rows) {
            if (!inks) {
                out += md_row_text(r) + "\n";
                continue;
            }
            for (const MdRun& run : r.runs) {
                char hex[16];
                const float* c = v.palette[run.ink < 32 ? run.ink : 0];
                auto byte = [](float f) { return static_cast<int>(std::lround(std::clamp(f, 0.0f, 1.0f) * 255)); };
                std::snprintf(hex, sizeof hex, "[#%02x%02x%02x]", byte(c[0]), byte(c[1]), byte(c[2]));
                out += hex + run.text;
            }
            out += "\n";
        }
        return out;
    }
    return "no preview\n";
}

}  // namespace blit
