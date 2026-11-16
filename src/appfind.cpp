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
#include "textbox.hpp"
#include "tree.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

constexpr u32 kMaxRows = 14;  // results shown at once
constexpr u32 kHeadRows = 2;  // what is typed, and a line that says what is being shown

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
    // The command search (kFindCommand): each command's keys and what is
    // searched (its name and its keys, in words), and whether the next keys
    // pressed are to be looked up rather than acted on.
    std::vector<std::string> item_keys, item_search;
    std::vector<u32> item_ids;
    bool capture = false, chord_query = false;  // (and whether what is typed is keys pressed)
    bool drop_text = false;  // (the text a key looked up brings is not typed too)
    bool secret = false;
    int back_mode = -1;
    std::string back_typed;
    bool path() const { return mode == kFindOpen || mode == kFindSave || mode == kFindFolder; }

    // The cells, and what they were made from.
    TextBox box;
    SDL_Rect rect{};
    u32 list_rows = 0;
    u64 state = 1, drawn = 0;
    u32 drawn_font = ~0u, drawn_cols = 0, drawn_rows = 0;
    void touch() { ++state; }

    size_t count() const {
        if (mode == kFindNames || mode == kFindText) return shown ? shown->hits.size() : 0;
        return entries.size();
    }
};

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

void App::find_open(int mode, const std::string& typed) {
    Find& f = *find_;
    if (find_open_ && f.script() && !(mode == f.mode && f.script())) find_close(false);  // (a script's picker is answered "nothing")
    f.chose = nullptr;  // (see find_choose)
    if (find_open_ && f.mode == kFindAsk && mode != kFindAsk) {  // (ssh's question waits, and comes back when this box shuts)
        wipe(f.typed);
        f.ask_id = 0;
        f.back_mode = -1;
    }
    lsp_close(kPopAll);
    const std::string* root = panel_root();
    const std::string workdir = root ? *root : default_workdir();
    find_open_ = true;
    f.mode = mode;
    f.typed = typed;
    f.sel = f.top = 0;
    f.hover = -1;
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
        f.engine->set_options(std::move(o));
        if (f.root != workdir) f.shown = nullptr;
        f.root = workdir;
        f.engine->index(workdir);  // read again every time: files come and go
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
    f.item_ids.clear();
    for (PaletteItem& it : palette_items()) {
        std::string words = it.title + " " + it.keys;
        for (char& ch : words)
            if (ch == '+') ch = ' ';
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
    if (f.engine) f.engine->cancel();
    if (!chosen && f.mode == kFindSave) quit_after_save_ = close_after_save_ = false;  // no name: what was to follow the save does not
    pane_->cache_rev = ~0ull;
    dirty_ = true;
    if (!chosen && f.script() && !f.chose) script_chose(false, -1, {});  // (the script hears that nothing was chosen)
    f.chose = nullptr;
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
        f.serial = f.engine->find_names(f.typed);
        return;
    }
    if (f.mode == kFindText) {
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
    if (f.mode != kFindNames && f.mode != kFindText) return true;
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
        find_close(true);
        script_chose(true, index, typed);
        return;
    }
    if (f.mode == kFindNames || f.mode == kFindText) {
        if (!find_current(400) || f.sel >= f.shown->hits.size()) return;
        const FindHit hit = f.shown->hits[f.sel];
        const std::string path = f.shown->index->root + "/" + std::string(f.shown->index->path(hit.file));
        const bool text = f.mode == kFindText;
        find_close(true);
        remember_place();  // (Ctrl-O comes back from it)
        if (!open_path(path.c_str())) return;
        if (text && pane_->doc->loading) {  // (on its way from another machine: there when it comes)
            pane_->doc->goto_line = hit.line;
            pane_->doc->goto_col = hit.col;
        } else if (text) {
            Editor& ed = pane_->doc->ed;
            const u64 line = std::min<u64>(hit.line, ed.buf.line_count() - 1), start = ed.buf.line_start(line);
            ed.move_to(std::min(start + hit.col, ed.line_end(line)), false);
            vim_mouse();  // (Vim takes the cursor's new place as it would a click's)
            scroll_to_cursor();
            // The line found goes to the middle of the view.
            const double ch = font_->cell_h();
            pane_->target_y = std::clamp(static_cast<double>(line) * ch - (view_h() - ch) / 2, 0.0, std::max(max_scroll_y(), 0.0));
            pane_->scroll_y = pane_->target_y;
        }
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
    find_current(100);
    const i64 n = static_cast<i64>(f.count());
    if (n == 0) return;
    // Up from the first is the last, and down from the last the first; a page stops at the ends.
    i64 to = static_cast<i64>(f.sel) + by;
    if (by == 1 || by == -1) to = ((to % n) + n) % n;
    f.sel = static_cast<u32>(std::clamp<i64>(to, 0, n - 1));
    f.touch();
    dirty_ = true;
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
        case SDLK_KP_ENTER: find_accept(); break;
        case SDLK_TAB: find_tab(); break;
        case SDLK_UP: find_move(-1); break;
        case SDLK_DOWN: find_move(1); break;
        case SDLK_PAGEUP: find_move(-page); break;
        case SDLK_PAGEDOWN: find_move(page); break;
        case SDLK_BACKSPACE:
            if (f.typed.empty()) break;
            if (cmd || alt) {
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
            else if (ctrl && k.key == SDLK_U) f.typed.clear(), find_query();
            else if (ctrl && k.key == SDLK_W) {
                while (!f.typed.empty() && (f.typed.back() == ' ' || f.typed.back() == '/')) f.typed.pop_back();
                while (!f.typed.empty() && f.typed.back() != ' ' && f.typed.back() != '/') f.typed.pop_back();
                find_query();
            } else if (cmd && k.key == SDLK_V) {
                // What the clipboard holds, as far as its first line goes.
                if (char* clip = SDL_GetClipboardText()) {
                    std::string_view text = clip;
                    text = text.substr(0, text.find('\n'));
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
    if (find_open_ && find_->drop_text) {
        find_->drop_text = false;
        return true;
    }
    if (find_open_ && find_->chord_query) {  // (typing after keys looked up starts a search of its own)
        find_->chord_query = false;
        find_->typed.clear();
    }
    Find& f = *find_;
    if (!find_open_) return false;
    f.typed += text;
    find_query();
    return true;
}

// The row of results a point (in pixels) is on, or -1.
int App::find_row_at(int px, int py) const {
    const Find& f = *find_;
    if (!find_open_ || px < f.rect.x || px >= f.rect.x + f.rect.w || py < f.rect.y || py >= f.rect.y + f.rect.h) return -1;
    const int ch = font_->cell_h();
    const int row = (py - f.rect.y - ch / 4) / ch - static_cast<int>(kHeadRows);
    if (py < f.rect.y + ch / 4 || row < 0 || row >= static_cast<int>(f.list_rows)) return -1;
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
    if (row >= 0 && b.button == SDL_BUTTON_LEFT) {
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
        // (The chosen row stays among the ones on show.)
        f.sel = static_cast<u32>(std::clamp<i64>(f.sel, top, top + std::max<i64>(rows, 1) - 1));
        f.touch();
        dirty_ = true;
    }
    return true;
}

// ---------------------------------------------------------------- drawing

void App::find_layout(int w, int h, FrameDesc& fd) {
    Find& f = *find_;
    if (!find_open_ || fd.popup_count >= std::size(fd.popups)) return;
    Font& font = *font_;
    const int cw = font.cell_w(), ch = font.cell_h(), pad_x = cw / 2 + 1, pad_y = ch / 4;
    const u32 cols = static_cast<u32>(std::clamp((w - 6 * cw) / cw, 20, 104));
    const u32 room = static_cast<u32>(std::clamp((h * 7 / 10 - 2 * pad_y) / ch - static_cast<int>(kHeadRows), 3, static_cast<int>(kMaxRows)));
    const size_t n = f.count();
    f.list_rows = f.mode == kFindInput || f.mode == kFindAsk ? 0 : static_cast<u32>(std::min<size_t>(room, std::max<size_t>(n, 1)));
    // The chosen row is kept among those on show.
    if (f.sel < f.top) f.top = f.sel;
    if (f.sel >= f.top + f.list_rows) f.top = f.sel + 1 - f.list_rows;
    if (f.top + f.list_rows > n) f.top = n > f.list_rows ? static_cast<u32>(n - f.list_rows) : 0;
    const u32 rows = kHeadRows + f.list_rows;

    if (f.drawn != f.state || f.drawn_font != font.generation() || f.drawn_cols != cols || f.drawn_rows != rows) {
        f.drawn = f.state;
        f.drawn_font = font.generation();
        f.drawn_cols = cols;
        f.drawn_rows = rows;
        TextBox& box = f.box;
        box.resize(cols, rows);
        const bool nerd = font.has(0xE5FF) && font.has(0xE61D);
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
        // Text with some of its letters (by where they start, in bytes
        // from `shift` on) standing out: the ones that matched.
        auto marked = [&](u32 row, u32 col, std::string_view s, const std::vector<u32>& at, size_t shift, u32 ink) {
            const u8* p = reinterpret_cast<const u8*>(s.data());
            for (size_t i = 0; i < s.size() && col < cols;) {
                const Decoded d = decode_utf8(p + i, s.size() - i);
                const bool hit = std::find(at.begin(), at.end(), static_cast<u32>(i + shift)) != at.end();
                col = box.put(font, row, col, s.substr(i, d.len), hit ? kInkAccent : ink, hit ? Font::kBold : 0);
                i += d.len;
            }
            return col;
        };

        // What is typed, after a word that says what the box is for; the
        // cell after it is lit, as a cursor.
        static const char* const kLabel[] = {"Files", "Text", "Open", "Save as", "Folder", "Pick", "Input", "ssh", "Commands"};
        u32 c = box.put(font, 0, 0, (f.script() || f.mode == kFindAsk || f.chose) && !f.prompt.empty() ? std::string_view(f.prompt) : std::string_view(kLabel[f.mode]), kInkAccent, Font::kBold) + 2;
        std::string hidden;  // (a secret shows as a dot for each letter)
        if (f.mode == kFindAsk && f.secret)
            for (size_t i = 0; i < f.typed.size(); ++i)
                if (!is_cont(static_cast<u8>(f.typed[i]))) hidden += font.has(0x2022) ? "\xE2\x80\xA2" : "*";
        std::string_view typed = f.mode == kFindAsk && f.secret ? std::string_view(hidden) : std::string_view(f.typed);
        const u32 space = cols > c + 2 ? cols - c - 2 : 1;
        if (width_of(typed) > space) {  // too long for the line: its end shows
            size_t cut = typed.size();
            for (u32 width = 0; cut > 0 && width < space - 1;) {
                --cut;
                if (!is_cont(static_cast<u8>(typed[cut]))) ++width;
            }
            c = box.put(font, 0, c, "\xE2\x80\xA6", kInkMuted);
            typed = typed.substr(cut);
        }
        c = box.put(font, 0, c, typed, 0);
        if (c < cols) box.cells[c] |= kCellSelected;

        // What is being shown, and of what.
        std::string status, right;
        const bool path = f.path();
        if (f.mode == kFindNames) {
            const size_t total = f.shown && f.shown->index ? f.shown->index->size() : 0;
            const bool reading = !f.shown || f.shown->indexing || !f.shown->index;
            if (f.shown && f.shown->index) status = std::to_string(f.shown->matched) + " of " + std::to_string(total) + (f.shown->index->truncated ? "+" : "") + " files";
            if (reading) status += status.empty() ? "Reading the folder\xE2\x80\xA6" : " \xC2\xB7 reading the folder again\xE2\x80\xA6";
            right = tilde(f.root);
        } else if (f.mode == kFindText) {
            size_t longest = 0;
            for (const FuzzyTerm& t : fuzzy_terms(f.typed)) longest = std::max(longest, t.text.size());
            if (longest < 2) status = "Type two letters or more; words in any order";
            else if (!f.shown || !f.shown->text) status = "Searching\xE2\x80\xA6";
            else
                status = std::to_string(f.shown->matched) + (f.shown->capped ? "+" : "") + (f.shown->matched == 1 ? " line in " : " lines in ") + std::to_string(f.shown->files_hit) +
                         (f.shown->files_hit == 1 ? " file" : " files") + (f.shown->searching ? " \xC2\xB7 searching\xE2\x80\xA6" : "");
            right = tilde(f.root);
        } else if (f.mode == kFindPick) {
            status = std::to_string(n) + " of " + std::to_string(f.items.size());
        } else if (f.mode == kFindCommand) {
            status = f.capture ? "Press the keys to look up Â· Tab: back to typing"
                               : std::to_string(n) + " of " + std::to_string(f.items.size()) + " Â· Enter runs it Â· Tab: press keys to look them up";
        } else if (f.mode == kFindInput) {
            status = "Enter accepts \xC2\xB7 Esc cancels";
        } else if (f.mode == kFindAsk) {
            status = std::string("Enter sends it to ssh") + (f.secret ? " (hidden as it is typed)" : "") + " \xC2\xB7 Esc cancels connecting";
            right = f.ask_host;
        } else {
            status = f.mode == kFindOpen ? "Enter opens \xC2\xB7 Tab completes" : f.mode == kFindSave ? "Enter saves under the name typed \xC2\xB7 Tab completes" : "Enter chooses \xC2\xB7 Tab goes into a folder";
        }
        const u32 end = box.put(font, 1, 0, status, kInkMuted);
        const u32 need = width_of(right);
        if (need && cols > end + need + 2) box.put(font, 1, cols - need, right, kInkMuted);

        // The rows.
        const std::vector<FuzzyTerm> terms = fuzzy_terms(path ? f.leaf : f.typed);
        std::vector<u32> at;
        // Lines found: the file and line number of each, fainter, in a
        // column as wide as the widest of those on show (but no wider than
        // two fifths of the box), so that the lines start level.
        const u32 most = std::max<u32>(12, cols * 2 / 5);
        auto where_of = [&](const FindHit& hit) {
            std::string where = std::string(f.shown->index->path(hit.file)) + ":" + std::to_string(hit.line + 1);
            if (width_of(where) > most) {
                size_t cut = where.size();
                for (u32 width = 0; cut > 0 && width < most - 1;) {
                    --cut;
                    if (!is_cont(static_cast<u8>(where[cut]))) ++width;
                }
                where = "\xE2\x80\xA6" + where.substr(cut);
            }
            return where;
        };
        u32 where_cols = 0;
        if (f.mode == kFindText)
            for (u32 r = 0; r < f.list_rows && f.top + r < n; ++r) where_cols = std::max(where_cols, width_of(where_of(f.shown->hits[f.top + r])));
        for (u32 r = 0; r < f.list_rows; ++r) {
            const size_t i = f.top + r;
            const u32 row = kHeadRows + r;
            if (i >= n) {
                if (r == 0) box.put(font, row, 0, path ? "Nothing here by that name" : f.mode == kFindPick ? "Nothing matches" : f.shown && !f.shown->searching && !f.shown->indexing ? "Nothing found" : "", kInkMuted);
                continue;
            }
            if (f.mode == kFindNames) {
                const std::string_view full = f.shown->index->path(f.shown->hits[i].file);
                const size_t slash = full.rfind('/');
                const size_t name = slash == std::string_view::npos ? 0 : slash + 1;
                at.clear();
                if (!terms.empty()) fuzzy_path_score(terms, full, &at);
                icon(row, 0, file_icon(full.substr(name), false, false));
                // The name, then the folder it is in, fainter.
                u32 col = marked(row, 3, full.substr(name), at, name, 0) + 2;
                if (name > 0) marked(row, col, full.substr(0, name - 1), at, 0, kInkMuted);
            } else if (f.mode == kFindText) {
                const FindHit& hit = f.shown->hits[i];
                box.put(font, row, 0, where_of(hit), kInkMuted);
                const u32 col = where_cols + 2;
                std::string_view line = hit.text;
                const size_t lead = std::min(line.find_first_not_of(" \t"), line.size());
                line = line.substr(lead);
                at.clear();
                for (const FuzzyTerm& t : terms)
                    for (size_t from = 0, found; (found = find_term(line, t, from)) != std::string_view::npos; from = found + t.text.size())
                        for (size_t k = 0; k < t.text.size(); ++k) at.push_back(static_cast<u32>(found + k));
                marked(row, col, line, at, 0, 0);
            } else if (f.mode == kFindPick || f.mode == kFindCommand) {
                const Find::Entry& e = f.entries[i];
                at.clear();
                for (const FuzzyTerm& t : terms) fuzzy_score(t, e.name, &at);
                const std::string& keys = f.mode == kFindCommand ? f.item_keys[e.index] : std::string();
                const u32 kw = width_of(keys);
                const u32 after = marked(row, 1, e.name, at, 0, 0);
                if (kw && cols > after + kw + 2) box.put(font, row, cols - kw - 1, keys, kInkAccent);
            } else {
                const Find::Entry& e = f.entries[i];
                icon(row, 0, file_icon(e.kind == Find::Entry::kHere || e.kind == Find::Entry::kParent ? std::string_view() : std::string_view(e.name), e.dir, false));
                if (e.kind == Find::Entry::kHere) {
                    const u32 col = box.put(font, row, 3, "Use this folder", 0, Font::kBold) + 2;
                    box.put(font, row, col, tilde(f.dir.size() > 1 ? f.dir.substr(0, f.dir.size() - 1) : f.dir), kInkMuted);
                } else if (e.kind == Find::Entry::kParent) {
                    const u32 col = box.put(font, row, 3, "..", 0) + 2;
                    box.put(font, row, col, "the folder above", kInkMuted);
                } else if (e.kind == Find::Entry::kNew) {
                    const u32 col = box.put(font, row, 3, e.name, 0, Font::kBold) + 2;
                    box.put(font, row, col, f.mode == kFindSave ? "save under this name" : "a new file", kInkMuted);
                } else {
                    at.clear();
                    if (!terms.empty()) fuzzy_score(terms[0], e.name, &at);
                    const u32 col = marked(row, 3, e.name, at, 0, 0);
                    if (e.dir) box.put(font, row, col, "/", kInkMuted);
                }
            }
            if (i == f.sel) box.select_row(row);
            else if (static_cast<int>(i) == f.hover)
                for (u32 k = 0; k < cols; ++k) box.cells[static_cast<size_t>(row) * cols + k] |= kCellHover;
        }
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
    p.border_px = static_cast<float>(std::max(1, static_cast<int>(std::lround(scale_))));
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
