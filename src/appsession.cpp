// Sessions: what was open in each project folder, kept so that opening the
// folder again brings it back. The tabs of each split in their order, how
// the window was split and where its dividers were, each file's cursor and
// view and the language chosen for it, the side panel (shown or not, its
// tab, its width), the Build tab's task, and the breakpoints and
// expressions watched. One small text file
// for each folder, in `sessions` beside `recent` in blit's data folder
// (~/.local/share/blit/sessions), named by a hash of the folder's path; its
// first lines say whose it is and how many tabs it brings back, which is
// all the start screen reads of it.
//
// A window takes a folder's session when the folder is opened to work in:
// from the start screen's Recent, the box that opens folders (d, Cmd+Shift+O),
// `blit DIR`, a new project, or another working tree from the Git tab. It
// is written when blit quits and when another folder is opened that way,
// on a thread of its own; the tab counts Recent shows are read on a
// thread while the window starts. Only a blit
// with a window of its own keeps sessions, as with `recent`, unless
// BLIT_RECENT_FILE names a file for scripted runs (the sessions go beside it).
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>

#include "app.hpp"
#include "grammars.hpp"
#include "remote.hpp"

namespace blit {

namespace {

constexpr std::string_view kFirstLine = "blit session 1";

// The folder sessions are kept in, and the list of what was opened lately,
// or nothing if this run keeps none (see the top of the file, and recent_file()
// in appstart.cpp, whose rule this is).
std::string recent_path(const Options& opt) {
    if (const char* p = std::getenv("BLIT_RECENT_FILE"); p && *p) return p;
    if (opt.bench || opt.script || opt.shot || opt.off_w > 0) return {};
    return grammar_dir() + "/recent";
}

// A file's path with the folders to it as the system names them (/private/tmp
// for /tmp, a linked folder by where it is), the file's own name kept (a
// link in the folder is the folder's). One on another machine, or whose
// folder is not there, as it is.
std::string real_place(const std::string& path) {
    if (path.empty() || is_remote(path)) return path;
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return path;
    char real[PATH_MAX];
    if (!::realpath(slash == 0 ? "/" : path.substr(0, slash).c_str(), real)) return path;
    const std::string up = real;
    return (up == "/" ? std::string() : up) + path.substr(slash);
}

std::string sessions_dir(const Options& opt) {
    const std::string recent = recent_path(opt);
    if (recent.empty()) return {};
    const size_t slash = recent.find_last_of('/');
    return (slash == std::string::npos ? std::string(".") : recent.substr(0, slash)) + "/sessions";
}

// A folder's file: its path's hash, so that any path makes a name.
std::string session_file(const std::string& dir, const std::string& folder) {
    u64 h = 1469598103934665603ull;
    for (const char c : folder) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    char name[24];
    std::snprintf(name, sizeof name, "/%016llx", static_cast<unsigned long long>(h));
    return dir + name;
}

// A folder as sessions know it: whole, with no slash at its end.
std::string folder_key(const std::string& path) {
    std::string out = full_path(path);
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

// (Plain C library calls: this one also runs on a thread before SDL is up.)
std::string read_file(const std::string& path) {
    std::string out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

void write_file(const std::string& path, const std::string& text) {
    if (const size_t slash = path.find_last_of('/'); slash != std::string::npos) SDL_CreateDirectory(path.substr(0, slash).c_str());  // (and the folders above it)
    const std::string tmp = path + ".tmp" + std::to_string(::getpid());  // (two blits on one folder each write whole files)
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) == 0 && ok) std::rename(tmp.c_str(), path.c_str());
    else std::remove(tmp.c_str());
}

std::vector<std::string_view> fields(std::string_view line, size_t most) {
    std::vector<std::string_view> out;
    while (out.size() + 1 < most) {
        const size_t tab = line.find('\t');
        if (tab == std::string_view::npos) break;
        out.push_back(line.substr(0, tab));
        line.remove_prefix(tab + 1);
    }
    out.push_back(line);
    return out;
}

double number(std::string_view s) { return std::strtod(std::string(s).c_str(), nullptr); }

// How many tabs a session brings back, from its first lines; -1 if it is
// not this folder's (or not a session).
int header_tabs(std::string_view text, const std::string& folder) {
    bool ours = false;
    for (int i = 0; i < 3 && !text.empty(); ++i) {
        const size_t nl = text.find('\n');
        const std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (i == 0 && line != kFirstLine) return -1;
        if (line.starts_with("folder\t")) ours = line.substr(7) == folder;
        if (line.starts_with("tabs\t") && ours) return static_cast<int>(number(line.substr(5)));
    }
    return -1;
}

}  // namespace

// One folder's session, as read.
struct App::Session {
    struct File {
        std::string path;
        u64 line = 0;            // the cursor: its line from 0, and the byte in that line
        u32 col = 0;
        double top = 0, left = 0;  // the view: the line at its top (and the part through it: 5.5 is halfway), columns across
    };
    // How the window was split (as SplitNode), each split's files.
    struct Node {
        bool leaf = true, side_by_side = true;
        float share = 0.5f;
        std::unique_ptr<Node> a, b;
        std::vector<File> files;
        size_t front = 0;
        bool focused = false;
    };
    std::string folder;
    bool panel = false, panel_open = false;
    std::string panel_tab, build;
    int panel_cols = 0;
    std::vector<std::string> debug;  // the breakpoints and the expressions watched, a line each
    std::map<std::string, std::string> langs;  // the language chosen for a file (from the status bar), by its path
    std::unique_ptr<Node> root;

    // Reads one; false if it is not a session of this folder.
    bool parse(std::string_view text, const std::string& want) {
        std::vector<std::string_view> lines;
        while (!text.empty()) {
            const size_t nl = text.find('\n');
            lines.push_back(text.substr(0, nl));
            text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        }
        if (lines.empty() || lines[0] != kFirstLine) return false;
        size_t i = 1;
        for (; i < lines.size(); ++i) {  // what comes before the splits (lines not known are left alone: a later blit's)
            const std::string_view l = lines[i];
            if (l.starts_with("H\t") || l.starts_with("V\t") || l.starts_with("pane\t")) break;
            const std::vector<std::string_view> f = fields(l, 4);
            if (f[0] == "folder" && f.size() == 2) folder = f[1];
            if (f[0] == "panel" && f.size() == 4) panel = true, panel_open = f[1] == "1", panel_tab = f[2], panel_cols = static_cast<int>(number(f[3]));
            if (f[0] == "build" && l.size() > 6) build = l.substr(6);  // (the task's key, which has a tab in it)
            if (f[0] == "break" || f[0] == "breakoff" || f[0] == "function" || f[0] == "watch") debug.emplace_back(l);  // (appdebug.cpp reads them)
            if (f[0] == "lang" && f.size() >= 3) langs[std::string(fields(l, 3)[2])] = f[1];  // lang, the language, the path (last: it may have tabs)
        }
        if (folder != want) return false;
        root = node(lines, i);
        return true;
    }
    static std::unique_ptr<Node> node(const std::vector<std::string_view>& lines, size_t& i) {
        if (i >= lines.size()) return nullptr;
        const std::vector<std::string_view> f = fields(lines[i], 3);
        auto n = std::make_unique<Node>();
        if ((f[0] == "H" || f[0] == "V") && f.size() >= 2) {
            ++i;
            n->leaf = false;
            n->side_by_side = f[0] == "H";
            n->share = std::clamp(static_cast<float>(number(f[1])), 0.05f, 0.95f);
            n->a = node(lines, i);
            n->b = node(lines, i);
            if (!n->a || !n->b) return std::move(n->a ? n->a : n->b);  // (half a split: what there is)
            return n;
        }
        if (f[0] != "pane") return nullptr;
        const std::vector<std::string_view> p = fields(lines[i], 3);
        n->front = p.size() > 1 ? static_cast<size_t>(number(p[1])) : 0;
        n->focused = p.size() > 2 && p[2] == "1";
        for (++i; i < lines.size() && lines[i].starts_with("file\t"); ++i) {
            const std::vector<std::string_view> x = fields(lines[i], 6);  // file, line, column, top, left, the path (last: it may have tabs)
            if (x.size() < 6 || x[5].empty()) continue;
            File file;
            file.line = static_cast<u64>(std::max(0.0, number(x[1])));
            file.col = static_cast<u32>(std::max(0.0, number(x[2])));
            file.top = std::max(0.0, number(x[3]));
            file.left = std::max(0.0, number(x[4]));
            file.path = x[5];
            n->files.push_back(std::move(file));
        }
        return n;
    }
    // Leaves out a file that is gone (or is a folder now), and a file its
    // split had already (another split may show it too: two views of it); a
    // split left with none goes, and its other half takes its room. The
    // files left (`seen`: every file kept, once).
    static size_t prune(std::unique_ptr<Node>& n, std::set<std::string>& seen) {
        if (!n) return 0;
        if (!n->leaf) {
            const size_t a = prune(n->a, seen), b = prune(n->b, seen);
            if (!a || !b) n = std::move(a ? n->a : n->b);
            return a + b;
        }
        const File* front = n->front < n->files.size() ? &n->files[n->front] : nullptr;
        const std::string front_path = front ? front->path : std::string();
        std::set<std::string> here;
        std::erase_if(n->files, [&](const File& f) {
            if (here.contains(f.path)) return true;
            if (!is_remote(f.path)) {  // (another machine's: found out when it is read)
                struct stat st;
                if (::stat(f.path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) return true;
            }
            here.insert(f.path);
            seen.insert(f.path);
            return false;
        });
        n->front = 0;
        for (size_t k = 0; k < n->files.size(); ++k)
            if (n->files[k].path == front_path) n->front = k;
        if (n->files.empty()) n.reset();
        return n ? n->files.size() : 0;
    }
};

struct App::SessionUi {
    std::string dir;     // where sessions are kept ("": this run keeps none)
    std::string recent;  // and the list of what was opened lately
    std::string folder;  // the project folder whose session this window holds ("": none)
    std::map<std::string, int> tabs;           // each folder's tab count, as Recent shows it
    std::map<std::string, std::string> wrote;  // what this run last wrote for a folder (newer than its file while that is on its way)
    // Files from another machine still on their way: the view to give each when it is here.
    std::map<u64, std::pair<double, double>> views;
    std::string dropped_why;  // what was last said of one that could not be read (said once)

    // The tab counts of the folders in `recent`, read on a thread.
    std::map<std::string, int> fetched;  // (the thread's until `ready`)
    std::atomic<bool> ready{false};
    bool fetching = false;
    std::jthread fetcher;

    // Sessions written on a thread of their own, in turn; all of them are
    // written before it ends (blit quitting waits for that).
    std::mutex mu;
    std::condition_variable_any cv;
    std::deque<std::pair<std::string, std::string>> jobs;  // (mu) file, text
    std::jthread writer;  // (last: stopped and joined first)

    // wake: the start screen is drawn again when they are in (user event
    // 19). Not at startup: SDL is starting meanwhile; start_init() waits.
    void fetch(bool wake) {
        if (fetching || dir.empty()) return;
        fetching = true;
        fetcher = std::jthread([this, wake, recent_file = recent, sessions = dir] {
            std::map<std::string, int> got;
            const std::string list = read_file(recent_file);
            std::string_view all = list;
            while (!all.empty()) {
                const size_t nl = all.find('\n');
                const std::string_view line = all.substr(0, nl);
                all = nl == std::string_view::npos ? std::string_view{} : all.substr(nl + 1);
                if (line.size() <= 2 || !line.starts_with("d\t")) continue;
                const std::string path(line.substr(2));
                const std::string text = read_file(session_file(sessions, path));
                if (const int n = header_tabs(text, path); n >= 0) got[path] = n;
            }
            fetched = std::move(got);
            ready = true;
            if (!wake) return;
            SDL_Event ev{};
            ev.type = SDL_EVENT_USER;
            ev.user.code = 19;
            SDL_PushEvent(&ev);
        });
    }

    void write(std::string file, std::string text) {
        {
            std::lock_guard lock(mu);
            std::erase_if(jobs, [&](const auto& j) { return j.first == file; });  // (only the newest of a folder's matters)
            jobs.emplace_back(std::move(file), std::move(text));
        }
        if (!writer.joinable())
            writer = std::jthread([this](std::stop_token stop) {
                for (;;) {
                    std::pair<std::string, std::string> job;
                    {
                        std::unique_lock lock(mu);
                        cv.wait(lock, stop, [&] { return !jobs.empty(); });
                        if (jobs.empty()) return;  // (asked to stop, and nothing is left)
                        job = std::move(jobs.front());
                        jobs.pop_front();
                    }
                    write_file(job.first, job.second);
                }
            });
        cv.notify_one();
    }
};

void App::session_create() {
    session_ = std::make_shared<SessionUi>();
    SessionUi& u = *session_;
    u.dir = sessions_dir(opt_);
    u.recent = recent_path(opt_);
    // The start screen will show Recent: its folders' tab counts are read
    // while the window starts, ready for its first frame.
    if (cfg_.restore && cfg_.start_screen && !opt_.file && !opt_.info) u.fetch(false);
}

// wait: for the counts being read (start_init(), before the first frame;
// they have had the window's whole start to come in). Else never waits.
int App::session_tabs(const std::string& dir, bool wait) {
    SessionUi& u = *session_;
    if (!cfg_.restore || u.dir.empty()) return -1;
    if (!u.fetching) u.fetch(true);  // (they come in a moment, and the start screen is drawn again)
    if ((u.ready || wait) && u.fetcher.joinable()) {
        u.fetcher.join();
        for (auto& [folder, n] : u.fetched) u.tabs.try_emplace(folder, n);  // (what this run wrote is newer)
        u.fetched.clear();
        ++session_gen_;
    }
    const auto it = u.tabs.find(dir);
    return it == u.tabs.end() ? -1 : it->second;
}

// A folder opened to work in. The folder the window had keeps its session;
// this one's comes back, if it has one and this one is not already the
// window's. True if it came back.
bool App::project_open(const std::string& path) {
    SessionUi& u = *session_;
    const std::string dir = folder_key(path);
    if (dir.empty()) return false;
    remote_unreached_drop(dir);
    const bool same = dir == u.folder;
    if (!same) session_save();
    panel_set_workdir(dir);
    recent_add(dir, true);
    if (same) return false;
    u.folder = dir;
    if (!cfg_.restore || u.dir.empty()) return false;
    const auto wrote = u.wrote.find(dir);
    const std::string text = wrote != u.wrote.end() ? wrote->second : read_file(session_file(u.dir, dir));
    Session st;
    if (text.empty() || !st.parse(text, dir)) return false;
    session_apply(st);
    return true;
}

// ---------------------------------------------------------------- writing

// The window as a session: a line for each split and each of its files,
// in the order of the tree (a split of two, then its halves).
std::string App::session_text() {
    SessionUi& u = *session_;
    const double ch = font_ ? font_->cell_h() : 1, cw = font_ ? font_->cell_w() : 1;
    size_t count = 0;
    auto kept = [](const Doc& d) { return !d.ed.path.empty() && d.title.empty() && d.ed.path.find('\n') == std::string::npos; };
    auto emit = [&](auto&& self, const SplitNode* n) -> std::string {
        if (!n) return {};
        if (!n->pane) {
            const std::string a = self(self, n->a.get()), b = self(self, n->b.get());
            if (a.empty() || b.empty()) return a + b;  // (a split with nothing to keep: its other half has the room)
            char head[32];
            std::snprintf(head, sizeof head, "%c\t%.4f\n", n->side_by_side ? 'H' : 'V', static_cast<double>(n->share));
            return head + a + b;
        }
        const Pane& p = *n->pane;
        std::string files;
        size_t shown = 0, front = 0;
        for (size_t i = 0; i < p.docs.size(); ++i) {
            const Doc& d = *p.docs[i];
            if (!kept(d)) continue;
            if (i <= p.tab) front = shown;
            u64 line = 0, col = 0;
            const auto kept_here = p.places.find(d.id);
            const bool own = kept_here != p.places.end() && kept_here->second.scrolled;  // (a file another split shows too: its view here)
            double top = (&d == p.doc ? p.target_y : own ? kept_here->second.scroll_y : d.scroll_y) / ch,
                   left = (&d == p.doc ? p.target_x : own ? kept_here->second.scroll_x : d.scroll_x) / cw;
            if (d.loading) {  // (on its way from another machine: where it is to go)
                if (d.goto_line != ~0ull) line = d.goto_line, col = d.goto_col;
                if (const auto v = u.views.find(d.id); v != u.views.end()) top = v->second.first, left = v->second.second;
            } else if (!d.image && !d.pdf) {
                const u64 cur = std::min(caret_in(p, d).cursor, d.ed.buf.size());
                line = d.ed.buf.line_of(cur);
                col = cur - d.ed.buf.line_start(line);
                top = view_line(p, d, top);  // (rows are lines only where nothing wraps or is folded)
            }
            char nums[96];
            std::snprintf(nums, sizeof nums, "file\t%llu\t%llu\t%.2f\t%.2f\t", static_cast<unsigned long long>(line), static_cast<unsigned long long>(col),
                          std::max(0.0, top), std::max(0.0, left));
            files += nums + full_path(d.ed.path) + "\n";
            ++shown;
        }
        if (!shown) return {};
        count += shown;
        return "pane\t" + std::to_string(front) + "\t" + (n->pane == pane_ ? "1" : "0") + "\n" + files;
    };
    const std::string splits = emit(emit, split_.get());
    bool open = false;
    std::string tab;
    int cols = 0;
    panel_session(&open, &tab, &cols);
    std::string out = std::string(kFirstLine) + "\nfolder\t" + u.folder + "\ntabs\t" + std::to_string(count) + "\n";
    out += "panel\t" + std::string(open ? "1" : "0") + "\t" + tab + "\t" + std::to_string(cols) + "\n";
    if (const std::string build = build_chosen(); !build.empty() && build.find('\n') == std::string::npos) out += "build\t" + build + "\n";
    out += debug_session_text();
    // The languages chosen for files, each once (a file may be in two splits).
    std::set<std::string> said;
    for (const std::unique_ptr<Pane>& p : panes_)
        for (const std::shared_ptr<Doc>& d : p->docs)
            if (kept(*d) && !d->lang.empty() && d->lang.find_first_of("\t\n") == std::string::npos && said.insert(full_path(d->ed.path)).second)
                out += "lang\t" + d->lang + "\t" + full_path(d->ed.path) + "\n";
    return out + splits;
}

// The project folder's session, as things are now; written on the
// writer's thread (on quit, blit waits for it before it goes).
void App::session_save() {
    SessionUi& u = *session_;
    if (u.folder.empty() || u.dir.empty() || !cfg_.restore) return;
    std::string text = session_text();
    u.tabs[u.folder] = header_tabs(text, u.folder);
    u.wrote[u.folder] = text;
    ++session_gen_;
    u.write(session_file(u.dir, u.folder), std::move(text));
}

std::string App::session_dump() {
    SessionUi& u = *session_;
    if (u.folder.empty()) return "no project folder";
    return session_text();
}

// ---------------------------------------------------------------- bringing back

namespace {

// A tab that cannot simply go when another arrangement takes the window's
// place: changes not saved, a program still running, a profile (made, not read).
bool must_stay(const Doc& d) {
    if (d.ed.modified() || d.prof) return true;
    return d.term && !d.term_closed;
}

}  // namespace

// The window as the session has it. A tab already open that the session
// has is taken as it is (its changes, its undo, its view); one that must
// stay (see must_stay) goes to the split that gets the keyboard; the rest
// are closed. A file here is read now; one on another machine gets its tab
// now and its text when the connection is up (as any file there does).
void App::session_apply(Session& st) {
    SessionUi& u = *session_;
    std::set<std::string> seen;
    const size_t files = Session::prune(st.root, seen);
    if (files > 0) {
        stop_lsp();
        lsp_close(kPopAll);
        drag_cancel();
        // The terminals' split along the bottom is put away whole while the
        // arrangement changes (its programs keep running), and back after.
        // (One with no terminal left in it is only a split like any other.)
        const Pane* dock = dock_pane();
        const bool docked = dock && std::ranges::any_of(dock->docs, [](const std::shared_ptr<Doc>& d) { return d->term != nullptr; });
        if (docked) dock_hide();
        // What there is now: the session's files, what must stay, the rest.
        std::map<std::string, std::shared_ptr<Doc>> have;
        std::vector<std::shared_ptr<Doc>> stay;
        std::vector<const Doc*> gone;  // (let go of: once, however many splits showed it)
        for (const std::unique_ptr<Pane>& p : panes_) {
            if (p->doc) p->doc->scroll_x = p->target_x, p->doc->scroll_y = p->target_y;  // (its view goes with it)
            for (std::shared_ptr<Doc>& d : p->docs) {
                const std::string full = d->ed.path.empty() || !d->title.empty() ? std::string() : full_path(d->ed.path);
                if (!full.empty() && seen.contains(full) && !have.contains(full)) have[full] = std::move(d);
                else if (const auto same = have.find(full); !full.empty() && same != have.end() && same->second.get() == d.get()) d.reset();  // (shown in two splits: one document)
                else if (std::ranges::any_of(stay, [&](const std::shared_ptr<Doc>& x) { return x.get() == d.get(); })) d.reset();
                else if (must_stay(*d)) stay.push_back(std::move(d));
                else if (std::ranges::find(gone, d.get()) == gone.end()) gone.push_back(d.get()), forget_doc(d.get());
            }
            p->docs.clear();
            p->doc = nullptr;
        }
        panes_.clear();
        split_.reset();
        pane_ = nullptr;

        const double ch = font_->cell_h(), cw = font_->cell_w();
        std::set<u64> placeholders;
        std::map<Pane*, Doc*> fronts;
        std::vector<std::tuple<Pane*, u64, double>> tops;  // (each view's top line, made rows once the splits are placed)
        Pane* focused = nullptr;
        restoring_ = true;
        auto build = [&](auto&& self, const Session::Node& n) -> std::unique_ptr<SplitNode> {
            auto out = std::make_unique<SplitNode>();
            if (!n.leaf) {
                out->side_by_side = n.side_by_side;
                out->share = n.share;
                out->a = self(self, *n.a);
                out->b = self(self, *n.b);
                return out;
            }
            Pane* p = add_pane();
            out->pane = p;
            pane_ = p;
            if (n.focused || !focused) focused = p;
            p->doc = add_doc(0);  // (open_path wants a tab in front: the first file read takes this one's place)
            p->tab = 0;
            placeholders.insert(p->doc->id);
            for (size_t i = 0; i < n.files.size(); ++i) {
                const Session::File& f = n.files[i];
                if (const auto it = have.find(f.path); it != have.end()) {  // open already: as it is
                    p->docs.push_back(std::move(it->second));
                    have.erase(it);
                    tab_show(p->docs.size() - 1);
                } else {
                    tab_show(p->docs.size() - 1);  // (a file opened goes after the one in front: the last)
                    if (!open_path(f.path.c_str()) || p->doc->ed.path.empty()) continue;
                    Doc& d = *p->doc;
                    if (d.loading) {  // another machine's: where it goes, once it is here
                        d.goto_line = f.line;
                        d.goto_col = f.col;
                        d.goto_utf16 = false;
                        u.views[d.id] = {f.top, f.left};
                    } else if (!d.image && !d.pdf) {
                        Editor& ed = d.ed;
                        const u64 last = ed.buf.line_count() > 0 ? ed.buf.line_count() - 1 : 0, line = std::min(f.line, last);
                        const u64 start = ed.buf.line_start(line);
                        u64 off = std::min(start + f.col, ed.line_end(line));
                        for (char c; off > start && ed.buf.read(off, &c, 1) == 1 && (static_cast<unsigned char>(c) & 0xC0) == 0x80;) --off;  // (not inside a character)
                        ed.move_to(off, false);
                        p->scroll_y = p->target_y = std::min(f.top, static_cast<double>(last)) * ch;
                        tops.emplace_back(p, d.id, f.top);
                        p->scroll_x = p->target_x = f.left * cw;
                        p->cache_rev = ~0ull;
                    }
                }
                if (i == n.front || !fronts.contains(p)) fronts[p] = p->doc;
            }
            return out;
        };
        split_ = build(build, *st.root);
        restoring_ = false;
        if (!focused) focused = panes_.front().get();
        // What must stay, in the split with the keyboard; and the empty tabs
        // the splits were begun with go where there is anything else.
        pane_ = focused;
        for (std::shared_ptr<Doc>& d : stay) {
            focused->docs.push_back(std::move(d));
            tab_show(focused->docs.size() - 1);
        }
        for (const std::unique_ptr<Pane>& p : panes_) {
            if (p->docs.size() > 1 && placeholders.contains(p->docs.front()->id)) {
                if (p->doc == p->docs.front().get()) p->doc = nullptr;
                p->docs.erase(p->docs.begin());
            }
            pane_ = p.get();
            Doc* front = fronts.contains(p.get()) ? fronts[p.get()] : nullptr;
            size_t at = p->docs.size() - 1;
            for (size_t k = 0; k < p->docs.size(); ++k)
                if (p->docs[k].get() == front) at = k;
            if (!p->doc) {  // (the one in front went: another, without keeping a view for the one gone)
                p->doc = p->docs[at].get();
                p->tab = at;
                p->scroll_x = p->target_x = p->doc->scroll_x;
                p->scroll_y = p->target_y = p->doc->scroll_y;
            }
            for (size_t k = 0; k < p->docs.size(); ++k)
                if (p->docs[k].get() == p->doc) p->tab = k;
            tab_show(at);
        }
        pane_ = focused;
        if (docked) dock_show(focused);
        pane_ = focused;
        place_panes(px_w_, px_h_);
        // Each view's top was kept as a line: the rows to it are this
        // window's (a file that wraps, at its split's width now).
        for (const auto& [p, id, top] : tops) {
            const auto it = std::ranges::find_if(p->docs, [&](const std::shared_ptr<Doc>& x) { return x->id == id; });
            if (it == p->docs.end()) continue;
            Doc& d = **it;
            pane_ = p;
            if (&d == p->doc) {
                wrap_sync(*p);
                p->scroll_y = p->target_y = view_rows(*p, d, top) * ch;
            } else if (const auto at = p->places.find(id); at != p->places.end() && at->second.scrolled) {
                at->second.scroll_y = view_rows(*p, d, top) * ch;
            } else {
                d.scroll_y = view_rows(*p, d, top) * ch;
            }
        }
        pane_ = focused;
        for_each_pane([this] {  // (a file that has grown shorter: its view within it)
            pane_->target_y = pane_->scroll_y = std::clamp(pane_->target_y, 0.0, std::max(0.0, max_scroll_y()));
        });
        // The languages chosen for files: their colours start again in them
        // (one on its way from another machine starts with it when it is here).
        for (const std::unique_ptr<Pane>& p : panes_)
            for (const std::shared_ptr<Doc>& d : p->docs) {
                const auto it = d->ed.path.empty() ? st.langs.end() : st.langs.find(full_path(d->ed.path));
                if (it == st.langs.end() || d->lang == it->second) continue;
                if (d->loading) d->lang = it->second;
                else set_language(*d, it->second);
            }
        lsp_pending_ = grammars_pending_ = true;
        vim_attach();
        // (A notebook in front of a split: its view beside it, as opening one shows it.)
        std::vector<Pane*> notebooks;
        for (const std::unique_ptr<Pane>& p : panes_)
            if (p->doc && p->doc->nb && !p->floating && p.get() != dock_pane()) notebooks.push_back(p.get());
        for (Pane* p : notebooks) {
            pane_ = p;
            md_open();
        }
        if (!notebooks.empty()) {
            pane_ = notebooks.back();
            focus_pane(focused);
        }
    }
    if (st.panel) panel_session_set(st.panel_open, st.panel_tab, st.panel_cols);
    if (!st.build.empty()) build_choose(st.build);
    debug_session_take(st.debug);
    u.tabs[st.folder] = static_cast<int>(files);
    ++session_gen_;
    place_panes(px_w_, px_h_);
    pick_theme();
    dirty_ = true;
}

// Another working tree of the repository opened as the folder worked in,
// from the tree `from`: its session, if it has one. Else it starts with
// the tabs the window has, each file of `from` at its place in this tree
// (those it has), and the rest go as for another folder's session; none
// of `from`'s files is left in front under this tree's name.
void App::worktree_open(const std::string& path, const std::string& from) {
    SessionUi& u = *session_;
    const std::string dir = folder_key(path), old = folder_key(from), was = u.folder;
    if (dir.empty() || old.empty() || dir == old || dir == was) return (void)project_open(path);
    // Where in `from` a file is (from its slash on), or nothing. Git names
    // the tree by its real path; the tabs keep the path they were opened
    // by, which may go through a link (/tmp, a linked project folder).
    char buf[PATH_MAX];
    const std::string real_old = !is_remote(old) && ::realpath(old.c_str(), buf) ? std::string(buf) : old;
    auto in_old = [&](const std::string& file) -> std::string {
        if (file.starts_with(old + "/")) return file.substr(old.size());
        if (const std::string real = real_place(file); real.starts_with(real_old + "/")) return real.substr(real_old.size());
        return {};
    };
    const std::string text = session_text();  // (as the window is, before the folder changes)
    if (project_open(dir)) return;
    Session st;
    if (!st.parse(text, was)) return;
    auto move = [&](auto&& self, Session::Node* n) -> void {
        if (!n) return;
        self(self, n->a.get());
        self(self, n->b.get());
        for (Session::File& f : n->files)
            if (const std::string rest = in_old(f.path); !rest.empty()) f.path = dir + rest;
    };
    move(move, st.root.get());
    st.folder = dir;
    st.panel = false;  // (the side panel as it is, and the Build tab's task is the other tree's)
    st.build.clear();
    std::set<std::string> seen;
    if (Session::prune(st.root, seen) > 0) return session_apply(st);
    // (None of them is in this tree: the other tree's go all the same.)
    std::vector<u64> theirs;
    for (const std::unique_ptr<Pane>& p : panes_)
        for (const std::shared_ptr<Doc>& d : p->docs)
            if (!d->ed.path.empty() && d->title.empty() && !in_old(full_path(d->ed.path)).empty() && !must_stay(*d)) theirs.push_back(d->id);
    Pane* const focused = pane_;
    for (const u64 id : theirs) {
        Pane* where = nullptr;
        size_t at = 0;
        while (find_doc(id, &where, &at)) {  // (each split that shows it)
            pane_ = where;
            tab_close(at, true);
        }
    }
    if (std::ranges::any_of(panes_, [&](const std::unique_ptr<Pane>& p) { return p.get() == focused; })) pane_ = focused;
    dirty_ = true;
}

// A file from another machine that a session opened is here: its view as
// the session had it (the cursor went where it was: goto_line).
bool App::session_arrived(Pane& p, Doc& d) {
    SessionUi& u = *session_;
    const auto v = u.views.find(d.id);
    if (v == u.views.end()) return false;
    const double ch = font_->cell_h(), cw = font_->cell_w();
    if (&d == p.doc) wrap_sync(p);  // (its rows: the line kept is turned into them)
    const double top = view_rows(p, d, v->second.first) * ch, left = v->second.second * cw;
    u.views.erase(v);
    if (&d == p.doc) {
        p.scroll_y = p.target_y = top;
        p.scroll_x = p.target_x = left;
        p.cache_rev = ~0ull;
    } else {
        d.scroll_y = top;
        d.scroll_x = left;
    }
    dirty_ = true;
    return true;
}

// One that could not be read: if a session opened it, it goes without a
// word if it is no longer there, and with one (once for the same reason)
// if it could not be read for another.
bool App::session_dropped(u64 doc, const std::string& why) {
    SessionUi& u = *session_;
    if (!u.views.erase(doc)) return false;
    if (!why.empty() && why != u.dropped_why) {
        u.dropped_why = why;
        notify(why, 1);
    }
    return true;
}

}  // namespace blit
