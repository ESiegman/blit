// The language server's side of the editor: squiggles under problems, the
// completion list, hover and signature popups, go to definition.
//
// Everything here reacts to results the client has already parsed (see
// lsp.hpp). Nothing waits for the server.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "app.hpp"
#include "remote.hpp"
#include "textbox.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

bool word_byte(u8 b) { return std::isalnum(b) || b == '_' || b >= 0x80; }

// One letter for a completion's kind, and the ink it is drawn in.
struct KindTag {
    char letter;
    u32 ink;
};

KindTag kind_tag(u8 kind) {
    const u32 fn = kInkDiagnostic + 2, type = kInkDiagnostic + 1, var = kInkDiagnostic + 3, key = kInkAccent;
    switch (kind) {
        case 2: return {'m', fn};     // method
        case 3: return {'f', fn};     // function
        case 4: return {'c', fn};     // constructor
        case 5: return {'d', var};    // field
        case 6: return {'v', var};    // variable
        case 7: return {'C', type};   // class
        case 8: return {'I', type};   // interface
        case 9: return {'M', type};   // module
        case 10: return {'p', var};   // property
        case 13: return {'E', type};  // enum
        case 14: return {'k', key};   // keyword
        case 15: return {'s', key};   // snippet
        case 17: return {'F', kInkMuted};  // file
        case 20: return {'e', var};   // enum member
        case 21: return {'K', var};   // constant
        case 22: return {'S', type};  // struct
        case 25: return {'T', type};  // type parameter
        default: return {' ', kInkMuted};
    }
}

// A line of popup text with how to draw it.
struct Line {
    std::string text;
    u32 ink = 0;
    u32 style = 0;      // Font::kBold, ...
    bool rule = false;  // a horizontal line instead of text
    // A stretch to draw differently (the argument being typed).
    u32 mark0 = 0, mark1 = 0, mark_ink = 0, mark_style = 0;
};

// Breaks lines that are wider than `width`, at spaces where there are any.
// At most max_rows lines come back; a last line of "..." says there was more.
std::vector<Line> wrap(const std::vector<Line>& in, u32 width, size_t max_rows) {
    std::vector<Line> out;
    for (const Line& l : in) {
        if (l.rule || width_of(l.text) <= width) {
            out.push_back(l);
            continue;
        }
        // Continuation lines keep the indentation of the first.
        const size_t indent = std::min<size_t>(l.text.find_first_not_of(' '), width / 2);
        std::string_view rest = l.text;
        bool first = true;
        while (!rest.empty()) {
            const u32 room = first ? width : width - static_cast<u32>(indent);
            // The longest prefix that fits, in bytes.
            size_t fit = 0, last_space = 0;
            u32 w = 0;
            const u8* p = reinterpret_cast<const u8*>(rest.data());
            while (fit < rest.size()) {
                const Decoded d = decode_utf8(p + fit, rest.size() - fit);
                const u32 cw = cp_width(d.cp);
                if (w + cw > room) break;
                if (rest[fit] == ' ') last_space = fit;
                w += cw;
                fit += d.len;
            }
            if (fit < rest.size() && last_space > room / 3) fit = last_space;
            if (fit == 0) fit = rest.size();  // a cell too narrow for even one character
            Line piece = l;
            piece.mark0 = piece.mark1 = 0;
            piece.text = (first ? std::string() : std::string(indent, ' ')) + std::string(rest.substr(0, fit));
            out.push_back(std::move(piece));
            rest.remove_prefix(fit);
            while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
            first = false;
        }
    }
    if (out.size() > max_rows) {
        out.resize(max_rows);
        out.back() = Line{"...", kInkMuted, 0, false, 0, 0, 0, 0};
    }
    return out;
}

// Lays out lines, one per row, and sizes the box to fit them.
void set_lines(TextBox& box, Font& font, const std::vector<Line>& lines, u32 min_cols = 1) {
    u32 widest = min_cols;
    for (const Line& l : lines) widest = std::max(widest, width_of(l.text));
    box.resize(widest, static_cast<u32>(lines.size()));
    for (u32 r = 0; r < box.rows; ++r) {
        const Line& l = lines[r];
        if (l.rule) {
            for (u32 c = 0; c < box.cols; ++c) box.put(font, r, c, "\xE2\x94\x80", kInkMuted);  // a box-drawing dash
        } else if (l.mark1 > l.mark0 && l.mark1 <= l.text.size()) {
            u32 c = box.put(font, r, 0, std::string_view(l.text).substr(0, l.mark0), l.ink, l.style);
            c = box.put(font, r, c, std::string_view(l.text).substr(l.mark0, l.mark1 - l.mark0), l.mark_ink, l.mark_style);
            box.put(font, r, c, std::string_view(l.text).substr(l.mark1), l.ink, l.style);
        } else {
            box.put(font, r, 0, l.text, l.ink, l.style);
        }
    }
}

const char* severity_word(u8 s) {
    return s == 1 ? "error" : s == 2 ? "warning" : s == 3 ? "info" : "hint";
}

}  // namespace

struct App::Assist {
    // The completion list.
    bool comp_open = false, comp_pending = false;
    u64 comp_anchor = 0;  // where the word being completed starts
    char comp_trigger = 0;
    struct Shown {
        u32 item;
        int score;
    };
    std::vector<Shown> shown;  // items that match what is typed, best first
    // For each problem, the furthest any up to it reaches (see lsp_decorate()).
    std::vector<u64> reach;
    u64 reach_gen = ~0ull;
    // What `shown` was made for: a longer word typed onto that one can only
    // match items that are there.
    std::string comp_prefix;
    u64 comp_serial = ~0ull, comp_from = 0;
    u32 sel = 0, top = 0;
    TextBox comp_box, doc_box;
    SDL_Rect comp_rect{};
    u32 comp_rows = 0;

    // The hover popup: problems at a spot, then what the server says about it.
    bool hover_open = false, hover_pending = false, hover_at_cursor = false;
    u64 hover_off = 0, hover_w0 = 0, hover_w1 = 0;
    std::vector<Line> hover_lines;
    TextBox hover_box;
    SDL_Rect hover_rect{};
    float mouse_x = -1, mouse_y = -1;
    i64 mouse_line = -1, mouse_col = -1;
    u64 hover_due_ns = 0;

    // Signature help.
    bool sig_open = false;
    u64 sig_line = 0;
    TextBox sig_box;

    // Squiggles and the message at the end of the cursor's line, as last
    // worked out, and what they were worked out for.
    std::vector<GridDeco> decos;
    std::vector<GridNote> notes;
    std::vector<u32> note_cells;
    u64 deco_gen = ~0ull, deco_top = ~0ull, deco_rev = ~0ull, deco_line = ~0ull;
    u32 deco_rows = 0, deco_font = ~0u;

    // Servers that would not start, or died, since the settings were last
    // read. They are not tried again; the next one for the language is.
    std::vector<std::string> failed;

    // Go to definition: where to land once another file has opened, and
    // the places to go back to.
    struct Place {
        std::string path;
        u64 off = 0;
    };
    std::vector<Place> back;
};

App::App() : assist_(std::make_unique<Assist>()) {
    splits_create();
    git_create();
    vim_create();
    fx_create();
    find_create();
    tabs_create();
    panel_create();
}
// (Git goes before the documents do, and by then its worker has stopped: no
// document needs to tell it anything on the way out.)
App::~App() {
    for (const std::unique_ptr<Pane>& p : panes_)
        for (const std::unique_ptr<Doc>& d : p->docs) d->git = nullptr;
}

// ---------------------------------------------------------------- lifecycle

// Looks for a language server for the open file and starts it. Returns at
// once; the server reports in through sync_lsp().
void App::lsp_forget_failed(const std::string& suffix) {
    if (assist_) std::erase_if(assist_->failed, [&](const std::string& n) { return n.ends_with(suffix); });
}

void App::start_lsp() {
    if (lsp_.attached() && !lsp_.idle()) return;
    // (A server kept running across a change of tab is let go of here if
    // the new tab has no use for it.)
    if (!cfg_.lsp || pane_->doc->ed.path.empty() || static_cast<double>(pane_->doc->ed.buf.size()) > static_cast<double>(cfg_.lsp_max_mb) * 1048576.0)
        return lsp_.detach();
    char head[128];
    const u64 n = pane_->doc->ed.buf.read(0, head, sizeof head);
    const std::string_view first(head, static_cast<size_t>(std::min<u64>(n, std::string_view(head, n).find('\n'))));
    lsp_.set_notify([] {  // on the client's thread: wake the event loop
        SDL_Event ev{};
        ev.type = SDL_EVENT_USER;
        ev.user.code = 4;
        SDL_PushEvent(&ev);
    });
    std::vector<LspServer> servers = lsp_server_list(cfg_.lsp_servers);
    std::string host, there;
    const bool remote = split_remote(pane_->doc->ed.path, &host, &there);
    std::erase_if(servers, [&](const LspServer& s) {
        const std::string name = remote ? s.name + "@" + host : s.name;  // (another machine's are its own)
        return std::find(assist_->failed.begin(), assist_->failed.end(), name) != assist_->failed.end();
    });
    if (lsp_.attach(pane_->doc->ed.buf, pane_->doc->ed.path, Syntax::language_for(pane_->doc->ed.path, first, cfg_.grammars), servers)) {
        lsp_doc_ = pane_->doc;
        lsp_doc_->ed.listen(&lsp_);
    }
}

// Must run before the buffer is replaced.
void App::stop_lsp(bool keep_server) {
    Assist& a = *assist_;
    if (lsp_doc_) lsp_doc_->ed.unlisten(&lsp_);
    lsp_doc_ = nullptr;
    if (keep_server) lsp_.release();
    else lsp_.detach();
    lsp_close(kPopAll);
    a.decos.clear();
    a.notes.clear();
    a.deco_gen = ~0ull;
    lsp_pending_ = false;
}

void App::lsp_close(u32 what) {
    Assist& a = *assist_;
    if ((what & kPopCompletion) && (a.comp_open || a.comp_pending)) {
        a.comp_open = a.comp_pending = false;
        dirty_ = true;
    }
    if ((what & kPopHover) && (a.hover_open || a.hover_pending || a.hover_due_ns)) {
        a.hover_open = a.hover_pending = false;
        a.hover_due_ns = 0;
        dirty_ = true;
    }
    if ((what & kPopSignature) && a.sig_open) {
        a.sig_open = false;
        dirty_ = true;
    }
}

// Takes in what the server has sent since last time.
void App::sync_lsp() {
    Assist& a = *assist_;
    if (!lsp_.attached()) return;
    const u32 news = lsp_.poll();
    if (!news) return;
    if (news & Lsp::kState) {
        const std::string msg = lsp_.take_message();
        if (!msg.empty()) std::fprintf(stderr, "blit: %s\n", msg.c_str());
        if (!lsp_.attached()) {
            pane_->doc->ed.unlisten(&lsp_);
            lsp_close(kPopAll);
            // That server is out; another one for the language may do.
            a.failed.push_back(lsp_.server_name());
            lsp_pending_ = true;
            dirty_ = true;
        }
    }
    if (news & Lsp::kDiagnostics) dirty_ = true;
    if ((news & Lsp::kCompletion) && a.comp_pending) {
        a.comp_pending = false;
        if (!filter_completions()) lsp_close(kPopCompletion);
        dirty_ = true;
    }
    if ((news & Lsp::kHover) && a.hover_pending && lsp_.hover_offset() == a.hover_off) {
        a.hover_pending = false;
        const std::vector<InfoLine>& lines = lsp_.hover_lines();
        if (!lines.empty()) {
            if (!a.hover_lines.empty()) a.hover_lines.push_back({"", 0, 0, true, 0, 0, 0, 0});
            for (const InfoLine& l : lines) {
                Line out;
                out.text = l.text;
                out.rule = l.kind == InfoLine::kRule;
                if (l.kind == InfoLine::kHeading) {
                    out.ink = kInkAccent;
                    out.style = Font::kBold;
                }
                a.hover_lines.push_back(std::move(out));
            }
            a.hover_open = true;
            dirty_ = true;
        }
    }
    if (news & Lsp::kSignature) {
        const bool open = !lsp_.signature_info().empty();
        if (open || a.sig_open) dirty_ = true;
        a.sig_open = open;
        if (open) a.sig_line = pane_->doc->ed.buf.line_of(pane_->doc->ed.cursor());
    }
    if (news & Lsp::kDefinition) goto_definition();
}

// ---------------------------------------------------------------- completion

void App::request_completion(char trigger) {
    Assist& a = *assist_;
    if (!lsp_.can_complete()) return;
    const u64 cur = pane_->doc->ed.cursor();
    u64 start = cur;
    if (!trigger) {
        char b;
        while (start > 0 && cur - start < 200 && pane_->doc->ed.buf.read(start - 1, &b, 1) == 1 && word_byte(static_cast<u8>(b))) --start;
    }
    a.comp_anchor = start;
    a.comp_trigger = trigger;
    a.comp_pending = true;
    a.comp_open = false;
    lsp_.complete(cur, trigger);
}

// Narrows the list to what matches the word typed so far. False if the
// list has nothing left to offer (or the cursor has left the word).
bool App::filter_completions() {
    Assist& a = *assist_;
    const u64 cur = pane_->doc->ed.cursor();
    const u64 was = a.comp_serial;
    a.comp_serial = ~0ull;  // (until `shown` is made again below)
    if (cur < a.comp_anchor || cur - a.comp_anchor > 200) return false;
    const std::string prefix = pane_->doc->ed.buf.text(a.comp_anchor, cur - a.comp_anchor);
    for (const char c : prefix)
        if (!word_byte(static_cast<u8>(c))) return false;
    const std::vector<CompletionItem>& items = lsp_.completions();
    // A letter typed onto the word: only what matched before can match now.
    // (Matching is by letters in order, so what has all of the longer word's has all of the shorter's.)
    const bool narrower = a.comp_from == a.comp_anchor && prefix.size() > a.comp_prefix.size() && prefix.starts_with(a.comp_prefix);
    if (narrower && was == lsp_.completions_serial()) {
        size_t keep = 0;
        for (const Assist::Shown& x : a.shown)
            if (const int score = lsp_match(prefix, items[x.item].filter); score >= 0) a.shown[keep++] = {x.item, score};
        a.shown.resize(keep);
    } else {
        a.shown.clear();
        for (u32 i = 0; i < items.size(); ++i) {
            const int score = lsp_match(prefix, items[i].filter);
            if (score >= 0) a.shown.push_back({i, score});
        }
    }
    // The best first; of equals, the server's order.
    if (!prefix.empty())
        std::sort(a.shown.begin(), a.shown.end(), [](const Assist::Shown& x, const Assist::Shown& y) { return x.score != y.score ? x.score > y.score : x.item < y.item; });
    a.comp_prefix = prefix;
    a.comp_from = a.comp_anchor;
    a.comp_serial = lsp_.completions_serial();
    // One candidate that is already typed out is nothing to offer.
    if (a.shown.empty() || (a.shown.size() == 1 && items[a.shown[0].item].insert == prefix)) return false;
    a.sel = a.top = 0;
    a.comp_open = true;
    return true;
}

void App::accept_completion() {
    Assist& a = *assist_;
    if (!a.comp_open || a.sel >= a.shown.size()) return;
    const CompletionItem item = lsp_.completions()[a.shown[a.sel].item];  // a copy: the edit below invalidates the list
    lsp_close(kPopCompletion);
    const u64 cur = pane_->doc->ed.cursor();
    u64 from = item.replace_from != ~0ull ? std::min(item.replace_from, cur) : a.comp_anchor;
    if (from > cur) from = cur;
    pane_->doc->ed.replace(from, cur, item.insert);
    // Edits elsewhere (an import, say), last first so earlier ones stay put.
    if (!item.extra.empty()) {
        u64 end = pane_->doc->ed.cursor();
        std::vector<CompletionItem::Extra> extra = item.extra;
        std::sort(extra.begin(), extra.end(), [](const auto& x, const auto& y) { return x.start > y.start; });
        for (const CompletionItem::Extra& x : extra) {
            if (x.end > from || x.start > x.end) continue;  // not safely before what was just typed
            pane_->doc->ed.replace(x.start, x.end, x.text);
            end = end + x.text.size() - (x.end - x.start);
        }
        pane_->doc->ed.move_to(end, false);
    }
    scroll_to_cursor();
}

// ---------------------------------------------------------------- hover

// Opens the popup for the text at `off`: the problems reported there right
// away, and the server's description when it arrives.
void App::show_hover(u64 off, bool at_cursor) {
    Assist& a = *assist_;
    u64 w0 = off, w1 = off;
    char b;
    while (w0 > 0 && off - w0 < 200 && pane_->doc->ed.buf.read(w0 - 1, &b, 1) == 1 && word_byte(static_cast<u8>(b))) --w0;
    while (w1 < pane_->doc->ed.buf.size() && w1 - off < 200 && pane_->doc->ed.buf.read(w1, &b, 1) == 1 && word_byte(static_cast<u8>(b))) ++w1;
    a.hover_lines.clear();
    a.hover_open = a.hover_pending = false;
    for (const Diagnostic& d : lsp_.diagnostics()) {
        if (d.start > std::max(off, w1) || std::max(d.end, d.start + 1) <= std::min(off, w0)) continue;
        if (!a.hover_lines.empty()) a.hover_lines.push_back({"", 0, 0, true, 0, 0, 0, 0});
        const u32 ink = kInkDiagnostic + d.severity - 1;
        std::string_view rest = d.message;
        bool first = true;
        while (!rest.empty()) {
            const size_t nl = rest.find('\n');
            Line l;
            l.text = (first ? std::string(severity_word(d.severity)) + ": " : std::string()) + std::string(rest.substr(0, nl));
            l.ink = ink;
            a.hover_lines.push_back(std::move(l));
            rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
            first = false;
        }
        if (!d.source.empty() || !d.code.empty()) {
            Line l;
            l.text = d.source + (d.source.empty() || d.code.empty() ? "" : " ") + d.code;
            l.ink = kInkMuted;
            a.hover_lines.push_back(std::move(l));
        }
    }
    a.hover_off = off;
    a.hover_w0 = w0;
    a.hover_w1 = w1;
    a.hover_at_cursor = at_cursor;
    a.hover_open = !a.hover_lines.empty();
    if (lsp_.can_hover() && w1 > w0) {
        lsp_.hover(off);
        a.hover_pending = true;
    }
    if (a.hover_open) dirty_ = true;
}

// Milliseconds until the mouse has rested long enough for a hover, or -1.
int App::lsp_wait_ms() const {
    const Assist& a = *assist_;
    if (!a.hover_due_ns) return -1;
    const u64 now = now_ns();
    return a.hover_due_ns <= now ? 0 : static_cast<int>((a.hover_due_ns - now) / 1'000'000) + 1;
}

void App::lsp_tick() {
    Assist& a = *assist_;
    if (!a.hover_due_ns || now_ns() < a.hover_due_ns) return;
    a.hover_due_ns = 0;
    if (!lsp_.ready() || a.comp_open || dragging_ || a.mouse_line < 0) return;
    if (static_cast<u64>(a.mouse_line) >= pane_->doc->ed.buf.line_count()) return;
    const u64 line = static_cast<u64>(a.mouse_line);
    const u64 start = pane_->doc->ed.buf.line_start(line), end = pane_->doc->ed.line_end(line);
    // Past the end of the line there is nothing to describe, except a
    // problem reported right at the end.
    const u32 width = pane_->doc->ed.col_between(start, end);
    if (static_cast<u64>(a.mouse_col) > width) return;
    show_hover(pane_->doc->ed.off_at(line, static_cast<u32>(a.mouse_col)), false);
}

void App::lsp_mouse(float x, float y) {
    Assist& a = *assist_;
    if (!lsp_.ready()) return;
    a.mouse_x = x;
    a.mouse_y = y;
    const int cw = font_->cell_w(), ch = font_->cell_h();
    const double px = x * density_, py = y * density_;
    const double tx = px - pane_->text_x + std::llround(pane_->scroll_x) - pad_x_, ty = py - pane_->text_y + std::llround(pane_->scroll_y);
    const i64 line = ty < 0 ? -1 : static_cast<i64>(ty / ch), col = tx < 0 ? -1 : static_cast<i64>(tx / cw);
    if (line == a.mouse_line && col == a.mouse_col) return;
    a.mouse_line = line;
    a.mouse_col = col;
    if (a.hover_open && !a.hover_at_cursor) {
        const bool in_popup = px >= a.hover_rect.x && px < a.hover_rect.x + a.hover_rect.w && py >= a.hover_rect.y &&
                              py < a.hover_rect.y + a.hover_rect.h;
        bool on_word = false;
        if (line >= 0 && static_cast<u64>(line) < pane_->doc->ed.buf.line_count() && col >= 0) {
            const u64 off = pane_->doc->ed.off_at(static_cast<u64>(line), static_cast<u32>(col));
            on_word = off >= a.hover_w0 && off <= a.hover_w1 && pane_->doc->ed.buf.line_of(a.hover_off) == static_cast<u64>(line);
        }
        if (!in_popup && !on_word) lsp_close(kPopHover);
        else return;
    }
    a.hover_pending = false;
    a.hover_due_ns = line >= 0 && col >= 0 ? now_ns() + static_cast<u64>(cfg_.lsp_hover_ms * 1e6f) : 0;
}

// ---------------------------------------------------------------- definition

void App::goto_definition() {
    Assist& a = *assist_;
    const LspLocation loc = lsp_.definition_target();
    if (!loc.valid()) return;
    const bool utf16 = lsp_.counts_utf16();
    const Assist::Place here{lsp_.path(), pane_->doc->ed.cursor()};
    if (loc.path != lsp_.path() && !open_path(loc.path.c_str())) return;  // in another file, which gets a tab
    a.back.push_back(here);
    if (a.back.size() > 64) a.back.erase(a.back.begin());
    if (pane_->doc->loading) {  // (on its way from another machine: the place, when it is here; near enough for a line with wide letters)
        pane_->doc->goto_line = loc.line;
        pane_->doc->goto_col = loc.character;
        return;
    }
    pane_->doc->ed.move_to(Lsp::offset_at(pane_->doc->ed.buf, loc.line, loc.character, utf16), false);
    scroll_to_cursor();
}

// Notes where the cursor is, as somewhere go_back() returns to (before
// going to a file or a line that was searched for).
void App::remember_place() {
    Assist& a = *assist_;
    if (pane_->doc->ed.path.empty()) return;
    a.back.push_back({lsp_.attached() ? lsp_.path() : pane_->doc->ed.path, pane_->doc->ed.cursor()});
    if (a.back.size() > 64) a.back.erase(a.back.begin());
}

void App::go_back() {
    Assist& a = *assist_;
    if (a.back.empty()) return;
    const Assist::Place place = a.back.back();
    a.back.pop_back();
    if (place.path != (lsp_.attached() ? lsp_.path() : pane_->doc->ed.path) && !open_path(place.path.c_str())) return;
    pane_->doc->ed.move_to(std::min<u64>(place.off, pane_->doc->ed.buf.size()), false);
    scroll_to_cursor();
}

// ---------------------------------------------------------------- input

// After typed text has gone in.
void App::lsp_typed(std::string_view text) {
    Assist& a = *assist_;
    if (!lsp_.ready() || text.empty()) return;
    lsp_close(kPopHover);
    const char c = text.back();
    const bool ident = word_byte(static_cast<u8>(c));
    if (a.comp_open) {
        if (!ident || !filter_completions()) lsp_close(kPopCompletion);
        else if (lsp_.completions_incomplete()) request_completion(0), a.comp_open = true;  // keep showing; a fuller list is coming
        dirty_ = true;
    } else if (a.comp_pending && !ident) {
        lsp_close(kPopCompletion);
    }
    if (!a.comp_open && !a.comp_pending && cfg_.lsp_completion) {
        if (text.size() == 1 && lsp_.completion_trigger(c)) request_completion(c);
        else if (ident) request_completion(0);
    }
    if (text.size() == 1 && lsp_.signature_trigger(c)) {
        lsp_.signature(pane_->doc->ed.cursor());
    } else if (a.sig_open) {
        if (c == ')') lsp_close(kPopSignature);
        else lsp_.signature(pane_->doc->ed.cursor());
    }
}

// A key press, before the editor sees it. True if it was for a popup or a
// language-server command.
bool App::lsp_key(const SDL_KeyboardEvent& k) {
    Assist& a = *assist_;
    const bool ctrl = k.mod & SDL_KMOD_CTRL, alt = k.mod & SDL_KMOD_ALT, shift = k.mod & SDL_KMOD_SHIFT;
    const bool cmd = k.mod & kPrimaryMod;
    if (k.key == SDLK_ESCAPE && !alt) {
        if (!(a.comp_open || a.comp_pending || a.hover_open || a.sig_open)) return false;
        lsp_close(kPopAll);
        return true;
    }
    if (a.comp_open) {
        const u32 n = static_cast<u32>(a.shown.size()), page = std::max<u32>(1, a.comp_rows);
        auto move = [&](i64 by) {
            const i64 to = static_cast<i64>(a.sel) + by;
            // One step past either end wraps round; a page stops at the end.
            a.sel = by == 1 && to >= n ? 0 : by == -1 && to < 0 ? n - 1 : static_cast<u32>(std::clamp<i64>(to, 0, n - 1));
            dirty_ = true;
        };
        switch (k.key) {
            case SDLK_DOWN: move(1); return true;
            case SDLK_UP: move(-1); return true;
            case SDLK_PAGEDOWN: move(page); return true;
            case SDLK_PAGEUP: move(-static_cast<i64>(page)); return true;
            case SDLK_N:
                if (ctrl) return move(1), true;
                break;
            case SDLK_P:
                if (ctrl) return move(-1), true;
                break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_TAB: accept_completion(); return true;
            default: break;
        }
    }
    if (!lsp_.ready()) return false;
    // Ctrl+Space, or Option+Escape as macOS has it: complete here.
    if ((ctrl && k.key == SDLK_SPACE && !shift) || (alt && k.key == SDLK_ESCAPE)) {
        request_completion(0);
        return true;
    }
    // (Ctrl-S in Insert mode is Neovim's key for it, where Ctrl-S is not "save".)
    const bool vim_insert = cfg_.modal && kPrimaryMod != SDL_KMOD_CTRL && (vim_.mode() == Vim::kInsert || vim_.mode() == Vim::kReplace);
    if ((ctrl && shift && k.key == SDLK_SPACE) || (vim_insert && ctrl && !shift && !alt && k.key == SDLK_S)) {
        lsp_.signature(pane_->doc->ed.cursor());
        return true;
    }
    if (cmd && k.key == SDLK_I) {
        if (a.hover_open && a.hover_at_cursor) lsp_close(kPopHover);
        else show_hover(pane_->doc->ed.cursor(), true);
        return true;
    }
    if (k.key == SDLK_F12) {
        lsp_.definition(pane_->doc->ed.cursor());
        return true;
    }
    // Back to where the last jump came from: Ctrl+- on macOS, Alt+Left
    // elsewhere.
#if defined(__APPLE__)
    const bool back = ctrl && k.key == SDLK_MINUS;
#else
    const bool back = alt && k.key == SDLK_LEFT;
#endif
    if (back && !assist_->back.empty()) {
        go_back();
        return true;
    }
    return false;
}

// After a key that moved the cursor or changed the text (other than typing).
void App::lsp_after_key(SDL_Keycode key) {
    Assist& a = *assist_;
    if (!lsp_.attached()) return;
    lsp_close(kPopHover);
    if (a.comp_open || a.comp_pending) {
        const u64 cur = pane_->doc->ed.cursor();
        const bool still = key == SDLK_BACKSPACE && cur >= a.comp_anchor && (cur > a.comp_anchor || a.comp_trigger);
        if (!still || (a.comp_open && !filter_completions())) lsp_close(kPopCompletion);
        dirty_ = true;
    }
    if (a.sig_open) {
        if (pane_->doc->ed.buf.line_of(pane_->doc->ed.cursor()) != a.sig_line) lsp_close(kPopSignature);
        else lsp_.signature(pane_->doc->ed.cursor());
    }
}

// A mouse press. True if it was used up here: a click on the completion
// list picks from it, and Cmd+click goes to the definition.
bool App::lsp_click(float x, float y, bool cmd) {
    Assist& a = *assist_;
    const int px = static_cast<int>(x * density_), py = static_cast<int>(y * density_);
    if (a.comp_open && px >= a.comp_rect.x && px < a.comp_rect.x + a.comp_rect.w && py >= a.comp_rect.y &&
        py < a.comp_rect.y + a.comp_rect.h) {
        const int row = (py - a.comp_rect.y - font_->cell_h() / 4) / font_->cell_h();
        if (row >= 0 && a.top + static_cast<u32>(row) < a.shown.size()) {
            a.sel = a.top + static_cast<u32>(row);
            accept_completion();
        }
        return true;
    }
    lsp_close(kPopAll);
    if (cmd && lsp_.can_define()) {
        lsp_.definition(offset_at_point(x, y));
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- drawing

// Works out the squiggles for the visible lines and the message for the
// cursor's line. Redone only when the text, the view or the diagnostics
// change.
void App::lsp_decorate(const GridSpec& g) {
    Assist& a = *assist_;
    const std::vector<Diagnostic>& diags = lsp_.diagnostics();
    if (diags.empty()) {
        a.decos.clear();
        a.notes.clear();
        a.deco_gen = ~0ull;
        return;
    }
    if (a.deco_gen == lsp_.diagnostics_generation() && a.deco_top == g.top_line && a.deco_rows == g.rows &&
        a.deco_rev == pane_->doc->ed.revision() && a.deco_line == pane_->cur_line && a.deco_font == font_->generation())
        return;
    a.deco_gen = lsp_.diagnostics_generation();
    a.deco_top = g.top_line;
    a.deco_rows = g.rows;
    a.deco_rev = pane_->doc->ed.revision();
    a.deco_line = pane_->cur_line;
    a.deco_font = font_->generation();
    a.decos.clear();
    a.notes.clear();
    a.note_cells.clear();

    const Buffer& b = pane_->doc->ed.buf;
    const u64 lines = b.line_count();
    const u64 first = g.top_line, last = std::min<u64>(lines, g.top_line + g.rows);  // [first, last)
    if (first >= last) return;
    const u64 lo = b.line_start(first), hi = last >= lines ? b.size() + 1 : b.line_start(last);
    // The first problem that reaches into view: the furthest any reaches,
    // of those up to each one, only grows, so it can be searched for.
    if (a.reach_gen != lsp_.diagnostics_generation() || a.reach.size() != diags.size()) {
        a.reach_gen = lsp_.diagnostics_generation();
        a.reach.resize(diags.size());
        u64 far = 0;
        for (size_t i = 0; i < diags.size(); ++i) a.reach[i] = far = std::max(far, std::max(diags[i].end, diags[i].start + 1));
    }
    const size_t from = static_cast<size_t>(std::upper_bound(a.reach.begin(), a.reach.end(), lo) - a.reach.begin());
    const Diagnostic* note = nullptr;
    for (size_t i = from; i < diags.size(); ++i) {
        const Diagnostic& d = diags[i];
        if (d.start >= hi) break;  // sorted by start
        if (std::max(d.end, d.start + 1) <= lo) continue;
        const u64 l0 = std::max(first, b.line_of(d.start)), l1 = std::min(last - 1, b.line_of(d.end));
        for (u64 line = l0; line <= l1; ++line) {
            const u64 ls = b.line_start(line);
            GridDeco deco;
            deco.line = line;
            deco.kind = d.severity;
            deco.col0 = d.start > ls ? pane_->doc->ed.col_between(ls, d.start) : 0;
            deco.col1 = b.line_of(d.end) == line ? pane_->doc->ed.col_between(ls, d.end) : pane_->doc->ed.col_between(ls, pane_->doc->ed.line_end(line));
            if (deco.col1 <= deco.col0) deco.col1 = deco.col0 + 1;  // a problem at a point still shows
            a.decos.push_back(deco);
        }
        // The cursor's line shows its most serious problem in words.
        if (cfg_.lsp_inline && b.line_of(d.start) <= pane_->cur_line && b.line_of(d.end) >= pane_->cur_line && pane_->cur_line >= first && pane_->cur_line < last &&
            (!note || d.severity < note->severity))
            note = &d;
    }
    if (note) {
        TextBox box;
        const std::string_view first_line = std::string_view(note->message).substr(0, note->message.find('\n'));
        box.resize(std::min<u32>(width_of(first_line), 300), 1);
        box.put(*font_, 0, 0, first_line, kInkDiagnostic + note->severity - 1);
        a.note_cells = std::move(box.cells);
        const u64 ls = b.line_start(pane_->cur_line);
        a.notes.push_back({pane_->cur_line, pane_->doc->ed.col_between(ls, pane_->doc->ed.line_end(pane_->cur_line)) + 2, a.note_cells.data(),
                           static_cast<u32>(a.note_cells.size())});
    }
}

// Adds the language server's part of a frame: underlines, and whichever
// popups are open.
void App::lsp_layout(int w, int h, FrameDesc& fd) {
    Assist& a = *assist_;
    popups_drawn_ = 0;
    if (!lsp_.attached()) return;
    // The wave under a problem: about a tenth of a line high.
    const float amp = std::max(1.0f, std::round(1.1f * scale_)), thick = std::max(1.0f, 0.75f * scale_);
    fd.squiggle[0] = 4 * amp;
    fd.squiggle[1] = amp;
    fd.squiggle[2] = thick;
    fd.squiggle[3] = amp + thick * 0.5f + 0.5f;
    // Problems show in the split that has the keyboard: its file is the one
    // the language server is following.
    if (focus_view_ < fd.view_count) {
        GridSpec& grid = fd.views[focus_view_].grid;
        lsp_decorate(grid);
        grid.decos = a.decos;
        grid.notes = a.notes;
    }

    const int cw = font_->cell_w(), ch = font_->cell_h();
    const i64 sx = std::llround(pane_->scroll_x), sy = std::llround(pane_->scroll_y);
    const int pad_x = cw / 2 + 1, pad_y = ch / 4, hair = std::max(1, static_cast<int>(std::lround(scale_)));
    const float border = static_cast<float>(hair);
    // (Boxes with round corners and shadows want a little air between them and what they belong to.)
    const int gap = look().box > 0 ? static_cast<int>(std::lround(3 * scale_)) : hair;
    // Pixel position of the top-left of the character cell at an offset.
    auto at_offset = [&](u64 off, int* x, int* y) {
        const u64 line = pane_->doc->ed.buf.line_of(off);
        *x = static_cast<int>(pane_->text_x + pad_x_ + static_cast<i64>(pane_->doc->ed.col_between(pane_->doc->ed.buf.line_start(line), off)) * cw - sx);
        *y = static_cast<int>(pane_->text_y + static_cast<i64>(line) * ch - sy);
    };
    // Places a box so that its text starts at x and it sits just below the
    // line at y, or just above when `above` is asked for or there is no
    // room below. Returns false if the line is not on screen.
    auto place = [&](const TextBox& box, int x, int y, bool above, SDL_Rect* rect, int kind) -> Popup* {
        if (fd.popup_count >= std::size(fd.popups) || box.cols == 0 || box.rows == 0) return nullptr;
        if (y + ch <= 0 || y >= h) return nullptr;
        Popup& p = fd.popups[fd.popup_count];
        p.cols = box.cols;
        p.rows = box.rows;
        p.cells = box.cells.data();
        p.pad_x = pad_x;
        p.pad_y = pad_y;
        p.w = static_cast<int>(box.cols) * cw + 2 * pad_x;
        p.h = static_cast<int>(box.rows) * ch + 2 * pad_y;
        p.border_px = border;
        const int below_y = y + ch + gap, above_y = y - p.h - gap;
        const bool fits_below = below_y + p.h <= h, fits_above = above_y >= 0;
        p.y = above ? (fits_above || !fits_below ? above_y : below_y) : (fits_below || !fits_above ? below_y : above_y);
        p.x = std::clamp(x - pad_x, std::min(pane_->text_x, std::max(0, w - p.w)), std::max(0, w - p.w));
        if (rect) *rect = {p.x, p.y, p.w, p.h};
        fx_float(p, kind);
        ++fd.popup_count;
        return &p;
    };
    const u32 max_cols = static_cast<u32>(std::max(20, (w - 4 * cw) / cw));

    if (a.sig_open) {
        const SignatureInfo& sig = lsp_.signature_info();
        std::vector<Line> lines;
        Line l;
        l.text = sig.label;
        l.mark0 = sig.param_start;
        l.mark1 = sig.param_end;
        l.mark_ink = kInkAccent;
        l.mark_style = Font::kBold;
        if (sig.count > 1) l.text += "   " + std::to_string(sig.index + 1) + "/" + std::to_string(sig.count);
        lines.push_back(std::move(l));
        for (const InfoLine& d : sig.doc) {
            if (lines.size() >= 4) break;
            lines.push_back({d.text, kInkMuted, 0, false, 0, 0, 0, 0});
        }
        set_lines(a.sig_box, *font_, wrap(lines, std::min<u32>(max_cols, 100), 6));
        int x, y;
        at_offset(pane_->doc->ed.cursor(), &x, &y);
        place(a.sig_box, x, y, true, nullptr, kFxSignature);
    }

    if (a.comp_open && !a.shown.empty()) {
        const std::vector<CompletionItem>& items = lsp_.completions();
        const u32 rows = std::min<u32>(static_cast<u32>(a.shown.size()), 10);
        a.comp_rows = rows;
        a.sel = std::min<u32>(a.sel, static_cast<u32>(a.shown.size()) - 1);
        if (a.sel < a.top) a.top = a.sel;
        if (a.sel >= a.top + rows) a.top = a.sel - rows + 1;
        a.top = std::min<u32>(a.top, static_cast<u32>(a.shown.size()) - rows);
        // Columns: kind letter, label, then the detail in a quieter ink.
        u32 label_w = 8, detail_w = 0;
        for (u32 r = 0; r < rows; ++r) {
            const CompletionItem& it = items[a.shown[a.top + r].item];
            label_w = std::max(label_w, width_of(it.label));
            detail_w = std::max(detail_w, width_of(it.detail));
        }
        label_w = std::min<u32>(label_w, 48);
        detail_w = std::min<u32>(detail_w, 36);
        const u32 cols = std::min<u32>(max_cols, 2 + label_w + (detail_w ? 2 + detail_w : 0));
        a.comp_box.resize(cols, rows);
        const std::string prefix = pane_->doc->ed.cursor() >= a.comp_anchor ? pane_->doc->ed.buf.text(a.comp_anchor, pane_->doc->ed.cursor() - a.comp_anchor) : std::string();
        std::vector<u32> hits;
        for (u32 r = 0; r < rows; ++r) {
            const CompletionItem& it = items[a.shown[a.top + r].item];
            const KindTag tag = kind_tag(it.kind);
            a.comp_box.put(*font_, r, 0, std::string_view(&tag.letter, 1), tag.ink);
            // The letters that matched what was typed stand out.
            const bool same = it.filter == it.label || it.label.starts_with(it.filter);
            if (same) lsp_match(prefix, it.filter, &hits);
            else hits.clear();
            u32 c = 2;
            size_t hit = 0;
            const u8* p = reinterpret_cast<const u8*>(it.label.data());
            for (size_t i = 0; i < it.label.size() && c < 2 + label_w;) {
                const Decoded d = decode_utf8(p + i, it.label.size() - i);
                while (hit < hits.size() && hits[hit] < i) ++hit;
                const bool matched = hit < hits.size() && hits[hit] == i;
                c = a.comp_box.put(*font_, r, c, std::string_view(it.label).substr(i, d.len), matched ? kInkAccent : 0, matched ? Font::kBold : 0);
                i += d.len;
            }
            if (detail_w) a.comp_box.put(*font_, r, 2 + label_w + 2, it.detail, kInkMuted);
            if (a.top + r == a.sel) a.comp_box.select_row(r);
        }
        int x, y;
        at_offset(a.comp_anchor, &x, &y);
        const Popup* list = place(a.comp_box, x - 2 * cw, y, false, &a.comp_rect, kFxCompletion);
        // What the selected item is, beside the list.
        const CompletionItem& sel = items[a.shown[a.sel].item];
        if (list && (!sel.doc.empty() || width_of(sel.detail) > detail_w)) {
            std::vector<Line> lines;
            if (!sel.detail.empty()) lines.push_back({sel.detail, 0, 0, false, 0, 0, 0, 0});
            if (!sel.doc.empty()) {
                if (!lines.empty()) lines.push_back({"", 0, 0, true, 0, 0, 0, 0});
                std::string_view rest = sel.doc;
                while (!rest.empty()) {
                    const size_t nl = rest.find('\n');
                    lines.push_back({std::string(rest.substr(0, nl)), kInkMuted, 0, false, 0, 0, 0, 0});
                    rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
                }
            }
            const int right = list->x + list->w, room_right = w - right - gap, room_left = list->x - gap;
            const int room = std::max(room_right, room_left);
            const u32 doc_cols = static_cast<u32>(std::clamp((room - 2 * pad_x) / cw, 0, 56));
            if (doc_cols >= 16 && fd.popup_count < std::size(fd.popups)) {
                set_lines(a.doc_box, *font_, wrap(lines, doc_cols, 12));
                Popup& p = fd.popups[fd.popup_count++];
                p.cols = a.doc_box.cols;
                p.rows = a.doc_box.rows;
                p.cells = a.doc_box.cells.data();
                p.pad_x = pad_x;
                p.pad_y = pad_y;
                p.w = static_cast<int>(p.cols) * cw + 2 * pad_x;
                p.h = static_cast<int>(p.rows) * ch + 2 * pad_y;
                p.border_px = border;
                p.x = room_right >= p.w || room_right >= room_left ? right + gap : list->x - gap - p.w;
                p.y = std::clamp(a.comp_rect.y, 0, std::max(0, h - p.h));
                fx_float(p, kFxDoc);
            }
        }
    }

    if (a.hover_open && !a.hover_lines.empty()) {
        set_lines(a.hover_box, *font_, wrap(a.hover_lines, std::min<u32>(max_cols, 90), 18));
        int x, y;
        at_offset(a.hover_at_cursor ? pane_->doc->ed.cursor() : a.hover_w0, &x, &y);
        if (!place(a.hover_box, x, y, !a.sig_open, &a.hover_rect, kFxHover)) a.hover_rect = {};
    }    popups_drawn_ = fd.popup_count;
}

u32 App::lsp_popups_open() const { return popups_drawn_; }

void App::lsp_forgive() { assist_->failed.clear(); }


}  // namespace blit
