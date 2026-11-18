// Soft wrap in the window: whether a split's file wraps (editor.soft_wrap,
// or its language in editor.soft_wrap_languages, or the file's own choice
// with "Toggle soft wrap"), and the split's map (wrap.hpp), kept for the
// split's width, which what goes by rows asks: the view, the cursor, a
// click, the moves up and down, the line numbers.
#include <algorithm>

#include "app.hpp"

namespace blit {

namespace {

constexpr u64 kLargest = 16u << 20;  // (a larger file does not wrap: its map would be made line by line, all of it)

}  // namespace

bool App::wrap_wanted(const Pane& p) const {
    if (!p.doc || p.floating) return p.doc && p.floating && p.doc->wrap_choice == 1;
    const Doc& d = *p.doc;
    if (d.term || d.prof || d.md || d.image || d.pdf || d.marks || d.loading || d.ed.buf.size() > kLargest) return false;
    if (d.wrap_choice >= 0) return d.wrap_choice == 1;
    if (cfg_.soft_wrap) return true;
    if (cfg_.soft_wrap_languages.empty()) return false;
    std::string lang;
    if (!d.lang.empty()) {
        lang = d.lang == "-" ? std::string() : d.lang;
    } else if (!d.ed.path.empty()) {
        char head[128];
        const u64 n = d.ed.buf.read(0, head, sizeof head);
        const std::string_view first(head, static_cast<size_t>(std::min<u64>(n, std::string_view(head, n).find('\n'))));
        lang = Syntax::language_for(d.ed.path, first, cfg_.grammars);
    }
    if (lang.empty()) lang = "text";
    return std::ranges::find(cfg_.soft_wrap_languages, lang) != cfg_.soft_wrap_languages.end();
}

// Before a split is laid out: its map made, or made again for a new width,
// file or folds; or let go of. The editor of the split with the keyboard
// moves up and down by its rows.
void App::wrap_sync(Pane& p) {
    if (!p.doc) return;
    Doc& d = *p.doc;
    if (!wrap_wanted(p)) {
        if (p.wrap) {
            p.wrap.reset();
            if (&p == pane_ || d.ed.skip) d.ed.skip = d.folds.empty() ? nullptr : &d.folds;
            p.cache_rev = ~0ull;  // (the cursor's row: worked out again)
        }
        return;
    }
    const int cw = font_->cell_w();
    const int text = p.x1 - p.text_x - pad_x_ - minimap_px(p) - cw;  // (a column left over: the cursor at a row's end)
    const u32 width = static_cast<u32>(std::max(8 * cw, text) / cw);
    if (!p.wrap) p.wrap = std::make_unique<Wrap>();
    const u64 before = p.wrap->rows();
    p.wrap->set(&d.ed, d.id, d.folds.empty() ? nullptr : &d.folds, d.folds.generation(), width, d.ed.tab_width);
    if (p.wrap->rows() != before) p.cache_rev = ~0ull;
    if (&p == pane_) d.ed.skip = p.wrap.get();
    p.scroll_x = p.target_x = 0;  // (nothing goes past the right edge)
}

const Wrap* App::wrap_on() const {
    const Pane* p = pane_;
    return p && p->wrap && p->doc && p->wrap->doc() == p->doc->id ? p->wrap.get() : nullptr;
}

// "Toggle soft wrap": the file in front wraps, or not, whatever the settings say.
void App::wrap_toggle() {
    Doc& d = *pane_->doc;
    d.wrap_choice = wrap_on() ? 0 : 1;
    wrap_sync(*pane_);
    pane_->cache_rev = ~0ull;
    scroll_to_cursor();
    dirty_ = true;
}

// A document that goes: the maps made for it let go of it first.
void App::wrap_forget(const Doc& d) {
    for (const std::unique_ptr<Pane>& p : panes_)
        if (p->wrap && p->wrap->doc() == d.id) {
            p->wrap.reset();
            p->cache_rev = ~0ull;
        }
}

// For the tests: the split in front's wrap (its width and rows, and the
// rows on show: each line from 1, and the column its row starts at), or off.
std::string App::wrap_dump() {
    const Wrap* w = wrap_on();
    if (!w) return "off";
    std::string out = "width " + std::to_string(w->width()) + ", " + std::to_string(w->rows()) + " rows:";
    const u64 top = static_cast<u64>(std::max(0.0, pane_->scroll_y)) / static_cast<u64>(font_->cell_h());
    for (u64 r = top; r < std::min<u64>(w->rows(), top + 12); ++r) out += " " + std::to_string(w->line_at(r) + 1) + "@" + std::to_string(w->start_col(r));
    return out;
}

}  // namespace blit
