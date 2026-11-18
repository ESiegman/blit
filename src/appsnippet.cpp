// Snippets (snippet.hpp reads them): a language server's completions that
// are snippets, and one's own from Lua (blit.snippet), put in with their
// tab stops. Tab goes to the next stop, Shift+Tab to the one before; a stop
// that is in more than one place has a caret in each (what is typed goes in
// at all of them); the stops still to come are marked. The last stop, or Esc,
// or Tab with the cursor gone from the snippet, ends it.
#include <ctime>

#include "app.hpp"
#include "applua.hpp"
#include "snippet.hpp"

namespace blit {

// A snippet being filled in: its stops, each as the editor's spans (which
// move with the text), and which stop the cursor is at.
struct App::SnippetRun {
    u64 doc = 0;
    std::vector<std::vector<size_t>> stops;  // each stop's places: indices into Editor::spans
    std::vector<std::vector<std::string>> choices;
    size_t at = 0;
    size_t spans = 0;  // how many spans it made (fewer: an undo dropped them, and it is over)
};

namespace {

std::string base_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

bool App::snippet_live() {
    if (!snippet_) return false;
    if (snippet_->doc != pane_->doc->id) return false;
    if (pane_->doc->ed.spans.size() != snippet_->spans) {
        snippet_end();
        return false;
    }
    return true;
}

void App::snippet_end() {
    if (!snippet_) return;
    if (Doc* d = find_doc(snippet_->doc)) d->ed.spans.clear();
    snippet_.reset();
    dirty_ = true;
}

// A snippet in place of [from, to): its variables filled in (`selected`:
// what TM_SELECTED_TEXT is), indented as the line is, then the cursor to its
// first stop.
void App::snippet_expand(std::string_view body, u64 from, u64 to, const std::string& selected) {
    Doc& d = *pane_->doc;
    Editor& ed = d.ed;
    snippet_end();
    const u64 line = ed.buf.line_of(from), start = ed.buf.line_start(line);
    const std::string line_text = ed.buf.text(start, ed.line_end(line) - start);
    std::string lead;
    for (const char c : line_text) {
        if ((c != ' ' && c != '\t') || start + lead.size() >= from) break;
        lead += c;
    }
    const std::string full = d.ed.path.empty() ? std::string() : full_path(d.ed.path);
    auto vars = [&](std::string_view name, std::string* value) {
        const std::time_t now = std::time(nullptr);
        std::tm t{};
        localtime_r(&now, &t);
        char buf[16];
        auto two = [&](int n) {
            std::snprintf(buf, sizeof buf, "%02d", n);
            return std::string(buf);
        };
        if (name == "TM_SELECTED_TEXT") *value = selected;
        else if (name == "TM_CURRENT_LINE") *value = line_text;
        else if (name == "TM_CURRENT_WORD") *value = ed.buf.text(from, to - from);
        else if (name == "TM_LINE_INDEX") *value = std::to_string(line);
        else if (name == "TM_LINE_NUMBER") *value = std::to_string(line + 1);
        else if (name == "TM_FILENAME") *value = base_of(full);
        else if (name == "TM_FILENAME_BASE") *value = base_of(full).substr(0, base_of(full).rfind('.'));
        else if (name == "TM_DIRECTORY") *value = full.substr(0, full.find_last_of('/'));
        else if (name == "TM_FILEPATH") *value = full;
        else if (name == "CLIPBOARD") {
            char* c = SDL_GetClipboardText();
            *value = c ? c : "";
            SDL_free(c);
        } else if (name == "CURRENT_YEAR") *value = std::to_string(t.tm_year + 1900);
        else if (name == "CURRENT_YEAR_SHORT") *value = two(t.tm_year % 100);
        else if (name == "CURRENT_MONTH") *value = two(t.tm_mon + 1);
        else if (name == "CURRENT_DATE") *value = two(t.tm_mday);
        else if (name == "CURRENT_HOUR") *value = two(t.tm_hour);
        else if (name == "CURRENT_MINUTE") *value = two(t.tm_min);
        else if (name == "CURRENT_SECOND") *value = two(t.tm_sec);
        else if (name == "LINE_COMMENT") *value = cfg_.modal ? vim_.comment_left : std::string();
        else return false;
        return true;
    };
    Snippet s = parse_snippet(body, vars);
    indent_snippet(s, lead, d.indent_tabs ? std::string("\t") : std::string(indent_step(), ' '));
    ed.clear_carets();
    ed.replace(from, to, s.text);
    if (s.stops.size() == 1 && s.stops[0].spans.size() == 1) {  // (nothing to fill in: the cursor where it ends)
        ed.move_to(from + s.stops[0].spans[0].start, false);
        scroll_to_cursor();
        return;
    }
    snippet_ = std::make_shared<SnippetRun>();
    SnippetRun& r = *snippet_;
    r.doc = d.id;
    ed.spans.clear();
    for (const Snippet::Stop& st : s.stops) {
        std::vector<size_t> places;
        for (const Snippet::Span& sp : st.spans) {
            places.push_back(ed.spans.size());
            ed.spans.push_back({from + sp.start, from + sp.end, false});
        }
        r.stops.push_back(std::move(places));
        r.choices.push_back(st.choices);
    }
    r.spans = ed.spans.size();
    snippet_go(0);
}

// The cursor to a stop: its placeholder selected (with Vim, in Insert mode,
// so that what is typed takes its place), a caret in each of its places; a
// choice's stop offers its choices. The last stop ends the snippet.
void App::snippet_go(size_t k) {
    if (!snippet_) return;
    SnippetRun& r = *snippet_;
    Editor& ed = pane_->doc->ed;
    k = std::min(k, r.stops.size() - 1);
    r.at = k;
    for (Editor::Span& s : ed.spans) s.sticky = false;
    for (const size_t i : r.stops[k]) ed.spans[i].sticky = true;
    if (cfg_.modal) {
        vim_attach();
        if (vim_.visual()) vim_.keys(ed, "<Esc>");
        if (vim_.mode() != Vim::kInsert) vim_.keys(ed, "i");
        vim_after(Vim::kHandled);
    }
    ed.clear_carets();
    const Editor::Span first = ed.spans[r.stops[k][0]];
    ed.set_caret({first.b, first.a, ~0u});
    for (size_t j = 1; j < r.stops[k].size(); ++j) {
        const Editor::Span& sp = ed.spans[r.stops[k][j]];
        ed.add_caret({sp.b, sp.a, ~0u});
    }
    const bool last = k + 1 == r.stops.size();
    std::vector<std::string> choices = r.choices[k];
    if (last) snippet_end();
    scroll_to_cursor();
    dirty_ = true;
    if (!last && choices.size() > 1)
        find_choose_item("Choice", choices, [this, choices](int i) {
            if (i < 0 || static_cast<size_t>(i) >= choices.size()) return;
            Editor& e = pane_->doc->ed;
            e.each_caret([&] { e.insert(choices[static_cast<size_t>(i)]); });
            dirty_ = true;
        });
}

// Tab and Shift+Tab within a snippet; Tab after one of one's own triggers
// (the longest that ends at the cursor, starting a word) puts it in.
bool App::snippet_key(const SDL_KeyboardEvent& k) {
    if (k.key != SDLK_TAB || (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) return false;
    Doc& d = *pane_->doc;
    if (d.read_only || d.term || d.prof || d.md || d.image || d.pdf || find_open_ || panel_has_keyboard()) return false;
    const bool shift = k.mod & SDL_KMOD_SHIFT;
    Editor& ed = d.ed;
    if (snippet_live()) {
        u64 lo = ~0ull, hi = 0;
        for (const Editor::Span& s : ed.spans) lo = std::min(lo, s.a), hi = std::max(hi, s.b);
        if (ed.cursor() >= lo && ed.cursor() <= hi) {
            if (!shift) snippet_go(snippet_->at + 1);
            else if (snippet_->at > 0) snippet_go(snippet_->at - 1);
            return true;
        }
        snippet_end();  // (the cursor has left it)
    }
    if (shift || ed.has_selection() || ed.multi() || !script_) return false;
    if (cfg_.modal) {
        vim_attach();
        if (vim_.mode() != Vim::kInsert) return false;
    }
    const std::string lang = file_language();
    const u64 cur = ed.cursor();
    const std::string before = ed.buf.text(cur > 64 ? cur - 64 : 0, std::min<u64>(cur, 64));
    const ScriptUi::OwnSnippet* best = nullptr;
    for (const ScriptUi::OwnSnippet& s : script_->snippets) {
        if (s.trigger.empty() || !before.ends_with(s.trigger) || (best && best->trigger.size() >= s.trigger.size())) continue;
        if (!s.langs.empty() && std::ranges::find(s.langs, lang) == s.langs.end()) continue;
        const size_t at = before.size() - s.trigger.size();
        const auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        if (at > 0 && word(before[at - 1]) && word(s.trigger.front())) continue;  // (inside a longer word)
        best = &s;
    }
    if (!best) return false;
    lsp_close(kPopAll);
    snippet_expand(best->body, cur - best->trigger.size(), cur, {});
    return true;
}

// One's own snippets for the file in front, to choose from; the one chosen
// goes in place of what is selected (its TM_SELECTED_TEXT).
void App::snippet_pick() {
    if (!script_ || pane_->doc->read_only) return;
    const std::string lang = file_language();
    std::vector<const ScriptUi::OwnSnippet*> mine;
    for (const ScriptUi::OwnSnippet& s : script_->snippets)
        if (s.langs.empty() || std::ranges::find(s.langs, lang) != s.langs.end()) mine.push_back(&s);
    if (mine.empty()) return notify("No snippets for this file (blit.snippet makes them)");
    std::vector<std::string> items;
    std::vector<std::pair<std::string, std::string>> bodies;  // (copies: the list may change before one is chosen)
    for (const ScriptUi::OwnSnippet* s : mine) {
        items.push_back(s->trigger + (s->desc.empty() ? std::string() : "  " + s->desc));
        bodies.emplace_back(s->trigger, s->body);
    }
    find_choose_item("Snippet", std::move(items), [this, bodies](int i) {
        if (i < 0 || static_cast<size_t>(i) >= bodies.size()) return;
        Editor& ed = pane_->doc->ed;
        if (cfg_.modal) {
            vim_attach();
            u64 lo = 0, hi = 0;
            if (vim_selection(&lo, &hi)) {  // (Visual mode's selection is what it goes over)
                vim_.keys(ed, "<Esc>");
                ed.move_to(lo, false);
                ed.move_to(hi, true);
            }
        }
        snippet_expand(bodies[static_cast<size_t>(i)].second, ed.sel_min(), ed.sel_max(), ed.selection_text());
    });
}

// The stops still to come (and those passed), each place marked as a
// search match is.
void App::snippet_marks(FrameDesc& fd) {
    if (focus_view_ >= fd.view_count || !snippet_live()) return;
    const Editor& ed = pane_->doc->ed;
    GridSpec& g = fd.views[focus_view_].grid;
    snippet_decos_.assign(g.decos.begin(), g.decos.end());
    const Buffer& b = ed.buf;
    for (size_t k = 0; k + 1 < snippet_->stops.size(); ++k) {
        if (k == snippet_->at) continue;
        for (const size_t i : snippet_->stops[k]) {
            const Editor::Span& s = ed.spans[i];
            if (s.b <= s.a) continue;
            const u64 l0 = b.line_of(s.a), l1 = b.line_of(s.b);
            for (u64 l = std::max(l0, g.top_line); l <= l1 && l < g.bottom(); ++l) {
                const u32 c0 = l == l0 ? ed.col_between(b.line_start(l), s.a) : 0;
                const u32 c1 = l == l1 ? ed.col_between(b.line_start(l), s.b) : ed.col_between(b.line_start(l), ed.line_end(l));
                snippet_decos_.push_back({l, c0, c1, kDecoMatch});
            }
        }
    }
    g.decos = snippet_decos_;
    fd.views[focus_view_].match_radius = look().sel;
}

// The snippet under way, for the tests: each stop's places as line:column
// (from 1) ranges, the one the cursor is at in brackets; or none.
std::string App::snippet_dump() {
    if (!snippet_live()) return "no snippet";
    const Editor& ed = pane_->doc->ed;
    auto place = [&](u64 off) {
        const u64 l = ed.buf.line_of(off);
        return std::to_string(l + 1) + ":" + std::to_string(ed.col_between(ed.buf.line_start(l), off) + 1);
    };
    std::string out;
    for (size_t k = 0; k < snippet_->stops.size(); ++k) {
        std::string stop;
        for (const size_t i : snippet_->stops[k]) stop += (stop.empty() ? "" : ",") + place(ed.spans[i].a) + "-" + place(ed.spans[i].b);
        out += (out.empty() ? "" : " ") + (k == snippet_->at ? "[" + stop + "]" : stop);
    }
    return out;
}

}  // namespace blit
