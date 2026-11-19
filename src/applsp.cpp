// The language servers' side of the editor: every tab's document open on
// its server, squiggles under problems, the completion list, hover and
// signature popups, go to definition, references, rename, formatting, code
// actions, inlay hints and the servers' colours.
//
// Everything here reacts to results the client has already parsed (see
// lsp.hpp). Nothing waits for a server, except formatting before a save,
// which waits a moment at most.
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <span>

#include <sys/stat.h>

#include "app.hpp"
#include "fsio.hpp"
#include "look.hpp"
#include "remote.hpp"
#include "textbox.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

// A path's last part, its name.
std::string_view base_name(std::string_view path) {
    const size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

bool word_byte(u8 b) { return std::isalnum(b) || b == '_' || b >= 0x80; }

// The widest an information box (hover, signature help) grows, in cells.
constexpr u32 kInfoCols = 72;

// One letter for a completion's kind, and the ink it is drawn in: what can
// be called stands out, the rest is quiet (the problems' inks are for problems).
struct KindTag {
    char letter;
    u32 ink;
};

KindTag kind_tag(u8 kind) {
    const u32 fn = ink::kAccent.ink, type = ink::kMuted.ink, var = ink::kMuted.ink, key = ink::kMuted.ink;
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
// At most max_rows lines come back; a last line of "…" says there was more.
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
        out.back() = Line{"\xE2\x80\xA6", kInkMuted, 0, false, 0, 0, 0, 0};
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

// A tab whose text a language server could follow (not a diff, a
// terminal, a profile, a preview, a picture or a PDF).
bool server_text(const Doc& d) { return !d.read_only && !d.term && !d.prof && !d.md && !d.image && !d.pdf; }

// Why a key that asks the language server for something (completion,
// hover, definition) got nothing, in words for the bottom line: the server
// that would serve the file and what keeps it, where that is known.
std::string why_no_server(const Config& cfg, const Lsp& lsp, bool following, const std::vector<std::string>& failed, const Doc& d) {
    if (following && lsp.attached() && !lsp.ready()) return lsp.server_name() + " is still starting";
    const std::string why = "No language server for this file: ";
    if (!cfg.lsp) return why + "language servers are off (lsp.enabled in Settings)";
    if (d.ed.path.empty()) return why + "it has no name yet";
    if (static_cast<double>(d.ed.buf.size()) > static_cast<double>(cfg.lsp_max_mb) * 1048576.0) return why + "it is larger than lsp.max_file_mb (Settings)";
    if (is_remote(d.ed.path)) return why + "none started on its machine";
    char head[128];
    const u64 n = d.ed.buf.read(0, head, sizeof head);
    const std::string_view first(head, static_cast<size_t>(std::min<u64>(n, std::string_view(head, n).find('\n'))));
    const std::string lang = Syntax::language_for(d.ed.path, first, cfg.grammars);
    if (lang.empty()) return why + "its language is not one blit knows";
    const std::vector<LspServer> servers = lsp_server_list(cfg.lsp_servers);
    const LspServer *missing = nullptr, *broke = nullptr;
    for (const LspServer& s : servers) {
        if (!s.enabled || s.command.empty() || std::find(s.languages.begin(), s.languages.end(), lang) == s.languages.end()) continue;
        if (Lsp::find_program(s.command[0]).empty()) {
            if (!missing) missing = &s;
        } else if (std::find(failed.begin(), failed.end(), s.name) != failed.end()) {
            if (!broke) broke = &s;
        } else {
            return s.name + " has not started yet";
        }
        if (missing && broke) break;
    }
    if (broke) return why + broke->name + " would not start (lsp.servers in Settings)";
    if (missing) return why + missing->name + " is not installed (lsp.servers in Settings)";
    return why + "none is set up for " + lang + " (lsp.servers in Settings)";
}

// A file's name as a list shows it: from the project folder, or from ~.
std::string shown_name(const std::string& path, const std::string* root) {
    if (root && path.size() > root->size() + 1 && path.starts_with(*root) && path[root->size()] == '/') return path.substr(root->size() + 1);
    const char* home = SDL_getenv("HOME");
    const size_t h = home ? std::strlen(home) : 0;
    if (h > 1 && path.size() > h && path.starts_with(home) && path[h] == '/') return "~" + path.substr(h);
    return path;
}

// Edits as byte ranges, applied last first so that the earlier ones' places
// hold; ones that overlap one already made are left out.
struct Change {
    u64 start, end;
    std::string_view text;
};
void sort_changes(std::vector<Change>& c) {
    std::stable_sort(c.begin(), c.end(), [](const Change& x, const Change& y) { return x.start > y.start; });
    u64 floor = ~0ull;
    std::erase_if(c, [&](const Change& x) {
        if (x.end > floor) return true;
        floor = x.start;
        return false;
    });
}

}  // namespace

// A call or type hierarchy being looked through (the find box's list, a
// tree): its rows in the order shown, each with its depth and whether its
// level is open; the row whose level is being asked for (-1: the first
// items, from the cursor's place).
struct HierRow {
    LspHierItem item;
    u32 depth = 0;
    bool open = false;
};

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
    u64 reach_gen = ~0ull, reach_doc = ~0ull;
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
    u64 deco_gen = ~0ull, deco_top = ~0ull, deco_rev = ~0ull, deco_line = ~0ull, deco_doc = ~0ull;
    u32 deco_rows = 0, deco_font = ~0u, deco_edge = ~0u;
    u64 deco_key = 0;

    // Servers that would not start, or died, since the settings were last
    // read. They are not tried again; the next one for the language is.
    std::vector<std::string> failed;
    // Tabs that no server took, by document and the name it had then: not
    // looked at again until something changes (a server comes or goes).
    std::vector<std::pair<u64, std::string>> tried;
    bool notify_set = false;

    // Found references, and which of them F4 went to last; while `refs_live`
    // they are what F4 steps through (another list in the box ends that).
    std::vector<LspPlace> refs;
    i64 refs_at = -1;
    bool refs_pending = false, refs_live = false, refs_picker = false;
    std::string refs_word = "Reference";  // what F4 says each is ("Implementation 2 of 3")
    // Implementations, type definitions or declarations asked for; the other
    // half of a header and source pair; the outline; the project's symbols
    // shown as they come.
    bool places_pending = false, switch_pending = false, outline_pending = false, symbols_live = false;
    std::vector<LspSymbol> symbol_rows;
    std::vector<LspSymbol> outline_rows;  // (the outline on show: the server's, or the tree's)
    std::vector<HierRow> hier_rows;  // a hierarchy on show (hier_show)
    // How many references each definition on show has (lens_marks): for
    // the document in front as of a version of it (the tag the answers
    // come with), by where the name is; what has been asked; when to ask
    // (typing has paused); the notes drawn of them.
    u64 lens_doc = 0, lens_rev = ~0ull, lens_tag = 0, lens_due_ns = 0;
    std::map<u64, u32> lens_counts;
    std::set<u64> lens_asked;
    std::vector<GridNote> lens_notes;
    std::vector<std::vector<u32>> lens_cells;
    std::vector<std::string> lens_texts;
    std::vector<u64> lens_at;  // (each note's definition: where its name is)
    u64 lens_key[5] = {};
    std::string hier_title;
    u8 hier_kind = 0;
    int hier_asking = -1;
    bool hier_waiting = false;
    // The uses of the symbol at the cursor: asked once the cursor has rested,
    // drawn while it stays on one of them and the text is as it was.
    u64 uses_due_ns = 0;
    u64 uses_seen_doc = 0, uses_seen_rev = ~0ull, uses_seen_cur = ~0ull;  // (the cursor as the last frame had it)
    u64 uses_doc = 0, uses_rev = ~0ull;                                   // (what the uses shown are of)
    u64 uses_ask_doc = 0, uses_ask_rev = ~0ull;                           // (what the last asked were of)
    std::vector<std::pair<u64, u64>> uses;                                // (in order)
    std::vector<GridDeco> decos_all;  // (the problems' marks and the uses', together)
    // A rename under way: asked where the name is, then for the new name.
    bool rename_pending = false;
    u64 rename_doc = 0, rename_off = 0;
    std::string rename_to;
    // Formatting asked for (and, before a save, waited for).
    bool format_pending = false, format_quiet = false;
    // Code actions asked for, and one being resolved before it is applied.
    bool actions_pending = false, resolving = false;
    u64 actions_doc = 0;
    // Lines of the document in front with a problem the server can fix.
    u64 marks_doc = ~0ull, marks_gen = ~0ull;
    std::vector<u64> fix_lines;
    std::vector<u64> action_lines;  // where Option+Enter found a fix (until the server's problems next change)

    // Each view's inlay hints and the servers' colours, as last worked out,
    // and what for.
    struct Extras {
        u64 doc = ~0ull, rev = ~0ull, rev_ns = 0;  // the text, and since when it has been as it is
        // Inlay hints: notes between characters, and their cells.
        std::vector<GridNote> notes;
        std::vector<u32> cells;
        std::vector<std::string> labels;  // (for the tests)
        u64 hints_key = ~0ull;
        // The servers' colours over tree-sitter's: the visible lines' marks,
        // and the cell bits of each style (tree-sitter's ids first, then
        // one for each kind of token that is coloured).
        Highlights merged;
        u64 merged_key = ~0ull;
        u32 lut[256] = {};
        u32 styles = 0;                         // tree-sitter's styles the table has
        u64 lut_key = ~0ull;
        std::vector<u8> type_style;            // the server's token type -> style id (0: left to tree-sitter)
        std::vector<u8> builtin_style;         // the same, for one from the language's own library
        std::vector<std::pair<u32, Rgb>> inks;  // the palette entries the table took, and their colours
        std::vector<std::string> made;         // the capture each style past tree-sitter's is
    };
    Extras extras[kMaxViews];
    u64 extras_due_ns = 0;  // when a text will have stood still long enough to ask about it

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
        for (const std::shared_ptr<Doc>& d : p->docs) d->git = nullptr;
}

// ---------------------------------------------------------------- lifecycle

// Servers that failed on another machine may be tried again (its
// connection is back).
void App::lsp_forget_failed(const std::string& suffix) {
    if (!assist_) return;
    std::erase_if(assist_->failed, [&](const std::string& n) { return n.ends_with(suffix); });
    assist_->tried.clear();
}

// Every tab whose text a server could have gets its document opened on one
// (started if need be), and one that no longer could is closed. Returns at
// once; the servers report in through sync_lsp().
void App::lsp_reconcile() {
    Assist& a = *assist_;
    if (!cfg_.lsp) return lsp_.shutdown();
    const std::vector<LspServer> all = lsp_server_list(cfg_.lsp_servers);
    std::vector<LspServer> here;  // (another machine's are filtered by its own failures, below)
    for (const LspServer& s : all)
        if (std::find(a.failed.begin(), a.failed.end(), s.name) == a.failed.end()) here.push_back(s);
    const double most = static_cast<double>(cfg_.lsp_max_mb) * 1048576.0;
    for (Doc* dp : open_docs()) {  // (each once, however many splits show it)
            Doc& d = *dp;
            const bool wanted = server_text(d) && !d.loading && !d.ed.path.empty() && static_cast<double>(d.ed.buf.size()) <= most;
            if (const std::string* as = lsp_.opened_as(d.id)) {
                if (wanted && *as == d.ed.path) {
                    lsp_.refresh(d.id);  // (read again from disk meanwhile: sent again)
                    continue;
                }
                lsp_.close_doc(d.id);
            }
            if (!wanted) continue;
            if (std::ranges::any_of(a.tried, [&](const auto& t) { return t.first == d.id && t.second == d.ed.path; })) continue;
            char head[128];
            const u64 n = d.ed.buf.read(0, head, sizeof head);
            const std::string_view first(head, static_cast<size_t>(std::min<u64>(n, std::string_view(head, n).find('\n'))));
            const std::string lang = Syntax::language_for(d.ed.path, first, cfg_.grammars);
            std::string host, there;
            const bool remote = split_remote(d.ed.path, &host, &there);
            std::vector<LspServer> theirs;
            if (remote)
                for (const LspServer& s : all)
                    if (std::find(a.failed.begin(), a.failed.end(), s.name + "@" + host) == a.failed.end()) theirs.push_back(s);
            bool wait = false;
            if (!lsp_.open(d.id, d.ed, d.ed.path, lang, remote ? theirs : here, &wait) && !wait) a.tried.push_back({d.id, d.ed.path});
        }
}

// The document in front is the one the questions are about. Any tab that
// has no server yet is offered one.
void App::start_lsp() {
    Assist& a = *assist_;
    if (!a.notify_set) {
        a.notify_set = true;
        lsp_.set_notify([] {  // on a client's thread: wake the event loop
            SDL_Event ev{};
            ev.type = SDL_EVENT_USER;
            ev.user.code = 4;
            SDL_PushEvent(&ev);
        });
    }
    lsp_reconcile();
    Doc* d = pane_->doc;
    if (d && lsp_.is_open(d->id)) {
        lsp_.follow(d->id);
        lsp_doc_ = d;
    } else {
        lsp_.follow(0);
        lsp_doc_ = nullptr;
    }
}

// Must run before the buffer of the document in front is replaced
// (keep_server false: its document is closed, to be opened again) or
// another document comes to the front (true: it stays open).
void App::stop_lsp(bool keep_server) {
    Assist& a = *assist_;
    if (lsp_doc_ && !keep_server) lsp_.close_doc(lsp_doc_->id);
    lsp_doc_ = nullptr;
    lsp_.unfollow();
    lsp_close(kPopAll);
    a.decos.clear();
    a.notes.clear();
    a.deco_gen = ~0ull;
    lsp_pending_ = false;
}

// A document is about to go: its server lets go of it first.
void App::lsp_forget(const Doc& d) {
    if (lsp_doc_ == &d) stop_lsp();
    lsp_.close_doc(d.id);
    std::erase_if(assist_->tried, [&](const auto& t) { return t.first == d.id; });
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

// Takes in what the servers have sent since last time.
void App::sync_lsp() {
    Assist& a = *assist_;
    if (a.refs_picker && !find_open_) a.refs_picker = false;  // (chosen from, or shut)
    if (find_open_ && !a.refs_picker) a.refs_live = false;     // another list in the box: F4 is its
    const u32 news = lsp_.poll();
    if (!news) return;
    if (news & Lsp::kState) {
        const std::string msg = lsp_.take_message();
        if (!msg.empty()) std::fprintf(stderr, "blit: %s\n", msg.c_str());
        // Those servers are out; another one for the language may do.
        for (std::string& name : lsp_.take_failed()) a.failed.push_back(std::move(name));
        a.tried.clear();
        if (lsp_doc_ && !lsp_.is_open(lsp_doc_->id)) {
            lsp_doc_ = nullptr;
            lsp_close(kPopAll);
        }
        lsp_pending_ = true;  // (and the tabs it had, or that waited for one to come up, are offered another)
        dirty_ = true;
    }
    if (news & (Lsp::kDiagnostics | Lsp::kHints | Lsp::kTokens)) dirty_ = true;
    if (news & Lsp::kDiagnostics) {  // (the gutter's marks of fixes, looked at again)
        a.action_lines.clear();
        ++lsp_marks_gen_;
    }
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
                    out.ink = ink::kHeading.ink;
                    out.style = ink::kHeading.style;
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
    if ((news & Lsp::kDefinition) && peek_pending_) {  // (asked for a peek: the place in a box, apppeek.cpp)
        peek_pending_ = false;
        peek_show(lsp_.definition_target());
    } else if (news & Lsp::kDefinition) {
        goto_definition();
    }

    // References: in a list, the file, the line and its text, as Search in files has them.
    if ((news & Lsp::kReferences) && a.refs_pending) {
        a.refs_pending = false;
        lsp_show_places(lsp_.reference_list(), "Reference");
        dirty_ = true;
    }

    // Implementations and the like: one gone to, several listed.
    if ((news & Lsp::kPlaces) && a.places_pending) {
        a.places_pending = false;
        static const char* const kWords[] = {"Implementation", "Type definition", "Declaration"};
        lsp_show_places(lsp_.place_list(), kWords[lsp_.place_kind()]);
    }
    if ((news & Lsp::kSwitch) && a.switch_pending) {
        a.switch_pending = false;
        std::string other = lsp_.switch_target();
        if (other.empty()) other = other_half(pane_->doc->ed.path);  // (clangd did not know: looked for beside it)
        if (other.empty()) notify("No header or source found for " + std::string(base_name(pane_->doc->ed.path)));
        else if (remember_place(), !open_path(other.c_str())) notify("Cannot open " + other);
    }
    if ((news & Lsp::kHierarchy) && a.hier_waiting) hier_take();
    if (news & Lsp::kCounts) {
        for (const Lsp::RefCount& r : lsp_.take_counts())
            if (r.tag == a.lens_tag) a.lens_counts[r.offset] = r.count;
        a.lens_key[0] = ~0ull;  // (drawn again)
        dirty_ = true;
    }
    if ((news & Lsp::kOutline) && a.outline_pending) {
        a.outline_pending = false;
        outline_show(lsp_.outline_list(), ~0ull);
    }
    if ((news & Lsp::kSymbols) && find_everywhere_open()) {  // (Search everywhere's)
        a.symbol_rows = lsp_.found_symbols();
        find_everywhere_rebuild();
    } else if ((news & Lsp::kSymbols) && a.symbols_live) {
        if (!find_open_) {
            a.symbols_live = false;
        } else {
            a.symbol_rows = lsp_.found_symbols();
            std::vector<std::string> items;
            const std::string* root = panel_root();
            for (const LspSymbol& y : a.symbol_rows) {
                std::string row = y.name;
                if (const char* k = symbol_kind_name(y.kind); *k) row += std::string("  \xC2\xB7 ") + k;
                if (!y.container.empty()) row += "  in " + y.container;
                row += "  \xC2\xB7 " + shown_name(y.path, root) + ":" + std::to_string(y.line + 1);
                items.push_back(std::move(row));
            }
            find_set_items(std::move(items));
        }
    }
    if (news & Lsp::kHighlights) {
        a.uses = lsp_.highlights();
        std::ranges::sort(a.uses);
        a.uses_doc = a.uses_ask_doc;  // (an answer to text since changed is never shown)
        a.uses_rev = a.uses_ask_rev;
        dirty_ = true;
    }

    // Rename: the name the server says can be renamed, then a new one for it.
    if ((news & Lsp::kRenameRange) && a.rename_pending) {
        a.rename_pending = false;
        if (lsp_.rename_lo() >= lsp_.rename_hi() || lsp_.front() != a.rename_doc) {
            notify(lsp_.rename_error().empty() ? std::string("Nothing to rename here") : "Cannot rename: " + lsp_.rename_error());
        } else {
            const std::string was = lsp_.rename_placeholder();
            a.rename_off = lsp_.rename_lo();
            find_choose(kFindInput, "Rename " + was + " to", was, [this, was](const std::string& name) {
                Assist& x = *assist_;
                if (name.empty() || name == was) return;
                if (!lsp_.can_rename() || lsp_.front() != x.rename_doc) return notify("The file changed: rename again");
                x.rename_to = name;
                x.rename_pending = true;
                lsp_.rename(x.rename_off, name);
            });
        }
    }
    if ((news & Lsp::kRename) && a.rename_pending) {
        a.rename_pending = false;
        const LspWorkspaceEdit& e = lsp_.rename_edit();
        if (!lsp_.rename_error().empty()) {
            notify("Cannot rename: " + lsp_.rename_error());
        } else if (e.empty()) {
            notify("Nothing to rename");
        } else {
            // What it changes, file by file: Enter makes the change.
            size_t edits = 0;
            for (const LspFileEdit& f : e.files) edits += f.edits.size();
            std::vector<std::string> items;
            items.push_back("Rename to " + a.rename_to + ": " + std::to_string(edits) + (edits == 1 ? " edit in " : " edits in ") + std::to_string(e.files.size()) +
                            (e.files.size() == 1 ? " file" : " files"));
            const std::string* root = panel_root();
            for (const LspFileEdit& f : e.files)
                items.push_back(shown_name(f.path, root) + "  " + std::to_string(f.edits.size()) + (f.edits.size() == 1 ? " edit" : " edits"));
            auto edit = std::make_shared<LspWorkspaceEdit>(e);
            find_choose_item("Rename", std::move(items), [this, edit](int) {
                std::string said;
                lsp_apply(*edit, &said);
                notify("Renamed: " + said);
            });
        }
    }

    // Formatting: made as one step that undo takes back.
    if ((news & Lsp::kFormat) && a.format_pending) {
        a.format_pending = false;
        Doc* d = find_doc(lsp_.format_key());
        const std::vector<Lsp::ByteEdit>& edits = lsp_.format_edits();
        if (!d) {
        } else if (lsp_.format_failed()) {
            notify("Not formatted: the file changed meanwhile, or the server would not");
        } else if (edits.empty()) {
            if (!a.format_quiet) notify("Nothing to format");
        } else {
            std::vector<Change> c;
            for (const Lsp::ByteEdit& e : edits) c.push_back({e.start, e.end, e.text});
            sort_changes(c);
            Editor& ed = d->ed;
            u64 cur = ed.cursor();
            ed.begin_group();
            for (const Change& x : c) {
                if (x.end > ed.buf.size()) continue;
                ed.replace(x.start, x.end, x.text);
                if (x.end <= cur) cur = cur + x.text.size() - (x.end - x.start);
                else if (x.start < cur) cur = x.start + std::min<u64>(x.text.size(), cur - x.start);
            }
            ed.end_group();
            ed.move_to(std::min(cur, ed.buf.size()), false);
            for (const std::unique_ptr<Pane>& p : panes_) p->cache_rev = ~0ull;
            if (!a.format_quiet) notify("Formatted");
            if (cfg_.modal) vim_attach();
        }
        dirty_ = true;
    }

    // Code actions: a menu; a resolved one, its edit made.
    if ((news & Lsp::kActions) && (a.actions_pending || a.resolving)) {
        const bool resolved = a.resolving;
        a.actions_pending = a.resolving = false;
        const std::vector<LspAction>& acts = lsp_.actions();
        if (resolved) {
            if (!acts.empty() && !acts[0].disabled.empty()) {
                notify(acts[0].title + ": " + acts[0].disabled);
            } else if (!acts.empty()) {
                std::string said;
                if (!acts[0].edit.empty()) lsp_apply(acts[0].edit, &said);
                if (!acts[0].command.empty()) lsp_.execute(acts[0]);
                else notify(acts[0].title);
            }
        } else if (acts.empty() || lsp_.front() != a.actions_doc) {
            notify("No actions here");
        } else {
            std::vector<std::string> items;
            bool fixes = false;
            for (const LspAction& x : acts) {
                items.push_back(x.title + (x.disabled.empty() ? "" : " (" + x.disabled + ")"));
                fixes = fixes || x.kind.starts_with("quickfix");
            }
            if (fixes && std::find(a.action_lines.begin(), a.action_lines.end(), pane_->cur_line) == a.action_lines.end()) {
                a.action_lines.push_back(pane_->cur_line);
                ++lsp_marks_gen_;
            }
            auto list = std::make_shared<std::vector<LspAction>>(acts);
            find_choose_item("Actions", std::move(items), [this, list](int i) {
                Assist& x = *assist_;
                if (i < 0 || static_cast<size_t>(i) >= list->size()) return;
                const LspAction& act = (*list)[static_cast<size_t>(i)];
                if (!act.disabled.empty()) return notify(act.title + ": " + act.disabled);
                if (!act.resolve.empty()) {  // (the server fills in its edit when asked)
                    x.resolving = true;
                    lsp_.resolve_action(act);
                    return;
                }
                std::string said;
                if (!act.edit.empty()) lsp_apply(act.edit, &said);
                if (!act.command.empty()) lsp_.execute(act);  // (what it changes, the server asks for, under its own name)
                else notify(act.title);
            });
        }
        dirty_ = true;
    }

    // A server asks for an edit to be made (a command's).
    if (news & Lsp::kApplyEdit) {
        LspWorkspaceEdit e;
        std::string label, said;
        while (lsp_.take_apply(&e, &label)) {
            lsp_.answer_apply(lsp_apply(e, &said));
            if (!label.empty()) notify(label);
        }
        dirty_ = true;
    }
}

// ---------------------------------------------------------------- changes to files

// A workspace edit (a rename, a fix): a file open in a tab is changed there,
// as one step that undo takes back; any other is read, changed and written,
// here or on another machine. *said: how much changed, in words.
bool App::lsp_apply(const LspWorkspaceEdit& e, std::string* said) {
    size_t edits = 0, files = 0;
    std::string failed;
    for (const LspFileEdit& f : e.files) {
        Doc* d = nullptr;
        if (const u64 key = lsp_.key_of(f.path)) d = find_doc(key);
        Pane* where = nullptr;
        size_t at = 0;
        if (!d && find_open(is_remote(f.path) ? f.path : full_path(f.path), &where, &at)) d = where->docs[at].get();
        std::vector<Change> c;
        if (d) {
            if (d->read_only || d->loading) {
                failed = f.path;
                continue;
            }
            for (const LspTextEdit& t : f.edits) {
                const u64 a0 = Lsp::offset_at(d->ed.buf, t.l0, t.c0, e.utf16), a1 = Lsp::offset_at(d->ed.buf, t.l1, t.c1, e.utf16);
                if (a1 >= a0) c.push_back({a0, a1, t.text});
            }
            sort_changes(c);
            Editor& ed = d->ed;
            u64 cur = ed.cursor();
            ed.begin_group();
            for (const Change& x : c) {
                ed.replace(x.start, x.end, x.text);
                if (x.end <= cur) cur = cur + x.text.size() - (x.end - x.start);
                else if (x.start < cur) cur = x.start + std::min<u64>(x.text.size(), cur - x.start);
            }
            ed.end_group();
            ed.move_to(std::min(cur, ed.buf.size()), false);
        } else {
            std::string text, out, err;
            if (!fs_read(f.path, &text, 64u << 20)) {
                failed = f.path;
                continue;
            }
            for (const LspTextEdit& t : f.edits) {
                const u64 a0 = Lsp::offset_in(text, t.l0, t.c0, e.utf16), a1 = Lsp::offset_in(text, t.l1, t.c1, e.utf16);
                if (a1 >= a0) c.push_back({a0, a1, t.text});
            }
            sort_changes(c);
            out = text;
            for (const Change& x : c) out.replace(x.start, x.end - x.start, x.text);
            if (!fs_write(f.path, out, &err)) {
                failed = f.path + " (" + err + ")";
                continue;
            }
        }
        edits += c.size();
        files += c.empty() ? 0 : 1;
    }
    for (const std::unique_ptr<Pane>& p : panes_) p->cache_rev = ~0ull;
    if (cfg_.modal) vim_attach();
    find_stale();
    dirty_ = true;
    *said = std::to_string(edits) + (edits == 1 ? " edit in " : " edits in ") + std::to_string(files) + (files == 1 ? " file" : " files");
    if (!failed.empty()) *said += "; not in " + failed;
    if (!e.unsupported.empty()) *said += "; not done: " + e.unsupported;
    return failed.empty();
}

// ---------------------------------------------------------------- references, rename, formatting, actions

void App::lsp_references() {
    Assist& a = *assist_;
    if (!lsp_.ready() || !lsp_.can_references()) {
        if (server_text(*pane_->doc))
            notify(lsp_.ready() ? lsp_.server_name() + " does not find references" : why_no_server(cfg_, lsp_, lsp_doc_ == pane_->doc, a.failed, *pane_->doc));
        return;
    }
    lsp_close(kPopAll);
    a.refs_pending = true;
    lsp_.references(pane_->doc->ed.cursor());
}

// Alt+Shift+F12: where the definition is, asked for a peek (apppeek.cpp).
void App::peek_open() {
    Assist& a = *assist_;
    if (!lsp_.ready() || !lsp_.can_define()) {
        if (server_text(*pane_->doc))
            notify(lsp_.ready() ? lsp_.server_name() + " does not find definitions" : why_no_server(cfg_, lsp_, lsp_doc_ == pane_->doc, a.failed, *pane_->doc));
        return;
    }
    lsp_close(kPopAll);
    peek_pending_ = true;
    lsp_.definition(pane_->doc->ed.cursor());
}

// The next (or previous) of the references found, round the ends. False
// when there is no list for F4 to go through.
bool App::lsp_step(int by) {
    Assist& a = *assist_;
    if (a.refs_picker && !find_open_) a.refs_picker = false;
    if (find_open_ && !a.refs_picker) a.refs_live = false;
    if (!a.refs_live || a.refs.empty()) return false;
    const i64 n = static_cast<i64>(a.refs.size());
    a.refs_at = a.refs_at < 0 ? (by < 0 ? n - 1 : 0) : ((a.refs_at + by) % n + n) % n;
    const LspPlace& p = a.refs[static_cast<size_t>(a.refs_at)];
    const u64 key = lsp_.key_of(p.path);
    Pane* where = nullptr;
    size_t at = 0;
    if (key && find_doc(key, &where, &at)) {
        remember_place();
        focus_pane(where);
        tab_show(at);
        show_place(p.line, p.col);
    } else if (!go_to_place(p.path, p.line + 1, p.col + 1)) {
        return true;
    }
    notify(a.refs_word + " " + std::to_string(a.refs_at + 1) + " of " + std::to_string(n));
    return true;
}

// Places a server found, in the list References has (F4 then steps through
// them): with one, it is gone to straight away (references are always listed).
void App::lsp_show_places(std::vector<LspPlace> list, const std::string& word) {
    Assist& a = *assist_;
    a.refs = std::move(list);
    a.refs_at = -1;
    a.refs_word = word;
    std::string lower = word;
    lower[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(lower[0])));
    if (a.refs.empty()) {
        a.refs_live = false;
        notify(word == "Reference" ? std::string("No references found") : "No " + lower + " found");
        return;
    }
    if (a.refs.size() == 1 && word != "Reference") {
        a.refs_live = true;
        a.refs_picker = false;
        lsp_step(1);
        a.refs_live = false;
        return;
    }
    std::vector<std::string> items;
    const std::string* root = panel_root();
    for (const LspPlace& p : a.refs) {
        std::string_view text = p.text;
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
        items.push_back(shown_name(p.path, root) + ":" + std::to_string(p.line + 1) + "  " + std::string(text));
    }
    const std::string title = (word == "Type definition" ? std::string("Type definitions") : word + "s") + " (" + std::to_string(a.refs.size()) + ")";
    find_choose_item(title, std::move(items), [this](int i) {
        Assist& x = *assist_;
        if (i < 0 || static_cast<size_t>(i) >= x.refs.size()) return;
        x.refs_at = i - 1;
        lsp_step(1);
    });
    a.refs_live = a.refs_picker = true;
}

void App::lsp_places(Lsp::PlaceKind kind) {
    Assist& a = *assist_;
    static const char* const kWhat[] = {"implementations", "type definitions", "declarations"};
    if (!lsp_.ready() || lsp_doc_ != pane_->doc) {
        if (server_text(*pane_->doc)) notify(why_no_server(cfg_, lsp_, lsp_doc_ == pane_->doc, a.failed, *pane_->doc));
        return;
    }
    if (!lsp_.can_place(kind)) {
        if (kind == Lsp::kDeclaration && lsp_.can_define()) {  // (a server with no declarations: the definition)
            lsp_.definition(pane_->doc->ed.cursor());
            return;
        }
        notify(lsp_.server_name() + " does not find " + kWhat[kind]);
        return;
    }
    lsp_close(kPopAll);
    a.places_pending = true;
    lsp_.places(pane_->doc->ed.cursor(), kind);
}

// The file of the same name with the other kind of extension: beside it;
// then in the include and source folders of the folders above it; then
// anywhere in the project (the nearest to it), a few thousand entries at most.
std::string App::other_half(const std::string& path) const {
    if (path.empty() || is_remote(path)) return {};
    static constexpr std::string_view kHeaders[] = {".h", ".hpp", ".hh", ".hxx", ".inl", ".cuh"};
    static constexpr std::string_view kSources[] = {".cpp", ".cc", ".cxx", ".c", ".cu", ".hip", ".m", ".mm"};
    const std::string full = full_path(path);
    const size_t slash = full.rfind('/'), dot = full.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    const std::string dir = full.substr(0, slash), stem = full.substr(slash + 1, dot - slash - 1), ext = full.substr(dot);
    const bool header = std::ranges::find(kHeaders, std::string_view(ext)) != std::end(kHeaders);
    const bool source = std::ranges::find(kSources, std::string_view(ext)) != std::end(kSources);
    if (!header && !source) return {};
    auto exts = [&]() -> std::span<const std::string_view> { return header ? std::span<const std::string_view>(kSources) : std::span<const std::string_view>(kHeaders); };
    auto is_file = [](const std::string& p) {
        struct stat st;
        return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    };
    for (const std::string_view e : exts())
        if (const std::string p = dir + "/" + stem + std::string(e); is_file(p)) return p;
    // include/ and src/ beside each folder up from it (as far as the project's).
    const std::string* root = panel_root();
    std::string up = dir;
    for (int levels = 0; levels < 4 && !up.empty(); ++levels) {
        for (const char* sub : {"/include/", "/src/", "/source/", "/inc/", "/lib/"})
            for (const std::string_view e : exts())
                if (const std::string p = up + sub + stem + std::string(e); is_file(p)) return p;
        if (root && up == *root) break;
        const size_t s = up.rfind('/');
        if (s == std::string::npos || s == 0) break;
        up.resize(s);
    }
    // Anywhere in the project: the one whose folder shares most of the path.
    if (!root) return {};
    std::string best;
    size_t best_common = 0, seen = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(*root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator() && seen < 20000; it.increment(ec), ++seen) {
        if (ec) break;
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (name.starts_with(".") || name.starts_with("build") || name == "node_modules" || name == "out") it.disable_recursion_pending();
            continue;
        }
        const size_t d = name.rfind('.');
        if (d == std::string::npos || name.substr(0, d) != stem) continue;
        if (std::ranges::find(exts(), std::string_view(name).substr(d)) == exts().end()) continue;
        const std::string p = it->path().string();
        size_t common = 0;
        while (common < p.size() && common < full.size() && p[common] == full[common]) ++common;
        if (common > best_common) best = p, best_common = common;
    }
    return best;
}

void App::lsp_switch_header() {
    Assist& a = *assist_;
    const std::string& path = pane_->doc->ed.path;
    if (path.empty()) return notify("This file has no name yet");
    if (lsp_.ready() && lsp_doc_ == pane_->doc && lsp_.can_switch()) {  // (clangd knows, from what includes what)
        a.switch_pending = true;
        lsp_.switch_header();
        return;
    }
    const std::string other = other_half(path);
    if (other.empty()) return notify("No header or source found for " + std::string(base_name(path)));
    remember_place();
    if (!open_path(other.c_str())) notify("Cannot open " + other);
}

void App::outline_show(std::vector<LspSymbol> symbols, u64 pick) {
    Assist& a = *assist_;
    a.outline_rows = std::move(symbols);
    const std::vector<LspSymbol>& all = a.outline_rows;
    {
        if (all.empty()) {
            notify("No symbols in " + std::string(base_name(pane_->doc->ed.path)));
        } else {
            // Each with its kind and the server's detail, indented as nested;
            // the one the cursor is in chosen to start with.
            std::vector<std::string> items;
            size_t at = 0;
            const u64 cur = pane_->doc->ed.cursor();
            for (size_t i = 0; i < all.size(); ++i) {
                const LspSymbol& y = all[i];
                std::string row(2 * std::min<u32>(y.depth, 8), ' ');
                row += y.name;
                if (!y.detail.empty() && y.detail.size() < 60) row += (y.detail.front() == '(' ? "" : " ") + y.detail;
                if (const char* k = symbol_kind_name(y.kind); *k) row += std::string("  \xC2\xB7 ") + k;
                items.push_back(std::move(row));
                if (pick == ~0ull ? y.offset <= cur : y.offset == pick) at = i;
            }
            const u64 back_off = cur;
            const double back_y = pane_->target_y;
            find_preview_item("Outline of " + std::string(base_name(pane_->doc->ed.path)), std::move(items), at,
                              [this](int i) {  // (the view follows the choice; the cursor stays until Enter)
                                  const std::vector<LspSymbol>& list = assist_->outline_rows;
                                  if (i < 0 || static_cast<size_t>(i) >= list.size()) return;
                                  const Editor& ed = pane_->doc->ed;
                                  const u64 line = ed.buf.line_of(std::min(list[static_cast<size_t>(i)].offset, ed.buf.size()));
                                  const double ch = font_->cell_h(), vh = view_h();
                                  pane_->target_y = std::clamp(static_cast<double>(row_of_line(line)) * ch - vh / 3, 0.0, std::max(0.0, max_scroll_y()));
                                  dirty_ = true;
                              },
                              [this](int i) {
                                  const std::vector<LspSymbol>& list = assist_->outline_rows;
                                  if (i < 0 || static_cast<size_t>(i) >= list.size()) return;
                                  remember_place();
                                  pane_->doc->ed.move_to(std::min(list[static_cast<size_t>(i)].offset, pane_->doc->ed.buf.size()), false);
                                  scroll_to_cursor();
                              },
                              [this, back_off, back_y] {
                                  pane_->doc->ed.move_to(std::min(back_off, pane_->doc->ed.buf.size()), false);
                                  pane_->target_y = back_y;
                                  dirty_ = true;
                              });
        }
    }
}

void App::lsp_outline() {
    Assist& a = *assist_;
    if (!lsp_.ready() || lsp_doc_ != pane_->doc || !lsp_.can_outline()) {
        // No server that gives one: the definitions the syntax tree has.
        Doc& d = *pane_->doc;
        SyntaxStructure all;
        if (d.syntax.active() && d.syntax.ask_all(d.ed.buf, &all)) {
            lsp_close(kPopAll);
            outline_show(tree_symbols(d, all), ~0ull);
        } else if (server_text(d)) {
            notify(lsp_.ready() && lsp_doc_ == pane_->doc ? lsp_.server_name() + " gives no outline" : why_no_server(cfg_, lsp_, lsp_doc_ == pane_->doc, a.failed, d));
        }
        return;
    }
    lsp_close(kPopAll);
    a.outline_pending = true;
    lsp_.outline();
}

// A document's definitions as the syntax tree has them, as a server's
// symbols would be.
std::vector<LspSymbol> App::tree_symbols(const Doc& d, const SyntaxStructure& s) {
    std::vector<LspSymbol> out;
    for (const SyntaxScope& sc : s.scopes) {
        LspSymbol y;
        y.name = sc.name;
        y.kind = sc.kind;
        y.depth = sc.depth;
        y.path = d.ed.path;
        y.offset = sc.name_at;
        y.line = static_cast<u32>(d.ed.buf.line_of(std::min(sc.name_at, d.ed.buf.size())));
        y.character = static_cast<u32>(sc.name_at - d.ed.buf.line_start(y.line));
        out.push_back(std::move(y));
    }
    return out;
}

// Whether the uses found are to be shown: of this text as it is, two or
// more, the cursor on one of them and nothing selected.
bool App::lsp_uses_shown() const {
    const Assist& a = *assist_;
    const Editor& ed = pane_->doc->ed;
    if (a.uses.size() < 2 || a.uses_doc != pane_->doc->id || a.uses_rev != ed.revision() || ed.has_selection()) return false;
    const u64 cur = ed.cursor();
    auto after = std::ranges::upper_bound(a.uses, cur, {}, &std::pair<u64, u64>::first);
    return after != a.uses.begin() && cur <= std::prev(after)->second;
}

std::string App::lsp_uses_dump() {
    Assist& a = *assist_;
    const Editor& ed = pane_->doc->ed;
    if (!lsp_uses_shown()) return "uses: none";
    std::string out = "uses:";
    for (const auto& [lo, hi] : a.uses) {
        const u64 line = ed.buf.line_of(lo), start = ed.buf.line_start(line);
        out += " " + std::to_string(line + 1) + ":" + std::to_string(lo - start + 1) + "-" + std::to_string(hi - start + 1);
    }
    return out;
}

void App::lsp_find_symbols() {
    Assist& a = *assist_;
    if (!lsp_.ready() || !lsp_.can_find_symbols()) {
        // No server that finds them: the definitions the syntax tree has in
        // the files that are open.
        std::vector<LspSymbol> rows;
        std::vector<u64> seen;
        for (const std::unique_ptr<Pane>& p : panes_)
            for (const std::shared_ptr<Doc>& d : p->docs) {
                if (std::ranges::find(seen, d->id) != seen.end()) continue;
                seen.push_back(d->id);
                SyntaxStructure all;
                if (d->syntax.active() && d->syntax.ask_all(d->ed.buf, &all))
                    for (LspSymbol& y : tree_symbols(*d, all)) rows.push_back(std::move(y));
            }
        if (!rows.empty()) {
            lsp_close(kPopAll);
            a.symbol_rows = std::move(rows);
            std::vector<std::string> items;
            const std::string* root = panel_root();
            for (const LspSymbol& y : a.symbol_rows) {
                std::string row = y.name;
                if (const char* k = symbol_kind_name(y.kind); *k) row += std::string("  \xC2\xB7 ") + k;
                row += "  \xC2\xB7 " + shown_name(y.path, root) + ":" + std::to_string(y.line + 1);
                items.push_back(std::move(row));
            }
            find_choose_item("Symbols in the open files", std::move(items), [this](int i) {
                if (i >= 0) lsp_go_symbol(static_cast<size_t>(i));
            });
            return;
        }
        if (server_text(*pane_->doc))
            notify(lsp_.ready() ? lsp_.server_name() + " does not find the project's symbols" : why_no_server(cfg_, lsp_, lsp_doc_ == pane_->doc, a.failed, *pane_->doc));
        return;
    }
    lsp_close(kPopAll);
    a.symbol_rows.clear();
    find_live_items("Symbols in the project", [this](const std::string& typed) { lsp_.find_symbols(typed); },
                    [this](int i) {
                        if (i >= 0) lsp_go_symbol(static_cast<size_t>(i));
                    });
    a.symbols_live = true;
}

void App::lsp_go_symbol(size_t i) {
    Assist& a = *assist_;
    if (i >= a.symbol_rows.size()) return;
    const LspSymbol y = a.symbol_rows[i];
    const u64 key = lsp_.key_of(y.path);
    Pane* where = nullptr;
    size_t at = 0;
    remember_place();
    if (key && find_doc(key, &where, &at)) {
        focus_pane(where);
        tab_show(at);
        pane_->doc->ed.move_to(Lsp::offset_at(pane_->doc->ed.buf, y.line, y.character, lsp_.counts_utf16()), false);
        scroll_to_cursor();
    } else if (go_to_place(y.path, y.line + 1, 1)) {
        // (Its column is as the server counts: worked out against the text,
        // once it is here.)
        Doc& d = *pane_->doc;
        if (d.loading) {
            d.goto_col = y.character;
            d.goto_utf16 = lsp_.counts_utf16();
        } else {
            d.ed.move_to(Lsp::offset_at(d.ed.buf, y.line, y.character, lsp_.counts_utf16()), false);
            scroll_to_cursor();
        }
    }
}

void App::lsp_everywhere_ask(const std::string& typed) {
    Assist& a = *assist_;
    a.symbols_live = false;
    if (typed.empty() || !lsp_.ready() || !lsp_.can_find_symbols()) {
        a.symbol_rows.clear();
        return;
    }
    lsp_.find_symbols(typed);  // (the rows there are stay, those that still match, until the answer comes)
}

std::vector<std::pair<std::string, std::string>> App::lsp_symbol_rows() const {
    const Assist& a = *assist_;
    std::vector<std::pair<std::string, std::string>> rows;
    const std::string* root = panel_root();
    for (const LspSymbol& y : a.symbol_rows) {
        std::string what = symbol_kind_name(y.kind);
        if (!y.container.empty()) what += (what.empty() ? "in " : " in ") + y.container;
        what += (what.empty() ? "" : "  \xC2\xB7 ") + shown_name(y.path, root) + ":" + std::to_string(y.line + 1);
        rows.emplace_back(y.name, std::move(what));
    }
    return rows;
}

void App::lsp_rename() {
    Assist& a = *assist_;
    if (!lsp_.ready() || !lsp_.can_rename()) {
        if (server_text(*pane_->doc))
            notify(lsp_.ready() ? lsp_.server_name() + " does not rename" : why_no_server(cfg_, lsp_, lsp_doc_ == pane_->doc, a.failed, *pane_->doc));
        return;
    }
    lsp_close(kPopAll);
    a.rename_pending = true;
    a.rename_doc = pane_->doc->id;
    lsp_.prepare_rename(pane_->doc->ed.cursor());
}

void App::lsp_format() {
    Doc& d = *pane_->doc;
    u64 lo = d.ed.sel_min(), hi = d.ed.sel_max();
    if (cfg_.modal) {
        vim_attach();
        if (!vim_selection(&lo, &hi)) lo = hi = 0;
    }
    if (hi > lo) {
        const Buffer& b = d.ed.buf;
        return lsp_format_lines(b.line_of(lo), b.line_of(hi > lo ? hi - 1 : hi));
    }
    lsp_format_lines(~0ull, ~0ull);
}

// Lines l0 to l1 (both ~0: the whole file).
void App::lsp_format_lines(u64 l0, u64 l1) {
    Assist& a = *assist_;
    const bool whole = l0 == ~0ull;
    if (!lsp_.ready() || !lsp_.can_format(!whole)) {
        if (server_text(*pane_->doc))
            notify(lsp_.ready() ? lsp_.server_name() + " does not format" : why_no_server(cfg_, lsp_, lsp_doc_ == pane_->doc, a.failed, *pane_->doc));
        return;
    }
    lsp_close(kPopAll);
    const Buffer& b = pane_->doc->ed.buf;
    u64 lo = 0, hi = 0;
    if (!whole) {
        l1 = std::min(std::max(l0, l1), b.line_count() - 1);
        lo = b.line_start(std::min(l0, l1));
        hi = l1 + 1 < b.line_count() ? b.line_start(l1 + 1) : b.size();
    }
    a.format_pending = true;
    a.format_quiet = false;
    lsp_.format(lo, hi, whole);
}

// Before the file in front is written: formatted, if its server formats,
// waiting a second at most for it (whatever else arrives meanwhile is taken
// in as usual).
void App::lsp_format_on_save() {
    Assist& a = *assist_;
    if (!cfg_.lsp || !cfg_.lsp_format_on_save) return;
    Doc& d = *pane_->doc;
    if (!lsp_.is_open(d.id)) return;
    lsp_.follow(d.id);
    lsp_doc_ = &d;
    if (!lsp_.can_format(false)) return;
    a.format_pending = true;
    a.format_quiet = true;
    lsp_.format(0, 0, true, d.id);
    const u64 until = now_ns() + 1'000'000'000ull;
    while (a.format_pending && now_ns() < until) {
        lsp_.wait(10);
        sync_lsp();
    }
    a.format_pending = false;
}

void App::lsp_actions() {
    Assist& a = *assist_;
    if (spell_fix()) return;  // (on a word underlined as wrong: its spelling, appspell.cpp)
    if (!lsp_.ready() || !lsp_.can_act()) {
        if (server_text(*pane_->doc))
            notify(lsp_.ready() ? lsp_.server_name() + " offers no actions" : why_no_server(cfg_, lsp_, lsp_doc_ == pane_->doc, a.failed, *pane_->doc));
        return;
    }
    lsp_close(kPopAll);
    Editor& ed = pane_->doc->ed;
    u64 lo = ed.sel_min(), hi = ed.sel_max();
    if (cfg_.modal) {
        vim_attach();
        if (!vim_selection(&lo, &hi)) lo = hi = ed.cursor();
    }
    a.actions_pending = true;
    a.actions_doc = pane_->doc->id;
    lsp_.code_actions(lo, hi);
}

// The mark in the gutter beside a line with a problem the server can fix:
// one it said so of (clangd says it of each problem), or one where Option+Enter
// found a fix.
u32 App::lsp_gutter(const Doc& d, u64 line) {
    Assist& a = *assist_;
    const std::vector<Diagnostic>* diags = lsp_.diagnostics_of(d.id);
    if (!diags || diags->empty()) return 0;
    if (a.marks_doc != d.id || a.marks_gen != lsp_marks_gen_) {
        a.marks_doc = d.id;
        a.marks_gen = lsp_marks_gen_;
        std::vector<u64> lines;
        for (const Diagnostic& x : *diags)
            if (x.fixes) lines.push_back(d.ed.buf.line_of(std::min(x.start, d.ed.buf.size())));
        if (d.id == a.actions_doc)
            for (const u64 l : a.action_lines) lines.push_back(l);
        std::sort(lines.begin(), lines.end());
        lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
        a.fix_lines = std::move(lines);
    }
    if (!std::binary_search(a.fix_lines.begin(), a.fix_lines.end(), line)) return 0;
    const char32_t bulb = font_->has(0xF0335) ? 0xF0335 : 0x2022;  // (a Nerd Font's light bulb, else a dot)
    return (font_->glyph(bulb) & (Font::kWide - 1)) | (kInkDiagnostic + 1) << kCellInkShift;
}

// (Tests.) The view in front's inlay hints as they are put in ("line:col
// label"), and the servers' colours laid over tree-sitter's ("text=capture").
std::string App::lsp_extras_dump() {
    const Assist::Extras& x = assist_->extras[pane_->view];
    std::string out = "hints:";
    for (size_t i = 0; i < x.notes.size() && i < x.labels.size(); ++i)
        out += " " + std::to_string(x.notes[i].line + 1) + ":" + std::to_string(x.notes[i].col + 1) + "[" + x.labels[i] + "]";
    out += " | colours:";
    const Highlights& m = x.merged;
    if (x.doc == pane_->doc->id)
        for (size_t i = 0; i < m.size(); ++i) {
            if (m.style[i] < x.styles || m.style[i] - x.styles >= x.made.size()) continue;
            const u64 a = m.at(i), b = i + 1 < m.size() ? m.at(i + 1) : m.end;
            out += " " + pane_->doc->ed.buf.text(a, std::min<u64>(b - a, 40)) + "=" + x.made[m.style[i] - x.styles];
        }
    return out;
}

std::string App::lsp_dump() {
    std::string out = lsp_.servers_dump() + " |";
    for (Doc* d : open_docs()) {
            if (d->ed.path.empty()) continue;
            const size_t slash = d->ed.path.rfind('/');
            out += " " + d->ed.path.substr(slash == std::string::npos ? 0 : slash + 1) + (lsp_.is_open(d->id) ? "+" : "-");
        }
    const std::vector<Diagnostic>* front = lsp_.diagnostics_of(pane_->doc->id);
    out += " | front " + std::string(lsp_doc_ == pane_->doc ? "followed" : "not followed") + " " + std::to_string(front ? front->size() : 0);
    return out;
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
    if (item.snippet) {  // with its tab stops: the edits elsewhere first (all before it), then it
        u64 at = from, end = cur;
        std::vector<CompletionItem::Extra> extra = item.extra;
        std::sort(extra.begin(), extra.end(), [](const auto& x, const auto& y) { return x.start > y.start; });
        for (const CompletionItem::Extra& x : extra) {
            if (x.end > from || x.start > x.end) continue;
            pane_->doc->ed.replace(x.start, x.end, x.text);
            at = at + x.text.size() - (x.end - x.start);
            end = end + x.text.size() - (x.end - x.start);
        }
        snippet_expand(item.insert, at, end, {});
        return;
    }
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
    if (at_cursor && !lsp_.ready() && server_text(*pane_->doc)) {  // (Vim's K, with nobody to ask)
        notify(why_no_server(cfg_, lsp_, lsp_doc_ == pane_->doc, a.failed, *pane_->doc));
        return;
    }
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

// Milliseconds until the mouse has rested long enough for a hover, or a
// server with no documents left is to stop; -1 if neither.
int App::lsp_wait_ms() const {
    const Assist& a = *assist_;
    u64 due = a.hover_due_ns;
    if (const u64 stop = lsp_.next_tick_ns(); stop && (!due || stop < due)) due = stop;
    if (a.extras_due_ns && (!due || a.extras_due_ns < due)) due = a.extras_due_ns;
    if (a.uses_due_ns && (!due || a.uses_due_ns < due)) due = a.uses_due_ns;
    if (a.lens_due_ns && (!due || a.lens_due_ns < due)) due = a.lens_due_ns;
    if (!due) return -1;
    const u64 now = now_ns();
    return due <= now ? 0 : static_cast<int>(std::min<u64>((due - now) / 1'000'000 + 1, 1u << 30));
}

void App::lsp_tick() {
    Assist& a = *assist_;
    if (const u64 stop = lsp_.next_tick_ns(); stop && now_ns() >= stop) lsp_.tick();
    if (a.extras_due_ns && now_ns() >= a.extras_due_ns) {  // (a frame asks for the hints and colours now due)
        a.extras_due_ns = 0;
        dirty_ = true;
    }
    if (a.lens_due_ns && now_ns() >= a.lens_due_ns) dirty_ = true;  // (typing has paused: a frame asks for the counts)
    if (a.uses_due_ns && now_ns() >= a.uses_due_ns) {  // (the cursor has rested: the uses of what it is on)
        a.uses_due_ns = 0;
        const Editor& ed = pane_->doc->ed;
        auto word = [&](u64 at) {  // (on a name, or just after one)
            char c = 0;
            return at < ed.buf.size() && ed.buf.read(at, &c, 1) == 1 &&
                   (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80);
        };
        const u64 cur = ed.cursor();
        if (cfg_.lsp_uses && lsp_.ready() && lsp_doc_ == pane_->doc && lsp_.can_highlight() && !ed.has_selection() &&
            (word(cur) || (cur > 0 && word(cur - 1)))) {
            a.uses_ask_doc = pane_->doc->id;
            a.uses_ask_rev = ed.revision();
            lsp_.highlight(cur);
        }
    }
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
    const i64 line = ty < 0 ? -1 : static_cast<i64>(line_of_row(static_cast<u64>(ty / ch)));
    i64 col = tx < 0 ? -1 : static_cast<i64>(tx / cw);
    if (const Wrap* w = wrap_on(); w && ty >= 0 && col >= 0) col += w->start_col(static_cast<u64>(ty / ch));  // (a wrapped line's row)
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
    if (!lsp_.ready()) {  // (Vim's gd, with nobody to ask)
        if (server_text(*pane_->doc)) notify(why_no_server(cfg_, lsp_, lsp_doc_ == pane_->doc, a.failed, *pane_->doc));
        return;
    }
    const LspLocation loc = lsp_.definition_target();
    if (!loc.valid()) return;
    const bool utf16 = lsp_.counts_utf16();
    const Assist::Place here{lsp_.path(), pane_->doc->ed.cursor()};
    jump_record();  // (Ctrl+- comes back here: app.cpp)
    if (loc.path != lsp_.path() && !open_path(loc.path.c_str())) return;  // in another file, which gets a tab
    a.back.push_back(here);
    if (a.back.size() > 64) a.back.erase(a.back.begin());
    if (pane_->doc->loading) {  // (on its way from another machine: the place, when it is here)
        pane_->doc->goto_line = loc.line;
        pane_->doc->goto_col = loc.character;
        pane_->doc->goto_utf16 = utf16;
        return;
    }
    pane_->doc->ed.move_to(Lsp::offset_at(pane_->doc->ed.buf, loc.line, loc.character, utf16), false);
    scroll_to_cursor();
}

// Notes where the cursor is, as somewhere go_back() returns to (before
// going to a file or a line that was searched for).
void App::remember_place() {
    jump_record();  // (into the one list Ctrl+- walks: app.cpp)
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

bool App::lsp_completing() const { return assist_->comp_open; }

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
    // References, rename, formatting and code actions (each says why, when
    // nobody can answer). (The text Option+Shift+F types is not typed.)
    if (k.key == SDLK_F12 && shift && !ctrl && !alt && !cmd) return lsp_references(), true;
    if (k.key == SDLK_F2 && !shift && !ctrl && !alt && !cmd) return lsp_rename(), true;
    if (k.key == SDLK_F && alt && shift && !ctrl && !cmd) {
        lsp_format();
        term_drop_text_ = true;
        return true;
    }
    if ((k.key == SDLK_RETURN || k.key == SDLK_KP_ENTER) && alt && !shift && !ctrl && !cmd) {
        lsp_actions();
        term_drop_text_ = true;
        return true;
    }
    if (!lsp_.ready()) {
        // The keys that ask a server for something say why nothing comes of them.
        const bool asks = (ctrl && k.key == SDLK_SPACE) || (alt && k.key == SDLK_ESCAPE) || (cmd && k.key == SDLK_I) || (k.key == SDLK_F12 && !cmd && !alt);
        if (!asks || !server_text(*pane_->doc)) return false;
        notify(why_no_server(cfg_, lsp_, lsp_doc_ == pane_->doc, a.failed, *pane_->doc));
        return true;
    }
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
    if (k.key == SDLK_F12 && !shift && !alt && !cmd && !ctrl) {
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
    static const std::vector<Diagnostic> none;
    const std::vector<Diagnostic>* of = lsp_.diagnostics_of(pane_->doc->id);  // (the split's own file's, whether or not it is followed yet)
    const std::vector<Diagnostic>& diags = of ? *of : none;
    if (diags.empty()) {
        a.decos.clear();
        a.notes.clear();
        a.deco_gen = ~0ull;
        return;
    }
    // The first column the view's right edge cuts (a message stops short of it).
    const i64 right = std::llround(pane_->scroll_x) - pad_x_ + (pane_->x1 - pane_->text_x);
    const u32 edge = right > 0 ? static_cast<u32>(right / font_->cell_w()) : 0;
    if (a.deco_gen == lsp_.diagnostics_generation() && a.deco_top == g.top_line && a.deco_rows == g.rows && a.deco_key == g.rows_key && a.deco_doc == pane_->doc->id &&
        a.deco_rev == pane_->doc->ed.revision() && a.deco_line == pane_->cur_line && a.deco_font == font_->generation() && a.deco_edge == edge)
        return;
    a.deco_gen = lsp_.diagnostics_generation();
    a.deco_doc = pane_->doc->id;
    a.deco_top = g.top_line;
    a.deco_rows = g.rows;
    a.deco_key = g.rows_key;
    a.deco_rev = pane_->doc->ed.revision();
    a.deco_line = pane_->cur_line;
    a.deco_font = font_->generation();
    a.deco_edge = edge;
    a.decos.clear();
    a.notes.clear();
    a.note_cells.clear();

    const Buffer& b = pane_->doc->ed.buf;
    const u64 lines = b.line_count();
    const u64 first = g.top_line, last = std::min<u64>(lines, g.bottom());  // [first, last)
    if (first >= last) return;
    const u64 lo = b.line_start(first), hi = last >= lines ? b.size() + 1 : b.line_start(last);
    // The first problem that reaches into view: the furthest any reaches,
    // of those up to each one, only grows, so it can be searched for.
    if (a.reach_gen != lsp_.diagnostics_generation() || a.reach.size() != diags.size() || a.reach_doc != pane_->doc->id) {
        a.reach_gen = lsp_.diagnostics_generation();
        a.reach_doc = pane_->doc->id;
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
        // The cursor's line shows its first error in words. (Warnings and
        // hints keep to their underline: the hover and the Problems tab say them.)
        if (cfg_.lsp_inline && !note && d.severity == 1 && b.line_of(d.start) <= pane_->cur_line && b.line_of(d.end) >= pane_->cur_line &&
            pane_->cur_line >= first && pane_->cur_line < last)
            note = &d;
    }
    if (note) {
        // Three columns after the code, and cut short of the view's edge; with
        // only a few columns left there, not at all.
        const u64 ls = b.line_start(pane_->cur_line);
        const u32 col = pane_->doc->ed.col_between(ls, pane_->doc->ed.line_end(pane_->cur_line)) + 3;
        const u32 room = edge > col + 1 ? edge - col - 1 : 0;
        if (room >= 8) {
            const std::string text = fit_text(std::string_view(note->message).substr(0, note->message.find('\n')), room);
            TextBox box;
            box.resize(std::max<u32>(width_of(text), 1), 1);
            put(box, *font_, 0, 0, text, ink::kError);
            a.note_cells = std::move(box.cells);
            a.notes.push_back({pane_->cur_line, col, a.note_cells.data(), static_cast<u32>(a.note_cells.size())});
        }
    }
}

// Adds the language server's part of a frame: underlines, and whichever
// popups are open.
void App::lsp_layout(int w, int h, FrameDesc& fd) {
    Assist& a = *assist_;
    popups_drawn_ = 0;
    if (a.refs_picker && !find_open_) a.refs_picker = false;
    if (find_open_ && !a.refs_picker) a.refs_live = false;  // (another list in the box: F4 is its)
    lsp_extras(fd);
    if (!lsp_.attached() && !lsp_.diagnostics_of(pane_->doc->id)) return;
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
        // The uses of the symbol at the cursor: asked for once it has rested
        // (lsp_tick), shown while it stays on one of them and the text is
        // as it was when they were found.
        const Editor& ed = pane_->doc->ed;
        const u64 cur = ed.cursor();
        if (cur != a.uses_seen_cur || pane_->doc->id != a.uses_seen_doc || ed.revision() != a.uses_seen_rev) {
            a.uses_seen_cur = cur, a.uses_seen_doc = pane_->doc->id, a.uses_seen_rev = ed.revision();
            a.uses_due_ns = cfg_.lsp_uses ? now_ns() + 120'000'000 : 0;
        }
        if (lsp_uses_shown()) {
            a.decos_all.assign(a.decos.begin(), a.decos.end());
            const u64 lines = ed.buf.line_count(), bottom = std::min<u64>(grid.bottom(), lines);
            const u64 from = ed.buf.line_start(std::min(grid.top_line, lines ? lines - 1 : 0)), to = bottom < lines ? ed.buf.line_start(bottom) : ed.buf.size() + 1;
            for (auto it = std::ranges::lower_bound(a.uses, from, {}, &std::pair<u64, u64>::first); it != a.uses.end() && it->first < to; ++it) {  // (those on show alone)
                const auto [lo, hi] = *it;
                const u64 line = ed.buf.line_of(lo);
                if (ed.buf.line_of(hi) != line) continue;
                const u64 start = ed.buf.line_start(line);
                const u32 c0 = ed.col_between(start, lo), c1 = ed.col_between(start, hi);
                a.decos_all.push_back({line, c0, std::max(c1, c0 + 1), kDecoMatch});
            }
            grid.decos = a.decos_all;
        }
    }

    const int cw = font_->cell_w(), ch = font_->cell_h();
    const i64 sx = std::llround(pane_->scroll_x), sy = std::llround(pane_->scroll_y);
    const Look lk = look();
    const int pad_x = lk.edge, pad_y = lk.pad_y, gap = lk.gap;
    const float border = static_cast<float>(lk.hair);
    // Pixel position of the top-left of the character cell at an offset.
    auto at_offset = [&](u64 off, int* x, int* y) {
        const u64 line = pane_->doc->ed.buf.line_of(off);
        *x = static_cast<int>(pane_->text_x + pad_x_ + static_cast<i64>(pane_->doc->ed.col_between(pane_->doc->ed.buf.line_start(line), off)) * cw - sx);
        *y = static_cast<int>(pane_->text_y + static_cast<i64>(row_of_line(line)) * ch - sy);
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
        l.mark_ink = ink::kAccentStrong.ink;  // (the argument being typed: where the cursor is)
        l.mark_style = ink::kAccentStrong.style;
        if (sig.count > 1) l.text += "   " + std::to_string(sig.index + 1) + "/" + std::to_string(sig.count);
        lines.push_back(std::move(l));
        for (const InfoLine& d : sig.doc) {
            if (lines.size() >= 4) break;
            lines.push_back({d.text, kInkMuted, 0, false, 0, 0, 0, 0});
        }
        set_lines(a.sig_box, *font_, wrap(lines, std::min<u32>(max_cols, kInfoCols), 6));
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
                c = put(a.comp_box, *font_, r, c, std::string_view(it.label).substr(i, d.len), matched ? ink::kAccentStrong : ink::kText);
                i += d.len;
            }
            if (detail_w) put(a.comp_box, *font_, r, 2 + label_w + 2, it.detail, ink::kMuted);
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
        set_lines(a.hover_box, *font_, wrap(a.hover_lines, std::min<u32>(max_cols, kInfoCols), 18));
        int x, y;
        at_offset(a.hover_at_cursor ? pane_->doc->ed.cursor() : a.hover_w0, &x, &y);
        if (!place(a.hover_box, x, y, !a.sig_open, &a.hover_rect, kFxHover)) a.hover_rect = {};
    }
    popups_drawn_ = fd.popup_count;
}

u32 App::lsp_popups_open() const { return popups_drawn_; }

// The settings changed: every server stops, to be started again as they
// now say, and those that failed may be tried again.
void App::lsp_forgive() {
    assist_->failed.clear();
    assist_->tried.clear();
    lsp_doc_ = nullptr;
    lsp_.shutdown();
}

// ---------------------------------------------------------------- inlay hints and semantic colours

namespace {

// How long a text stands still before its hints and colours are asked for
// again (each key would otherwise ask, and the answers be out of date).
constexpr u64 kSettleNs = 250'000'000;

// The servers' kinds of token that say more than tree-sitter does, and the
// capture each is coloured as when the theme has no "lsp.KIND" of its own.
// (Keywords, strings, numbers, comments and operators are left to
// tree-sitter, which tells more of them apart; so are plain variables.)
struct TokenCapture {
    std::string_view type, capture, builtin;
};
constexpr TokenCapture kTokenCaptures[] = {
    {"namespace", "module", "module"},
    {"type", "type", "type.builtin"},
    {"class", "type", "type.builtin"},
    {"enum", "type", "type.builtin"},
    {"interface", "type", "type.builtin"},
    {"struct", "type", "type.builtin"},
    {"concept", "type", "type"},
    {"typeParameter", "type.parameter", "type.parameter"},
    {"parameter", "variable.parameter", "variable.parameter"},
    {"property", "property", "property"},
    {"enumMember", "constant", "constant.builtin"},
    {"function", "function", "function.builtin"},
    {"method", "function.method", "function.builtin"},
    {"macro", "function.macro", "function.macro"},
    {"decorator", "attribute", "attribute"},
};

}  // namespace

// Each view's inlay hints (as notes between characters) and the servers'
// colours (a layer of marks over tree-sitter's, for the lines on show),
// asked for once a text has stood still a moment, and worked out again only
// when what they are made from changes.
void App::lsp_extras(FrameDesc& fd) {
    Assist& a = *assist_;
    a.extras_due_ns = 0;
    if (!cfg_.lsp || (!cfg_.lsp_inlay_hints && !cfg_.lsp_semantic)) return;
    const u64 now = now_ns();
    for (const std::unique_ptr<Pane>& pp : panes_) {
        Pane& p = *pp;
        if (!p.doc || p.view >= fd.view_count) continue;
        Doc& d = *p.doc;
        GridSpec& g = fd.views[p.view].grid;
        if (fd.views[p.view].buf != &d.ed.buf || g.doc != d.id || !lsp_.is_open(d.id)) continue;
        Assist::Extras& x = a.extras[p.view];
        if (x.doc != d.id || x.rev != d.ed.revision()) {
            x.rev_ns = x.doc == d.id ? now : 0;  // (another file in the view: asked about at once)
            x.doc = d.id;
            x.rev = d.ed.revision();
            x.hints_key = x.merged_key = ~0ull;
        }
        const bool settled = now - x.rev_ns >= kSettleNs;
        if (!settled && (!a.extras_due_ns || x.rev_ns + kSettleNs < a.extras_due_ns)) a.extras_due_ns = x.rev_ns + kSettleNs;
        const Buffer& b = d.ed.buf;
        const u64 lines = b.line_count();
        const u64 first = g.top_line, last = std::min<u64>(lines, g.bottom());  // [first, last)
        if (first >= last) continue;

        // Inlay hints: those on show, but not on the cursor's line, where
        // the cursor goes by the text's own columns.
        if (cfg_.lsp_inlay_hints && lsp_.can_hint(d.id)) {
            if (settled && !lsp_.hints_cover(d.id, first, last - 1))
                lsp_.ask_hints(d.id, first > g.rows / 2 ? first - g.rows / 2 : 0, last + g.rows / 2);
            const u64 key = (((lsp_.hints_generation(d.id) * 1000003 + first) * 1000003 + last) * 1000003 + p.cur_line) * 1000003 + font_->generation();
            if (key != x.hints_key) {
                x.hints_key = key;
                x.notes.clear();
                x.cells.clear();
                x.labels.clear();
                const std::vector<InlayHint>& hints = *lsp_.hints(d.id);
                const u64 lo = b.line_start(first), hi = last < lines ? b.line_start(last) : b.size() + 1;
                auto from = std::lower_bound(hints.begin(), hints.end(), lo, [](const InlayHint& h, u64 off) { return h.off < off; });
                std::vector<std::pair<size_t, u32>> spans;  // (where in cells, how many) of each note
                TextBox box;
                for (auto it = from; it != hints.end() && it->off < hi; ++it) {
                    const u64 line = b.line_of(std::min(it->off, b.size()));
                    if (line == p.cur_line) continue;
                    const std::string text = (it->pad_left ? " " : "") + fit_text(it->label, 40) + (it->pad_right ? " " : "");
                    box.resize(std::max<u32>(1, width_of(text)), 1);
                    put(box, *font_, 0, 0, text, ink::kMuted);
                    x.notes.push_back({line, d.ed.col_between(b.line_start(line), it->off), nullptr, box.cols});
                    spans.push_back({x.cells.size(), box.cols});
                    x.labels.push_back(text);
                    x.cells.insert(x.cells.end(), box.cells.begin(), box.cells.end());
                }
                for (size_t i = 0; i < x.notes.size(); ++i) x.notes[i].cells = x.cells.data() + spans[i].first;
                // (In order of line and column: they come so, as the hints are sorted by place.)
            }
            g.inserts = x.notes;
        }

        // The servers' colours, laid over tree-sitter's where they say more.
        // (A file with no tree-sitter colours of its own gets the server's alone.)
        const std::vector<SemanticToken>* toks = lsp_.tokens(d.id);
        const Highlights* own = d.syntax.active() ? &d.syntax.highlights() : nullptr;
        static const Highlights kNone;
        if (cfg_.lsp_semantic && lsp_.can_tokens(d.id) && !d.marks && g.highlights == own) {
            if (settled && !lsp_.tokens_current(d.id)) lsp_.ask_tokens(d.id);
            if (!toks || toks->empty()) continue;
            // The style table: tree-sitter's styles as pick_theme() made
            // them, then one for each kind of token, in an ink of its own
            // (or one already showing that colour).
            const std::vector<std::string>& types = *lsp_.token_types(d.id);
            const u32 styles = own ? static_cast<u32>(std::min<size_t>(d.syntax.style_names().size(), 256)) : 1;
            bool inks_hold = true;
            for (const auto& [ink, c] : x.inks)
                inks_hold = inks_hold && theme_.palette[ink][0] == c.r && theme_.palette[ink][1] == c.g && theme_.palette[ink][2] == c.b && theme_.palette[ink][3] == 1;
            const u64 lut_key = (reinterpret_cast<u64>(&types) * 31 + styles) * 2 + (SDL_GetSystemTheme() != SDL_SYSTEM_THEME_LIGHT ? 1 : 0);
            if (lut_key != x.lut_key || !inks_hold || x.styles != styles || (own && std::memcmp(x.lut, d.style_bits, styles * sizeof(u32)) != 0)) {
                x.lut_key = lut_key;
                x.styles = styles;
                x.merged_key = ~0ull;
                if (own) std::memcpy(x.lut, d.style_bits, styles * sizeof(u32));
                std::fill(x.lut + (own ? styles : 0), x.lut + 256, 0u);
                x.inks.clear();
                x.type_style.assign(types.size(), 0);
                x.builtin_style.assign(types.size(), 0);
                const ThemeColors& tc = cfg_.theme(SDL_GetSystemTheme() != SDL_SYSTEM_THEME_LIGHT);
                u32 next = styles;
                std::vector<std::string>& made = x.made;
                made.clear();
                auto style_of = [&](const std::string& capture) -> u8 {
                    for (size_t i = 0; i < made.size(); ++i)
                        if (made[i] == capture) return static_cast<u8>(styles + i);
                    if (next >= 256) return 0;
                    const TokenStyle st = tc.syntax_style(capture);
                    u32 face = cfg_.font_style_of(st);
                    if (!font_->ensure_style(face)) face = 0;
                    // The ink: one already that colour, else a free one.
                    u32 ink = 0;
                    for (u32 i = 1; i < kInkSyntaxEnd && !ink; ++i)
                        if (theme_.palette[i][3] == 1 && theme_.palette[i][0] == st.color.r && theme_.palette[i][1] == st.color.g && theme_.palette[i][2] == st.color.b) ink = i;
                    for (u32 i = 1; i < kInkSyntaxEnd && !ink; ++i)
                        if (theme_.palette[i][3] == 0) {
                            ink = i;
                            std::ranges::copy(std::array<float, 4>{st.color.r, st.color.g, st.color.b, 1}, theme_.palette[i]);
                            std::ranges::copy(std::array<float, 4>{st.color.r, st.color.g, st.color.b, 1}, fd.theme.palette[i]);  // (this frame's copy was taken already)
                        }
                    if (ink) x.inks.push_back({ink, st.color});
                    x.lut[next] = (ink << kCellInkShift) | (face << Font::kStyleShift);
                    made.push_back(capture);
                    return static_cast<u8>(next++);
                };
                for (size_t t = 0; t < types.size(); ++t)
                    for (const TokenCapture& tcap : kTokenCaptures) {
                        if (types[t] != tcap.type) continue;
                        // A theme's own "lsp.KIND" first, else the capture tree-sitter would use.
                        const std::string theme_key = "lsp." + types[t];
                        const bool themed = std::ranges::any_of(tc.syntax, [&](const ThemeColors::Entry& e) { return e.capture == theme_key; });
                        x.type_style[t] = style_of(themed ? theme_key : std::string(tcap.capture));
                        x.builtin_style[t] = style_of(themed ? theme_key : std::string(tcap.builtin));
                    }
                dirty_ = true;
            }
            // The marks for the lines on show: tree-sitter's, with the
            // tokens' runs laid over them.
            const Highlights& hl = own ? *own : kNone;
            const u64 merged_key = ((((lsp_.tokens_generation(d.id) * 1000003 + first) * 1000003 + last) * 1000003 + reinterpret_cast<u64>(hl.off.data())) * 1000003 +
                                    hl.size()) * 1000003 + hl.base + x.lut_key;
            if (merged_key != x.merged_key) {
                x.merged_key = merged_key;
                const std::vector<std::string>& mods = *lsp_.token_modifiers(d.id);
                u32 library = 0;  // (the modifier bit for "defaultLibrary")
                for (size_t i = 0; i < mods.size() && i < 32; ++i)
                    if (mods[i] == "defaultLibrary") library = 1u << i;
                const u64 lo = b.line_start(first), hi = last < lines ? b.line_start(last) : b.size();
                Highlights& m = x.merged;
                m.clear();
                m.base = lo;
                m.end = hi;
                size_t si = hl.empty() ? 0 : hl.upper(lo);  // tree-sitter's next mark
                u8 ts = si > 0 && lo < hl.end ? hl.style[si - 1] : 0;
                auto put_mark = [&](u64 at, u8 style) {
                    if (!m.off.empty() && m.style.back() == style) return;
                    if (!m.off.empty() && m.off.back() == static_cast<u32>(at - lo)) {
                        m.style.back() = style;
                        return;
                    }
                    m.off.push_back(static_cast<u32>(at - lo));
                    m.style.push_back(style);
                };
                // Tree-sitter's marks from `from` up to `to`, with the style in force at `from`.
                auto syntax_to = [&](u64 from, u64 to) {
                    while (si < hl.size() && hl.at(si) <= from) ts = hl.style[si++];
                    if (from >= hl.end) ts = 0;
                    put_mark(from, ts);
                    while (si < hl.size() && hl.at(si) < to) {
                        ts = hl.style[si];
                        put_mark(hl.at(si++), ts);
                    }
                };
                u64 pos = lo;
                auto t = std::lower_bound(toks->begin(), toks->end(), lo, [](const SemanticToken& k, u64 off) { return k.end <= off; });
                for (; t != toks->end() && t->start < hi; ++t) {
                    if (t->type >= x.type_style.size()) continue;
                    const u8 style = (t->mods & library) ? x.builtin_style[t->type] : x.type_style[t->type];
                    const u64 s0 = std::max(t->start, lo), s1 = std::min(t->end, hi);
                    if (!style || s0 < pos || s1 <= s0) continue;
                    if (s0 > pos) syntax_to(pos, s0);
                    put_mark(s0, style);
                    pos = s1;
                }
                if (pos < hi) syntax_to(pos, hi);
                m.seal();
            }
            g.highlights = &x.merged;
            g.style_bits = x.lut;
        }
    }
}


// ---------------------------------------------------------------- call and type hierarchies

// The calls to the function at the cursor (or from it), or the type's
// supertypes (or subtypes): in the find box, a level at a time.
void App::hier_show(u8 kind) {
    Assist& a = *assist_;
    const bool types = kind >= Lsp::kSupertypes;
    if (!lsp_.ready() || !lsp_.can_hierarchy(types)) {
        if (server_text(*pane_->doc))
            notify(lsp_.ready() ? lsp_.server_name() + (types ? " has no type hierarchy" : " has no call hierarchy")
                                : why_no_server(cfg_, lsp_, lsp_doc_ == pane_->doc, a.failed, *pane_->doc));
        return;
    }
    lsp_close(kPopAll);
    a.hier_rows.clear();
    a.hier_kind = kind;
    a.hier_asking = -1;
    a.hier_waiting = true;
    lsp_.hierarchy_prepare(pane_->doc->ed.cursor(), types);
}

namespace {

std::string hier_row_text(const HierRow& r, bool leaf_known, const std::string* root) {
    std::string row(2 * std::min<u32>(r.depth, 12), ' ');
    row += r.open ? "\xE2\x96\xBE " : leaf_known ? "  " : "\xE2\x96\xB8 ";  // ▾ open, ▸ not yet
    // (clangd's detail is the name with its scope: that, in the name's place.)
    const std::string& d = r.item.detail;
    const bool scoped = d.size() > r.item.name.size() + 2 && d.ends_with("::" + r.item.name);
    row += scoped ? d : r.item.name;
    if (!scoped && !d.empty() && d != r.item.name && d.size() < 60) row += (d.front() == '(' ? "" : " ") + d;
    if (const char* k = symbol_kind_name(static_cast<u8>(r.item.kind)); *k) row += std::string("  \xC2\xB7 ") + k;
    row += "  \xC2\xB7 " + shown_name(r.item.at.path, root) + ":" + std::to_string(r.item.at.line + 1);
    return row;
}

}  // namespace

std::vector<std::string> App::hier_texts() {
    Assist& a = *assist_;
    const std::string* root = panel_root();
    std::vector<std::string> out;
    for (size_t i = 0; i < a.hier_rows.size(); ++i) {
        const HierRow& r = a.hier_rows[i];
        const bool leaf = r.open || (i + 1 < a.hier_rows.size() && a.hier_rows[i + 1].depth > r.depth);
        out.push_back(hier_row_text(r, leaf && !r.open, root));
    }
    return out;
}

// The server's answer: the first items (shown, and the first opened a
// level), or the level under the row asked about.
void App::hier_take() {
    Assist& a = *assist_;
    a.hier_waiting = false;
    std::vector<LspHierItem> items = lsp_.hierarchy_list();
    static const char* const kTitles[] = {"Calls to ", "Calls from ", "Supertypes of ", "Subtypes of "};
    if (a.hier_asking < 0) {
        if (items.empty()) {
            notify(a.hier_kind >= Lsp::kSupertypes ? "No type here" : "No function here");
            return;
        }
        for (LspHierItem& it : items) a.hier_rows.push_back({std::move(it), 0, false});
        a.hier_title = kTitles[a.hier_kind] + a.hier_rows[0].item.name;
        find_choose_item(a.hier_title, hier_texts(), [this](int i) { hier_go(i); });
        find_on_side([this](int i, bool right) { return hier_side(i, right); });
        hier_open(0);
        return;
    }
    const size_t at = static_cast<size_t>(a.hier_asking);
    if (at >= a.hier_rows.size() || !find_open_) return;  // (the box has gone since)
    const u32 depth = a.hier_rows[at].depth + 1;
    a.hier_rows[at].open = true;
    std::vector<HierRow> under;
    for (LspHierItem& it : items) under.push_back({std::move(it), depth, false});
    a.hier_rows.insert(a.hier_rows.begin() + static_cast<std::ptrdiff_t>(at + 1), under.begin(), under.end());
    find_set_rows(hier_texts(), at);
}

// A row's level asked for.
void App::hier_open(size_t i) {
    Assist& a = *assist_;
    if (i >= a.hier_rows.size() || a.hier_waiting) return;
    static const Lsp::HierKind kKinds[] = {Lsp::kCallsIn, Lsp::kCallsOut, Lsp::kSupertypes, Lsp::kSubtypes};
    a.hier_asking = static_cast<int>(i);
    a.hier_waiting = true;
    lsp_.hierarchy_expand(a.hier_rows[i].item.raw, kKinds[a.hier_kind]);
}

// Right on a row: its level, asked for; Left: an open one closed, else
// the choice to the row it is under.
bool App::hier_side(int index, bool right) {
    Assist& a = *assist_;
    if (index < 0 || static_cast<size_t>(index) >= a.hier_rows.size()) return false;
    const size_t i = static_cast<size_t>(index);
    HierRow& r = a.hier_rows[i];
    if (right) {
        if (!r.open) hier_open(i);
        return true;
    }
    if (r.open) {
        size_t end = i + 1;
        while (end < a.hier_rows.size() && a.hier_rows[end].depth > r.depth) ++end;
        a.hier_rows.erase(a.hier_rows.begin() + static_cast<std::ptrdiff_t>(i + 1), a.hier_rows.begin() + static_cast<std::ptrdiff_t>(end));
        r.open = false;
        find_set_rows(hier_texts(), i);
        return true;
    }
    for (size_t k = i; k-- > 0;)
        if (a.hier_rows[k].depth < r.depth) {
            find_set_rows(hier_texts(), k);
            break;
        }
    return true;
}

// Enter: to the row's place (a call coming in: where it calls).
void App::hier_go(int index) {
    Assist& a = *assist_;
    if (index < 0 || static_cast<size_t>(index) >= a.hier_rows.size()) return;
    const LspLocation at = a.hier_rows[static_cast<size_t>(index)].item.at;
    remember_place();
    if (at.path != pane_->doc->ed.path && full_path(pane_->doc->ed.path) != at.path && !open_path(at.path.c_str())) return;
    if (pane_->doc->loading) {
        pane_->doc->goto_line = at.line;
        pane_->doc->goto_col = at.character;
        pane_->doc->goto_utf16 = lsp_.counts_utf16();
        return;
    }
    pane_->doc->ed.move_to(Lsp::offset_at(pane_->doc->ed.buf, at.line, at.character, lsp_.counts_utf16()), false);
    scroll_to_cursor();
}

// For the tests: the hierarchy's rows as shown, or none.
std::string App::hier_dump() {
    Assist& a = *assist_;
    if (a.hier_waiting) return "waiting";
    std::string out;
    for (const std::string& t : hier_texts()) out += (out.empty() ? "" : " | ") + t;
    return out.empty() ? "no hierarchy" : out;
}

// ---------------------------------------------------------------- reference counts

// After each definition on show in the split in front (a function, a
// method, a class, a struct, as the syntax tree has them), how many
// references the server finds to it, muted. Asked for once typing has
// paused, a definition at a time as it comes into view; all asked again
// when the text changes or a file is saved.
void App::lens_marks(FrameDesc& fd) {
    Assist& a = *assist_;
    if (!cfg_.lsp_ref_counts || focus_view_ >= fd.view_count || !lsp_.ready() || lsp_doc_ != pane_->doc || !lsp_.can_references()) return;
    Doc& d = *pane_->doc;
    const u64 now = now_ns();
    if (a.lens_doc != d.id || a.lens_rev != d.ed.revision()) {  // (another file, or this one changed: the counts it had are gone)
        a.lens_doc = d.id;
        a.lens_rev = d.ed.revision();
        ++a.lens_tag;
        a.lens_counts.clear();
        a.lens_asked.clear();
        a.lens_due_ns = now + 400'000'000ull;
        a.lens_key[0] = ~0ull;
    }
    GridSpec& g = fd.views[focus_view_].grid;
    const std::vector<SyntaxScope>& scopes = d.syntax.structure().scopes;
    auto counted = [](u8 kind) { return kind == 12 || kind == 6 || kind == 5 || kind == 23 || kind == 9 || kind == 11 || kind == 10; };
    if (a.lens_due_ns && now >= a.lens_due_ns) {
        a.lens_due_ns = 0;
        size_t asked = 0;
        for (const SyntaxScope& sc : scopes) {
            if (!counted(sc.kind) || sc.name_at >= d.ed.buf.size() || a.lens_asked.contains(sc.name_at)) continue;
            const u64 line = d.ed.buf.line_of(sc.name_at);
            if (line < g.top_line || line >= g.bottom()) continue;
            a.lens_asked.insert(sc.name_at);
            lsp_.count_references(sc.name_at, a.lens_tag);
            if (++asked == 40) break;
        }
    } else if (!a.lens_due_ns) {  // (scrolled to more: asked for in a moment)
        for (const SyntaxScope& sc : scopes)
            if (counted(sc.kind) && sc.name_at < d.ed.buf.size() && !a.lens_asked.contains(sc.name_at)) {
                const u64 line = d.ed.buf.line_of(sc.name_at);
                if (line >= g.top_line && line < g.bottom()) {
                    a.lens_due_ns = now + 150'000'000ull;
                    break;
                }
            }
    }
    if (a.lens_counts.empty()) return;
    const u64 key[5] = {a.lens_tag, g.top_line, g.bottom(), a.lens_counts.size(), font_->generation()};
    if (!std::equal(std::begin(key), std::end(key), std::begin(a.lens_key))) {
        std::copy(std::begin(key), std::end(key), std::begin(a.lens_key));
        a.lens_notes.clear();
        a.lens_cells.clear();
        a.lens_texts.clear();
        a.lens_at.clear();
        Font& font = *font_;
        for (const auto& [at, n] : a.lens_counts) {
            if (at >= d.ed.buf.size()) continue;
            const u64 line = d.ed.buf.line_of(at);
            if (line < g.top_line || line >= g.bottom()) continue;
            if (std::ranges::any_of(a.lens_notes, [&](const GridNote& x) { return x.line == line; })) continue;
            const std::string said = n == 1 ? "1 reference" : std::to_string(n) + " references";
            TextBox box;
            box.resize(width_of(said), 1);
            put(box, font, 0, 0, said, ink::kMuted);
            a.lens_cells.push_back(std::move(box.cells));
            a.lens_texts.push_back(std::to_string(line + 1) + ": " + said);
            a.lens_at.push_back(at);
            const u64 start = d.ed.buf.line_start(line);
            a.lens_notes.push_back({line, d.ed.col_between(start, d.ed.line_end(line)) + 3, nullptr, static_cast<u32>(a.lens_cells.back().size())});
        }
        for (size_t k = 0; k < a.lens_notes.size(); ++k) a.lens_notes[k].cells = a.lens_cells[k].data();
    }
    if (a.lens_notes.empty()) return;
    // Each where nothing else is said (a problem's message, a value while debugging, come first).
    lens_view_notes_.assign(g.notes.begin(), g.notes.end());
    for (const GridNote& n : a.lens_notes)
        if (std::ranges::none_of(g.notes, [&](const GridNote& o) { return o.line == n.line; })) lens_view_notes_.push_back(n);
    g.notes = lens_view_notes_;
}

// A click on a count: those references, listed as Find references lists them.
bool App::lens_click(const SDL_MouseButtonEvent& b) {
    Assist& a = *assist_;
    if (b.button != SDL_BUTTON_LEFT || a.lens_notes.empty() || a.lens_doc != pane_->doc->id || a.lens_rev != pane_->doc->ed.revision() || !lsp_.ready()) return false;
    const int px = static_cast<int>(b.x * density_), py = static_cast<int>(b.y * density_);
    if (py < pane_->text_y || py >= pane_->y1 || px < pane_->text_x || px >= pane_->x1) return false;
    u64 line = 0;
    u32 col = 0;
    point_line_col(b.x, b.y, &line, &col);
    for (size_t k = 0; k < a.lens_notes.size() && k < a.lens_at.size(); ++k) {
        const GridNote& n = a.lens_notes[k];
        if (n.line != line || col < n.col || col > n.col + n.count) continue;
        lsp_close(kPopAll);
        a.refs_pending = true;
        lsp_.references(a.lens_at[k], false);  // (those counted: not the definition itself)
        return true;
    }
    return false;
}

// A file saved: what refers to what may have changed.
void App::lens_saved() {
    if (assist_) assist_->lens_rev = ~0ull;
}

// Counts due to be asked for, or asked for and not yet answered.
bool App::lens_waiting() const {
    const Assist& a = *assist_;
    return a.lens_due_ns || a.lens_asked.size() > a.lens_counts.size();
}

// For the tests: the counts drawn in the split in front, or none.
std::string App::lens_dump() {
    Assist& a = *assist_;
    std::string out;
    for (const std::string& t : a.lens_texts) out += (out.empty() ? "" : " | ") + t;
    return out.empty() ? "no counts" : out;
}

}  // namespace blit
