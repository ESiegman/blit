// Soft wrap in the window: whether a split's file wraps (editor.soft_wrap,
// or its language in editor.soft_wrap_languages, or the file's own choice
// with "Toggle soft wrap"), and the split's map (wrap.hpp), kept for the
// split's width, which what goes by rows asks: the view, the cursor, a
// click, the moves up and down, the line numbers.
#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "textbox.hpp"

namespace blit {

namespace {

constexpr u64 kLargest = 16u << 20;  // (a larger file does not wrap: its map would be made line by line, all of it)

}  // namespace

bool App::wrap_wanted(const Pane& p, const Doc* doc) const {
    if (!doc) doc = p.doc;
    if (!doc || p.floating) return doc && p.floating && doc->wrap_choice == 1;
    const Doc& d = *doc;
    if (d.term || d.prof || d.md || d.image || d.pdf || d.wave || d.marks || d.loading || d.ed.buf.size() > kLargest) return false;
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
    if (!p.wrap) p.wrap = std::make_unique<Wrap>();
    const u64 before = p.wrap->rows();
    p.wrap->set(&d.ed, d.id, d.folds.empty() ? nullptr : &d.folds, d.folds.generation(), wrap_width(p), d.ed.tab_width);
    // The inlay hints take room in the rows: given again as the server sends
    // new ones (the map moves them with each edit meanwhile, as the server's
    // own are moved). On the cursor's line too, where they are not drawn, so
    // that a line's rows stay as they are as the cursor comes and goes.
    const std::vector<InlayHint>* hints = cfg_.lsp && cfg_.lsp_inlay_hints && lsp_.can_hint(d.id) ? lsp_.hints(d.id) : nullptr;
    const u64 key = hints ? lsp_.hints_given(d.id) * 0x9E3779B97F4A7C15ull + reinterpret_cast<u64>(hints) : 0;
    if (key != p.wrap->hints_key()) {
        std::vector<Wrap::Hint> h;
        if (hints) {
            h.reserve(hints->size());
            for (const InlayHint& x : *hints) h.push_back({x.off, std::max<u32>(1, width_of(hint_text(x)))});
        }
        // A server answers for the lines near the view only, so its new
        // hints give rows to some lines and take them from others above
        // it: the view stays on the line it was on.
        const double ch = font_->cell_h(), top = p.wrap->top_line(p.scroll_y / ch), target = p.wrap->top_line(p.target_y / ch);
        if (p.wrap->set_hints(std::move(h), key)) {
            p.scroll_y = p.wrap->top_rows(top) * ch;
            p.target_y = p.wrap->top_rows(target) * ch;
            p.cache_rev = ~0ull;
        }
    }
    if (p.wrap->rows() != before) p.cache_rev = ~0ull;
    if (&p == pane_) d.ed.skip = p.wrap.get();
    p.scroll_x = p.target_x = 0;  // (nothing goes past the right edge)
}

// The columns a split's rows hold.
u32 App::wrap_width(const Pane& p) const {
    const int cw = font_->cell_w();
    const int text = p.x1 - p.text_x - pad_x_ - minimap_px(p) - cw;  // (a column left over: the cursor at a row's end)
    return static_cast<u32>(std::max(8 * cw, text) / cw);
}

// A view's top, as a session keeps it: rows of the split's map (its soft
// wrap, or what is folded) as a line and the part through it, and back. A
// file not in front there has no map of its own: it is walked to.
double App::view_line(const Pane& p, const Doc& d, double rows) const {
    rows = std::max(0.0, rows);
    const Folds* f = d.folds.empty() ? nullptr : &d.folds;
    if (f) f->sync(d.ed.buf, d.ed.revision());
    if (&d == p.doc && p.wrap && p.wrap->doc() == d.id) return p.wrap->top_line(rows);
    if (&d != p.doc && wrap_wanted(p, &d)) return Wrap::walk_line(d.ed, f, wrap_width(p), d.ed.tab_width, rows);
    if (!f) return rows;
    const double row = std::floor(rows);
    return static_cast<double>(f->line_at(static_cast<u64>(row))) + (rows - row);
}

double App::view_rows(const Pane& p, const Doc& d, double line) const {
    const u64 lines = d.ed.buf.line_count();
    line = std::clamp(line, 0.0, static_cast<double>(lines > 0 ? lines - 1 : 0) + 0.9999);
    const Folds* f = d.folds.empty() ? nullptr : &d.folds;
    if (f) f->sync(d.ed.buf, d.ed.revision());
    if (&d == p.doc && p.wrap && p.wrap->doc() == d.id) return p.wrap->top_rows(line);
    if (&d != p.doc && wrap_wanted(p, &d)) return Wrap::walk_rows(d.ed, f, wrap_width(p), d.ed.tab_width, line);
    if (!f) return line;
    const double whole = std::floor(line);
    const u64 l = static_cast<u64>(whole);
    return static_cast<double>(f->row_of(l)) + (f->hidden(l) ? 0 : line - whole);
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
    if (d.wrap_choice == 1 && !wrap_on() && !d.loading) {  // (a file on its way wraps once it is here)
        d.wrap_choice = -1;
        return notify(d.ed.buf.size() > kLargest ? "A file larger than 16 MB does not wrap" : "This tab does not wrap");
    }
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
