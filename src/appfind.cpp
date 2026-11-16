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
#include "look.hpp"
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
constexpr Hint kHintsText[] = {{"Enter", "open"}, {kCmdEnter, "open beside"}, {"Tab", "go to file"}};
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
    // The command search (kFindCommand): each command's keys and what is
    // searched (its name and its keys, in words), and whether the next keys
    // pressed are to be looked up rather than acted on.
    std::vector<std::string> item_keys, item_search, item_syms;  // (item_syms: the keys as the box shows them, ⇧⌘P)
    std::vector<u32> item_ids;
    bool capture = false, chord_query = false;  // (and whether what is typed is keys pressed)
    bool drop_text = false;  // (the text a key looked up brings is not typed too)
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
    bool nearby() const { return mode == kFindNames && query.empty() && !go_line && !near.empty(); }
    bool line_only() const { return mode == kFindNames && query.empty() && go_line; }
    bool heading(size_t i) const { return nearby() && i < near.size() && !near[i].heading.empty(); }

    bool beside = false;  // what is chosen opens in the split to the right (Cmd+Enter)
    bool all = false;     // what is typed is selected: typing replaces it
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
        if (nearby()) return near.size();
        if (line_only()) return 0;
        if (mode == kFindNames || mode == kFindText) return shown ? shown->hits.size() : 0;
        return entries.size();
    }

    // (Find is the App's, and can reach what the App keeps to itself.)
    static void list_near(App& a);
    static void go(App& a, u64 line, u64 col);
    static bool open_beside(App& a, const std::string& path);
    static void say(App& a, const std::string& text);
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

// Opens a file in the split to the right of the one with the keyboard (a
// new one if there is none there), which gets the keyboard. A file shows in
// one place: one open elsewhere moves there.
bool App::Find::open_beside(App& a, const std::string& path) {
    const Pane& here = *a.pane_;
    Pane* dest = a.pane_at(here.x1 + 4, (here.y0 + here.y1) / 2);
    if (!dest) dest = a.split_pane(a.pane_, kEdgeRight);
    if (!dest) return a.open_path(path.c_str());  // (no room for another split)
    Pane* where = nullptr;
    size_t index = 0;
    if (a.find_open(full_path(path), &where, &index)) {
        if (where != dest) a.move_doc(where, index, dest, dest->docs.size());
        a.focus_pane(dest);
        if (where == dest) a.tab_show(index);
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
    f.seen_cols = 0;  // (as wide as this purpose needs)
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
    f.beside = f.all = false;
    if (f.engine) f.engine->cancel();
    if (!chosen && f.mode == kFindSave) quit_after_save_ = close_after_save_ = false;  // no name: what was to follow the save does not
    pane_->cache_rev = ~0ull;
    dirty_ = true;
    if (!chosen && f.script() && !f.chose && !f.chose_item) script_chose(false, -1, {});  // (the script hears that nothing was chosen)
    f.chose = nullptr;
    f.chose_item = nullptr;
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
        f.serial = f.engine->find_text(f.typed);
        return;
    }
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
        std::string there;
        std::vector<RemoteEntry> list;
        if (f.dir == "ssh://") {  // the machines there are to go to
            for (std::string& h : ssh_hosts()) f.listing.push_back({std::move(h), true, Find::Entry::kPlain, 0});
        } else if (is_remote(f.dir)) {  // a folder on another machine (listed once it is connected: see find_remote_ready)
            const std::shared_ptr<Remote> r = remote_for(f.dir, &there);
            if (r && r->list(there, &list))
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
    if (!pane_->doc->ed.modified()) {
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
    if (f.mode == kFindNames || f.mode == kFindText) {
        const bool beside = std::exchange(f.beside, false);
        if (f.line_only()) {  // ":120": a line of the file in front
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
        if (!(beside ? Find::open_beside(*this, path) : open_path(path.c_str()))) return;
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
        recent_add(here, true);
        return panel_set_workdir(here);
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
    if (f.mode == kFindNames || f.mode == kFindText) return find_open(f.mode == kFindNames ? kFindText : kFindNames, f.typed);
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
    f.drop_text = false;
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
    const i64 page = std::max<i64>(1, static_cast<i64>(f.list_rows) - 1);
    switch (k.key) {
        case SDLK_ESCAPE: find_close(false); break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
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
            if (f.all) {  // what is typed stays, no longer selected, to be typed onto
                f.all = false;
                f.touch();
                dirty_ = true;
            }
            break;
        case SDLK_BACKSPACE:
            if (f.typed.empty()) break;
            if (f.all) {  // selected: it all goes
                f.typed.clear();
                f.all = false;
            } else if (cmd || alt) {
                // A word back; in a path, a folder back.
                const bool path = f.path();
                const char stop = path ? '/' : ' ';
                while (!f.typed.empty() && f.typed.back() == stop) f.typed.pop_back();
                while (!f.typed.empty() && f.typed.back() != stop) f.typed.pop_back();
                if (cmd && !path) f.typed.clear();
            } else {
                pop_char(&f.typed);
            }
            find_query();
            break;
        default:
            if (ctrl && (k.key == SDLK_P || k.key == SDLK_K)) find_move(-1);
            else if (ctrl && (k.key == SDLK_N || k.key == SDLK_J)) find_move(1);
            else if (ctrl && k.key == SDLK_U) f.typed.clear(), f.all = false, find_query();
            else if (ctrl && k.key == SDLK_W) {
                if (std::exchange(f.all, false)) f.typed.clear();
                while (!f.typed.empty() && (f.typed.back() == ' ' || f.typed.back() == '/')) f.typed.pop_back();
                while (!f.typed.empty() && f.typed.back() != ' ' && f.typed.back() != '/') f.typed.pop_back();
                find_query();
            } else if (cmd && k.key == SDLK_V) {
                // What the clipboard holds, as far as its first line goes (in place of what is typed, if that is selected).
                if (char* clip = SDL_GetClipboardText()) {
                    std::string_view text = clip;
                    text = text.substr(0, text.find('\n'));
                    if (std::exchange(f.all, false)) f.typed.clear();
                    f.typed += std::string(text.substr(0, 400));
                    SDL_free(clip);
                    find_query();
                }
            }
            break;
    }
    return true;
}

// Typed text goes into the box while it is open.
bool App::find_typed(const char* text) {
    Find& f = *find_;
    if (!find_open_) return false;
    if (f.drop_text) {
        f.drop_text = false;
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
    const int row = (py - f.rect.y - pad_y) / ch - 1;  // (under the field)
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
    f.list_rows = f.mode == kFindInput || f.mode == kFindAsk ? 0 : static_cast<u32>(std::min<size_t>(room, std::max<size_t>(n, 1)));
    // The chosen row is kept among those on show.
    if (f.sel < f.top) f.top = f.sel;
    if (f.sel >= f.top + f.list_rows) f.top = f.sel + 1 - f.list_rows;
    if (f.top + f.list_rows > n) f.top = n > f.list_rows ? static_cast<u32>(n - f.list_rows) : 0;
    const u32 rows = 1 + f.list_rows + 1;  // the field, the rows, the footer
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

        // The field: a word that says what the box is for, what is typed (a
        // secret as a dot for each letter, keys looked up as keys) and how
        // many there are.
        static const char* const kLabel[] = {"Go to file", "Search in files", "Open", "Save as", "Folder", "Pick", "Input", "ssh", "Command search"};
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
        size_t longest = 0;  // (a search for text: its longest word)
        if (text)
            for (const FuzzyTerm& t : fuzzy_terms(f.typed)) longest = std::max(longest, t.text.size());
        std::string count;
        bool going = reading;  // (there may be more: an ellipsis after the count)
        if (names && f.shown && f.shown->index && !f.line_only()) {
            const std::string total = std::to_string(files) + (f.shown->index->truncated ? "+" : "");
            count = f.nearby() ? total + " files" : std::to_string(f.shown->matched) + " / " + total;
        } else if (text && longest >= 2 && f.shown && f.shown->text) {
            count = std::to_string(f.shown->matched) + (f.shown->capped ? "+" : "") + " in " + std::to_string(f.shown->files_hit) + (f.shown->files_hit == 1 ? " file" : " files");
            going = going || f.shown->searching;
        } else if (f.mode == kFindPick || (f.mode == kFindCommand && !f.capture)) {
            count = std::to_string(n) + " / " + std::to_string(f.items.size());
        }
        if (going && !count.empty()) count += "\xE2\x80\xA6";

        // The footer: where the box looks (or what it waits for), and its keys.
        std::string where;
        std::span<const Hint> hints;
        switch (f.mode) {
            case kFindNames: hints = f.line_only() ? std::span<const Hint>(kHintsLine) : std::span<const Hint>(kHintsNames); break;
            case kFindText: hints = kHintsText; break;
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

        // A list with nothing in it says why, or what to do.
        std::string empty;
        bool empty_plain = false;  // (it says what Enter does)
        if (n == 0 && f.list_rows > 0) {
            const std::string_view asked = names ? std::string_view(f.query) : path ? std::string_view(f.leaf) : std::string_view(f.typed);
            const std::string quoted = "'" + fit_text(asked, 40) + "'";
            if (f.line_only()) {
                const std::string& in = pane_->doc->ed.path;
                empty = "Go to line " + std::to_string(f.go_line) + (f.go_col ? ", column " + std::to_string(f.go_col) : std::string()) + " of " +
                        (in.empty() ? std::string("this file") : in.substr(in.rfind('/') + 1));
                empty_plain = true;
            } else if (names) {
                empty = reading && !files   ? std::string("Reading the folder\xE2\x80\xA6")
                        : !asked.empty()    ? "No file matches " + quoted
                        : !panel_root()     ? "Choose a project folder: " + key_symbols(kCmdShiftO, font)
                                            : "No files in " + tilde(f.root);
            } else if (text) {
                empty = longest < 2 ? std::string("Type two letters or more; words in any order")
                        : !f.shown || !f.shown->text || f.shown->searching || (reading && !files) ? std::string("Searching\xE2\x80\xA6")
                                                                                                  : "No line has " + quoted;
            } else if (path) {
                empty = "Nothing here by that name";
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
                need = width_of(line);
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
        u32 need = width_of(label) + 2 + std::max(width_of(typed) + 1, kTypeRoom) + (count.empty() ? 0 : 2 + width_of(count));
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
        u32 c = put(box, font, 0, 0, fit_text(label, std::max<u32>(cols / 2, 8)), ink::kHeading) + 2;
        const u32 count_w = width_of(count);
        const u32 stop = count_w && c + 8 + 2 + count_w <= cols ? cols - count_w - 2 : cols;  // (what is typed ends before the count)
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
        if (stop < cols) put(box, font, 0, cols - count_w, count, ink::kMuted);
        u32* field = box.cells.data();
        if (f.all) {  // (typing replaces it)
            for (u32 k = from; k < c; ++k) field[k] |= kCellSelected;
        } else if (c < cols) {
            if (bar) put(box, font, 0, c, "\xE2\x96\x8F", ink::kAccent);
            else field[c] |= kCellSelected;
        }
        if (bar)
            for (u32 k = 0; k < cols; ++k)
                if (!(field[k] & kCellSelected)) field[k] |= kCellSelected | kFieldTone << kCellDecoShift;

        // The rows.
        std::vector<u32> at;
        for (u32 r = 0; r < f.list_rows; ++r) {
            const size_t i = f.top + r;
            const u32 row = 1 + r;
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
                for (const FuzzyTerm& term : terms)
                    for (size_t start = 0, found; (found = find_term(line, term, start)) != std::string_view::npos; start = found + term.text.size())
                        for (size_t k = 0; k < term.text.size(); ++k) at.push_back(static_cast<u32>(found + k));
                marked(row, col, line, at, 0, ink::kText, cols - col);
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
        if (!where.empty() && left >= 8) put(box, font, foot, 0, fit_text(where, left, names || text), ink::kMuted);
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
    static const char* const kMode[] = {"names", "text", "open", "save", "folder", "pick", "input", "ask", "commands"};
    if (!find_open_) return "closed";
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
    std::string out = std::string(kMode[f.mode]) + " [" + typed + "] " + std::to_string(f.count());
    if ((f.mode == kFindNames || f.mode == kFindText) && f.shown) out += " of " + std::to_string(f.shown->matched);
    out += " :";
    for (size_t i = 0; i < f.count() && i < 6; ++i) {
        out += i == f.sel ? " >" : " ";
        if (f.mode == kFindNames) out += std::string(f.shown->index->path(f.shown->hits[i].file));
        else if (f.mode == kFindText) out += std::string(f.shown->index->path(f.shown->hits[i].file)) + ":" + std::to_string(f.shown->hits[i].line + 1) + ":" + std::to_string(f.shown->hits[i].col + 1);
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

void App::find_settle() {
    if (find_->engine) find_->engine->wait_idle();
    find_sync();
}

}  // namespace blit
