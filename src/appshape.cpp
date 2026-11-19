// The shape of the text, from its syntax tree (the worker's SyntaxStructure,
// made with the colours): the bracket that pairs with the one at the cursor.
// (Brackets coloured by depth come with the colours themselves.)
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "app.hpp"
#include "look.hpp"
#include "textbox.hpp"

namespace blit {

namespace {

// The bracket at the cursor (or just before it) and the one it pairs with,
// as the tree has them: where each is.
bool matched_pair(const Doc& d, u64* a, u64* b) {
    if (!d.syntax.active()) return false;
    const std::vector<std::pair<u64, u64>>& pairs = d.syntax.structure().pairs;
    const Editor& ed = d.ed;
    if (pairs.empty() || ed.has_selection()) return false;
    const u64 cur = ed.cursor();
    auto bracket = [&](u64 at, bool* open) {
        char ch = 0;
        if (at >= ed.buf.size() || ed.buf.read(at, &ch, 1) != 1) return false;
        *open = ch == '(' || ch == '[' || ch == '{';
        return *open || ch == ')' || ch == ']' || ch == '}';
    };
    bool open = false;
    u64 at = cur;
    if (!bracket(at, &open)) {
        if (cur == 0 || !bracket(cur - 1, &open)) return false;
        at = cur - 1;
    }
    const std::pair<u64, u64>* hit = nullptr;
    if (open) {
        const auto it = std::ranges::lower_bound(pairs, at, {}, &std::pair<u64, u64>::first);
        if (it != pairs.end() && it->first == at) hit = &*it;
    } else {
        for (const std::pair<u64, u64>& p : pairs)
            if (p.second == at) hit = &p;
    }
    if (!hit) return false;
    *a = hit->first;
    *b = hit->second;
    return true;
}

}  // namespace

// The bracket at the cursor and its pair, marked as a match is, in the
// split that has the keyboard.
void App::shape_marks(FrameDesc& fd) {
    u64 a = 0, b = 0;
    if (focus_view_ >= fd.view_count || !matched_pair(*pane_->doc, &a, &b)) return;
    const Editor& ed = pane_->doc->ed;
    GridSpec& grid = fd.views[focus_view_].grid;
    shape_decos_.assign(grid.decos.begin(), grid.decos.end());
    for (const u64 o : {a, b}) {
        const u64 line = ed.buf.line_of(o);
        if (line < grid.top_line || line >= grid.top_line + grid.rows) continue;
        const u32 c0 = ed.col_between(ed.buf.line_start(line), o);
        shape_decos_.push_back({line, c0, c0 + 1, kDecoMatch});
    }
    grid.decos = shape_decos_;
}

// What is selected, as Vim's Visual mode or the editor has it ([lo, hi)).
static void selected(const Editor& ed, bool vim, u64 lo_vim, u64 hi_vim, u64* lo, u64* hi) {
    if (vim) {
        *lo = lo_vim, *hi = hi_vim;
    } else {
        *lo = ed.has_selection() ? ed.sel_min() : ed.cursor();
        *hi = ed.has_selection() ? ed.sel_max() : ed.cursor();
    }
}

void App::select_grow(bool grow) {
    Doc& d = *pane_->doc;
    Editor& ed = d.ed;
    u64 vlo = 0, vhi = 0;
    const bool vim = vim_selection(&vlo, &vhi);
    u64 lo = 0, hi = 0;
    selected(ed, vim, vlo, vhi, &lo, &hi);
    Grow& g = grow_;
    const bool same = g.doc == d.id && g.revision == ed.revision() && g.now == std::pair<u64, u64>{lo, hi};
    auto take = [&](std::pair<u64, u64> r) {
        if (r.second > r.first) {
            ed.move_to(r.first, false);
            ed.move_to(r.second, true);
        } else {
            ed.move_to(r.first, false);
        }
        if (cfg_.modal) vim_.adopt_selection(ed);
        u64 a = 0, b = 0;
        const bool v = vim_selection(&a, &b);
        selected(ed, v, a, b, &g.now.first, &g.now.second);
        scroll_to_cursor();
        dirty_ = true;
    };
    if (!grow) {
        if (!same || g.at == 0) return;
        --g.at;
        take(g.at == 0 ? g.start : g.chain[g.at - 1]);
        return;
    }
    if (!same) {
        g = Grow{};
        g.doc = d.id;
        g.revision = ed.revision();
        g.start = g.now = {lo, hi};
        if (!d.syntax.active() || !d.syntax.ask_around(ed.buf, lo, hi, &g.chain) || g.chain.empty()) {
            // No tree: the word, its line (or the selection's lines), the whole text.
            auto word = [&](u64 at) {
                char ch = 0;
                return at < ed.buf.size() && ed.buf.read(at, &ch, 1) == 1 && (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || static_cast<unsigned char>(ch) >= 0x80);
            };
            g.chain.clear();
            u64 a = lo, b = hi;
            while (a > 0 && lo - a < 400 && word(a - 1)) --a;
            while (b < ed.buf.size() && b - hi < 400 && word(b)) ++b;
            auto offer = [&](u64 x, u64 y) {
                const std::pair<u64, u64> have = g.chain.empty() ? std::pair<u64, u64>{lo, hi} : g.chain.back();
                if (x <= have.first && y >= have.second && (x < have.first || y > have.second)) g.chain.emplace_back(x, y);
            };
            offer(a, b);
            offer(ed.buf.line_start(ed.buf.line_of(lo)), ed.line_end(ed.buf.line_of(hi > lo ? hi - 1 : hi)));
            offer(0, ed.buf.size());
        }
    }
    if (g.at >= g.chain.size()) return;
    take(g.chain[g.at++]);
    g.revision = ed.revision();
}

// For the tests: each bracket on the lines on show in the file in front,
// line:column=depth (from 1; "-" a bracket the tree does not have, as in a
// string), and the pair marked at the cursor.
std::string App::shape_dump() {
    settle_syntax();
    const Doc& d = *pane_->doc;
    if (!d.syntax.active()) return "no syntax";
    const Highlights& hl = d.syntax.highlights();
    const std::vector<std::string>& names = d.syntax.style_names();
    const Editor& ed = d.ed;
    const u64 lines = ed.buf.line_count(), top = static_cast<u64>(std::max(0.0, pane_->scroll_y)) / static_cast<u64>(font_->cell_h());
    std::string out = "brackets";
    std::vector<u32> inks;  // (the inks the tree's brackets are drawn in)
    for (u64 line = top; line < lines && line < top + static_cast<u64>(visible_rows()); ++line) {
        const u64 start = ed.buf.line_start(line);
        const std::string text = ed.buf.text(start, std::min<u64>(ed.line_end(line) - start, 2000));
        for (size_t i = 0; i < text.size(); ++i) {
            if (!std::strchr("()[]{}", text[i]) || text[i] == 0) continue;
            const u64 off = start + i;
            std::string depth = "-";
            if (off >= hl.base && off < hl.end) {
                const size_t k = hl.upper(off);
                const std::string& name = k > 0 ? names[hl.style[k - 1]] : names[0];
                if (name.starts_with("bracket.")) {
                    depth = name.substr(8);
                    const u32 ink = (d.style_bits[hl.style[k - 1]] >> kCellInkShift) & 31;
                    if (std::ranges::find(inks, ink) == inks.end()) inks.push_back(ink);
                }
            }
            out += " " + std::to_string(line + 1) + ":" + std::to_string(i + 1) + "=" + depth;
        }
    }
    out += " | inks " + std::to_string(inks.size());
    u64 a = 0, b = 0;
    if (!matched_pair(d, &a, &b)) return out + " | match none";
    auto where = [&](u64 o) {
        const u64 line = ed.buf.line_of(o);
        return std::to_string(line + 1) + ":" + std::to_string(o - ed.buf.line_start(line) + 1);
    };
    return out + " | match " + where(a) + " " + where(b);
}

// ---------------------------------------------------------------- breadcrumbs

// A split's breadcrumbs: the file's folders (from the project's) and name,
// then the definitions the cursor is in, outermost first. Each part's
// columns, and what a click on it does: a folder or the name, Go to file
// in that folder; a definition, the outline there.
struct Crumbs {
    struct Part {
        std::string text;
        bool scope = false;
        u64 at = 0;           // a definition's name
        std::string folder;   // a path part: the folder Go to file starts in
        int x0 = 0, x1 = 0;   // columns, once laid out
    };
    std::vector<Part> parts;
    TextBox box;
    u64 asked = 0;  // (what they were made from: unchanged, nothing is made again)
    u64 made = 0;   // (and what they are: the file, the room, the definitions the cursor is in)
};

int App::crumbs_px(const Pane& p) const {
    if (!cfg_.breadcrumbs || !font_ || !p.doc || focus_mode_) return 0;
    const Doc& d = *p.doc;
    if (d.term || d.image || d.pdf || d.md || d.prof || d.marks || d.ed.path.empty()) return 0;
    const int ch = font_->cell_h();
    return ch + ch / 4;
}

namespace {

// The parts, for a document with its cursor at `cur`.
std::vector<Crumbs::Part> crumb_parts(const Doc& d, u64 cur, const std::string* root) {
    std::vector<Crumbs::Part> out;
    std::string path = full_path(d.ed.path);
    std::string base;
    if (root && path.starts_with(*root + "/")) {
        base = *root;
        path = path.substr(root->size() + 1);
    }
    std::string folder = base;
    for (size_t at = 0;;) {
        const size_t slash = path.find('/', at);
        Crumbs::Part p;
        p.text = path.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
        if (slash != std::string::npos) folder += (folder.empty() || folder.back() == '/' ? "" : "/") + p.text;
        p.folder = base.empty() ? std::string() : folder.size() > base.size() ? folder.substr(base.size() + 1) : std::string();
        if (!p.text.empty()) out.push_back(std::move(p));
        if (slash == std::string::npos) break;
        at = slash + 1;
    }
    if (base.empty() && out.size() > 2) out.erase(out.begin(), out.end() - 2);  // (outside the project: the file and its folder)
    if (!d.syntax.active()) return out;
    for (const SyntaxScope& sc : d.syntax.structure().scopes) {
        if (cur < sc.start || cur >= sc.end) continue;
        Crumbs::Part p;
        p.text = sc.name + (sc.kind == 12 || sc.kind == 6 || sc.kind == 9 ? "()" : "");
        p.scope = true;
        p.at = sc.name_at;
        out.push_back(std::move(p));
    }
    return out;
}

}  // namespace

void App::crumbs_layout(FrameDesc& fd, bool focused) {
    const int h = crumbs_px(*pane_);
    if (!h || fd.popup_count >= std::size(fd.popups)) return;
    if (!pane_->crumbs) pane_->crumbs = std::make_shared<Crumbs>();
    Crumbs& c = *pane_->crumbs;
    Font& font = *font_;
    const Look lk = look();
    const int cw = font.cell_w(), ch = font.cell_h(), pad_x = lk.edge;
    const u32 cols = static_cast<u32>(std::max(1, (pane_->x1 - pane_->x0 - 2 * pad_x) / cw));
    const Doc& d = *pane_->doc;
    const u64 cur = caret_in(*pane_, d).cursor;
    // What they are made from, cheaply: the file, the cursor, the text, the
    // tree's definitions as they last came, the room, the font.
    u64 key = 1469598103934665603ull;
    auto mix = [&](u64 v) { key = (key ^ v) * 1099511628211ull; };
    mix(d.id);
    mix(cur);
    mix(d.ed.revision());
    mix(std::hash<std::string>{}(d.ed.path));
    mix(reinterpret_cast<uintptr_t>(panel_root()));
    if (d.syntax.active()) {
        const std::vector<SyntaxScope>& sc = d.syntax.structure().scopes;
        mix(sc.size());
        if (!sc.empty()) mix(sc.front().start * 31 + sc.back().end);
    }
    mix(cols);
    mix(font.generation());
    mix(focused);
    bool remake = key != c.asked;
    if (remake) {
        c.asked = key;
        // Then what they show: the file, the room, and the definitions the
        // cursor is in (where each one's name is, its kind, its name). The
        // cursor moved, or the text changed, inside the same ones, and the
        // parts and their cells are as they were.
        u64 shown = 1469598103934665603ull;
        auto add = [&](u64 v) { shown = (shown ^ v) * 1099511628211ull; };
        add(d.id);
        add(std::hash<std::string>{}(d.ed.path));
        add(reinterpret_cast<uintptr_t>(panel_root()));
        add(cols);
        add(font.generation());
        add(focused);
        add(d.syntax.active());
        if (d.syntax.active())
            for (const SyntaxScope& sc : d.syntax.structure().scopes)
                if (cur >= sc.start && cur < sc.end) {
                    add(sc.name_at);
                    add(sc.kind);
                    add(std::hash<std::string>{}(sc.name));
                }
        remake = shown != c.made;
        c.made = shown;
    }
    if (remake) {
        std::vector<Crumbs::Part> parts = crumb_parts(d, cur, panel_root());
        // " › " between parts; too wide, the first ones go (folders first), an ellipsis for them.
        const std::string_view sep = " \xE2\x80\xBA ";
        auto width = [&](size_t from) {
            u32 w = from > 0 ? 2 : 0;
            for (size_t i = from; i < parts.size(); ++i) w += width_of(parts[i].text) + (i > from ? 3 : 0);
            return w;
        };
        size_t from = 0;
        while (from + 1 < parts.size() && width(from) > cols) ++from;
        c.parts.assign(parts.begin() + static_cast<std::ptrdiff_t>(from), parts.end());
        c.box.resize(cols, 1);
        u32 col = 0;
        if (from > 0) col = put(c.box, font, 0, col, "\xE2\x80\xA6 ", ink::kMuted);
        for (size_t i = 0; i < c.parts.size(); ++i) {
            Crumbs::Part& p = c.parts[i];
            if (i > 0) col = put(c.box, font, 0, col, sep, ink::kMuted);
            p.x0 = static_cast<int>(col);
            const bool last = i + 1 == c.parts.size();
            col = put(c.box, font, 0, col, fit_text(p.text, cols > col ? cols - col : 0), last && focused ? ink::kStrong : p.scope ? ink::kText : ink::kMuted);
            p.x1 = static_cast<int>(col);
        }
    }
    Popup& p = fd.popups[fd.popup_count++];
    p.x = pane_->x0;
    p.y = pane_->y0 + tabs_px();
    p.w = std::max(1, pane_->x1 - pane_->x0);
    p.h = std::min(h, std::max(0, pane_->y1 - p.y));
    p.pad_x = pad_x;
    p.pad_y = (h - ch) / 2;
    p.cols = c.box.cols;
    p.rows = c.box.rows;
    p.cells = c.box.cells.data();
    p.border_px = 0;
}

bool App::crumbs_mouse_down(const SDL_MouseButtonEvent& b) {
    const int h = crumbs_px(*pane_);
    const int px = static_cast<int>(b.x * density_), py = static_cast<int>(b.y * density_), y = pane_->y0 + tabs_px();
    if (!h || !pane_->crumbs || b.button != SDL_BUTTON_LEFT || py < y || py >= y + h || px < pane_->x0 || px >= pane_->x1) return false;
    const int col = static_cast<int>(std::floor(static_cast<double>(px - pane_->x0 - look().edge) / font_->cell_w()));
    for (const Crumbs::Part& p : pane_->crumbs->parts) {
        if (col < p.x0 || col >= p.x1) continue;
        if (p.scope) {  // the outline, the tree's (the breadcrumbs are), at this definition
            Doc& d = *pane_->doc;
            SyntaxStructure all;
            if (d.syntax.active() && d.syntax.ask_all(d.ed.buf, &all)) {
                lsp_close(kPopAll);
                outline_show(tree_symbols(d, all), p.at);
            }
        } else {
            find_open(kFindNames, p.folder.empty() ? std::string() : p.folder + "/");
        }
        break;
    }
    dirty_ = true;
    return true;
}

std::string App::crumbs_dump() {
    settle_syntax();
    if (!crumbs_px(*pane_)) return "none";
    std::string out;
    for (const Crumbs::Part& p : crumb_parts(*pane_->doc, caret_in(*pane_, *pane_->doc).cursor, panel_root())) out += (out.empty() ? "" : " > ") + p.text;
    return out;
}

// ---------------------------------------------------------------- sticky scroll

// The lines that open the scopes (the folds the tree has) that the top of
// the view is in, pinned over it, outermost first, five at most.
struct Sticky {
    std::vector<u64> lines;
    std::vector<u32> cells;
    u32 cols = 0, gutter = 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // where they are, for clicks
};

namespace {

constexpr size_t kStickyMost = 5;

std::vector<u64> sticky_lines(const Doc& d, double scroll_y, int ch) {
    std::vector<u64> rows;
    if (!d.syntax.active() || scroll_y < 1) return rows;
    const std::vector<SyntaxFold>& folds = d.syntax.structure().folds;
    const u64 top = static_cast<u64>(scroll_y) / static_cast<u64>(ch);  // (a row: with lines folded, not a line)
    if (!d.folds.empty()) d.folds.sync(d.ed.buf, d.ed.revision());
    // Each row the outermost scope, inside the one pinned before it, that
    // the line under the rows so far is in.
    while (rows.size() < kStickyMost) {
        const u64 line = d.folds.empty() ? top + rows.size() : d.folds.line_at(top + rows.size());
        const SyntaxFold* pick = nullptr;
        for (const SyntaxFold& f : folds) {
            if (f.header >= line) break;
            if (f.last < line || (!rows.empty() && f.header <= rows.back())) continue;
            pick = &f;
            break;
        }
        if (!pick) break;
        rows.push_back(pick->header);
    }
    return rows;
}

}  // namespace

void App::sticky_layout(FrameDesc& fd, const TextView& v) {
    Pane& p = *pane_;
    if (!cfg_.sticky_scroll || p.doc->term || p.doc->image || p.doc->pdf || p.doc->md || p.doc->prof || p.doc->marks) return;
    if (p.sticky) p.sticky->lines.clear();
    const int ch = font_->cell_h(), cw = font_->cell_w();
    std::vector<u64> lines = sticky_lines(*p.doc, p.scroll_y, ch);
    if (lines.empty() || fd.popup_count >= std::size(fd.popups)) return;
    if (!p.sticky) p.sticky = std::make_shared<Sticky>();
    Sticky& s = *p.sticky;
    s.lines = std::move(lines);
    // The cells line up with the text's: as many whole columns of the
    // gutter as fit left of the text's first column, the line's number in
    // them; then the text, filled as the view fills it.
    const int text_left = v.x - static_cast<int>(v.origin[0]);
    s.gutter = static_cast<u32>(std::max(0, (text_left - p.x0) / cw));
    s.cols = s.gutter + v.grid.cols;
    const u32 rows = static_cast<u32>(s.lines.size());
    s.cells.assign(static_cast<size_t>(s.cols) * rows, 0);
    TextBox numbers;
    numbers.resize(s.gutter, 1);
    for (u32 r = 0; r < rows; ++r) {
        GridSpec g = v.grid;
        g.top_line = s.lines[r];
        g.rows = 1;
        g.decos = {};
        g.notes = {};
        g.inserts = {};
        g.pict_rows = nullptr;
        g.has_sel = false;
        g.edits = nullptr;
        fill_grid(p.doc->ed.buf, *font_, g, s.cells.data() + static_cast<size_t>(r) * s.cols + s.gutter);
        if (s.gutter > 1) {
            std::ranges::fill(numbers.cells, 0u);
            const std::string n = std::to_string(s.lines[r] + 1);
            const u32 w = static_cast<u32>(n.size());
            if (w + 1 <= s.gutter) put(numbers, *font_, 0, s.gutter - 1 - w, n, ink::kMuted);
            std::ranges::copy(numbers.cells, s.cells.begin() + static_cast<std::ptrdiff_t>(r) * s.cols);
        }
    }
    Popup& q = fd.popups[fd.popup_count++];
    q.x = p.x0;
    q.y = p.text_y;
    q.w = std::max(1, p.x1 - p.x0);
    q.h = std::min(static_cast<int>(rows) * ch, std::max(0, p.y1 - p.text_y));
    q.pad_x = std::max(0, text_left - p.x0 - static_cast<int>(s.gutter) * cw);
    q.pad_y = 0;
    q.cols = s.cols;
    q.rows = rows;
    q.cells = s.cells.data();
    q.border_px = 0;
    q.bg = theme_.bg;
    const Look lk = look();
    if (lk.shadow > 0) {
        q.shadow = std::round(lk.shadow * 0.5f);
        q.shadow_dy = std::round(q.shadow * 0.3f);
        q.shadow_alpha = 0.8f;
        q.clip[0] = p.x0, q.clip[1] = p.text_y, q.clip[2] = p.x1, q.clip[3] = p.y1;
    }
    s.x0 = q.x, s.y0 = q.y, s.x1 = q.x + q.w, s.y1 = q.y + q.h;
}

bool App::sticky_mouse_down(const SDL_MouseButtonEvent& b) {
    if (!pane_->sticky || pane_->sticky->lines.empty() || b.button != SDL_BUTTON_LEFT) return false;
    const Sticky& s = *pane_->sticky;
    const int px = static_cast<int>(b.x * density_), py = static_cast<int>(b.y * density_);
    if (px < s.x0 || px >= s.x1 || py < s.y0 || py >= s.y1) return false;
    const size_t row = static_cast<size_t>((py - s.y0) / font_->cell_h());
    if (row >= s.lines.size()) return true;
    remember_place();  // (Ctrl-O comes back from it)
    Editor& ed = pane_->doc->ed;
    const u64 start = ed.buf.line_start(s.lines[row]);
    u64 at = start;
    char ch = 0;
    while (at < ed.line_end(s.lines[row]) && ed.buf.read(at, &ch, 1) == 1 && (ch == ' ' || ch == '\t')) ++at;
    ed.move_to(at, false);
    vim_mouse();
    scroll_to_cursor();
    dirty_ = true;
    return true;
}

std::string App::sticky_dump() {
    settle_syntax();
    const std::vector<u64> lines = cfg_.sticky_scroll ? sticky_lines(*pane_->doc, pane_->scroll_y, font_->cell_h()) : std::vector<u64>{};
    if (lines.empty()) return "none";
    std::string out;
    for (const u64 l : lines) out += (out.empty() ? "" : " ") + std::to_string(l + 1);
    return out;
}

// ---------------------------------------------------------------- folding

const Folds* App::folds_on() const {
    const Doc& d = *pane_->doc;
    if (d.folds.empty()) return nullptr;
    d.folds.sync(d.ed.buf, d.ed.revision());
    return &d.folds;
}

u64 App::row_of_line(u64 line) const {
    if (const Wrap* w = wrap_on()) return w->row_of(line);  // (lines wrapped: their first rows; folds are in its map too)
    const Folds* f = folds_on();
    return f ? f->row_of(line) : line;
}

u64 App::line_of_row(u64 row) const {
    const u64 lines = pane_->doc->ed.buf.line_count();
    if (const Wrap* w = wrap_on()) return std::min(w->line_at(row), lines ? lines - 1 : 0);
    const Folds* f = folds_on();
    return std::min(f ? f->line_at(row) : row, lines ? lines - 1 : 0);
}

u64 App::view_line_count() const {
    if (const Wrap* w = wrap_on()) return w->rows();
    const Folds* f = folds_on();
    const u64 lines = pane_->doc->ed.buf.line_count();
    return f ? f->rows(lines) : lines;
}

namespace {

// The folds hear of edits, and the editor's moves go by rows, only while
// there are any.
void fold_listen(Doc& d) {
    if (d.folds.empty()) {
        d.ed.unlisten(&d.folds);
        d.ed.skip = nullptr;
    } else {
        d.folds.attach(&d.ed);
        d.ed.listen(&d.folds);
        d.ed.skip = &d.folds;
    }
}

// How far a line is indented (-1: blank), for folding without a tree.
i64 indent_of(const Editor& ed, u64 line) {
    const u64 start = ed.buf.line_start(line), end = ed.line_end(line);
    i64 n = 0;
    char ch = 0;
    for (u64 at = start; at < end && ed.buf.read(at, &ch, 1) == 1; ++at) {
        if (ch == ' ') ++n;
        else if (ch == '\t') n += ed.tab_width;
        else return n;
    }
    return -1;
}

// What can fold around a line: the tree's folds of the text on show; or,
// with no tree, the lines indented further than the one before them.
std::vector<SyntaxFold> fold_points(const Doc& d, u64 line) {
    if (d.syntax.active()) return d.syntax.structure().folds;
    std::vector<SyntaxFold> out;
    const Editor& ed = d.ed;
    const u64 lines = ed.buf.line_count();
    auto block = [&](u64 header) {  // the lines after header indented further than it
        const i64 own = indent_of(ed, header);
        if (own < 0) return;
        u64 last = header;
        for (u64 l = header + 1; l < lines && l - header < 100000; ++l) {
            const i64 in = indent_of(ed, l);
            if (in < 0) continue;
            if (in <= own) break;
            last = l;
        }
        if (last > header) out.push_back({static_cast<u32>(header), static_cast<u32>(last)});
    };
    // The headers above it, each indented less than the one before.
    i64 limit = indent_of(ed, line);
    block(line);
    for (u64 l = line; l-- > 0 && line - l < 100000;) {
        const i64 in = indent_of(ed, l);
        if (in < 0 || (limit >= 0 && in >= limit)) continue;
        block(l);
        limit = in;
        if (in == 0) break;
    }
    std::ranges::sort(out, [](const SyntaxFold& a, const SyntaxFold& b) { return a.header < b.header; });
    return out;
}

}  // namespace

// The fold at the cursor's line: one that starts there, else the innermost
// it is in; on a line already folded, the one around that.
void App::fold_close() {
    Doc& d = *pane_->doc;
    Editor& ed = d.ed;
    const u64 line = ed.buf.line_of(ed.cursor());
    d.folds.sync(ed.buf, ed.revision());
    const bool closed = !d.folds.empty() && d.folds.header(line);
    const SyntaxFold* pick = nullptr;
    const std::vector<SyntaxFold> points = fold_points(d, line);
    for (const SyntaxFold& f : points) {
        if (f.header > line) break;
        if (f.header == line && !closed) pick = &f;
        else if (f.header < line && f.last >= line && (!pick || pick->header != line)) pick = &f;  // (later ones are inside earlier ones)
    }
    if (!pick || !d.folds.add(ed.buf, pick->header, pick->last)) return;
    fold_listen(d);
    if (line != pick->header) {  // (the cursor to the line it is folded under)
        const u64 start = ed.buf.line_start(pick->header);
        u64 at = start;
        char ch = 0;
        while (at < ed.line_end(pick->header) && ed.buf.read(at, &ch, 1) == 1 && (ch == ' ' || ch == '\t')) ++at;
        ed.move_to(at, false);
        vim_mouse();
    }
    scroll_to_cursor();
    dirty_ = true;
}

void App::fold_open() {
    Doc& d = *pane_->doc;
    const u64 line = d.ed.buf.line_of(d.ed.cursor());
    if (d.folds.open_header(d.ed.buf, line) || d.folds.open_around(d.ed.buf, line)) {
        fold_listen(d);
        scroll_to_cursor();
        dirty_ = true;
    }
}

void App::fold_toggle() {
    Doc& d = *pane_->doc;
    const u64 line = d.ed.buf.line_of(d.ed.cursor());
    d.folds.sync(d.ed.buf, d.ed.revision());
    if (!d.folds.empty() && d.folds.header(line)) fold_open();
    else fold_close();
}

// Every fold the whole file has closed (the tree's), or every one opened.
void App::fold_all(bool close) {
    Doc& d = *pane_->doc;
    if (!close) {
        d.folds.clear();
    } else {
        SyntaxStructure all;
        if (!d.syntax.active() || !d.syntax.ask_all(d.ed.buf, &all)) return;
        for (const SyntaxFold& f : all.folds) d.folds.add(d.ed.buf, f.header, f.last);
        fold_reveal();  // (the cursor's line stays on show)
    }
    fold_listen(d);
    scroll_to_cursor();
    dirty_ = true;
}

// The cursor has gone into lines folded away (a search, a jump, an edit):
// the folds that hide its line open.
void App::fold_reveal() {
    Doc& d = *pane_->doc;
    if (d.folds.empty()) return;
    const u64 line = d.ed.buf.line_of(d.ed.cursor());
    d.folds.sync(d.ed.buf, d.ed.revision());
    if (!d.folds.hidden(line)) return;
    while (d.folds.open_around(d.ed.buf, line)) {
    }
    fold_listen(d);
    dirty_ = true;
}

// After a folded line's text, in the quiet ink: how many lines are under it.
void App::fold_notes(FrameDesc& fd) {
    for (const std::unique_ptr<Pane>& up : panes_) {
        Pane& p = *up;
        if (!p.doc || p.doc->folds.empty() || p.view >= fd.view_count) continue;
        GridSpec& g = fd.views[p.view].grid;
        if (!g.row_lines) continue;
        const Folds& f = p.doc->folds;
        f.sync(p.doc->ed.buf, p.doc->ed.revision());
        std::vector<GridNote>& notes = p.fold_notes;
        std::vector<u32>& cells = p.fold_note_cells;
        notes.assign(g.notes.begin(), g.notes.end());
        cells.clear();
        std::vector<std::pair<size_t, u64>> mine;  // (where each note's cells start, and its line)
        for (u32 r = 0; r < g.rows; ++r) {
            const u64 line = g.row_lines[r];
            if (line == ~0ull) break;
            const u64 n = f.hidden_under(line);
            if (!n) continue;
            TextBox box;
            const std::string text = " \xE2\x8B\xAF " + std::to_string(n) + (n == 1 ? " line" : " lines");
            box.resize(width_of(text), 1);
            put(box, *font_, 0, 0, text, ink::kMuted);
            mine.emplace_back(cells.size(), line);
            cells.insert(cells.end(), box.cells.begin(), box.cells.end());
        }
        if (mine.empty()) continue;
        const Editor& ed = p.doc->ed;
        for (size_t i = 0; i < mine.size(); ++i) {
            const size_t from = mine[i].first, to = i + 1 < mine.size() ? mine[i + 1].first : cells.size();
            const u64 line = mine[i].second;
            const u32 col = ed.col_between(ed.buf.line_start(line), ed.line_end(line)) + 1;
            notes.push_back({line, col, cells.data() + from, static_cast<u32>(to - from)});
        }
        std::ranges::sort(notes, [](const GridNote& a, const GridNote& b) { return a.line < b.line; });
        g.notes = notes;
    }
}

bool App::fold_mouse_down(const SDL_MouseButtonEvent& b) {
    Doc& d = *pane_->doc;
    if (d.folds.empty() || b.button != SDL_BUTTON_LEFT) return false;
    const int px = static_cast<int>(b.x * density_), py = static_cast<int>(b.y * density_);
    if (py < pane_->text_y || py >= pane_->y1 || px < pane_->x0 || px >= pane_->x1) return false;
    const i64 doc_y = std::llround(pane_->scroll_y) + py - pane_->text_y;
    if (doc_y < 0) return false;
    const u64 line = line_of_row(static_cast<u64>(doc_y) / static_cast<u64>(font_->cell_h()));
    d.folds.sync(d.ed.buf, d.ed.revision());
    if (!d.folds.header(line)) return false;
    // On its mark in the line numbers, or past the end of its text (the note).
    const double tx = px - pane_->text_x + std::llround(pane_->scroll_x) - pad_x_;
    const u64 end_col = d.ed.col_between(d.ed.buf.line_start(line), d.ed.line_end(line));
    if (px >= pane_->text_x && tx < static_cast<double>(end_col + 1) * font_->cell_w()) return false;
    d.folds.open_header(d.ed.buf, line);
    fold_listen(d);
    dirty_ = true;
    return true;
}

std::string App::fold_dump() {
    const Doc& d = *pane_->doc;
    std::string out = "folds";
    for (const Folds::Fold& f : d.folds.all())
        out += " " + std::to_string(d.ed.buf.line_of(f.head) + 1) + "-" + std::to_string(d.ed.buf.line_of(std::min(f.end, d.ed.buf.size())) + 1);
    if (d.folds.empty()) out += " none";
    out += " | rows";
    const u64 lines = d.ed.buf.line_count(), rows = std::min<u64>(view_line_count(), 12);
    for (u64 r = 0; r < rows; ++r) out += " " + std::to_string(std::min(line_of_row(r), lines - 1) + 1);
    return out;
}

// ---------------------------------------------------------------- the scroll bar's marks

// Down the right edge of a view whose text is longer than it: where the
// problems, the matches of Find in this file, the lines changed since the
// last commit, the breakpoints and the cursor are, each a thin bar at its
// place in the whole file (three places to a cell). Where several fall
// together the one that matters most shows: the cursor, an error, a
// breakpoint, a warning, a match, a change.
struct Ruler {
    TextBox box;  // one column: a bar in a row's cell, or nothing
    u64 key = 0;
    u64 col = 0;  // the display column the view's right edge is in
    std::vector<GridNote> notes;  // (the view's other notes, and a cell of these on the line each row shows)
    std::vector<std::pair<u32, char>> marks;  // (for the tests: each place's row third and kind)
    std::vector<u8> prio;  // each place's mark but the cursor's: how much it matters (0: none), and its ink
    std::vector<u32> ink;
    u32 cursor_at = ~0u;
    u32 font = ~0u;
};

void App::ruler_layout(const TextView& v) {
    Pane& p = *pane_;
    const Doc& d = *p.doc;
    if (d.term || d.image || d.pdf || d.md || d.prof) return;
    const int ch = font_->cell_h(), cw = font_->cell_w();
    const u32 rows = static_cast<u32>(std::max(0, v.h / ch));
    const u64 total = view_line_count();
    if (!rows || max_scroll_y() <= 0 || total == 0) {
        if (p.ruler) p.ruler->marks.clear();
        return;
    }
    if (!p.ruler) p.ruler = std::make_shared<Ruler>();
    Ruler& r = *p.ruler;
    const std::vector<Diagnostic>* diags = lsp_.diagnostics_of(d.id);
    const std::vector<u64>* matches = find_file_matches(d.id);
    const u32 places = rows * 3;
    auto place_of = [&](u64 line) { return static_cast<u32>(std::min<u64>(places - 1, row_of_line(line) * places / total)); };
    // All but the cursor, made again when what they come from changes (an
    // edit on a line moves none of them: only the count of lines is looked at).
    u64 key = 1469598103934665603ull;
    auto mix = [&](u64 x) { key = (key ^ x) * 1099511628211ull; };
    mix(d.id);
    mix(d.ed.buf.line_count());
    mix(rows);
    mix(total);
    mix(d.folds.generation());
    mix(lsp_.diagnostics_generation());
    mix(diags ? diags->size() : 0);
    mix(d.hunks_gen);
    mix(debug_gen_);
    mix(matches ? matches->size() * 31 + (matches->empty() ? 0 : matches->front()) : ~0ull);
    if (key != r.key) {
        r.key = key;
        r.prio.assign(places, 0);
        r.ink.assign(places, 0);
        auto mark = [&](u64 line, u8 pr, u32 in) {
            const u32 at = place_of(line);
            if (pr > r.prio[at]) r.prio[at] = pr, r.ink[at] = in;
        };
        const Buffer& b = d.ed.buf;
        for (const GitHunk& h : d.hunks) {
            const u32 in = h.kind == GitHunk::kAdded ? kInkAdded : h.kind == GitHunk::kModified ? kInkChanged : kInkDiagnostic;
            const u64 n = std::max<u32>(h.count, 1);
            for (u64 l = h.line; l < h.line + n && l < b.line_count(); l += std::max<u64>(1, total / places)) mark(l, 1, in);
        }
        if (matches)
            for (const u64 m : *matches) mark(b.line_of(std::min(m, b.size())), 2, kInkDiagnostic + 2);
        if (diags)
            for (const Diagnostic& g : *diags)
                if (g.severity <= 2) mark(b.line_of(std::min(g.start, b.size())), g.severity == 1 ? 5 : 3, kInkDiagnostic + g.severity - 1);
        for (const u64 l : debug_break_lines(d)) mark(l, 4, kInkDiagnostic);
        r.cursor_at = ~0u;
    }
    // The cursor's over them.
    const u32 cursor_at = place_of(p.cur_line);
    if (cursor_at != r.cursor_at || r.box.rows != rows || r.font != font_->generation()) {
        r.cursor_at = cursor_at;
        r.font = font_->generation();
        r.box.resize(1, rows);
        r.marks.clear();
        static constexpr u32 kBars[] = {0x2594, 0x2501, 0x2581};  // (a third of a cell each: its top, its middle, its foot)
        static constexpr char kKinds[] = {0, 'c', 'f', 'w', 'b', 'e', '>'};
        auto prio = [&](u32 at) -> u8 { return at == cursor_at ? 6 : r.prio[at]; };
        for (u32 row = 0; row < rows; ++row) {  // (a cell, one bar: of its three places, the one that matters most)
            u32 at = row * 3;
            for (u32 k = 1; k < 3; ++k)
                if (prio(row * 3 + k) > prio(at)) at = row * 3 + k;
            r.box.cells[row] = 0;
            if (!prio(at)) continue;
            const u32 in = at == cursor_at ? kInkAccent : r.ink[at];
            r.box.cells[row] = (font_->glyph(kBars[at % 3]) & (Font::kWide - 1)) | (in << kCellInkShift);
            r.marks.emplace_back(at, kKinds[prio(at)]);
        }
    }
    // The text's last column on show, right of which nothing is: the marks
    // are put there, over the text (ruler_notes()).
    const i64 last = (static_cast<i64>(v.w) + static_cast<i64>(v.origin[0])) / cw - 1 - static_cast<i64>(v.grid.lead);
    r.col = v.grid.first_col + static_cast<u64>(std::max<i64>(0, last));
}

// The marks, as one cell each on the line their row shows, with the
// view's other notes (after the language server's are set).
void App::ruler_notes(FrameDesc& fd) {
    for (const std::unique_ptr<Pane>& up : panes_) {
        Pane& p = *up;
        if (!p.doc || !p.ruler || p.ruler->marks.empty() || p.view >= fd.view_count) continue;
        Ruler& r = *p.ruler;
        GridSpec& g = fd.views[p.view].grid;
        const u64 lines = p.doc->ed.buf.line_count();
        r.notes.assign(g.notes.begin(), g.notes.end());
        for (u32 row = 0; row < r.box.rows && row < g.rows; ++row) {
            const u64 line = g.line_at(row);
            if (r.box.cells[row] && line < lines) r.notes.push_back({line, static_cast<u32>(r.col), &r.box.cells[row], 1});
        }
        g.notes = r.notes;
    }
}

std::string App::ruler_dump() {
    if (!pane_->ruler || pane_->ruler->marks.empty()) return "none";
    std::string out;
    for (const auto& [at, kind] : pane_->ruler->marks) out += (out.empty() ? "" : " ") + std::to_string(at) + ":" + kind;
    return out;
}

}  // namespace blit
