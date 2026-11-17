// The shape of the text, from its syntax tree (the worker's SyntaxStructure,
// made with the colours): the bracket that pairs with the one at the cursor.
// (Brackets coloured by depth come with the colours themselves.)
#include <algorithm>
#include <cctype>
#include <cstring>

#include "app.hpp"

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

}  // namespace blit
