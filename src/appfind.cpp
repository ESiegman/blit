// Finding things, in a box over the window: files by name (Cmd+P), lines by
// what is in them (Cmd+Shift+F), and, where the system's file dialogs used
// to be, a path typed with the folder's contents listed under it: a file
// to open (Cmd+O), a name to save under, a folder to work in. The names and
// the text are looked for by the worker in finder.cpp; this is the box, its
// keys and what choosing something does.
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "app.hpp"
#include "remote.hpp"
#include "finder.hpp"
#include "fsio.hpp"
#include "look.hpp"
#include "regex.hpp"
#include "textbox.hpp"
#include "tree.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

constexpr u32 kMaxRows = 14;  // results shown at once
constexpr u64 kWalkAgainNs = 2'000'000'000;  // the folder is read again at most this often (unless something says it changed)
constexpr u32 kTypeRoom = 24;  // the field has room for this much typed, however little is
constexpr u32 kFieldTone = 1;  // (Popup::tones: the field's cells)
constexpr size_t kMostRecent = 8;  // Go to file with nothing typed: how many files opened lately it lists

// The keys the footer names, by what the box is for.
constexpr std::string_view kCmdEnter = kPrimaryMod == SDL_KMOD_GUI ? "Cmd+Enter" : "Ctrl+Enter";
constexpr std::string_view kCmdShiftO = kPrimaryMod == SDL_KMOD_GUI ? "Cmd+Shift+O" : "Ctrl+Shift+O";
constexpr std::string_view kCmdShiftF = kPrimaryMod == SDL_KMOD_GUI ? "Cmd+Shift+F" : "Ctrl+Shift+F";
constexpr Hint kHintsNames[] = {{"Enter", "open"}, {kCmdEnter, "open beside"}, {"Tab", "search in files"}};
constexpr Hint kHintsLine[] = {{"Enter", "go to the line"}};
constexpr Hint kHintsText[] = {{"Enter", "open"}, {kCmdEnter, "open beside"}, {"Tab", "replace"}};
constexpr Hint kHintsTextReplace[] = {{kCmdEnter, "replace all"}, {"Enter", "open"}, {"Tab", "other field"}};
constexpr Hint kHintsFile[] = {{"Enter", "next"}, {"Shift+Enter", "previous"}, {"Tab", "replace"}};
constexpr Hint kHintsFileReplace[] = {{"Enter", "replace"}, {kCmdEnter, "all"}, {"Tab", "other field"}};
constexpr Hint kHintsSure[] = {{"Enter", "replace"}, {"Esc", "not now"}};
constexpr std::string_view kWithLabel = "Replace with";
// Matches counted in the file ("3 of 12"), at most, and in a file no larger
// than this: a count is made again at every letter typed, and a regular
// expression is read at about half a gigabyte a second (text as it stands
// at several).
constexpr u64 kMostCounted = 100000;
constexpr u64 kCountBytes = 64ull << 20, kCountBytesRegex = 8ull << 20;
constexpr Hint kHintsCommand[] = {{"Enter", "run"}, {"Tab", "find by keys"}};
constexpr Hint kHintsCapture[] = {{"Tab", "back to typing"}};
constexpr Hint kHintsOpen[] = {{"Enter", "open"}, {"Tab", "complete"}};
constexpr Hint kHintsSave[] = {{"Enter", "save"}, {"Tab", "complete"}};
constexpr Hint kHintsFolder[] = {{"Enter", "choose"}, {"Tab", "go into"}};
constexpr Hint kHintsPick[] = {{"Enter", "choose"}, {"Esc", "cancel"}};
constexpr Hint kHintsInput[] = {{"Enter", "accept"}, {"Esc", "cancel"}};
constexpr Hint kHintsAsk[] = {{"Enter", "send"}, {"Esc", "cancel connecting"}};

// What Go to file is asked, apart: a name, and a line (and a column) to go
// to in the file chosen ("render.cpp:120", "render.cpp:120:5"; ":120" is
// a line of the file in front). Numbers from 1; 0: none.
void split_place(std::string_view t, std::string* name, u64* line, u64* col) {
    *line = *col = 0;
    auto number = [](std::string_view s, u64* out) {
        if (s.empty() || s.size() > 9) return false;
        u64 v = 0;
        for (const char c : s) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + static_cast<u64>(c - '0');
        }
        *out = v;
        return true;
    };
    while (!t.empty() && t.back() == ':') t.remove_suffix(1);  // (the number is on its way)
    if (const size_t a = t.rfind(':'); a != std::string_view::npos && number(t.substr(a + 1), line)) {
        t = t.substr(0, a);
        if (const size_t b = t.rfind(':'); b != std::string_view::npos && number(t.substr(b + 1), col)) {
            std::swap(*line, *col);  // (the last number was the column)
            t = t.substr(0, b);
        }
    }
    name->assign(t);
}

std::string home_dir() {
    const char* h = SDL_getenv("HOME");
    std::string s = h ? h : "";
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

// A path as it is shown: "~" for the home folder.
std::string tilde(const std::string& path) {
    const std::string h = home_dir();
    if (!h.empty() && (path == h || path.starts_with(h + "/"))) return "~" + path.substr(h.size());
    return path;
}

// A path as it is typed, in full: "~" is the home folder, and one that does
// not start at the root starts at `base`.
// On another machine ("ssh://host/path", or anything typed while the
// working directory is on one), "~" is the home folder there, once the
// connection has said where that is.
std::string in_full(std::string_view typed, const std::string& base) {
    // ("ssh://" typed after the folder the box starts in begins again there.)
    if (const size_t at = typed.rfind("ssh://"); at != std::string_view::npos) typed.remove_prefix(at);
    std::string host, there;
    auto home_there = [&](std::string_view rest) {  // rest: what follows "~"
        for (const std::shared_ptr<Remote>& r : Remote::all())
            if (r->host() == host && !r->home().empty()) return remote_path(host, r->home() + std::string(rest));
        return remote_path(host, "/~" + std::string(rest));
    };
    if (is_remote(typed)) {
        if (split_remote(typed, &host, &there) && (there == "/~" || there.starts_with("/~/"))) return home_there(std::string_view(there).substr(2));
        return std::string(typed);
    }
    if (split_remote(base, &host, &there)) {
        if (typed == "~" || typed.starts_with("~/")) return home_there(typed.substr(1));
        if (typed.starts_with("/")) return remote_path(host, typed);
        return base + "/" + std::string(typed);
    }
    if (typed == "~" || typed.starts_with("~/")) return home_dir() + std::string(typed.substr(1));
    if (typed.starts_with("/")) return std::string(typed);
    return base + "/" + std::string(typed);
}

// The machines ~/.ssh/config names (each "Host" that is not a pattern), for
// "ssh://" typed into the box that opens a file or a folder.
std::vector<std::string> ssh_hosts() {
    std::vector<std::string> out;
    const std::string file = home_dir() + "/.ssh/config";
    size_t n = 0;
    char* text = static_cast<char*>(SDL_LoadFile(file.c_str(), &n));
    if (!text) return out;
    std::string_view all(text, n);
    while (!all.empty()) {
        const size_t nl = all.find('\n');
        std::string_view line = all.substr(0, nl);
        all = nl == std::string_view::npos ? std::string_view{} : all.substr(nl + 1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.size() < 5 || SDL_strncasecmp(line.data(), "host", 4) != 0 || (line[4] != ' ' && line[4] != '\t' && line[4] != '=')) continue;
        line.remove_prefix(5);
        while (!line.empty()) {
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '=')) line.remove_prefix(1);
            size_t k = 0;
            while (k < line.size() && line[k] != ' ' && line[k] != '\t' && line[k] != '\r') ++k;
            const std::string_view name = line.substr(0, k);
            line.remove_prefix(k);
            if (!name.empty() && name.find_first_of("*?!") == std::string_view::npos && std::ranges::find(out, name) == out.end()) out.emplace_back(name);
        }
    }
    SDL_free(text);
    return out;
}

void pop_char(std::string* s) {
    while (!s->empty() && is_cont(static_cast<u8>(s->back()))) s->pop_back();
    if (!s->empty()) s->pop_back();
}

// A line found, and what replacing makes of it: the bytes of each match in
// the one, and of what each became in the other, to be lit. (A new line
// put in shows as "↵".)
void preview(std::string_view line, const Vim::Pattern& p, std::string_view with, bool rules, std::vector<u32>* lit, std::string* after, std::vector<u32>* lit_after) {
    u64 from = 0;
    std::string made;
    Vim::each_whole(line, p, 0, line.size() + 1, false, [&](const RegexMatch& m) {
        if (m.start < from) return true;
        after->append(line.substr(from, m.start - from));
        for (u64 k = m.start; k < m.end && k < line.size(); ++k) lit->push_back(static_cast<u32>(k));
        made.clear();
        Vim::replacement(m, with, rules, [&](u64 x, u64 y) { return std::string(line.substr(x, y > x ? y - x : 0)); }, &made);
        const size_t a = after->size();
        for (const char ch : made) {
            if (ch == '\n') *after += "\xE2\x86\xB5";
            else *after += ch;
        }
        for (size_t k = a; k < after->size(); ++k) lit_after->push_back(static_cast<u32>(k));
        from = m.end;
        return lit->size() < 4000;
    });
    after->append(line.substr(std::min<u64>(from, line.size())));
}

// The bytes of each match of a pattern in a line, to be lit.
void lit_matches(std::string_view line, const Vim::Pattern& p, std::vector<u32>* lit) {
    Vim::each_whole(line, p, 0, line.size() + 1, false, [&](const RegexMatch& m) {
        for (u64 k = m.start; k < m.end && k < line.size(); ++k) lit->push_back(static_cast<u32>(k));
        return lit->size() < 4000;
    });
}

}  // namespace

struct App::Find {
    int mode = kFindNames;
    std::string typed;
    u32 sel = 0, top = 0;
    int hover = -1;

    // Names and text: the worker, the folder it looks under, the search
    // last asked for and the answer on show.
    std::unique_ptr<Finder> engine;  // made when the box is first opened: it has a thread of its own
    std::string root;
    u64 serial = 0;
    std::shared_ptr<const FindResults> shown;
    // The folder is read again only when it may have changed: another one,
    // other settings, the window came back to the front, or the last reading
    // is a while old. Until then the list there is is searched at once.
    u64 walked_ns = 0;
    bool stale = true;
    FinderOptions walked_opt;

    // A path: the folder typed so far (in full, with its slash), what is
    // in it, and of that what the rest of what is typed matches.
    struct Entry {
        enum : u8 { kPlain, kHere, kParent, kNew };
        std::string name;
        bool dir = false;
        u8 kind = kPlain;
        int score = 0;
        u32 index = 0;  // kFindPick: which of the items
    };
    std::string dir, leaf, listed;
    std::vector<Entry> listing, entries;
    u64 save_doc = 0;  // kFindSave: the document a name is wanted for

    // A script's picker (kFindPick) or line to type (kFindInput): what it
    // offers, and the word that says what it is for.
    std::vector<std::string> items;
    std::string prompt;
    bool script() const { return mode == kFindPick || mode == kFindInput; }
    // ssh's question (kFindAsk): which, from where, whether it is hidden as
    // typed, and the box that was open before it (to open again after).
    u64 ask_id = 0;
    std::string ask_host;
    // Asked for by blit itself (the Profile tab): what is chosen or typed
    // goes here instead of being opened (a path) or to a script (a line).
    std::function<void(const std::string&)> chose;
    std::function<void(int)> chose_item;  // (a list of blit's own, kFindPick: which of the items)
    // A list of blit's own whose rows are tried out as the choice moves
    // (find_preview_item): the row last tried, and what Esc puts back.
    std::function<void(int)> moved;
    std::function<void()> cancelled;
    int moved_at = -1;
    u64 into_pane = 0;  // Go to file for a split just made (find_for_split): what is chosen opens there
    // The command search (kFindCommand): each command's keys and what is
    // searched (its name and its keys, in words), and whether the next keys
    // pressed are to be looked up rather than acted on.
    std::vector<std::string> item_keys, item_search, item_syms;  // (item_syms: the keys as the box shows them, ⇧⌘P)
    std::vector<u32> item_ids;
    bool capture = false, chord_query = false;  // (and whether what is typed is keys pressed)
    bool drop_text = false;  // (the text a key looked up brings is not typed too)
    bool drop_option = false;  // (nor the character Option and a letter types, after a toggle: "®")
    bool secret = false;
    int back_mode = -1;
    std::string back_typed;
    bool path() const { return mode == kFindOpen || mode == kFindSave || mode == kFindFolder; }

    // Go to file with nothing typed: the files open (the one in front
    // last), then those opened lately, each kind under its heading. What is
    // looked for is what is typed without the line (and column) on its end.
    struct Near {
        std::string heading;  // a heading's words; empty: a file
        std::string full, name, folder;
    };
    std::vector<Near> near;
    std::string query;
    u64 go_line = 0, go_col = 0;  // (from 1; 0: none)
    bool nearby() const { return mode == kFindNames && query.empty() && !go_line && typed != ":" && !near.empty(); }
    bool line_only() const { return mode == kFindNames && query.empty() && (go_line || typed == ":"); }  // (":" alone: the status bar's Ln, Col)
    bool heading(size_t i) const { return nearby() && i < near.size() && !near[i].heading.empty(); }

    bool beside = false;  // what is chosen opens in the split to the right (Cmd+Enter)
    bool all = false;     // what is typed is selected: typing replaces it

    // Finding in the file in front (kFindFile), and replacing there and in
    // files (kFindText): the toggles (kept from one opening to the next),
    // the second field, and what is typed read as a pattern.
    bool regex = false, exact = false;
    bool replacing = false;  // "Replace with" is there
    bool in_with = false;    // and has the keyboard
    std::string with;
    bool with_all = false;   // what is in it is selected
    Vim::Pattern pattern;    // what is typed, read (kFindFile; kFindText's lines lit and previewed)
    u64 file_doc = 0, file_from = 0;  // kFindFile: the document, and where its cursor was when the box opened
    // The match on show, selected in the document, and the text it was found in.
    RegexMatch cur{};
    u64 cur_rev = ~0ull;
    bool cur_ok = false;
    // Where the matches in the file start ("3 of 12"), for the text and pattern they were counted for.
    std::vector<u64> starts;
    bool starts_capped = false, counted = false;
    u64 starts_key = 0;
    Vim::Pattern last_file;  // what kFindFile looked for last (Cmd+G with the box shut)
    // Replacing in files: the question asked first.
    bool asking = false;
    u64 ask_hits = 0, ask_files = 0;
    u32 toggle_col = 0;  // where ".*" is drawn in the field (and "Aa" three columns on)
    u32 fields() const { return 1 + (replacing && (mode == kFindText || mode == kFindFile) ? 1 : 0); }
    bool file() const { return mode == kFindFile; }
    // Search in files: what was last searched for (it is there again the
    // next time), and the hits last shown, to go through without the box
    // (find_step): which of them was last gone to (-1: none yet).
    std::string last_text;
    std::shared_ptr<const FindResults> last;
    i64 last_at = -1;
    void keep_hits() {  // (the hits on show, as the box shuts or turns to something else: from the first, unless one was chosen)
        if (mode != kFindText || !shown || !shown->text || shown->hits.empty() || shown == last) return;
        last = shown;
        last_at = -1;
    }

    // The cells, and what they were made from.
    TextBox box;
    SDL_Rect rect{};
    u32 list_rows = 0;
    u64 state = 1, drawn = 0;
    u32 drawn_font = ~0u, drawn_cap = 0, drawn_rows = 0;
    u32 seen_cols = 0;  // the widest it has needed since it opened: it grows to fit, and does not shrink back and forth as rows come and go
    float tones[8][4] = {};
    void touch() { ++state; }

    size_t count() const {
        if (mode == kFindFile) return 0;
        if (nearby()) return near.size();
        if (line_only()) return 0;
        if (mode == kFindNames || mode == kFindText) return shown ? shown->hits.size() : 0;
        return entries.size();
    }

    // (Find is the App's, and can reach what the App keeps to itself.)
    static void list_near(App& a);
    static void go(App& a, u64 line, u64 col);
    static bool open_beside(App& a, const std::string& path);
    static bool open_in(App& a, Pane* dest, const std::string& path, bool drop_blank);
    static void tried(App& a);
    static void say(App& a, const std::string& text);
    // Finding and replacing in the file (kFindFile).
    static void file_query(App& a);
    static bool file_go(App& a, u64 from, bool forward, bool at_from);
    static void file_select(App& a, const RegexMatch& m);
    static void file_count(App& a);
    static void file_replace(App& a, bool all);
    // Replacing in files: the question, then the work.
    static void files_ask(App& a);
    static void files_replace(App& a);
};

// The files Go to file lists with nothing typed: those open, the one in
// front last, and those opened lately. Among those that match as well as
// each other, these come first once something is typed.
void App::Find::list_near(App& a) {
    Find& f = *a.find_;
    f.near.clear();
    std::vector<std::string> preferred;
    auto add = [&](const std::string& full) {
        if (full.empty() || std::ranges::any_of(f.near, [&](const Near& e) { return e.full == full; })) return false;
        Near e;
        e.full = full;
        const size_t slash = full.rfind('/');
        e.name = slash == std::string::npos ? full : full.substr(slash + 1);
        const std::string dir = slash == std::string::npos ? std::string() : full.substr(0, slash);
        if (full.starts_with(f.root + "/")) {  // (in the folder: from it)
            preferred.push_back(full.substr(f.root.size() + 1));
            e.folder = dir.size() > f.root.size() ? dir.substr(f.root.size() + 1) : std::string();
        } else {
            e.folder = tilde(dir);
        }
        f.near.push_back(std::move(e));
        return true;
    };
    auto section = [&](const char* heading, size_t from) {  // (a heading with nothing under it goes)
        if (f.near.size() == from + 1) f.near.pop_back();
        else f.near[from].heading = heading;
    };
    size_t from = f.near.size();
    f.near.emplace_back();
    for (const std::unique_ptr<Pane>& p : a.panes_)
        for (const std::unique_ptr<Doc>& d : p->docs)
            if (d.get() != a.pane_->doc && !d->ed.path.empty() && d->title.empty()) add(full_path(d->ed.path));
    if (!a.pane_->doc->ed.path.empty() && a.pane_->doc->title.empty()) add(full_path(a.pane_->doc->ed.path));
    section("Open", from);
    from = f.near.size();
    f.near.emplace_back();
    size_t recent = 0;
    for (const std::string& p : a.recent_files(kMostRecent * 2))
        if (recent < kMostRecent && add(p)) ++recent;
    section("Recent", from);
    f.engine->set_preferred(std::move(preferred));
}

// The file in front, at a line (from 0) and a byte in it, with the line in
// the middle of the view.
void App::Find::go(App& a, u64 line, u64 col) {
    Doc& d = *a.pane_->doc;
    if (d.loading) {  // (on its way from another machine: there when it comes)
        d.goto_line = line;
        d.goto_col = static_cast<u32>(std::min<u64>(col, ~0u));
        return;
    }
    Editor& ed = d.ed;
    line = std::min<u64>(line, ed.buf.line_count() - 1);
    const u64 start = ed.buf.line_start(line);
    ed.move_to(std::min(start + col, ed.line_end(line)), false);
    a.vim_mouse();  // (Vim takes the cursor's new place as it would a click's)
    a.scroll_to_cursor();
    const double ch = a.font_->cell_h();
    a.pane_->target_y = std::clamp(static_cast<double>(line) * ch - (a.view_h() - ch) / 2, 0.0, std::max(a.max_scroll_y(), 0.0));
    a.pane_->scroll_y = a.pane_->target_y;
    a.dirty_ = true;
}

void App::show_place(u64 line, u64 col) { Find::go(*this, line, col); }

// Opens a file in the split to the right of the one with the keyboard (a
// new one if there is none there), which gets the keyboard. A file shows in
// one place: one open elsewhere moves there.
bool App::Find::open_beside(App& a, const std::string& path) {
    const Pane& here = *a.pane_;
    Pane* dest = a.pane_at(here.x1 + 4, (here.y0 + here.y1) / 2);
    if (!dest) dest = a.split_pane(a.pane_, kEdgeRight);
    if (!dest) return a.open_path(path.c_str());  // (no room for another split)
    return open_in(a, dest, path, false);
}

// Opens a file in a split, which gets the keyboard; one open elsewhere
// moves there. drop_blank: the split was made for it (Cmd+\), and the new
// file it was made with goes if nothing was typed in it.
bool App::Find::open_in(App& a, Pane* dest, const std::string& path, bool drop_blank) {
    Pane* where = nullptr;
    size_t index = 0;
    if (a.find_open(full_path(path), &where, &index)) {
        const bool moved = where != dest;
        u64 id = 0;
        if (moved) {
            id = where->docs[index]->id;
            a.move_doc(where, index, dest, dest->docs.size());
        }
        a.focus_pane(dest);
        if (!moved) a.tab_show(index);
        for (size_t i = 0; drop_blank && moved && i < a.pane_->docs.size() && a.pane_->docs.size() > 1; ++i) {
            const Doc& d = *a.pane_->docs[i];
            if (d.id != id && d.ed.path.empty() && !d.ed.modified() && d.ed.buf.size() == 0 && d.title.empty() && !d.term && !d.prof) {
                a.tab_close(i, true);
                break;
            }
        }
    } else {
        a.focus_pane(dest);
        if (dest->docs.empty()) {  // a new split: a new file, which the one opened takes the place of
            a.add_doc(0);
            a.tab_show(0);
        }
        if (!a.open_path(path.c_str())) return false;
    }
    a.panel_focus(false);
    a.pick_theme();
    a.dirty_ = true;
    return true;
}

// A word on the bottom line: Vim's, with its modes on; else a message box.
void App::Find::say(App& a, const std::string& text) {
    if (a.cfg_.modal) a.vim_.say(text);
    else a.notify(text);
    a.dirty_ = true;
}

// ---------------------------------------------------------------- finding and replacing in the file

// What is typed has changed: read it, and the first match from where the
// cursor was when the box opened is chosen (round the end if need be).
void App::Find::file_query(App& a) {
    Find& f = *a.find_;
    f.pattern = Vim::make_pattern(f.typed, f.regex, f.exact);
    f.cur_ok = false;
    if (a.pane_->doc->id == f.file_doc && !f.pattern.empty() && !f.pattern.bad()) file_go(a, f.file_from, true, true);
    file_count(a);
    f.touch();
    a.dirty_ = true;
}

// The next match after `from` (or at it, with at_from), or the one before
// it, chosen; false if there is none.
bool App::Find::file_go(App& a, u64 from, bool forward, bool at_from) {
    Find& f = *a.find_;
    Doc& d = *a.pane_->doc;
    if (d.id != f.file_doc || f.pattern.empty() || f.pattern.bad()) return false;
    RegexMatch m;
    const u64 after = at_from ? (from == 0 ? ~0ull : from - 1) : from;
    if (Vim::find(d.ed.buf, f.pattern, forward ? after : from, forward, true, nullptr, ~0ull, &m) == ~0ull) {
        f.cur_ok = false;
        f.touch();
        return false;
    }
    file_select(a, m);
    return true;
}

// A match, selected in the document (with modes, as Visual mode would
// select it), and shown.
void App::Find::file_select(App& a, const RegexMatch& m) {
    Find& f = *a.find_;
    Editor& ed = a.pane_->doc->ed;
    f.cur = m;
    f.cur_rev = ed.revision();
    f.cur_ok = true;
    ed.move_to(m.start, false);
    if (m.end > m.start) ed.move_to(m.end, true);
    if (a.cfg_.modal) a.vim_mouse();
    a.pane_->cache_rev = ~0ull;
    a.scroll_to_cursor();
    f.touch();
    a.dirty_ = true;
}

// Where every match in the file starts, for "3 of 12": counted again only
// when the text or the pattern has changed (and not in a very large file).
void App::Find::file_count(App& a) {
    Find& f = *a.find_;
    const Doc& d = *a.pane_->doc;
    u64 key = 1469598103934665603ull;
    auto mix = [&](u64 v) { key = (key ^ v) * 1099511628211ull; };
    mix(d.id);
    mix(d.ed.revision());
    mix(std::hash<std::string>{}(f.pattern.text));
    mix(reinterpret_cast<u64>(f.pattern.re.get()));
    mix((f.pattern.ignore_case ? 1u : 0u) | (f.pattern.word ? 2u : 0u));
    if (key == f.starts_key) return;
    f.starts_key = key;
    f.starts.clear();
    f.starts_capped = false;
    f.counted = d.id == f.file_doc && !f.pattern.empty() && !f.pattern.bad() && d.ed.buf.size() <= (f.pattern.re ? kCountBytesRegex : kCountBytes);
    if (!f.counted) return;
    Vim::each_whole(d.ed.buf, f.pattern, 0, d.ed.buf.size() + 1, false, [&](const RegexMatch& m) {
        f.starts.push_back(m.start);
        f.starts_capped = f.starts.size() >= kMostCounted;
        return !f.starts_capped;
    });
}

// Enter with "Replace with" there: the match chosen becomes what is typed
// there (by :s's rules, with regular expressions on: \1, &, \n), and the
// next is chosen; if none was chosen (or the text has changed since), the
// next is chosen first. Cmd+Enter: all of them, in one undo step.
void App::Find::file_replace(App& a, bool all) {
    Find& f = *a.find_;
    Doc& d = *a.pane_->doc;
    Editor& ed = d.ed;
    if (d.id != f.file_doc || f.pattern.empty() || f.pattern.bad()) return;
    if (d.read_only) {
        say(a, "This tab is for reading: nothing is replaced");
        return;
    }
    if (all) {
        const u64 n = Vim::replace_all(ed, f.pattern, f.with, f.regex, 0, ed.buf.size() + 1);
        f.cur_ok = false;
        if (a.cfg_.modal) a.vim_mouse();
        a.pane_->cache_rev = ~0ull;
        a.scroll_to_cursor();
        file_count(a);
        f.touch();
        say(a, n == 0 ? std::string("Nothing to replace") : "Replaced " + std::to_string(n) + (n == 1 ? " match" : " matches"));
        return;
    }
    if (!f.cur_ok || f.cur_rev != ed.revision()) {
        file_go(a, ed.has_selection() ? ed.sel_min() : ed.cursor(), true, true);
        file_count(a);
        return;
    }
    std::string out;
    Vim::replacement(f.cur, f.with, f.regex, [&](u64 x, u64 y) { return ed.buf.text(x, y > x ? y - x : 0); }, &out);
    const u64 at = f.cur.start, was_end = f.cur.end;
    ed.replace(at, was_end, out);
    // On from after what was put in (one of nothing in place of nothing: from the next character).
    u64 next = at + out.size();
    if (out.empty() && was_end == at) next = ed.next_char(next);
    f.cur_ok = false;
    if (!file_go(a, next, true, true)) {
        ed.move_to(at + out.size(), false);
        if (a.cfg_.modal) a.vim_mouse();
        a.scroll_to_cursor();
    }
    a.pane_->cache_rev = ~0ull;
    file_count(a);
    f.touch();
    a.dirty_ = true;
}

// ---------------------------------------------------------------- replacing in files

// Cmd+Enter with "Replace with" there: how many there are to replace, asked
// about first, in the box.
void App::Find::files_ask(App& a) {
    Find& f = *a.find_;
    if (f.pattern.empty() || f.pattern.bad() || !a.find_current(400) || !f.shown) return;
    const FindResults& r = *f.shown;
    if (r.searching) return say(a, "Still searching: replace when it has finished");
    if (r.capped) return say(a, "More than " + std::to_string(Finder::kMaxLines) + " lines have it: narrow the search to replace");
    u64 hits = 0;
    for (const FindHit& h : r.hits) hits += h.count;
    if (hits == 0) return say(a, "Nothing to replace");
    f.asking = true;
    f.ask_hits = hits;
    f.ask_files = r.files_hit;
    f.touch();
    a.dirty_ = true;
}

// Every hit replaced. A file open in a tab is changed there (one undo step,
// left for its owner to save); any other is read, changed and written, here
// or on another machine.
void App::Find::files_replace(App& a) {
    Find& f = *a.find_;
    f.asking = false;
    const std::shared_ptr<const FindResults> r = f.shown;
    if (!r || !r->index || f.pattern.empty() || f.pattern.bad()) return;
    std::vector<u32> files;
    for (const FindHit& h : r->hits)
        if (files.empty() || files.back() != h.file) files.push_back(h.file);
    const Vim::Pattern p = f.pattern;
    const std::string with = f.with;
    const bool rules = f.regex;
    const u64 most = f.walked_opt.max_file_bytes;
    u64 hits = 0, changed = 0;
    std::string failed;
    for (const u32 i : files) {
        const std::string rel(r->index->path(i));
        const std::string full = r->index->root + "/" + rel;
        Pane* where = nullptr;
        size_t at = 0;
        u64 n = 0;
        if (a.find_open(full_path(full), &where, &at)) {
            Doc& d = *where->docs[at];
            if (d.read_only || d.loading) {
                failed = rel;
                continue;
            }
            n = Vim::replace_all(d.ed, p, with, rules, 0, d.ed.buf.size() + 1);
            where->cache_rev = ~0ull;
        } else {
            std::string text, err;
            if (!fs_read(full, &text, most + 1) || text.size() > most) {
                failed = rel;
                continue;
            }
            const std::string out = Vim::replace_text(text, p, with, rules, &n);
            if (n && !fs_write(full, out, &err)) {
                failed = rel + " (" + err + ")";
                continue;
            }
        }
        hits += n;
        changed += n ? 1 : 0;
    }
    a.find_close(true);
    if (a.cfg_.modal) a.vim_attach();
    say(a, "Replaced " + std::to_string(hits) + (hits == 1 ? " hit in " : " hits in ") + std::to_string(changed) + (changed == 1 ? " file" : " files") +
               (failed.empty() ? std::string() : "; not in " + failed));
}

void App::find_file_open(bool replace, const std::string& typed) {
    Find& f = *find_;
    Doc& d = *pane_->doc;
    if (cfg_.modal) {  // (out of Insert mode and the command line: the match is shown as Visual mode would select it)
        vim_attach();
        if (vim_.mode() == Vim::kInsert || vim_.mode() == Vim::kReplace || vim_.mode() == Vim::kCommand) {
            vim_.keys(d.ed, "<Esc>");
            vim_after(Vim::kHandled);
        }
    }
    if (find_open_ && f.mode == kFindFile && f.file_doc == d.id) {  // open already: "Replace with" comes, with the keyboard
        if (replace && !f.replacing) f.with_all = !f.with.empty();
        f.replacing = f.replacing || replace;
        f.in_with = f.replacing;
        f.touch();
        dirty_ = true;
        return;
    }
    find_open(kFindFile, f.regex ? regex_escape(typed, 0) : typed);
    if (replace) {
        f.replacing = true;
        f.with_all = !f.with.empty();
        f.touch();
    }
}

void App::find_replace_in_files() {
    Find& f = *find_;
    if (find_open_ && f.mode == kFindText) {  // open already: the field comes, or goes
        f.replacing = !f.replacing;
        f.in_with = f.replacing;
        f.with_all = f.replacing && !f.with.empty();
        f.asking = false;
        find_query();
        return;
    }
    find_open(kFindText);
    f.replacing = true;
    f.in_with = !f.typed.empty();  // (the keyboard where something is to be typed first)
    f.with_all = !f.with.empty();
    find_query();
}

void App::find_file_step(int by) {
    Find& f = *find_;
    Doc& d = *pane_->doc;
    Editor& ed = d.ed;
    if (f.last_file.empty() || f.last_file.bad()) {
        notify("Nothing to find yet: " + key_symbols(chord_text(SDLK_F, kPrimaryMod), *font_) + " finds in the file");
        return;
    }
    RegexMatch m;
    if (Vim::find(ed.buf, f.last_file, ed.sel_min(), by > 0, true, nullptr, ~0ull, &m) == ~0ull) {
        notify("No match for '" + f.last_file.text + "'");
        return;
    }
    ed.move_to(m.start, false);
    ed.move_to(m.end, true);
    scroll_to_cursor();
    dirty_ = true;
}

const Vim::Pattern* App::find_highlight() const {
    const Find& f = *find_;
    if (!find_open_ || f.mode != kFindFile || f.file_doc != pane_->doc->id || f.pattern.empty() || f.pattern.bad()) return nullptr;
    return &f.pattern;
}

void App::find_toggle(bool regex) {
    Find& f = *find_;
    (regex ? f.regex : f.exact) = !(regex ? f.regex : f.exact);
    if (find_open_ && (f.mode == kFindFile || f.mode == kFindText)) {
        f.asking = false;
        find_query();
        return;
    }
    Find::say(*this, regex ? (f.regex ? "Regular expressions on, in Find and Search in files" : "Regular expressions off: text is found as it stands")
                           : (f.exact ? "Match case on, in Find and Search in files" : "Match case off: small letters match either case"));
}

void App::find_create() { find_ = std::make_shared<Find>(); }

void App::find_save_for(u64 doc) { find_->save_doc = doc; }

bool App::find_is_open() const { return find_open_; }

// Opens the box (or, open already, turns it to another purpose).
// (Typed answers to ssh are not left in memory.)
static void wipe(std::string& s) {
    volatile char* p = s.data();
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
}

// Files come and go: the next time the box opens it reads the folder again.
void App::find_stale() { find_->stale = true; }

void App::find_open(int mode, const std::string& typed) {
    Find& f = *find_;
    if (find_open_ && f.script() && !(mode == f.mode && f.script())) find_close(false);  // (a script's picker is answered "nothing")
    f.chose = nullptr;  // (see find_choose)
    f.chose_item = nullptr;
    f.moved = nullptr;
    if (auto back = std::exchange(f.cancelled, nullptr)) back();  // (what a list tried out is put back)
    if (mode != kFindNames && mode != kFindText) f.into_pane = 0;  // (Tab between those two keeps it)
    if (find_open_ && f.mode == kFindAsk && mode != kFindAsk) {  // (ssh's question waits, and comes back when this box shuts)
        wipe(f.typed);
        f.ask_id = 0;
        f.back_mode = -1;
    }
    lsp_close(kPopAll);
    if (find_open_) f.keep_hits();  // (for F4 to go through)
    const std::string* root = panel_root();
    const std::string workdir = root ? *root : default_workdir();
    find_open_ = true;
    f.mode = mode;
    f.typed = typed;
    f.sel = f.top = 0;
    f.hover = -1;
    f.all = f.beside = false;
    f.replacing = f.in_with = f.with_all = f.asking = false;  // (asked for afterwards, by whoever wants them)
    f.cur_ok = false;
    f.seen_cols = 0;  // (as wide as this purpose needs)
    if (mode == kFindFile) {  // the file in front, from where its cursor (or selection) is
        f.file_doc = pane_->doc->id;
        const Editor& ed = pane_->doc->ed;
        u64 lo = 0, hi = 0;
        f.file_from = vim_selection(&lo, &hi) ? lo : ed.has_selection() ? ed.sel_min() : ed.cursor();
        f.all = !f.typed.empty();  // (typing replaces what was put there)
        f.starts_key = 0;
    }
    if (mode == kFindNames || mode == kFindText) {
        if (!f.engine) {
            f.engine = std::make_unique<Finder>();
            f.engine->set_notify([] {
                SDL_Event ev{};
                ev.type = SDL_EVENT_USER;
                ev.user.code = 10;
                SDL_PushEvent(&ev);
            });
        }
        FinderOptions o;
        o.ignore = cfg_.search_ignore;
        o.hidden = cfg_.search_hidden;
        o.gitignore = cfg_.search_gitignore;
        const bool other = f.root != workdir || o.ignore != f.walked_opt.ignore || o.hidden != f.walked_opt.hidden || o.gitignore != f.walked_opt.gitignore;
        const u64 now = SDL_GetTicksNS();
        const bool walk = other || f.stale || !f.walked_ns || now - f.walked_ns > kWalkAgainNs;
        f.walked_opt = o;
        f.engine->set_options(std::move(o));
        if (f.root != workdir) f.shown = nullptr;
        f.root = workdir;
        if (walk) {  // (in the background: what is typed is searched for in the list there is, and again when the new one is in)
            f.walked_ns = now;
            f.stale = false;
            f.engine->index(workdir);
        }
        if (mode == kFindNames) Find::list_near(*this);
        if (mode == kFindText && f.typed.empty()) {
            // What is selected in the text is what is searched for (its first
            // line); else what was searched for last. Either is selected, so
            // that typing replaces it.
            const Editor& ed = pane_->doc->ed;
            u64 lo = 0, hi = 0;
            if (!vim_selection(&lo, &hi) && ed.has_selection()) lo = ed.sel_min(), hi = ed.sel_max();
            if (hi > lo) {
                std::string words = ed.buf.text(lo, std::min<u64>(hi - lo, 400));
                if (const size_t nl = words.find('\n'); nl != std::string::npos) words.resize(nl);
                else if (hi - lo > 400) pop_char(&words);  // (it may have been cut in the middle of a character)
                const size_t a = words.find_first_not_of(" \t\r"), b = words.find_last_not_of(" \t\r");
                f.typed = a == std::string::npos ? std::string() : words.substr(a, b + 1 - a);
            }
            if (f.typed.empty()) f.typed = f.last_text;
            f.all = !f.typed.empty();
        }
    } else if (f.path() && f.typed.empty()) {
        f.typed = tilde(workdir) + "/";  // a path starts in the working directory
    }
    find_query();
    dirty_ = true;
}

void App::find_drop_script() {
    if (find_open_ && find_->script()) find_close(false);
}

void App::find_pick(std::vector<std::string> items, const std::string& prompt, bool input, const std::string& typed) {
    Find& f = *find_;
    if (find_open_ && f.script()) find_close(false);  // (the one open is answered "nothing" first)
    f.items = std::move(items);
    f.prompt = prompt;
    find_open(input ? kFindInput : kFindPick, typed);
}

void App::find_ask(u64 id, const std::string& host, const std::string& prompt, bool secret) {
    Find& f = *find_;
    if (find_open_ && f.script()) find_close(false);
    if (find_open_ && f.mode != kFindAsk) {  // (that box comes back when this is answered)
        f.back_mode = f.mode;
        f.back_typed = f.typed;
    }
    f.ask_id = id;
    f.ask_host = host;
    f.secret = secret;
    f.prompt = prompt;
    find_open(kFindAsk);
}

// The command search: every command, with its keys.
void App::palette_open() {
    Find& f = *find_;
    if (find_open_ && f.script()) find_close(false);
    f.items.clear();
    f.item_keys.clear();
    f.item_search.clear();
    f.item_syms.clear();
    f.item_ids.clear();
    for (PaletteItem& it : palette_items()) {
        std::string words = it.title + " " + it.keys;
        for (char& ch : words)
            if (ch == '+') ch = ' ';
        f.item_syms.push_back(key_symbols(it.keys, *font_));
        f.items.push_back(std::move(it.title));
        f.item_keys.push_back(std::move(it.keys));
        f.item_search.push_back(std::move(words));
        f.item_ids.push_back(it.id);
    }
    f.capture = f.chord_query = false;
    find_open(kFindCommand);
}

// The box, for a path or a line that comes back to `chose` (kFindOpen: a
// file, kFindFolder: a folder, kFindInput: what is typed after `prompt`).
void App::find_choose(int mode, const std::string& prompt, const std::string& typed, std::function<void(const std::string&)> chose) {
    Find& f = *find_;
    if (find_open_ && f.script()) find_close(false);
    f.items.clear();
    f.prompt = prompt;
    find_open(mode, typed);
    f.chose = std::move(chose);
}

// The box, with a list of blit's own to choose from: `chose` hears which (its index in items).
void App::find_choose_item(const std::string& prompt, std::vector<std::string> items, std::function<void(int)> chose, const std::string& typed) {
    Find& f = *find_;
    if (find_open_ && f.script()) find_close(false);
    f.items = std::move(items);
    f.prompt = prompt;
    find_open(kFindPick, typed);
    f.chose_item = std::move(chose);
}

// The same, with each row tried out as the choice comes to it, from the
// one at `at` (which is in effect already).
void App::find_preview_item(const std::string& prompt, std::vector<std::string> items, size_t at, std::function<void(int)> moved,
                            std::function<void(int)> chose, std::function<void()> cancelled) {
    find_choose_item(prompt, std::move(items), std::move(chose));
    Find& f = *find_;
    for (size_t i = 0; i < f.entries.size(); ++i)
        if (f.entries[i].index == at) f.sel = static_cast<u32>(i);
    f.moved = std::move(moved);
    f.cancelled = std::move(cancelled);
    f.moved_at = static_cast<int>(at);
    f.touch();
}

// The choice has moved in a list whose rows are tried out: this one is.
void App::Find::tried(App& a) {
    Find& f = *a.find_;
    if (!f.moved || f.mode != kFindPick || f.sel >= f.entries.size()) return;
    const int index = static_cast<int>(f.entries[f.sel].index);
    if (index == f.moved_at) return;
    f.moved_at = index;
    f.moved(index);
}

// Go to file, for a split just made: what is chosen goes into it, and Esc
// leaves it with the new file it has.
void App::find_for_split(Pane* p) {
    find_open(kFindNames);
    find_->into_pane = p ? p->id : 0;
}

u64 App::find_ask_id() const { return find_open_ && find_->mode == kFindAsk ? find_->ask_id : 0; }

// The question went unanswered (the connection ended): the box goes.
void App::find_drop_ask() {
    if (find_ask_id()) {
        find_->ask_id = 0;  // (nobody to tell)
        find_close(false);
    }
}

void App::find_close(bool chosen) {
    Find& f = *find_;
    if (!find_open_) return;
    if (f.mode == kFindAsk) {
        if (!chosen && f.ask_id) Remote::answer(f.ask_id, nullptr);  // (connecting fails)
        f.ask_id = 0;
        wipe(f.typed);
        find_open_ = false;
        pane_->cache_rev = ~0ull;
        dirty_ = true;
        if (f.back_mode >= 0) {  // the box that was open before the question
            const int mode = std::exchange(f.back_mode, -1);
            find_open(mode, std::exchange(f.back_typed, {}));
        }
        remote_asks();  // (another question, if there is one)
        return;
    }
    find_open_ = false;
    f.keep_hits();  // (for F4 to go through)
    if (f.mode == kFindFile && !f.pattern.empty() && !f.pattern.bad()) {  // what Cmd+G (and with modes n) goes on with
        f.last_file = f.pattern;
        if (cfg_.modal) vim_.set_search(f.pattern);
    }
    f.beside = f.all = f.asking = false;
    if (f.engine) f.engine->cancel();
    if (!chosen && f.mode == kFindSave) quit_after_save_ = close_after_save_ = false;  // no name: what was to follow the save does not
    pane_->cache_rev = ~0ull;
    dirty_ = true;
    if (!chosen && f.script() && !f.chose && !f.chose_item) script_chose(false, -1, {});  // (the script hears that nothing was chosen)
    f.chose = nullptr;
    f.chose_item = nullptr;
    f.moved = nullptr;
    f.into_pane = 0;
    if (auto back = std::exchange(f.cancelled, nullptr); back && !chosen) back();  // (Esc: what was tried out is put back)
    remote_asks();  // (a question ssh asked while this box was open)
}

// What is typed has changed: ask the worker again, or list the folder again.
void App::find_query() {
    Find& f = *find_;
    f.sel = f.top = 0;
    f.hover = -1;
    f.touch();
    dirty_ = true;
    if (f.mode == kFindNames) {
        split_place(f.typed, &f.query, &f.go_line, &f.go_col);
        while (f.heading(f.sel)) ++f.sel;  // (a heading is not chosen; one is never last)
        f.serial = f.engine->find_names(f.query);
        return;
    }
    if (f.mode == kFindText) {
        if (!f.typed.empty()) f.last_text = f.typed;
        // Words, as a rule; a pattern with regular expressions on; and with
        // "Replace with" there, what is typed as it stands, as replacing needs.
        f.pattern = f.regex || f.replacing ? Vim::make_pattern(f.typed, f.regex, f.exact) : Vim::Pattern{};
        f.serial = f.engine->find_text(f.typed, TextQuery{f.regex, f.replacing && !f.regex, f.exact});
        return;
    }
    if (f.mode == kFindFile) return Find::file_query(*this);
    if (f.mode == kFindAsk) {
        f.entries.clear();
        return;
    }
    if (f.mode == kFindCommand) {  // the commands whose name or keys have every word typed, the best first
        f.entries.clear();
        // Keys (pressed after Tab, or typed with a +): the commands with exactly those keys.
        auto lower = [](std::string t) {
            for (char& ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return t;
        };
        if (f.chord_query || f.typed.find('+') != std::string::npos) {
            const std::string want = lower(f.typed);
            for (u32 i = 0; i < f.items.size(); ++i) {
                const std::string keys = lower(f.item_keys[i]);
                for (size_t at = 0; at <= keys.size();) {
                    const size_t gap = keys.find("  ", at);
                    if (keys.substr(at, gap == std::string::npos ? std::string::npos : gap - at) == want) {
                        f.entries.push_back({f.items[i], false, Find::Entry::kPlain, 0, i});
                        break;
                    }
                    if (gap == std::string::npos) break;
                    at = gap + 2;
                }
            }
            if (!f.entries.empty() || f.chord_query) return;
        }
        const std::vector<FuzzyTerm> terms = fuzzy_terms(f.typed);
        for (u32 i = 0; i < f.items.size(); ++i) {
            int score = 0;
            bool all = true;
            for (const FuzzyTerm& t : terms) {
                const int one = fuzzy_score(t, f.item_search[i]);
                if (one < 0) {
                    all = false;
                    break;
                }
                score += one;
            }
            if (all) f.entries.push_back({f.items[i], false, Find::Entry::kPlain, score, i});
        }
        if (!terms.empty()) std::stable_sort(f.entries.begin(), f.entries.end(), [](const Find::Entry& a, const Find::Entry& b) { return a.score > b.score; });
        return;
    }
    if (f.script()) {
        // A script's items, those that have every word typed in them, the best first.
        f.entries.clear();
        if (f.mode == kFindInput) return;
        const std::vector<FuzzyTerm> terms = fuzzy_terms(f.typed);
        for (u32 i = 0; i < f.items.size(); ++i) {
            int score = 0;
            bool all = true;
            for (const FuzzyTerm& t : terms) {
                const int one = fuzzy_score(t, f.items[i]);
                if (one < 0) {
                    all = false;
                    break;
                }
                score += one;
            }
            if (all) f.entries.push_back({f.items[i], false, Find::Entry::kPlain, score - static_cast<int>(std::min<size_t>(f.items[i].size(), 64) / 8), i});
        }
        if (!terms.empty()) std::stable_sort(f.entries.begin(), f.entries.end(), [](const Find::Entry& a, const Find::Entry& b) { return a.score > b.score; });
        Find::tried(*this);  // (what is typed has put another row first)
        return;
    }
    const std::string* root = panel_root();
    const std::string full = in_full(f.typed, root ? *root : default_workdir());
    const size_t slash = full.rfind('/');
    f.dir = full.substr(0, slash + 1);
    f.leaf = full.substr(slash + 1);
    if (f.dir != f.listed) {
        f.listed = f.dir;
        f.listing.clear();
        std::vector<RemoteEntry> list;
        if (f.dir == "ssh://") {  // the machines there are to go to
            for (std::string& h : ssh_hosts()) f.listing.push_back({std::move(h), true, Find::Entry::kPlain, 0});
        } else if (is_remote(f.dir)) {  // a folder on another machine: listed on a thread, and here when it has been (see find_remote_ready)
            if (remote_listing(f.dir, &list))
                for (RemoteEntry& e : list) f.listing.push_back({std::move(e.name), e.dir, Find::Entry::kPlain, 0});
            else f.listed.clear();
        } else if (DIR* d = ::opendir(f.dir.c_str())) {
            while (const dirent* e = ::readdir(d)) {
                const char* name = e->d_name;
                if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
                bool dir = e->d_type == DT_DIR;
                if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
                    struct stat st;
                    if (::stat((f.dir + name).c_str(), &st) != 0) continue;
                    dir = S_ISDIR(st.st_mode);
                }
                f.listing.push_back({name, dir, Find::Entry::kPlain, 0});
            }
            ::closedir(d);
        }
        // Folders first, then by name, capitals among the small letters.
        std::sort(f.listing.begin(), f.listing.end(), [](const Find::Entry& a, const Find::Entry& b) {
            if (a.dir != b.dir) return a.dir;
            const int c = SDL_strcasecmp(a.name.c_str(), b.name.c_str());
            return c != 0 ? c < 0 : a.name < b.name;
        });
    }
    f.entries.clear();
    const bool folders_only = f.mode == kFindFolder;
    // (A folder is chosen by going into it and taking "this folder", which
    // is the first row there; while a name is being typed the folders it
    // matches come first.)
    if (folders_only && f.leaf.empty()) f.entries.push_back({".", true, Find::Entry::kHere, 0});
    if (f.leaf.empty()) {
        if (f.dir.size() > 1) f.entries.push_back({"..", true, Find::Entry::kParent, 0});
        for (const Find::Entry& e : f.listing)
            if ((e.dir || !folders_only) && e.name[0] != '.') f.entries.push_back(e);  // (names with a dot in front show once a dot is typed)
        return;
    }
    const std::vector<FuzzyTerm> terms = fuzzy_terms(f.leaf);
    std::vector<Find::Entry> some;
    bool exact = false;
    for (const Find::Entry& e : f.listing) {
        if (!e.dir && folders_only) continue;
        const int s = terms.empty() ? 0 : fuzzy_score(terms[0], e.name);
        if (s < 0) continue;
        some.push_back(e);
        some.back().score = s - static_cast<int>(std::min<size_t>(e.name.size(), 64));  // of equals, the shorter name
        exact = exact || e.name == f.leaf;
    }
    std::stable_sort(some.begin(), some.end(), [](const Find::Entry& a, const Find::Entry& b) { return a.score > b.score; });
    // A machine ~/.ssh/config does not name (user@host, an address): it as typed, first.
    if (f.dir == "ssh://" && !exact && f.leaf.find_first_of(" \t/") == std::string::npos && f.leaf.front() != '-')
        some.insert(some.begin(), {f.leaf, true, Find::Entry::kPlain, 0});
    // A name that is not there yet: one to save under, or a new file to open.
    if (!exact && !folders_only && (f.mode == kFindSave || some.empty())) f.entries.push_back({f.leaf, false, Find::Entry::kNew, 0});
    for (Find::Entry& e : some) f.entries.push_back(std::move(e));
    // (To save under a name already there is to choose it: it comes first.)
    if (f.mode == kFindSave && exact)
        std::stable_partition(f.entries.begin(), f.entries.end(), [&](const Find::Entry& e) { return e.name == f.leaf; });
}

// The worker has something new.
void App::find_sync() {
    Find& f = *find_;
    if (!find_open_ || !f.engine || (f.mode != kFindNames && f.mode != kFindText)) return;
    std::shared_ptr<const FindResults> r = f.engine->results();
    if (!r || r->text != (f.mode == kFindText)) return;
    // (An answer to an earlier search is shown until the one asked for
    // comes, but only of the same folder.)
    if (r->index && r->index->root != f.root) return;
    f.shown = std::move(r);
    const size_t n = f.count();
    if (f.sel >= n) f.sel = n ? static_cast<u32>(n - 1) : 0;
    f.touch();
    dirty_ = true;
}

// Are the rows on show the answer to what is typed now? A key that acts
// on a row (Enter, an arrow) may come before the worker has answered the
// last letter; it waits a moment for the answer rather than act on the rows
// of an earlier search, or on none.
bool App::find_current(int wait_ms) {
    Find& f = *find_;
    if ((f.mode != kFindNames && f.mode != kFindText) || f.nearby() || f.line_only()) return true;  // (the rows are not the worker's)
    if (!f.engine) return false;
    auto current = [&] { return f.shown && f.shown->serial == f.serial && f.shown->index && f.shown->index->root == f.root; };
    if (!current()) {
        f.engine->wait_idle_for(wait_ms);
        find_sync();
    }
    return current();
}

// The file a save was waiting for a name for gets it, and is saved.
void App::save_named(const std::string& path, u64 doc) {
    const bool quitting = quit_after_save_, closing = close_after_save_;
    quit_after_save_ = close_after_save_ = false;
    size_t at = 0;
    Pane* where = nullptr;
    if (!find_doc(doc, &where, &at)) return;  // (the tab may have gone meanwhile)
    focus_pane(where);
    tab_show(at);
    stop_lsp();  // it knows the file by its name
    pane_->doc->ed.path = path;
    save();
    if (!pane_->doc->ed.modified() || remote_saving(doc)) {  // (to another machine: closing and quitting wait for it)
        if (closing) tab_close(at);
        if (quitting) request_quit();  // on to the next unsaved file, or out
    }
}

// Enter: open what is chosen, go into the folder chosen, save under the
// name typed.
void App::find_accept() {
    Find& f = *find_;
    if (f.mode == kFindCommand) {
        if (f.sel >= f.entries.size()) return;
        const u32 id = f.item_ids[f.entries[f.sel].index];
        find_close(true);
        palette_run(id);
        return;
    }
    if (f.mode == kFindAsk) {  // the answer, to ssh
        const u64 id = std::exchange(f.ask_id, 0);
        Remote::answer(id, &f.typed);
        find_close(true);
        return;
    }
    if (f.mode == kFindInput && f.chose) {
        auto chose = std::move(f.chose);
        const std::string typed = f.typed;
        f.chose = nullptr;
        find_open_ = false;  // (shut without a script hearing of it)
        pane_->cache_rev = ~0ull;
        dirty_ = true;
        chose(typed);
        return;
    }
    if (f.script()) {
        if (f.mode == kFindPick && f.sel >= f.entries.size()) return;
        const int index = f.mode == kFindPick ? static_cast<int>(f.entries[f.sel].index) : -1;
        const std::string typed = f.typed;
        if (f.chose_item) {  // blit's own list
            auto chose = std::move(f.chose_item);
            f.chose_item = nullptr;
            find_close(true);
            chose(index);
            return;
        }
        find_close(true);
        script_chose(true, index, typed);
        return;
    }
    if (f.mode == kFindFile) {  // (Enter: the next match, or with "Replace with" there, this one replaced)
        if (f.replacing) Find::file_replace(*this, false);
        else Find::file_go(*this, f.cur_ok ? f.cur.start : pane_->doc->ed.cursor(), true, !f.cur_ok);
        Find::file_count(*this);
        return;
    }
    if (f.mode == kFindText && f.asking) return Find::files_replace(*this);
    if (f.mode == kFindNames || f.mode == kFindText) {
        const bool beside = std::exchange(f.beside, false);
        Pane* into = nullptr;  // (the split it was opened for, if it still is)
        for (const std::unique_ptr<Pane>& p : panes_)
            if (f.into_pane && p->id == f.into_pane) into = p.get();
        if (f.line_only()) {  // ":120": a line of the file in front
            if (!f.go_line) return;  // (no number yet)
            const u64 line = f.go_line - 1, col = f.go_col ? f.go_col - 1 : 0;
            find_close(true);
            remember_place();  // (Ctrl-O comes back from it)
            Find::go(*this, line, col);
            return;
        }
        std::string path;
        u64 line = ~0ull, col = 0;  // (from 0; ~0: wherever the file is shown)
        if (f.nearby()) {
            if (f.sel >= f.near.size() || f.heading(f.sel)) return;
            path = f.near[f.sel].full;
        } else {
            if (!find_current(400) || f.sel >= f.shown->hits.size()) return;
            const FindHit& hit = f.shown->hits[f.sel];
            path = f.shown->index->root + "/" + std::string(f.shown->index->path(hit.file));
            if (f.mode == kFindText) {  // the line found, and F4 goes on from it
                line = hit.line;
                col = hit.col;
                f.last = f.shown;
                f.last_at = f.sel;
            } else if (f.go_line) {
                line = f.go_line - 1;
                col = f.go_col ? f.go_col - 1 : 0;
            }
        }
        find_close(true);
        remember_place();  // (Ctrl-O comes back from it)
        if (!(beside ? Find::open_beside(*this, path) : into ? Find::open_in(*this, into, path, true) : open_path(path.c_str()))) return;
        if (line != ~0ull) Find::go(*this, line, col);
        dirty_ = true;
        return;
    }
    if (f.sel >= f.entries.size()) return;
    const Find::Entry e = f.entries[f.sel];
    const std::string here = f.dir.size() > 1 ? f.dir.substr(0, f.dir.size() - 1) : f.dir;
    if (e.kind == Find::Entry::kParent) {
        const std::string up = parent_of(here);
        f.typed = tilde(up) + (up == "/" ? "" : "/");
        return find_query();
    }
    if (e.kind == Find::Entry::kHere) {
        if (auto chose = std::move(f.chose)) {
            find_close(true);
            return chose(here);
        }
        find_close(true);
        project_open(here);  // (its session, if it has one)
        return;
    }
    if (e.dir) {  // into it (a machine: its home folder)
        f.typed = f.dir == "ssh://" ? f.dir + e.name + "/~/" : tilde(f.dir + e.name) + "/";
        return find_query();
    }
    const std::string path = f.dir + e.name;
    if (f.mode == kFindOpen && f.chose) {
        auto chose = std::move(f.chose);
        find_close(true);
        return chose(path);
    }
    if (f.mode == kFindOpen) {
        find_close(true);
        remember_place();
        open_path(path.c_str());
        return;
    }
    if (f.mode == kFindSave) {
        if (e.kind != Find::Entry::kNew && win_) {  // (a scripted run has nobody to ask)
            const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"}, {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Replace"}};
            const std::string what = "\"" + e.name + "\" is there already. Replace it?";
            const SDL_MessageBoxData box{SDL_MESSAGEBOX_WARNING, win_, "blit", what.c_str(), 2, buttons, nullptr};
            int hit = 0;
            if (!SDL_ShowMessageBox(&box, &hit) || hit != 1) return;
        }
        const u64 doc = f.save_doc;
        find_close(true);
        save_named(path, doc);
    }
}

// Tab: a path is completed with the row chosen; names and text change
// places (what is typed stays).
void App::find_tab() {
    Find& f = *find_;
    // Search in files and Find: "Replace with" opens, and then Tab goes from one field to the other.
    if (f.mode == kFindText || f.mode == kFindFile) {
        if (!f.replacing) {
            f.replacing = f.in_with = true;
            f.with_all = !f.with.empty();
            if (f.mode == kFindText) find_query();  // (what is typed is then looked for as it stands)
        } else {
            f.in_with = !f.in_with;
        }
        f.asking = false;
        f.touch();
        dirty_ = true;
        return;
    }
    if (f.mode == kFindNames) return find_open(kFindText, f.typed);
    if (f.script() || f.sel >= f.entries.size()) return;
    const Find::Entry& e = f.entries[f.sel];
    if (e.kind != Find::Entry::kPlain) return;
    f.typed = f.dir == "ssh://" ? f.dir + e.name + "/~/" : tilde(f.dir + e.name) + (e.dir ? "/" : "");
    find_query();
}

void App::find_move(i64 by) {
    Find& f = *find_;
    // (A key pressed just after typing moves in what was typed's results:
    // they are waited for a moment if they are not in yet. Once they are,
    // nothing is waited for, however fast the key repeats.)
    find_current(100);
    const i64 n = static_cast<i64>(f.count());
    if (n == 0) return;
    // Up from the first is the last, and down from the last the first; a page stops at the ends.
    i64 to = static_cast<i64>(f.sel) + by;
    if (by == 1 || by == -1) {
        to = ((to % n) + n) % n;
        for (i64 k = 0; k < n && f.heading(static_cast<size_t>(to)); ++k) to = ((to + by) % n + n) % n;  // (over a heading)
    } else {
        to = std::clamp<i64>(to, 0, n - 1);
        // (On a heading: the nearest row past it, or else before it.)
        const i64 step = by < 0 ? -1 : 1;
        for (i64 a = to, b = to; f.heading(static_cast<size_t>(to));) {
            a += step, b -= step;
            if (a >= 0 && a < n && !f.heading(static_cast<size_t>(a))) to = a;
            else if (b >= 0 && b < n && !f.heading(static_cast<size_t>(b))) to = b;
            else if ((a < 0 || a >= n) && (b < 0 || b >= n)) return;
        }
    }
    f.sel = static_cast<u32>(to);
    f.touch();
    dirty_ = true;
    Find::tried(*this);
}

// F4 (and Shift+F4, back): the next hit of the last search in files,
// opened without the box, and which of how many it is on the bottom line.
void App::find_step(int by) {
    Find& f = *find_;
    const std::shared_ptr<const FindResults> r = f.last;
    if (!r || !r->index || r->hits.empty()) {
        Find::say(*this, "No search in files yet: " + key_symbols(kCmdShiftF, *font_));
        return;
    }
    const i64 n = static_cast<i64>(r->hits.size());
    const i64 at = f.last_at < 0 ? (by < 0 ? n - 1 : 0) : ((f.last_at + by) % n + n) % n;
    f.last_at = at;
    const FindHit& hit = r->hits[static_cast<size_t>(at)];
    const std::string path = r->index->root + "/" + std::string(r->index->path(hit.file));
    remember_place();  // (Ctrl-O comes back from it)
    if (!open_path(path.c_str())) return;
    Find::go(*this, hit.line, hit.col);
    Find::say(*this, "Hit " + std::to_string(at + 1) + " of " + std::to_string(n) + (r->capped ? "+" : ""));
}

// A key press. While the box is open they are all its own; closed, three
// of them open it.
bool App::find_key(const SDL_KeyboardEvent& k) {
    Find& f = *find_;
    const bool cmd = k.mod & kPrimaryMod, shift = k.mod & SDL_KMOD_SHIFT, ctrl = k.mod & SDL_KMOD_CTRL, alt = k.mod & SDL_KMOD_ALT;
    int want = -1;
    if (cmd && !alt && k.key == SDLK_P && !shift) want = kFindNames;
    else if (cmd && !alt && k.key == SDLK_F && shift) want = kFindText;
    else if (cmd && !alt && k.key == SDLK_O && !shift) want = kFindOpen;
    if (want >= 0) {
        if (find_open_ && f.mode == want) find_close(false);  // the same key again shuts it
        else find_open(want, find_open_ && want != kFindOpen && (f.mode == kFindNames || f.mode == kFindText) ? f.typed : std::string());
        return true;
    }
    if (!find_open_) return false;
    if (cmd && k.key == SDLK_BACKSLASH) {  // another split: this box shuts, and the split is made as ever
        find_close(false);
        return false;
    }
    // (While the box is open the commands' keys are not looked at: those that are about finding are its own.)
    if (cmd && k.key == SDLK_F && !shift) {  // Cmd+F, and Cmd+Option+F with "Replace with"
        if (f.mode == kFindFile && !alt) find_close(false);
        else find_in_file(alt);
        return true;
    }
    if (cmd && shift && !alt && k.key == SDLK_H) {
        find_replace_in_files();
        return true;
    }
    f.drop_text = f.drop_option = false;
    if (alt && !cmd && !ctrl && (k.key == SDLK_R || k.key == SDLK_C) && (f.mode == kFindText || f.mode == kFindFile)) {
        find_toggle(k.key == SDLK_R);
        f.drop_option = true;  // (Option+R types "®", Option+C "ç")
        return true;
    }
    if (f.mode == kFindFile && ((cmd && k.key == SDLK_G) || k.key == SDLK_F3 || k.key == SDLK_UP || k.key == SDLK_DOWN)) {  // the next match, or the one before
        const bool back = k.key == SDLK_UP || ((k.key == SDLK_G || k.key == SDLK_F3) && shift);
        Find::file_go(*this, f.cur_ok ? f.cur.start : pane_->doc->ed.cursor(), !back, false);
        Find::file_count(*this);
        return true;
    }
    if (f.mode == kFindCommand && f.capture) {  // keys to look up: what they are is what is searched for
        const SDL_Keycode key = k.key;
        if (key == SDLK_LSHIFT || key == SDLK_RSHIFT || key == SDLK_LCTRL || key == SDLK_RCTRL || key == SDLK_LALT || key == SDLK_RALT || key == SDLK_LGUI ||
            key == SDLK_RGUI)
            return true;
        f.capture = false;
        f.typed = chord_text(k.key, k.mod);
        f.chord_query = true;
        f.drop_text = true;
        find_query();
        return true;
    }
    if (f.mode == kFindCommand && f.chord_query && k.key == SDLK_BACKSPACE) {  // (keys looked up go whole)
        f.chord_query = false;
        f.typed.clear();
        find_query();
        return true;
    }
    if (f.mode == kFindCommand && k.key == SDLK_TAB) {  // Tab: the next keys pressed are looked up
        f.capture = !f.capture;
        f.touch();
        dirty_ = true;
        return true;
    }
    if (f.asking && k.key != SDLK_RETURN && k.key != SDLK_KP_ENTER) {  // the question has its answer: anything but Enter is "no"
        f.asking = false;
        f.touch();
        dirty_ = true;
        if (k.key == SDLK_ESCAPE) return true;
    }
    // The field with the keyboard: what is typed, or "Replace with".
    std::string& field = f.in_with ? f.with : f.typed;
    bool& lit = f.in_with ? f.with_all : f.all;
    auto edited = [&] {
        if (f.in_with) {  // (nothing to look for again: the previews are drawn afresh)
            f.touch();
            dirty_ = true;
        } else {
            find_query();
        }
    };
    const i64 page = std::max<i64>(1, static_cast<i64>(f.list_rows) - 1);
    switch (k.key) {
        case SDLK_ESCAPE: find_close(false); break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (f.mode == kFindFile) {
                if (cmd && f.replacing) Find::file_replace(*this, true);
                else if (shift) Find::file_go(*this, f.cur_ok ? f.cur.start : pane_->doc->ed.cursor(), false, false), Find::file_count(*this);
                else find_accept();
                break;
            }
            if (f.mode == kFindText && f.replacing && cmd && !f.asking) {
                Find::files_ask(*this);
                break;
            }
            f.beside = cmd && (f.mode == kFindNames || f.mode == kFindText);  // (Cmd+Enter: in the split to the right)
            find_accept();
            break;
        case SDLK_TAB: find_tab(); break;
        case SDLK_UP: find_move(-1); break;
        case SDLK_DOWN: find_move(1); break;
        case SDLK_PAGEUP: find_move(-page); break;
        case SDLK_PAGEDOWN: find_move(page); break;
        case SDLK_LEFT:
        case SDLK_RIGHT:
        case SDLK_HOME:
        case SDLK_END:
            if (lit) {  // what is typed stays, no longer selected, to be typed onto
                lit = false;
                f.touch();
                dirty_ = true;
            }
            break;
        case SDLK_BACKSPACE:
            if (field.empty()) break;
            if (lit) {  // selected: it all goes
                field.clear();
                lit = false;
            } else if (cmd || alt) {
                // A word back; in a path, a folder back.
                const bool path = f.path();
                const char stop = path ? '/' : ' ';
                while (!field.empty() && field.back() == stop) field.pop_back();
                while (!field.empty() && field.back() != stop) field.pop_back();
                if (cmd && !path) field.clear();
            } else {
                pop_char(&field);
            }
            edited();
            break;
        default:
            if (ctrl && (k.key == SDLK_P || k.key == SDLK_K)) find_move(-1);
            else if (ctrl && (k.key == SDLK_N || k.key == SDLK_J)) find_move(1);
            else if (ctrl && k.key == SDLK_U) field.clear(), lit = false, edited();
            else if (ctrl && k.key == SDLK_W) {
                if (std::exchange(lit, false)) field.clear();
                while (!field.empty() && (field.back() == ' ' || field.back() == '/')) field.pop_back();
                while (!field.empty() && field.back() != ' ' && field.back() != '/') field.pop_back();
                edited();
            } else if (cmd && k.key == SDLK_V) {
                // What the clipboard holds, as far as its first line goes (in place of what is typed, if that is selected).
                if (char* clip = SDL_GetClipboardText()) {
                    std::string_view text = clip;
                    text = text.substr(0, text.find('\n'));
                    if (std::exchange(lit, false)) field.clear();
                    field += std::string(text.substr(0, 400));
                    SDL_free(clip);
                    edited();
                }
            }
            break;
    }
    return true;
}

// Typed text goes into the box while it is open (into the field with the keyboard).
bool App::find_typed(const char* text) {
    Find& f = *find_;
    if (!find_open_) return false;
    if (f.drop_text) {
        f.drop_text = false;
        return true;
    }
    if (std::exchange(f.drop_option, false) && static_cast<u8>(text[0]) >= 0x80) return true;
    if (f.asking) {  // (typing is no answer to the question)
        f.asking = false;
        f.touch();
    }
    if (f.in_with) {
        if (std::exchange(f.with_all, false)) f.with.clear();
        f.with += text;
        f.touch();
        dirty_ = true;
        return true;
    }
    // (Typing after keys looked up starts a search of its own; typed over
    // what is selected, replaces it.)
    if (f.chord_query || f.all) {
        f.chord_query = f.all = false;
        f.typed.clear();
    }
    f.typed += text;
    find_query();
    return true;
}

// The row of results a point (in pixels) is on, or -1.
int App::find_row_at(int px, int py) const {
    const Find& f = *find_;
    if (!find_open_ || px < f.rect.x || px >= f.rect.x + f.rect.w || py < f.rect.y || py >= f.rect.y + f.rect.h) return -1;
    const int ch = font_->cell_h(), pad_y = look().pad_y;
    const int row = (py - f.rect.y - pad_y) / ch - static_cast<int>(f.fields());  // (under the fields)
    if (py < f.rect.y + pad_y || row < 0 || row >= static_cast<int>(f.list_rows)) return -1;
    const size_t at = f.top + static_cast<size_t>(row);
    return at < f.count() ? static_cast<int>(at) : -1;
}

bool App::find_mouse_down(const SDL_MouseButtonEvent& b) {
    Find& f = *find_;
    if (!find_open_) return false;
    const int px = static_cast<int>(b.x * density_), py = static_cast<int>(b.y * density_);
    const bool inside = px >= f.rect.x && px < f.rect.x + f.rect.w && py >= f.rect.y && py < f.rect.y + f.rect.h;
    if (!inside) {  // a click beside it shuts it
        find_close(false);
        return true;
    }
    // The toggles in the field: ".*" (regular expressions) and "Aa" (case).
    if (f.toggle_col && b.button == SDL_BUTTON_LEFT && py >= f.rect.y + look().pad_y && py < f.rect.y + look().pad_y + font_->cell_h()) {
        const int col = (px - f.rect.x - look().edge) / font_->cell_w();
        if (col >= static_cast<int>(f.toggle_col) && col < static_cast<int>(f.toggle_col) + 5) {
            find_toggle(col < static_cast<int>(f.toggle_col) + 3);
            return true;
        }
    }
    const int row = find_row_at(px, py);
    if (row >= 0 && b.button == SDL_BUTTON_LEFT && !f.heading(static_cast<size_t>(row))) {
        f.sel = static_cast<u32>(row);
        find_accept();
    }
    return true;
}

bool App::find_mouse_motion(const SDL_MouseMotionEvent& m) {
    Find& f = *find_;
    if (!find_open_) return false;
    const int row = find_row_at(static_cast<int>(m.x * density_), static_cast<int>(m.y * density_));
    if (row != f.hover) {
        f.hover = row;
        f.touch();
        dirty_ = true;
    }
    return true;
}

bool App::find_wheel(const SDL_MouseWheelEvent& w) {
    Find& f = *find_;
    if (!find_open_) return false;
    const i64 n = static_cast<i64>(f.count()), rows = static_cast<i64>(f.list_rows);
    const i64 by = w.y > 0 ? -std::max<i64>(1, std::llround(w.y * 3)) : w.y < 0 ? std::max<i64>(1, std::llround(-w.y * 3)) : 0;
    const i64 top = std::clamp<i64>(static_cast<i64>(f.top) + by, 0, std::max<i64>(0, n - rows));
    if (top != static_cast<i64>(f.top)) {
        f.top = static_cast<u32>(top);
        // (The chosen row stays among the ones on show, and is not a heading.)
        f.sel = static_cast<u32>(std::clamp<i64>(f.sel, top, top + std::max<i64>(rows, 1) - 1));
        while (f.heading(f.sel) && f.sel + 1 < n) ++f.sel;
        f.touch();
        dirty_ = true;
        Find::tried(*this);
    }
    return true;
}

// ---------------------------------------------------------------- drawing

// The box: a field (what the box is for, what is typed, how many there
// are), the rows, and a footer (where it looks, and its keys), all from one
// left edge. It is as wide as what is in it needs, up to kLookPickerCols.
void App::find_layout(int w, int h, FrameDesc& fd) {
    Find& f = *find_;
    if (!find_open_ || fd.popup_count >= std::size(fd.popups)) return;
    Font& font = *font_;
    const Look l = look();
    const int cw = font.cell_w(), ch = font.cell_h(), pad_x = l.edge, pad_y = l.pad_y;
    const u32 cap = static_cast<u32>(std::clamp((w - 2 * pad_x) / cw - 4, 20, static_cast<int>(kLookPickerCols)));
    const u32 room = static_cast<u32>(std::clamp((h * 7 / 10 - 2 * pad_y) / ch - 2, 3, static_cast<int>(kMaxRows)));
    const size_t n = f.count();
    f.list_rows = f.mode == kFindInput || f.mode == kFindAsk || f.mode == kFindFile ? 0 : static_cast<u32>(std::min<size_t>(room, std::max<size_t>(n, 1)));
    // The chosen row is kept among those on show.
    if (f.sel < f.top) f.top = f.sel;
    if (f.sel >= f.top + f.list_rows) f.top = f.sel + 1 - f.list_rows;
    if (f.top + f.list_rows > n) f.top = n > f.list_rows ? static_cast<u32>(n - f.list_rows) : 0;
    const u32 fields = f.fields();
    const u32 rows = fields + f.list_rows + 1;  // the fields, the rows, the footer
    // The field's tone: the box's background a little way toward a chosen
    // row's. (Each frame: it is not in the cells, and the theme may change.)
    for (int k = 0; k < 3; ++k) f.tones[kFieldTone][k] = theme_.popup_bg[k] + (theme_.sel[k] - theme_.popup_bg[k]) * 0.35f;
    f.tones[kFieldTone][3] = 1;

    if (f.drawn != f.state || f.drawn_font != font.generation() || f.drawn_cap != cap || f.drawn_rows != rows) {
        f.drawn = f.state;
        f.drawn_font = font.generation();
        f.drawn_cap = cap;
        f.drawn_rows = rows;
        TextBox& box = f.box;
        const bool nerd = font.has(0xE5FF) && font.has(0xE61D);
        const bool path = f.path(), names = f.mode == kFindNames, text = f.mode == kFindText, listed = f.mode == kFindPick || f.mode == kFindCommand;
        const bool infile = f.mode == kFindFile, toggles = text || infile;
        const bool lit_by_pattern = text && (f.regex || f.replacing) && !f.pattern.empty() && !f.pattern.bad();

        // The field: a word that says what the box is for, what is typed (a
        // secret as a dot for each letter, keys looked up as keys) and how
        // many there are.
        static const char* const kLabel[] = {"Go to file", "Search in files", "Open", "Save as", "Folder", "Pick", "Input", "ssh", "Command search", "Find"};
        const std::string_view label = (f.script() || f.mode == kFindAsk || f.chose) && !f.prompt.empty() ? std::string_view(f.prompt) : std::string_view(kLabel[f.mode]);
        const bool secret = f.mode == kFindAsk && f.secret, pressed = f.mode == kFindCommand && f.chord_query;
        std::string masked;
        if (secret)
            for (size_t i = 0; i < f.typed.size(); ++i)
                if (!is_cont(static_cast<u8>(f.typed[i]))) masked += font.has(0x2022) ? "\xE2\x80\xA2" : "*";
        if (pressed) masked = key_symbols(f.typed, font);
        const std::string_view typed = secret || pressed ? std::string_view(masked) : std::string_view(f.typed);
        const bool reading = (names || text) && (!f.shown || !f.shown->index || f.shown->indexing);
        const size_t files = f.shown && f.shown->index ? f.shown->index->size() : 0;
        size_t longest = 0;  // (a search for text: its longest word; a phrase or a pattern, all of it)
        if (text && (f.regex || f.replacing)) longest = f.pattern.bad() ? 0 : f.regex ? std::max<size_t>(f.typed.size(), f.typed.empty() ? 0 : 2) : f.typed.size();
        else if (text)
            for (const FuzzyTerm& t : fuzzy_terms(f.typed)) longest = std::max(longest, t.text.size());
        std::string count;
        bool going = reading;  // (there may be more: an ellipsis after the count)
        if (names && f.shown && f.shown->index && !f.line_only()) {
            const std::string total = std::to_string(files) + (f.shown->index->truncated ? "+" : "");
            count = f.nearby() ? total + " files" : std::to_string(f.shown->matched) + " / " + total;
        } else if (text && longest >= 2 && f.shown && f.shown->text) {
            u64 found = f.shown->matched;  // (lines; with "Replace with" there, the matches on them)
            if (f.replacing) {
                found = 0;
                for (const FindHit& hit : f.shown->hits) found += hit.count;
            }
            count = std::to_string(found) + (f.shown->capped ? "+" : "") + " in " + std::to_string(f.shown->files_hit) + (f.shown->files_hit == 1 ? " file" : " files");
            going = going || f.shown->searching;
        } else if (infile && !f.pattern.empty() && !f.pattern.bad()) {
            Find::file_count(*this);  // (the text may have changed under the box)
            if (f.counted && f.starts.empty()) {
                count = "No matches";
            } else if (f.counted) {
                const u64 at = f.cur_ok ? static_cast<u64>(std::lower_bound(f.starts.begin(), f.starts.end(), f.cur.start) - f.starts.begin()) : f.starts.size();
                const std::string total = std::to_string(f.starts.size()) + (f.starts_capped ? "+" : "");
                count = at < f.starts.size() && f.starts[at] == f.cur.start ? std::to_string(at + 1) + " of " + total : total + (f.starts.size() == 1 ? " match" : " matches");
            } else if (!f.cur_ok) {
                count = "No match";
            }
        } else if (f.mode == kFindPick || (f.mode == kFindCommand && !f.capture)) {
            count = std::to_string(n) + " / " + std::to_string(f.items.size());
        } else if (path && f.listed != f.dir && remote_listing_busy()) {
            count = "Listing\xE2\x80\xA6";  // (a folder on another machine, on its way)
        }
        if (going && !count.empty()) count += "\xE2\x80\xA6";

        // The footer: where the box looks (or what it waits for), and its keys.
        std::string where;
        std::span<const Hint> hints;
        switch (f.mode) {
            case kFindNames: hints = f.line_only() ? std::span<const Hint>(kHintsLine) : std::span<const Hint>(kHintsNames); break;
            case kFindText: hints = f.asking ? std::span<const Hint>(kHintsSure) : f.replacing ? std::span<const Hint>(kHintsTextReplace) : std::span<const Hint>(kHintsText); break;
            case kFindFile: hints = f.replacing ? std::span<const Hint>(kHintsFileReplace) : std::span<const Hint>(kHintsFile); break;
            case kFindOpen: hints = kHintsOpen; break;
            case kFindSave: hints = kHintsSave; break;
            case kFindFolder: hints = kHintsFolder; break;
            case kFindPick: hints = kHintsPick; break;
            case kFindInput: hints = kHintsInput; break;
            case kFindAsk:
                hints = kHintsAsk;
                where = f.ask_host + (f.secret ? " (hidden as it is typed)" : "");
                break;
            case kFindCommand:
                hints = f.capture ? std::span<const Hint>(kHintsCapture) : std::span<const Hint>(kHintsCommand);
                if (f.capture) where = "Press the keys to look up";
                break;
            default: break;
        }
        if (names || text) where = tilde(f.root);
        // What the footer says in place of where: the question before
        // replacing in files, or why what is typed is no pattern.
        Ink where_ink = ink::kMuted;
        if (toggles && f.pattern.bad()) {
            where = f.pattern.error;
            where_ink = ink::kError;
        } else if (f.asking) {
            where = "Replace " + std::to_string(f.ask_hits) + (f.ask_hits == 1 ? " hit in " : " hits in ") + std::to_string(f.ask_files) + (f.ask_files == 1 ? " file?" : " files?");
            where_ink = ink::kAccentStrong;
        } else if (infile) {
            const std::string& in = pane_->doc->ed.path;
            where = in.empty() ? std::string("in this file") : "in " + in.substr(in.rfind('/') + 1);
        }

        // A list with nothing in it says why, or what to do.
        std::string empty;
        bool empty_plain = false;  // (it says what Enter does)
        if (n == 0 && f.list_rows > 0) {
            const std::string_view asked = names ? std::string_view(f.query) : path ? std::string_view(f.leaf) : std::string_view(f.typed);
            const std::string quoted = "'" + fit_text(asked, 40) + "'";
            if (f.line_only()) {
                const std::string& in = pane_->doc->ed.path;
                const std::string file = in.empty() ? std::string("this file") : in.substr(in.rfind('/') + 1);
                empty = f.go_line ? "Go to line " + std::to_string(f.go_line) + (f.go_col ? ", column " + std::to_string(f.go_col) : std::string()) + " of " + file
                                  : std::to_string(pane_->doc->ed.buf.line_count()) + " lines in " + file;
                empty_plain = f.go_line != 0;
            } else if (names) {
                empty = reading && !files   ? std::string("Reading the folder\xE2\x80\xA6")
                        : !asked.empty()    ? "No file matches " + quoted
                        : !panel_root()     ? "Choose a project folder: " + key_symbols(kCmdShiftO, font)
                                            : "No files in " + tilde(f.root);
            } else if (text) {
                empty = f.pattern.bad()                                                                   ? std::string("Not a pattern: see below")
                        : longest < 2                                                                     ? std::string(f.regex     ? "Type a pattern"
                                                                                                                        : f.replacing ? "Type two letters or more, as they stand"
                                                                                                                                      : "Type two letters or more; words in any order")
                        : !f.shown || !f.shown->text || f.shown->searching || (reading && !files) ? std::string("Searching\xE2\x80\xA6")
                        : !f.shown->note.empty()                                                  ? f.shown->note
                                                                                                  : "No line has " + quoted;
            } else if (path) {
                empty = f.listed != f.dir && remote_listing_busy() ? "Listing\xE2\x80\xA6" : "Nothing here by that name";
            } else if (f.mode == kFindCommand) {
                empty = f.chord_query ? std::string("No command has those keys") : "No command matches " + quoted;
            } else {
                empty = "Nothing matches " + quoted;
            }
        }

        // The rows on show, measured first: the box is as wide as they need.
        const std::vector<FuzzyTerm> terms = fuzzy_terms(path ? f.leaf : names ? f.query : f.typed);
        const std::string here = path ? tilde(f.dir.size() > 1 ? f.dir.substr(0, f.dir.size() - 1) : f.dir) : std::string();
        const std::string_view note = f.mode == kFindSave ? "save under this name" : "a new file";
        // Lines found: the file and line number of each, fainter, in a
        // column as wide as the widest of those on show (but no wider than
        // two fifths of the widest box), so that the lines start level.
        const u32 most = std::max<u32>(12, cap * 2 / 5);
        std::vector<std::string> wheres;
        u32 where_cols = 0, need_rows = 0;
        for (u32 r = 0; r < f.list_rows && f.top + r < n; ++r) {
            const size_t i = f.top + r;
            u32 need = 0;
            if (f.nearby()) {
                const Find::Near& e = f.near[i];
                need = e.heading.empty() ? 3 + width_of(e.name) + (e.folder.empty() ? 0 : 2 + width_of(e.folder)) : width_of(e.heading);
            } else if (names) {
                const std::string_view full = f.shown->index->path(f.shown->hits[i].file);
                need = 3 + width_of(full) + (full.find('/') == std::string_view::npos ? 0 : 1);  // (the name, two spaces and its folder)
            } else if (text) {
                const FindHit& hit = f.shown->hits[i];
                wheres.push_back(fit_text(std::string(f.shown->index->path(hit.file)) + ":" + std::to_string(hit.line + 1), most, true));
                where_cols = std::max(where_cols, width_of(wheres.back()));
                std::string_view line = hit.text;
                line.remove_prefix(std::min(line.find_first_not_of(" \t"), line.size()));
                need = width_of(line) * (f.replacing ? 2 : 1) + (f.replacing ? 3 : 0);  // (before → after)
            } else if (listed) {
                const Find::Entry& e = f.entries[i];
                const u32 kw = f.mode == kFindCommand ? width_of(f.item_syms[e.index]) : 0;
                need = width_of(e.name) + (kw ? 3 + kw : 0);
            } else {
                const Find::Entry& e = f.entries[i];
                need = 3 + (e.kind == Find::Entry::kHere     ? width_of("Use this folder") + 2 + width_of(here)
                            : e.kind == Find::Entry::kParent ? 4 + width_of("the folder above")
                            : e.kind == Find::Entry::kNew    ? width_of(e.name) + 2 + width_of(note)
                                                             : width_of(e.name) + (e.dir ? 1 : 0));
            }
            need_rows = std::max(need_rows, need);
        }
        if (text && need_rows) need_rows += where_cols + 2;
        // (Both fields' text starts in one column, past the longer label.)
        const u32 label_w = std::max(width_of(label), fields > 1 ? width_of(kWithLabel) : 0u);
        const u32 toggles_w = toggles ? 2 + 5 : 0;  // ".*  Aa", two columns from what is before
        u32 need = label_w + 2 + std::max(width_of(typed) + 1, kTypeRoom) + (count.empty() ? 0 : 2 + width_of(count)) + toggles_w;
        need = std::max(need, (where.empty() ? 0 : width_of(where) + 3) + hints_width(hints, font));
        need = std::max({need, need_rows, width_of(empty)});
        f.seen_cols = std::max(f.seen_cols, need);
        const u32 cols = std::min(f.seen_cols, cap);
        box.resize(cols, rows);

        auto icon = [&](u32 row, u32 col, const FileIcon& ic) {
            if (col + 2 > cols) return;
            u32* cell = box.cells.data() + static_cast<size_t>(row) * cols + col;
            if (nerd) {
                cell[0] = (font.glyph(ic.nerd) & (Font::kWide - 1)) | (ic.ink << kCellInkShift);
            } else {
                const u32 slot = font.icon(ic.drawn);
                cell[0] = slot | (ic.ink << kCellInkShift);
                cell[1] = (slot + 1) | (ic.ink << kCellInkShift);
            }
        };
        // Text with the letters that matched (by where they start, in bytes
        // from `shift` on) in the accent, bold; cut, with an ellipsis, where
        // it is wider than `fit`.
        auto marked = [&](u32 row, u32 col, std::string_view s, const std::vector<u32>& at, size_t shift, Ink in, u32 fit) {
            if (fit == 0) return col;
            const bool cut = width_of(s) > fit;
            const u32 end = col + fit - (cut ? 1 : 0);
            const u8* p = reinterpret_cast<const u8*>(s.data());
            for (size_t i = 0; i < s.size();) {
                const Decoded d = decode_utf8(p + i, s.size() - i);
                if (col + cp_width(d.cp) > end) break;
                const bool hit = std::find(at.begin(), at.end(), static_cast<u32>(i + shift)) != at.end();
                col = put(box, font, row, col, s.substr(i, d.len), hit ? ink::kAccentStrong : in);
                i += d.len;
            }
            if (cut) col = put(box, font, row, col, "\xE2\x80\xA6", in);
            return col;
        };

        // The field. Its cells take its tone, but for what is selected in it
        // (lit as a chosen row is) and the caret after what is typed: a thin
        // bar in the accent where the font can draw one; else a lit cell,
        // and then the field keeps the box's own background (a lit cell's
        // rounded ends would cut into the tone).
        const bool bar = font.can_draw(0x258F);
        put(box, font, 0, 0, fit_text(label, std::max<u32>(cols / 2, 8)), ink::kHeading);
        u32 c = std::min(label_w, std::max<u32>(cols / 2, 8)) + 2;
        const u32 text_col = c;
        // The toggles at the right end: lit when on.
        u32 right_end = cols;
        f.toggle_col = 0;
        if (toggles && cols > c + 8 + toggles_w) {
            f.toggle_col = cols - 5;
            put(box, font, 0, f.toggle_col, ".*", f.regex ? ink::kAccentStrong : ink::kMuted);
            put(box, font, 0, f.toggle_col + 3, "Aa", f.exact ? ink::kAccentStrong : ink::kMuted);
            right_end = cols - toggles_w;
        }
        const u32 count_w = width_of(count);
        const u32 stop = count_w && c + 8 + 2 + count_w <= right_end ? right_end - count_w - 2 : right_end;  // (what is typed ends before the count)
        std::string_view t = typed;
        const u32 space = stop > c + 1 ? stop - c - 1 : 0;  // (and one for the caret)
        if (width_of(t) > space && space > 1) {  // too long for the line: its end shows
            size_t cut = t.size();
            for (u32 width = 0; cut > 0 && width < space - 1;) {
                --cut;
                if (!is_cont(static_cast<u8>(t[cut]))) ++width;
            }
            c = put(box, font, 0, c, "\xE2\x80\xA6", ink::kMuted);
            t = t.substr(cut);
        }
        const u32 from = c;
        c = put(box, font, 0, c, t, ink::kText);
        if (stop < right_end) put(box, font, 0, right_end - count_w, count, infile && count.starts_with("No") ? ink::kError : ink::kMuted);
        // The caret (or what is selected) in the field that has the keyboard.
        auto caret = [&](u32 row, u32 a, u32 b, bool all) {
            u32* cells = box.cells.data() + static_cast<size_t>(row) * cols;
            if (all) {  // (typing replaces it)
                for (u32 k = a; k < b; ++k) cells[k] |= kCellSelected;
            } else if (b < cols) {
                if (bar) put(box, font, row, b, "\xE2\x96\x8F", ink::kAccent);
                else cells[b] |= kCellSelected;
            }
        };
        if (!f.in_with) caret(0, from, c, f.all);
        // "Replace with", under it.
        if (fields > 1) {
            put(box, font, 1, 0, kWithLabel, ink::kHeading);
            std::string_view wt = f.with;
            const u32 wspace = cols > text_col + 1 ? cols - text_col - 1 : 0;
            u32 wc = text_col;
            if (width_of(wt) > wspace && wspace > 1) {  // too long for the line: its end shows
                size_t cut = wt.size();
                for (u32 width = 0; cut > 0 && width < wspace - 1;) {
                    --cut;
                    if (!is_cont(static_cast<u8>(wt[cut]))) ++width;
                }
                wc = put(box, font, 1, wc, "\xE2\x80\xA6", ink::kMuted);
                wt = wt.substr(cut);
            }
            const u32 wfrom = wc;
            wc = put(box, font, 1, wc, wt, ink::kText);
            if (f.in_with) caret(1, wfrom, wc, f.with_all);
        }
        if (bar)
            for (u32 k = 0; k < cols * fields; ++k)
                if (!(box.cells[k] & kCellSelected)) box.cells[k] |= kCellSelected | kFieldTone << kCellDecoShift;

        // The rows.
        std::vector<u32> at;
        for (u32 r = 0; r < f.list_rows; ++r) {
            const size_t i = f.top + r;
            const u32 row = fields + r;
            if (i >= n) {
                if (r == 0) put(box, font, row, 0, fit_text(empty, cols), empty_plain ? ink::kText : ink::kMuted);
                continue;
            }
            at.clear();
            if (f.nearby()) {
                const Find::Near& e = f.near[i];
                if (!e.heading.empty()) {  // (never chosen, nor lit under the mouse)
                    put(box, font, row, 0, e.heading, ink::kHeading);
                    continue;
                }
                icon(row, 0, file_icon(e.name, false, false));
                const u32 col = marked(row, 3, e.name, at, 0, ink::kText, cols - 3) + 2;
                if (!e.folder.empty() && cols > col + 4) put(box, font, row, col, fit_text(e.folder, cols - col, true), ink::kMuted);
            } else if (names) {
                const std::string_view full = f.shown->index->path(f.shown->hits[i].file);
                const size_t slash = full.rfind('/');
                const size_t name = slash == std::string_view::npos ? 0 : slash + 1;
                if (!terms.empty()) fuzzy_path_score(terms, full, &at);
                icon(row, 0, file_icon(full.substr(name), false, false));
                // The name, the letters that matched lit; then the folder
                // it is in, fainter and as it is.
                const u32 col = marked(row, 3, full.substr(name), at, name, ink::kText, cols - 3) + 2;
                if (name > 0 && cols > col + 4) put(box, font, row, col, fit_text(full.substr(0, name - 1), cols - col, true), ink::kMuted);
            } else if (text) {
                const FindHit& hit = f.shown->hits[i];
                put(box, font, row, 0, wheres[r], ink::kMuted);
                const u32 col = std::min(where_cols + 2, cols);
                std::string_view line = hit.text;
                line.remove_prefix(std::min(line.find_first_not_of(" \t"), line.size()));
                if (f.replacing && lit_by_pattern) {  // the line as it is, and as replacing leaves it
                    std::string after;
                    std::vector<u32> at_after;
                    preview(line, f.pattern, f.with, f.regex, &at, &after, &at_after);
                    const u32 room_cols = cols > col ? cols - col : 0, half = room_cols > 3 ? (room_cols - 3) / 2 : 0;
                    const u32 c2 = marked(row, col, line, at, 0, ink::kMuted, half);
                    const u32 arrow = put(box, font, row, c2 + 1, "\xE2\x86\x92", ink::kMuted) + 1;
                    if (arrow < cols) marked(row, arrow, after, at_after, 0, ink::kText, cols - arrow);
                } else {
                    if (lit_by_pattern) {
                        lit_matches(line, f.pattern, &at);
                    } else {
                        for (const FuzzyTerm& term : terms)
                            for (size_t start = 0, found; (found = find_term(line, term, start)) != std::string_view::npos; start = found + term.text.size())
                                for (size_t k = 0; k < term.text.size(); ++k) at.push_back(static_cast<u32>(found + k));
                    }
                    marked(row, col, line, at, 0, ink::kText, cols - col);
                }
            } else if (listed) {
                const Find::Entry& e = f.entries[i];
                for (const FuzzyTerm& term : terms) fuzzy_score(term, e.name, &at);
                // A command's keys at the right, as far from the edge as its name is on the left.
                const std::string_view keys = f.mode == kFindCommand ? std::string_view(f.item_syms[e.index]) : std::string_view();
                u32 kw = width_of(keys);
                if (kw + 2 + 8 > cols) kw = 0;
                marked(row, 0, e.name, at, 0, ink::kText, kw ? cols - kw - 2 : cols);
                if (kw) put(box, font, row, cols - kw, keys, ink::kMuted);
            } else {
                const Find::Entry& e = f.entries[i];
                icon(row, 0, file_icon(e.kind == Find::Entry::kHere || e.kind == Find::Entry::kParent ? std::string_view() : std::string_view(e.name), e.dir, false));
                if (e.kind == Find::Entry::kHere) {
                    const u32 col = put(box, font, row, 3, "Use this folder", ink::kStrong) + 2;
                    if (cols > col + 4) put(box, font, row, col, fit_text(here, cols - col, true), ink::kMuted);
                } else if (e.kind == Find::Entry::kParent) {
                    const u32 col = put(box, font, row, 3, "..", ink::kText) + 2;
                    put(box, font, row, col, "the folder above", ink::kMuted);
                } else if (e.kind == Find::Entry::kNew) {
                    const u32 col = put(box, font, row, 3, fit_text(e.name, cols - 3), ink::kStrong) + 2;
                    put(box, font, row, col, note, ink::kMuted);
                } else {
                    if (!terms.empty()) fuzzy_score(terms[0], e.name, &at);
                    const u32 col = marked(row, 3, e.name, at, 0, ink::kText, cols - 3 - (e.dir ? 1 : 0));
                    if (e.dir) put(box, font, row, col, "/", ink::kMuted);
                }
            }
            if (i == f.sel) box.select_row(row);
            else if (static_cast<int>(i) == f.hover)
                for (u32 k = 0; k < cols; ++k) box.cells[static_cast<size_t>(row) * cols + k] |= kCellHover;
        }

        // The footer: where, on the left; as many of the keys as fit, on the right.
        const u32 foot = rows - 1;
        size_t keys_shown = hints.size();
        while (keys_shown > 0 && hints_width(hints.first(keys_shown), font) > cols) --keys_shown;
        const u32 hw = hints_width(hints.first(keys_shown), font);
        const u32 left = hw == 0 ? cols : cols > hw + 3 ? cols - hw - 3 : 0;
        if (!where.empty() && left >= 8) put(box, font, foot, 0, fit_text(where, left, (names || text) && where_ink.ink == ink::kMuted.ink), where_ink);
        put_hints(box, font, foot, cols - hw, hints.first(keys_shown));
    }

    Popup& p = fd.popups[fd.popup_count++];
    p.cols = f.box.cols;
    p.rows = f.box.rows;
    p.cells = f.box.cells.data();
    p.pad_x = pad_x;
    p.pad_y = pad_y;
    p.w = static_cast<int>(p.cols) * cw + 2 * pad_x;
    p.h = static_cast<int>(p.rows) * ch + 2 * pad_y;
    p.x = std::max(0, (w - p.w) / 2);
    // Near the top, and at the same height whatever is in it, so that it does not jump about as results come and go.
    p.y = std::min(std::max(tabs_px() + ch, h / 9), std::max(0, h - p.h));
    if (f.mode == kFindFile) {  // finding in the file: at the top right of its split, over as little of the text as may be
        p.x = std::clamp(pane_->x1 - p.w - l.gap - l.edge, 0, std::max(0, w - p.w));
        p.y = std::clamp(pane_->text_y + l.gap, 0, std::max(0, h - p.h));
    }
    p.border_px = static_cast<float>(l.hair);
    p.tones = &f.tones[0][0];
    f.rect = {p.x, p.y, p.w, p.h};
    fx_float(p, kFxFinder);
}

// Where things stand, on one line (for the tests): what the box is for,
// what is typed, how many rows there are, and the first few of them, the
// chosen one marked.
std::string App::find_dump() {
    Find& f = *find_;
    static const char* const kMode[] = {"names", "text", "open", "save", "folder", "pick", "input", "ask", "commands", "file"};
    if (!find_open_) return "closed";
    // Find in the file, and the replace fields: what is typed in each, the toggles, how many, which.
    std::string extra;
    if (f.replacing && (f.mode == kFindText || f.mode == kFindFile)) extra += std::string(" with") + (f.in_with ? ">" : "") + " [" + f.with + "]";
    if (f.mode == kFindText || f.mode == kFindFile) extra += std::string(f.regex ? " .*" : "") + (f.exact ? " Aa" : "") + (f.pattern.bad() ? " (" + f.pattern.error + ")" : "");
    if (f.asking) extra += " asking " + std::to_string(f.ask_hits) + " in " + std::to_string(f.ask_files);
    if (f.mode == kFindFile) {
        Find::file_count(*this);
        std::string out = "file [" + f.typed + "]" + extra;
        if (f.counted) {
            const u64 at = f.cur_ok ? static_cast<u64>(std::lower_bound(f.starts.begin(), f.starts.end(), f.cur.start) - f.starts.begin()) : f.starts.size();
            out += " " + (at < f.starts.size() && f.starts[at] == f.cur.start ? std::to_string(at + 1) : std::string("-")) + " of " + std::to_string(f.starts.size());
        }
        if (f.cur_ok) out += " at " + std::to_string(f.cur.start) + "-" + std::to_string(f.cur.end);
        return out;
    }
    std::string typed = f.typed;
    if (f.mode == kFindAsk) {  // (never a secret in a dump)
        if (f.secret) typed.assign(f.typed.size(), '*');
        return "ask " + f.ask_host + " \"" + f.prompt + "\" [" + typed + "]";
    }
    if (f.nearby()) {  // Go to file with nothing typed: the files open and opened lately, after "Open:" and "Recent:"
        const auto files = std::ranges::count_if(f.near, [](const Find::Near& e) { return e.heading.empty(); });
        std::string out = "names [] " + std::to_string(files) + " of " + std::to_string(f.shown ? f.shown->matched : 0) + " :";
        for (size_t i = 0; i < f.near.size() && i < 6; ++i) {
            const Find::Near& e = f.near[i];
            if (!e.heading.empty()) out += " " + e.heading + ":";
            else out += (i == f.sel ? " >" : " ") + (e.folder.empty() ? e.name : e.folder + "/" + e.name);
        }
        return out;
    }
    std::string out = std::string(kMode[f.mode]) + " [" + typed + "]" + extra + " " + std::to_string(f.count());
    if ((f.mode == kFindNames || f.mode == kFindText) && f.shown) out += " of " + std::to_string(f.shown->matched);
    out += " :";
    for (size_t i = 0; i < f.count() && i < 6; ++i) {
        out += i == f.sel ? " >" : " ";
        if (f.mode == kFindNames) out += std::string(f.shown->index->path(f.shown->hits[i].file));
        else if (f.mode == kFindText) {
            const FindHit& h = f.shown->hits[i];
            out += std::string(f.shown->index->path(h.file)) + ":" + std::to_string(h.line + 1) + ":" + std::to_string(h.col + 1);
            if (f.replacing && !f.pattern.empty() && !f.pattern.bad()) {  // (the preview: what the line becomes)
                std::vector<u32> a, b;
                std::string after;
                preview(h.text, f.pattern, f.with, f.regex, &a, &after, &b);
                out += "=" + after;
            }
        }
        else if (f.mode == kFindCommand) out += f.entries[i].name + (f.item_keys[f.entries[i].index].empty() ? "" : " (" + f.item_keys[f.entries[i].index] + ")");
        else out += f.entries[i].kind == Find::Entry::kNew ? "+" + f.entries[i].name : f.entries[i].name + (f.entries[i].dir ? "/" : "");
    }
    return out;
}

// Waits for the worker to finish what it was asked (scripted runs).
// A connection is up: a folder on that machine typed into the box is listed now.
void App::find_remote_ready() {
    if (!find_open_ || !find_) return;
    Find& f = *find_;
    if ((f.mode == kFindOpen || f.mode == kFindFolder || f.mode == kFindSave) && f.listed != f.dir && is_remote(f.dir)) find_query();
}

void App::find_settle(bool listing) {
    if (find_->engine) find_->engine->wait_idle();
    // A folder on another machine is listed on the connection's thread: its
    // listing too, when asked (ten seconds at most).
    for (int i = 0; listing && i < 2000 && remote_listing_busy(); ++i) {
        SDL_Delay(5);
        remote_sync();
    }
    if (listing) remote_sync();
    find_sync();
}

}  // namespace blit
