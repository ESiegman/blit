// The Markdown preview: a tab beside a Markdown file that shows it drawn
// (headings, emphasis, lists, tables, code, Mermaid diagrams: markdown.cpp,
// mermaid.cpp), kept up to date as it is typed in, and scrolled to where
// the file's cursor is. Cmd+Shift+V opens it; it is read only.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>

#include "app.hpp"
#include "markdown.hpp"
#include "textbox.hpp"
#include "utf8.hpp"

namespace blit {

struct MdView {
    u64 source = 0;  // the document it shows
    std::vector<MdRow> rows;
    u64 rev = ~0ull;  // the text's revision, and the width, they were drawn at
    u32 width = 0;
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
    // Backgrounds: the text's own, a little towards its colour.
    auto tone = [&](u32 i, float k) {
        for (int c = 0; c < 3; ++c) v.tones[i][c] = bg[c] + (v.palette[kMdText][c] - bg[c]) * k;
        v.tones[i][3] = 1;
    };
    tone(kMdCodeTone, 0.07f);
    tone(kMdHeaderTone, 0.12f);
    tone(kMdNodeTone, 0.05f);
}

}  // namespace

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
        const u32 cols = static_cast<u32>(w / cw), rows = static_cast<u32>(h / ch);
        v.x = x, v.y = y, v.cols = cols, v.shown = rows;
        v.pad_x = (w - static_cast<int>(cols) * cw) / 2;
        v.pad_y = (h - static_cast<int>(rows) * ch) / 2;
        colours(tc, theme_.bg, v);
        // Drawn again when the text or the width changes.
        Pane* src_pane = nullptr;
        size_t src_at = 0;
        Doc* src = find_doc(v.source, &src_pane, &src_at);
        const u32 width = cols > 2 * kMargin + 12 ? cols - 2 * kMargin : cols;
        bool fresh = false;
        if (src && (src->ed.revision() != v.rev || width != v.width)) {
            v.rows = md_render(src->ed.buf.text(0, src->ed.buf.size()), width);
            v.rev = src->ed.revision();
            v.width = width;
            fresh = true;
        }
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
        mix(cols), mix(rows), mix(font.generation()), mix(v.top), mix(v.rev), mix(v.width), mix(src ? 1 : 0);
        for (int c = 0; c < 3; ++c) mix(static_cast<u64>(v.palette[kMdText][c] * 1000));
        if (key != v.key) {
            v.key = key;
            v.box.resize(cols, rows);
            v.hits.clear();
            if (!src) v.box.put(font, 1, kMargin, "The file this showed has been closed.", kMdMuted, Font::kItalic);
            for (u32 r = 0; src && r < rows && v.top + r < v.rows.size(); ++r) {
                const MdRow& row = v.rows[v.top + r];
                u32* line = v.box.cells.data() + static_cast<size_t>(r) * cols;
                if (row.fill)
                    for (u32 c = kMargin + row.fill_from; c < std::min(cols, kMargin + row.fill_to); ++c) line[c] |= kCellSelected | static_cast<u32>(row.fill) << kCellDecoShift;
                u32 c = kMargin;
                for (const MdRun& run : row.runs) {
                    const u32 style = (run.style & kMdBold ? Font::kBold : 0) | (run.style & kMdItalic ? Font::kItalic : 0);
                    const u32 end = v.box.put(font, r, c, run.text, run.ink, style);
                    if (run.tone)
                        for (u32 k = c; k < end; ++k) line[k] = (line[k] & ~kCellDecoMask) | kCellSelected | static_cast<u32>(run.tone) << kCellDecoShift;
                    if (!run.link.empty()) v.hits.push_back({r, c, end, run.link});
                    c = end;
                }
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

// The preview in front (the first there is), its rows as text (for the tests).
std::string App::md_dump() {
    for (const std::unique_ptr<Pane>& p : panes_) {
        if (!p->doc || !p->doc->md) continue;
        const MdView& v = *p->doc->md;
        std::string out;
        for (const MdRow& r : v.rows) out += md_row_text(r) + "\n";
        return out;
    }
    return "no preview\n";
}

}  // namespace blit
