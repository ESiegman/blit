// Editing comforts: lines moved up and down and copied, brackets and
// quotes closed as they are typed, and more carets than one.
#include <algorithm>
#include <cctype>

#include "app.hpp"

namespace blit {

namespace {

// The lines the cursor or the selection is on: a selection that ends at a
// line's start leaves that line out.
void chosen_lines(const Editor& ed, u64 lo, u64 hi, u64* l0, u64* l1) {
    *l0 = ed.buf.line_of(lo);
    *l1 = ed.buf.line_of(hi);
    if (*l1 > *l0 && hi == ed.buf.line_start(*l1)) --*l1;
}

}  // namespace

// The chosen lines go up (dir -1) or down (+1) past the line beside them,
// as one undo step; what is selected moves with them.
void App::lines_move(int dir) {
    Doc& d = *pane_->doc;
    Editor& ed = d.ed;
    if (d.read_only) return;
    u64 vlo = 0, vhi = 0;
    const bool vim = vim_selection(&vlo, &vhi);
    const u64 lo = vim ? vlo : ed.sel_min(), hi = vim ? vhi : ed.sel_max();
    u64 l0 = 0, l1 = 0;
    chosen_lines(ed, lo, hi, &l0, &l1);
    const u64 lines = ed.buf.line_count();
    if (dir < 0 ? l0 == 0 : l1 + 1 >= lines) return;
    // The lines from the one passed over to the last chosen, as text without
    // their ends, put back in the other order.
    const u64 a = dir < 0 ? l0 - 1 : l0, b = dir < 0 ? l1 : l1 + 1;
    const u64 from = ed.buf.line_start(a), to = ed.line_end(b);
    const u64 other = dir < 0 ? a : b;  // the line passed over
    const std::string passed = ed.buf.text(ed.buf.line_start(other), ed.line_end(other) - ed.buf.line_start(other));
    const u64 block_from = ed.buf.line_start(l0), block_to = ed.line_end(l1);
    const std::string block = ed.buf.text(block_from, block_to - block_from);
    const std::string moved = dir < 0 ? block + "\n" + passed : passed + "\n" + block;
    const u64 shift = passed.size() + 1;
    const u64 cursor = ed.cursor(), anchor = ed.anchor();
    ed.begin_group();
    ed.replace(from, to, moved);
    ed.end_group();
    auto moved_by = [&](u64 off) { return dir < 0 ? off - shift : off + shift; };
    if (vim) {
        const u64 nlo = moved_by(lo), nhi = moved_by(hi);
        const bool forward = cursor >= anchor;
        ed.move_to(forward ? nlo : nhi, false);
        ed.move_to(forward ? nhi : nlo, true);
        vim_.adopt_selection(ed);
    } else {
        ed.move_to(moved_by(anchor), false);
        if (anchor != cursor) ed.move_to(moved_by(cursor), true);
    }
    scroll_to_cursor();
    dirty_ = true;
}

// A copy of the chosen lines below them (dir +1: the cursor goes with the
// copy) or above them (-1: it stays with the lines it was on).
void App::lines_copy(int dir) {
    Doc& d = *pane_->doc;
    Editor& ed = d.ed;
    if (d.read_only) return;
    u64 vlo = 0, vhi = 0;
    const bool vim = vim_selection(&vlo, &vhi);
    const u64 lo = vim ? vlo : ed.sel_min(), hi = vim ? vhi : ed.sel_max();
    u64 l0 = 0, l1 = 0;
    chosen_lines(ed, lo, hi, &l0, &l1);
    const u64 block_from = ed.buf.line_start(l0), block_to = ed.line_end(l1);
    const std::string block = ed.buf.text(block_from, block_to - block_from);
    const u64 cursor = ed.cursor(), anchor = ed.anchor();
    ed.begin_group();
    ed.replace(block_to, block_to, "\n" + block);
    ed.end_group();
    const u64 shift = dir > 0 ? block.size() + 1 : 0;
    if (vim) {
        const bool forward = cursor >= anchor;
        ed.move_to((forward ? lo : hi) + shift, false);
        ed.move_to((forward ? hi : lo) + shift, true);
        vim_.adopt_selection(ed);
    } else {
        ed.move_to(anchor + shift, false);
        if (anchor != cursor) ed.move_to(cursor + shift, true);
    }
    scroll_to_cursor();
    dirty_ = true;
}

// ---------------------------------------------------------------- pairs

namespace {

char closer_of(char c) {
    switch (c) {
        case '(': return ')';
        case '[': return ']';
        case '{': return '}';
        case '"': case '\'': case '`': return c;
        default: return 0;
    }
}

bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80; }

char char_at(const Editor& ed, u64 at) {
    char c = 0;
    return at < ed.buf.size() && ed.buf.read(at, &c, 1) == 1 ? c : 0;
}

// In a string or a comment, as the colours have it (just before `at`).
bool in_text(const Doc& d, u64 at) {
    if (!d.syntax.active() || at == 0) return false;
    const Highlights& hl = d.syntax.highlights();
    const u64 p = at - 1;
    if (p < hl.base || p >= hl.end) return false;
    const size_t k = hl.upper(p);
    if (k == 0) return false;
    const std::string& name = d.syntax.style_names()[hl.style[k - 1]];
    return name.starts_with("string") || name.starts_with("comment");
}

}  // namespace

// A character typed: an opening bracket or quote puts its closer after the
// cursor (before nothing, a space or another closer; a quote not after a
// word; neither in a string or a comment); with a selection, the two go
// round it. A closer typed where one was put in steps over it.
bool App::pair_text(const char* text) {
    if (!cfg_.close_pairs || !text[0] || text[1]) return false;
    Doc& d = *pane_->doc;
    Editor& ed = d.ed;
    if (d.read_only || d.term) return false;
    const char c = text[0];
    if (pair_doc_ != d.id || pair_rev_ != ed.revision()) pair_closers_.clear();
    const u64 cur = ed.cursor();
    // Over a closer it put in.
    if (!ed.has_selection() && char_at(ed, cur) == c && std::ranges::find(pair_closers_, cur) != pair_closers_.end()) {
        std::erase(pair_closers_, cur);
        ed.move_to(cur + 1, false);
        pair_rev_ = ed.revision();
        scroll_to_cursor();
        return true;
    }
    const char close = closer_of(c);
    if (!close) return false;
    const bool quote = close == c;
    if (ed.has_selection() && !(cfg_.modal && vim_.mode() != Vim::kInsert)) {  // round what is selected
        const u64 lo = ed.sel_min(), hi = ed.sel_max();
        ed.begin_group();
        ed.replace(hi, hi, std::string(1, close));
        ed.replace(lo, lo, std::string(1, c));
        ed.end_group();
        ed.move_to(lo + 1, false);
        ed.move_to(hi + 1, true);
        scroll_to_cursor();
        return true;
    }
    const char next = char_at(ed, cur), prev = cur > 0 ? char_at(ed, cur - 1) : 0;
    const bool room = next == 0 || next == ' ' || next == '\t' || next == '\n' || next == '\r' || next == ')' || next == ']' || next == '}' || next == ',' || next == ';';
    if (!room || in_text(d, cur)) return false;
    if (quote && (word_char(prev) || prev == c)) return false;
    ed.insert(std::string{c, close});
    ed.move_to(cur + 1, false);
    for (u64& p : pair_closers_)
        if (p >= cur) p += 2;
    pair_closers_.push_back(cur + 1);
    pair_doc_ = d.id;
    pair_rev_ = ed.revision();
    scroll_to_cursor();
    lsp_typed(text);
    return true;
}

// Backspace between a pair it put in takes both; Enter between brackets
// opens an indented line between them.
bool App::pair_key(const SDL_KeyboardEvent& k) {
    if (!cfg_.close_pairs || (k.key != SDLK_BACKSPACE && k.key != SDLK_RETURN && k.key != SDLK_KP_ENTER)) return false;
    if (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI | SDL_KMOD_SHIFT)) return false;
    if (cfg_.modal && vim_.mode() != Vim::kInsert) return false;
    Doc& d = *pane_->doc;
    Editor& ed = d.ed;
    if (d.read_only || d.term || d.prof || d.image || d.pdf || d.md || ed.has_selection() || ed.multi()) return false;
    const u64 cur = ed.cursor();
    if (cur == 0) return false;
    const char prev = char_at(ed, cur - 1), next = char_at(ed, cur);
    if (closer_of(prev) == 0 || closer_of(prev) != next) return false;
    if (pair_doc_ != d.id || pair_rev_ != ed.revision()) pair_closers_.clear();
    if (k.key == SDLK_BACKSPACE) {
        if (std::ranges::find(pair_closers_, cur) == pair_closers_.end()) return false;
        std::erase(pair_closers_, cur);
        ed.replace(cur - 1, cur + 1, "");
        for (u64& p : pair_closers_)
            if (p > cur) p -= 2;
        pair_rev_ = ed.revision();
    } else {
        if (prev == '"' || prev == '\'' || prev == '`') return false;
        const u64 line = ed.buf.line_of(cur), start = ed.buf.line_start(line);
        std::string indent;
        for (u64 at = start; at < cur; ++at) {
            const char c = char_at(ed, at);
            if (c != ' ' && c != '\t') break;
            indent += c;
        }
        const std::string unit = d.indent_tabs ? std::string("\t") : std::string(std::max<u32>(1, indent_step()), ' ');
        const std::string nl = ed.crlf() ? "\r\n" : "\n";
        ed.replace(cur, cur, nl + indent + unit + nl + indent);
        ed.move_to(cur + nl.size() + indent.size() + unit.size(), false);
        pair_closers_.clear();
    }
    scroll_to_cursor();
    dirty_ = true;
    return true;
}

// After what was typed went in as it would anyway: the closers put in
// after it move along by as much.
void App::pairs_follow(u64 cursor_before, u64 size_before, u64 rev_before) {
    const Editor& ed = pane_->doc->ed;
    if (pair_closers_.empty() || pair_doc_ != pane_->doc->id || pair_rev_ != rev_before) return;
    const u64 size = ed.buf.size();
    if (size > size_before && ed.cursor() == cursor_before + (size - size_before)) {
        for (u64& p : pair_closers_)
            if (p >= cursor_before) p += size - size_before;
        pair_rev_ = ed.revision();
    } else if (ed.revision() != rev_before) {
        pair_closers_.clear();
    }
}

// ---------------------------------------------------------------- more carets than one

// The other carets of the split with the keyboard, drawn: each a bar in the
// cursor's colour, and what each has selected (Visual mode's, with Vim: the
// character under it too) as a selection is.
void App::caret_marks(FrameDesc& fd) {
    if (focus_view_ >= fd.view_count) return;
    const Editor& ed = pane_->doc->ed;
    if (!ed.multi()) return;
    GridSpec& g = fd.views[focus_view_].grid;
    caret_decos_.assign(g.decos.begin(), g.decos.end());
    const Buffer& b = ed.buf;
    const bool visual = cfg_.modal && vim_.visual();
    for (const Editor::Caret& c : ed.carets()) {
        u64 lo = std::min(c.cursor, c.anchor), hi = std::max(c.cursor, c.anchor);
        if (visual) hi = std::min(ed.next_char(hi), b.size());
        if (hi > lo) {
            const u64 l0 = b.line_of(lo), l1 = b.line_of(hi);
            for (u64 l = std::max(l0, g.top_line); l <= l1 && l < g.bottom(); ++l) {
                const u32 c0 = l == l0 ? ed.col_between(b.line_start(l), lo) : 0;
                const u32 c1 = l == l1 ? ed.col_between(b.line_start(l), hi) : ~0u;
                caret_decos_.push_back({l, c0, c1, kDecoSelect});
            }
            fd.views[focus_view_].sel_radius = look().sel;
        }
        const u64 line = b.line_of(c.cursor);
        if (line < g.top_line || line >= g.bottom()) continue;
        const u32 col = ed.col_between(b.line_start(line), c.cursor);
        caret_decos_.push_back({line, col, col + 1, kDecoCaret});
    }
    g.decos = caret_decos_;
}

// Cmd+D: the word at the cursor selected; then, each time, the next place
// the selection's text is found (after the last caret, round past the end)
// gets a caret of its own, selected, the view going to it. (every: all of
// them at once.)
void App::caret_add_match(bool every) {
    Doc& d = *pane_->doc;
    Editor& ed = d.ed;
    if (cfg_.modal) vim_attach();  // (first: taken up, Vim puts the selection away)
    if (cfg_.modal && vim_.visual()) carets_from_visual();  // (Visual mode's selections, as the editor's)
    if (!ed.has_selection()) {
        ed.select_word(ed.cursor());
        if (!every) {
            if (cfg_.modal) carets_to_visual();
            scroll_to_cursor();
            dirty_ = true;
            return;
        }
    }
    const std::string what = ed.selection_text();
    if (what.empty()) return;
    const std::string text = ed.buf.text(0, ed.buf.size());
    auto taken = [&](u64 at) {
        for (const Editor::Caret& c : ed.all_carets())
            if (std::min(c.cursor, c.anchor) == at) return true;
        return false;
    };
    if (every) {
        for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size()))
            if (!taken(at)) ed.add_caret({at + what.size(), at, ~0u});
    } else {
        u64 last = 0;
        for (const Editor::Caret& c : ed.all_carets()) last = std::max(last, std::max(c.cursor, c.anchor));
        size_t at = text.find(what, last);
        if (at == std::string::npos || taken(at)) at = text.find(what);
        while (at != std::string::npos && taken(at)) at = text.find(what, at + 1);
        if (at == std::string::npos) return notify("No more of it to find");
        ed.add_primary({at + what.size(), at, ~0u});
    }
    if (cfg_.modal) carets_to_visual();  // (selected carets are Visual mode's to go on with)
    scroll_to_cursor();
    dirty_ = true;
}

// With Vim: what the carets have selected becomes Visual mode's, each
// caret's anchor where its Visual mode begins and its cursor on the last
// character selected (as Vim has them); and back, the editor's selections.
void App::carets_to_visual() {
    Editor& ed = pane_->doc->ed;
    vim_attach();
    std::vector<Editor::Caret> others = ed.carets();
    ed.clear_carets();
    vim_.adopt_selection(ed);
    Editor::Caret primary = ed.caret();
    primary.anchor = vim_.visual() ? vim_.visual_start() : primary.cursor;
    ed.set_caret(primary);
    for (Editor::Caret c : others) {
        const u64 lo = std::min(c.cursor, c.anchor), hi = std::max(c.cursor, c.anchor);
        c.anchor = lo;
        c.cursor = hi > lo ? ed.prev_char(hi) : lo;
        ed.add_caret(c);
    }
    vim_after(Vim::kHandled);
}

void App::carets_from_visual() {
    Editor& ed = pane_->doc->ed;
    std::vector<Editor::Caret> all = ed.all_carets();
    u64 plo = 0, phi = 0;
    const bool primary_sel = vim_selection(&plo, &phi);
    vim_.keys(ed, "<Esc>");
    vim_after(Vim::kHandled);
    ed.clear_carets();
    const u64 size = ed.buf.size();
    for (Editor::Caret c : all) {
        const u64 lo = std::min(c.cursor, c.anchor), hi = std::min(std::max(c.cursor, c.anchor) < size ? ed.next_char(std::max(c.cursor, c.anchor)) : size, size);
        if (primary_sel && lo == plo) continue;  // (the primary's, below)
        ed.add_caret({hi, lo, ~0u});
    }
    if (primary_sel) {
        ed.move_to(plo, false);
        ed.move_to(phi, true);
    }
}

// A caret on the line above the highest (dir -1) or below the lowest, at
// the column the primary keeps.
void App::caret_add_line(int dir) {
    Editor& ed = pane_->doc->ed;
    const std::vector<Editor::Caret> all = ed.all_carets();
    const Editor::Caret& edge = dir < 0 ? all.front() : all.back();
    const u64 line = ed.buf.line_of(edge.cursor);
    if (dir < 0 ? line == 0 : line + 1 >= ed.buf.line_count()) return;
    const u32 col = ed.caret().want_col != ~0u ? ed.caret().want_col : ed.col_between(ed.buf.line_start(ed.buf.line_of(ed.cursor())), ed.cursor());
    const u64 at = ed.off_at(dir < 0 ? line - 1 : line + 1, col);
    Editor::Caret primary = ed.caret();
    primary.want_col = col;
    ed.add_caret({at, at, col});
    ed.set_caret(primary);
    dirty_ = true;
}

// Alt+click: a caret there, or (on one) that one gone. Alt+drag after it:
// a caret on each line between, selecting the columns between.
void App::caret_click(float x, float y, bool drag) {
    Editor& ed = pane_->doc->ed;
    u64 line = 0;
    u32 col = 0;
    point_line_col(x, y, &line, &col);
    if (!drag) {
        const u64 off = ed.off_at(line, col);
        column_from_line_ = line, column_from_col_ = col;
        std::vector<Editor::Caret> keep;
        bool removed = false;
        for (const Editor::Caret& c : ed.carets()) {
            if (c.cursor == off && c.anchor == off) removed = true;
            else keep.push_back(c);
        }
        if (!removed && ed.multi() && ed.cursor() == off && !ed.has_selection()) {  // (the primary itself: the last other one takes its place)
            ed.set_caret(keep.back());
            keep.pop_back();
            removed = true;
        }
        if (removed) {
            ed.clear_carets();
            for (const Editor::Caret& c : keep) ed.add_caret(c);
        } else if (ed.cursor() != off || ed.has_selection()) {
            ed.add_primary({off, off, ~0u});
        }
        column_keep_ = ed.carets();
        dirty_ = true;
        return;
    }
    // (The carets there were before the press stay; the column's take the place of the one it made.)
    const u64 l0 = std::min(column_from_line_, line), l1 = std::max(column_from_line_, line);
    const u32 c0 = std::min(column_from_col_, col), c1 = std::max(column_from_col_, col);
    ed.clear_carets();
    auto at_line = [&](u64 l) { return Editor::Caret{ed.off_at(l, c1), ed.off_at(l, c0), c1}; };
    ed.set_caret(at_line(line));  // (the primary first: the one on the mouse's line)
    for (const Editor::Caret& c : column_keep_) ed.add_caret(c);
    for (u64 l = l0; l <= l1; ++l)
        if (l != line) ed.add_caret(at_line(l));
    dirty_ = true;
}

// Copied with more carets than one: what each has selected, a line each.
std::string App::carets_text() const {
    const Editor& ed = pane_->doc->ed;
    std::string out;
    for (const Editor::Caret& c : ed.all_carets()) {
        const u64 lo = std::min(c.cursor, c.anchor), hi = std::max(c.cursor, c.anchor);
        if (!out.empty()) out += '\n';
        out += ed.buf.text(lo, hi - lo);
    }
    return out;
}

// Pasted with more carets than one: as many lines as carets, a line each;
// else all of it at each.
void App::carets_paste(const std::string& text) {
    Editor& ed = pane_->doc->ed;
    std::vector<std::string> lines;
    size_t at = 0;
    while (at <= text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        lines.push_back(text.substr(at, nl - at));
        at = nl + 1;
    }
    if (!text.empty() && text.back() == '\n') lines.pop_back();
    const bool spread = lines.size() == ed.carets().size() + 1;
    size_t k = 0;
    ed.each_caret([&] { ed.insert(spread ? lines[k++] : text); });
}

// Every caret, for the tests: line:column (from 1) of each, its selection's
// other end after a dash when it has one; the primary starred.
std::string App::caret_dump() const {
    const Editor& ed = pane_->doc->ed;
    size_t primary = 0;
    const std::vector<Editor::Caret> all = ed.all_carets(&primary);
    auto place = [&](u64 off) {
        const u64 l = ed.buf.line_of(off);
        return std::to_string(l + 1) + ":" + std::to_string(ed.col_between(ed.buf.line_start(l), off) + 1);
    };
    std::string out;
    for (size_t i = 0; i < all.size(); ++i) {
        out += (i ? " " : "") + std::string(i == primary ? "*" : "") + place(all[i].cursor);
        if (all[i].anchor != all[i].cursor) out += "-" + place(all[i].anchor);
    }
    return out;
}

}  // namespace blit
