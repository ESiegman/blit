// Editing comforts: lines moved up and down and copied, and brackets and
// quotes closed as they are typed.
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
    if (d.read_only || d.term || d.prof || d.image || d.pdf || d.md || ed.has_selection()) return false;
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

}  // namespace blit
