// Peek: the definition of what the cursor is on, in a box under its line
// that is a split of its own (its own tab, cursor and scroll, the text
// editable), floating over the split it was asked from and not part of the
// window's splits. Esc closes it and gives the keyboard back; so does going
// to another split. A file it opened that has changes stays, in a tab of
// the split it was asked from.
#include <algorithm>

#include "app.hpp"
#include "applua.hpp"

namespace blit {

Pane* App::peek_pane() const {
    if (!peek_) return nullptr;
    for (const std::unique_ptr<Pane>& p : panes_)
        if (p->id == peek_) return p.get();
    return nullptr;
}

// The definition's place, in a box under the cursor's line.
void App::peek_show(LspLocation loc) {
    if (!loc.valid()) {
        notify("No definition found");
        return;
    }
    Pane* host = pane_;
    if (Pane* p = peek_pane()) {  // (from the peek: another in its place, under the line it was asked from)
        if (pane_ == p) host = nullptr;
        for (const std::unique_ptr<Pane>& q : panes_)
            if (!host && q->id == p->host) host = q.get();
        peek_close();
        if (!host) host = pane_;
    }
    focus_pane(host);
    host->peek_line = host->doc->ed.buf.line_of(host->doc->ed.cursor());
    Pane* p = add_pane();
    p->floating = true;
    p->host = host->id;
    peek_ = p->id;
    pane_ = p;
    add_doc(0);
    pane_->doc = pane_->docs[0].get();
    pane_->tab = 0;
    if (!open_path(loc.path.c_str())) {
        pane_ = host;
        peek_close();
        return;
    }
    // (open_path put it in a tab after the new file's, or in its place.)
    for (size_t i = pane_->docs.size(); i-- > 0;)
        if (pane_->docs.size() > 1 && pane_->docs[i]->ed.path.empty() && !pane_->docs[i]->ed.modified()) tab_close(i, true);
    // (The keyboard to the box first: a file the split shows too has one
    // editor, which takes up the box's own place in it then.)
    Pane* const peek = pane_;
    pane_ = host;
    focus_pane(peek);
    Doc& d = *pane_->doc;
    if (d.loading) {
        d.goto_line = loc.line;
        d.goto_col = loc.character;
    } else {
        d.ed.move_to(Lsp::offset_at(d.ed.buf, loc.line, loc.character, lsp_.counts_utf16()), false);
    }
    peek_place(*p, *host);
    // The definition's line a little way down the box.
    const double ch = font_->cell_h();
    refresh_cursor_cache();
    p->target_y = p->scroll_y = std::max(0.0, static_cast<double>(p->cur_row) * ch - 2 * ch);
    pick_theme();
    dirty_ = true;
}

// Where the box goes: under the line it was asked from, across the split,
// a dozen lines or so (above the line, if there is more room there).
void App::peek_place(Pane& p, Pane& host) {
    const int ch = font_->cell_h();
    Pane* const keep = pane_;
    pane_ = &host;
    const i64 line_y = host.text_y + static_cast<i64>(row_of_line(host.peek_line)) * ch - std::llround(host.scroll_y);
    pane_ = keep;
    // Under the line, at most a dozen lines or so; above it if there is more
    // room there; never over the line itself.
    const int tall = tabs_px() + 14 * ch;
    const int line_top = static_cast<int>(std::clamp<i64>(line_y, host.text_y, host.y1)), line_end = std::min(host.y1, line_top + ch);
    const int below = host.y1 - line_end, above = line_top - host.text_y;
    if (below >= std::min(tall, tabs_px() + 4 * ch) || below >= above) {
        p.y0 = line_end;
        p.y1 = std::min(host.y1, line_end + tall);
    } else {
        p.y1 = line_top;
        p.y0 = std::max(host.text_y, line_top - tall);
    }
    p.x0 = host.x0;
    p.x1 = host.x1;
    p.text_y = std::min(p.y0 + tabs_px(), p.y1);
    p.text_x = std::min(p.x0 + gutter_px(), p.x1);
}

// The box goes, and the keyboard back to where it was asked from. Its file,
// if the box had the only view of it and it has changes, goes to a tab
// there.
void App::peek_close() {
    Pane* p = peek_pane();
    peek_ = 0;
    if (!p) return;
    Pane* host = nullptr;
    for (const std::unique_ptr<Pane>& q : panes_)
        if (q->id == p->host) host = q.get();
    if (!host)
        for (const std::unique_ptr<Pane>& q : panes_)
            if (q.get() != p && !q->floating) host = q.get();
    if (!host) return;
    for (size_t i = p->docs.size(); i-- > 0;)
        if (p->docs[i]->ed.modified() && views_of(p->docs[i].get()) == 1) {
            const bool last = p->docs.size() == 1;
            move_doc(p, i, host, host->tab + 1);
            if (last) p = nullptr;  // (the move let go of the box)
            if (!p) break;
        }
    if (p) {
        Pane* const keep = pane_ == p ? host : pane_;
        remove_pane(p);
        focus_pane(keep);
        pane_ = keep;
    } else {
        focus_pane(host);
    }
    pick_theme();
    dirty_ = true;
}

// Each frame, before the splits are laid out: the box under its line (it
// follows the split as it scrolls), last of the splits (drawn over the
// one it floats on); gone when the keyboard has gone to another split.
void App::peek_layout() {
    Pane* p = peek_pane();
    if (!p) return;
    Pane* host = nullptr;
    for (const std::unique_ptr<Pane>& q : panes_)
        if (q->id == p->host) host = q.get();
    if (!host || (pane_ != p && !find_open_) || !p->doc) {
        peek_close();
        return;
    }
    if (panes_.back().get() != p) {
        auto it = std::ranges::find_if(panes_, [p](const std::unique_ptr<Pane>& q) { return q.get() == p; });
        std::rotate(it, it + 1, panes_.end());
    }
    peek_place(*p, *host);
}

// Esc in the box (outside Insert mode, with Vim): it closes.
bool App::peek_key(const SDL_KeyboardEvent& k) {
    if (k.key != SDLK_ESCAPE || !peek_ || pane_ != peek_pane() || find_open_ || panel_has_keyboard()) return false;
    if (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI | SDL_KMOD_SHIFT)) return false;
    if (cfg_.modal && (vim_.mode() != Vim::kNormal || vim_.pending() || !vim_.showcmd().empty())) return false;
    if (pane_->doc->ed.multi() || snippet_) return false;
    peek_close();
    return true;
}

// For the tests: the box, its file and line, and what is on it; or none.
std::string App::peek_dump() {
    Pane* p = peek_pane();
    if (!p || !p->doc) return "no peek";
    const Editor& ed = p->doc->ed;
    const u64 line = ed.buf.line_of(ed.cursor());
    const u64 b = ed.buf.line_start(line);
    std::string out = std::string(base_name(ed.path)) + ":" + std::to_string(line + 1) + " | " + ed.buf.text(b, ed.line_end(line) - b);
    out += pane_ == p ? " | keyboard" : " | not";
    Pane* host = nullptr;
    for (const std::unique_ptr<Pane>& q : panes_)
        if (q->id == p->host) host = q.get();
    if (host) out += p->y0 >= host->text_y && p->y1 <= host->y1 ? " | inside" : " | outside";
    return out;
}

}  // namespace blit
