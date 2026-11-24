// Building: the side panel's Build tab lists what the working folder can
// be built with (CMake, make, Cargo, npm ... found by build.cpp, and what
// init.lua adds with blit.build.add), and the Run button at the window's
// top right runs the task chosen in an output terminal below the text
// (Cmd+Shift+B). The folder is looked at on a thread of its own: on
// another machine each file read is a round trip.
#include <SDL3/SDL.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include "app.hpp"
#include "buildread.hpp"
#include "crashscan.hpp"
#include "flash.hpp"
#include "look.hpp"
#include "missing.hpp"
#include "notify.hpp"
#include "remote.hpp"
#include "serial.hpp"
#include "serialtab.hpp"
#include "simrun.hpp"
#include "textbox.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 16;
    SDL_PushEvent(&ev);
}

std::string task_key(const BuildTask& t) { return t.system + "\t" + t.name; }

// A row that is picked: lit as selected while the panel has the keyboard,
// in the quieter tone of the mouse's while it does not.
void light_row(TextBox& box, u32 row, bool focused) {
    if (focused) return box.select_row(row);
    if (row >= box.rows) return;
    for (u32 c = 0; c < box.cols; ++c) box.cells[static_cast<size_t>(row) * box.cols + c] |= kCellHover;
}

// Text in lines of at most `cols` columns, broken between words.
std::vector<std::string> wrapped(std::string_view text, u32 cols) {
    std::vector<std::string> out(1);
    while (!text.empty()) {
        const size_t space = text.find(' ');
        const std::string_view word = text.substr(0, space);
        text = space == std::string_view::npos ? std::string_view{} : text.substr(space + 1);
        if (!out.back().empty() && width_of(out.back()) + 1 + width_of(word) > cols) out.emplace_back();
        if (!out.back().empty()) out.back() += ' ';
        out.back() += fit_text(word, cols);
    }
    return out;
}

// A time as a clock says it: "0:07", "12:34", "1:02:03".
std::string clock_text(u64 ns) {
    const u64 s = ns / 1'000'000'000ull;
    char out[32];
    if (s >= 3600) std::snprintf(out, sizeof out, "%llu:%02llu:%02llu", static_cast<unsigned long long>(s / 3600), static_cast<unsigned long long>(s / 60 % 60), static_cast<unsigned long long>(s % 60));
    else std::snprintf(out, sizeof out, "%llu:%02llu", static_cast<unsigned long long>(s / 60), static_cast<unsigned long long>(s % 60));
    return out;
}

constexpr u64 kAskAfterNs = 10'000'000'000ull;  // (a long task that has run this long is asked about before it is ended)

}  // namespace

struct App::BuildUi {
    std::vector<BuildTask> tasks;
    std::string root;          // the folder they were found in
    bool looked = false;       // tasks are root's (else: look, when the window is free to)
    bool again = false;        // what it builds with may have changed while it was being looked at: looked at again
    std::string current;       // the task the Run button runs (task_key)
    std::string taken;         // the run configuration Debug and Profile last took (and its command)
    // The worker looking at a folder, and what it found.
    struct Found {
        std::string root;
        std::vector<FsEntry> files;
        std::vector<BuildTask> tasks;
        RunEnvironments environments;  // (run.json's)
        std::string problem;  // what is wrong with .blit/run.json, if anything
    };
    std::mutex mu;
    std::optional<Found> found;  // (mu)
    bool looking = false;
    std::vector<std::function<void()>> when_ready;  // (build_when_ready)
    std::string looking_at;
    // The output terminal of the last run.
    u64 doc = 0;
    std::string dir;  // the folder it ran in (on another machine: ssh://host/...)
    bool running = false;
    int last_status = -1;
    std::string last_missing;  // (its program was not there: said, "cmake is not installed on this Mac")
    std::string last_name;
    std::string last_key;  // the task it ran (task_key), when it was one of the tab's
    // The terminal the runs that are no long task's share: each one there
    // ends the one before (a long task's has a tab of its own).
    u64 term = 0;
    // What holds problems in the Problems tab, under a heading of its own: a
    // run, or a file read for them (build_read). Its place among the others
    // (a run again takes the place of the one it replaced), the group of each
    // of its files (BuildProblem::group), and how many of each kind it has.
    struct Held {
        u64 order = 0;
        std::unordered_map<std::string, u32> files;
        u32 errors = 0, warnings = 0;
    };
    // The runs whose tabs are open: each one's task (task_key; empty: no
    // task's, as the tests' run), what it is, how it went, and its problems.
    struct Run : Held {
        u64 doc = 0;
        std::string key, name, kind, cmd;
        std::string dir;  // (where it ran)
        bool lasting = false, running = true, stopped = false;
        int status = -1;
        std::string missing;  // (as last_missing)
        u64 started_ns = 0;
        i64 started_at = 0;  // (the clock's time, in seconds: for the files it wrote)
    };
    std::vector<Run> runs;
    Run* run_of(u64 id) {
        for (Run& r : runs)
            if (r.doc == id && id) return &r;
        return nullptr;
    }
    const Run* run_of(u64 id) const { return const_cast<BuildUi*>(this)->run_of(id); }
    // The serial tabs that have crashed (their dumps decoded: appterm.cpp),
    // each under its tab's title, while the tab is open.
    struct Port : Held {
        u64 doc = 0;
        std::string title;
    };
    std::vector<Port> ports;
    Port* port_of(u64 id) {
        for (Port& p : ports)
            if (p.doc == id && id) return &p;
        return nullptr;
    }
    // The long run whose time is on show (the last run, if it is one; else
    // the one that has run longest), and its seconds as last drawn.
    const Run* ticking() const {
        if (const Run* r = run_of(doc); r && r->lasting && r->running) return r;
        const Run* oldest = nullptr;
        for (const Run& r : runs)
            if (r.lasting && r.running && (!oldest || r.started_ns < oldest->started_ns)) oldest = &r;
        return oldest;
    }
    i64 clock_shown = -1;
#if BLIT_DEV
    std::vector<std::string> drawn_rows;  // (tests) each task's row as the Build tab last drew it: its name, then its words
    std::vector<std::string> drawn_shown;  // (and its words as they fitted)
#endif
    // What the open runs' output said was wrong, and a file read's: each
    // run's together, in their order; in a run, a file's together, the files
    // in the order they came. Each group's run's order (by
    // BuildProblem::group; kGone once its run has gone), the last order
    // given, and the order a run that is replacing another takes. The one
    // F8 last went to.
    std::vector<BuildProblem> problems;
    std::vector<u64> group_order;
    static constexpr u64 kGone = ~0ull;
    u64 orders = 0, take_order = 0;
    u32 seq = 0;
    i64 at = -1;
    bool before(u32 a, u32 b) const {  // (group a's problems come before group b's)
        const u64 x = group_order[a], y = group_order[b];
        return x != y ? x < y : a < b;
    }
    void take(Held& h, std::vector<BuildProblem>& come, u32* errors, u32* warnings);
    bool drop(Held& h, u32* errors, u32* warnings);
    // How a run is going, for its heading ("running…", "failed (exit 2)"); and
    // what to say of one that failed with no error in its output ("make
    // failed with exit code 2: no problem found in its output"; "" when not so).
    static std::string state_of(const Run& r);
    static std::string failure_of(const Run& r);
    // The Problems tab's groups (build_groups), as last made, and when.
    std::vector<BuildGroup> group_list;
    u64 groups_gen = 0;
    // What a task's run came to (its tests' count, sizes, timing: its
    // output's, buildtests.cpp), in words for its row and the Run button;
    // each task's sizes as its last run that gave them had them, and those
    // this run's are set against (kept for the session, not across).
    std::string summary;
    std::unordered_map<std::string, std::vector<BuildScan::SizeUsed>> sizes;
    std::vector<BuildScan::SizeUsed> sizes_before;
    // How far an engine the run drives has got (flashing, an FPGA's load:
    // buildprogress.cpp), for the Run button and the task's row while it
    // runs; what it came to goes before the outcome's words (said) in the
    // summary.
    BuildScan::Progress progress;
    std::string said;
    bool unrun = false;  // the last "run" ran nothing: its summary says why (a flash whose image is unchanged: build_note)
    // A file read for problems (build_read), on a thread of its own, and
    // what it gave; the file the problems are from, once taken, and them.
    struct Read {
        std::string path, kind, why;  // (why: it could not be read)
        bool ok = false;
        std::vector<BuildProblem> found;
    };
    std::optional<Read> read;  // (mu)
    bool reading = false;
    std::string read_from;
    Held held_read;
    // Those problems in an open file's text, by its document: each a line
    // (from 0) and the bytes of it underlined, made once when they come
    // (not each frame), moved as lines are put in or taken out above them,
    // and dropped when their own line is changed (what the build said of
    // it no longer stands).
    struct Mark {
        u64 line = 0;
        u32 c0 = 0, c1 = 0;  // bytes into the line
        u32 len = 0;         // the line's bytes when the mark was made
        u32 group = 0;       // its problem's (it goes when that run's problems do)
        u8 severity = 1;
        std::string message;  // (its first line)
    };
    struct Marks final : EditListener {
        std::vector<Mark> marks;  // by line
        u64 gen = 0;              // build_prob_gen_ when made
        u32 next_seq = 0;         // the problems after those taken
        bool listening = false;
        void on_edit(const TextEdit& e) override;
    };
    std::unordered_map<u64, std::unique_ptr<Marks>> marks;
    // The underlines and the cursor line's words, as last worked out, and what for.
    std::vector<GridDeco> decos, decos_all;
    std::vector<u32> note_cells;
    GridNote note;
    std::string note_text;
    bool noted = false;
    u64 deco_key[9] = {};
    // CMake's build type and target, chosen in the Build tab (empty: the
    // project's own, and all), put into its tasks as they run.
    std::string cmake_type, cmake_target;
    // run.json's environments, and the one chosen in the Build tab (empty:
    // none), laid over each run configuration's own variables.
    RunEnvironments environments;
    std::string environment;
    // The build folder's targets, listed on a thread of its own (on another
    // machine the listing is a round trip), and what it found.
    struct Listed {
        bool ran = false;  // (else: CMake listed none)
        std::string missing;  // (none, as cmake is not installed there: said)
        std::vector<std::string> targets;
    };
    std::optional<Listed> listed;  // (mu)
    bool listing = false;
    bool said_no_notifier = false;  // (that the system could not be told how a run went: said once)
    // The Build tab: a row for each system and for each task, and the one chosen.
    struct Row {
        int task = -1;  // -1: a system's heading; kTypeRow, kTargetRow: CMake's build type and target
        std::string text;
        u32 count = 0;  // a heading's: its tasks
    };
    std::vector<Row> rows;
    u32 sel = 0, top = 0;
    u32 body = 0;  // the rows of tasks the tab last had room for
    // The Run button, where it was drawn, and what its cells were made from.
    TextBox box;
    u64 drawn = 0;
    int reserve = 0;  // its width and margin: the top right tab bar ends before it
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    // A build run first (build_first): what waits for it, and how it went
    // once it has ended (taken on the main loop's next turn, not inside
    // term_sync, whose terminals what waits may open and close).
    struct First {
        std::string what, task;
        std::function<void()> go;
        u64 doc = 0;  // its run's terminal
        bool ended = false;
        int status = 0;
    };
    std::optional<First> first;
    // Last, so that it is joined first, while all it uses is still there.
    std::jthread worker;
};

void App::build_create() { buildui_ = std::make_shared<BuildUi>(); }

namespace {

constexpr int kTypeRow = -2, kTargetRow = -3;  // (the Build tab's rows for CMake's build type and target)
constexpr int kEnvRow = -4;                     // (and for the run configurations' environment)
bool choosable(int task) { return task >= 0 || task == kTypeRow || task == kTargetRow || task == kEnvRow; }

// A CMake task's command with the build type and target chosen: a
// configure given -DCMAKE_BUILD_TYPE (and, where a build is all it does, a
// configure first, so that the type is the folder's), a build given
// --target. A preset's build type is its own, and its build is given the
// target after the preset's name.
std::string with_cmake(std::string cmd, const std::string& type, const std::string& target) {
    auto word_end = [&](size_t from) {  // (a word sh_quote quoted ends at a space outside its quotes)
        bool quoted = false;
        size_t e = from;
        for (; e < cmd.size() && (quoted || cmd[e] != ' '); ++e) {
            if (cmd[e] == '\'') quoted = !quoted;
            else if (cmd[e] == '\\' && !quoted) ++e;
        }
        return std::min(e, cmd.size());
    };
    if (!type.empty() && cmd.find("--preset") == std::string::npos) {
        const size_t conf = cmd.find("cmake -S . -B ");
        if (conf != std::string::npos) {
            cmd.insert(word_end(conf + 14), " -DCMAKE_BUILD_TYPE=" + type);
        } else if (const size_t build = cmd.find("cmake --build "); build != std::string::npos && cmd.find("--target clean") == std::string::npos) {
            const std::string dir = cmd.substr(build + 14, word_end(build + 14) - build - 14);
            cmd = "cmake -S . -B " + dir + " -DCMAKE_BUILD_TYPE=" + type + " && " + cmd;
        }
    }
    if (!target.empty() && cmd.find("--target") == std::string::npos)
        if (const size_t build = cmd.find("cmake --build "); build != std::string::npos) {
            const size_t preset = std::string_view(cmd).substr(build + 14).starts_with("--preset ") ? build + 23 : build + 14;
            cmd.insert(word_end(preset), " --target " + sh_word(target));
        }
    return cmd;
}

template <class Fn>
void each_line(std::string_view text, Fn fn) {
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        fn(text.substr(0, nl));
        if (nl == std::string_view::npos) break;
        text.remove_prefix(nl + 1);
    }
}

// A task's command as it runs: after its setup (an SDK's environment), in
// the same shell.
std::string with_setup(const BuildTask& t, const std::string& cmd) { return t.setup.empty() ? cmd : t.setup + " && " + cmd; }

// A build folder's targets, "all" first, from what build_option's command
// printed: CMake's help, then a form feed and (with Ninja) Ninja's own list
// of what it makes. `said`: the help printed anything.
std::vector<std::string> cmake_targets(std::string_view out, bool* said) {
    const size_t ff = out.find("\n\f\n");
    const std::string_view help = out.substr(0, ff), made = ff == std::string_view::npos ? std::string_view{} : out.substr(ff + 3);
    *said = !help.empty();
    std::vector<std::string> targets{"all"};
    each_line(help, [&](std::string_view line) {
        std::string name;
        if (line.starts_with("... ")) name = std::string(line.substr(4, line.find(' ', 4) - 4));  // (make's "... name")
        else if (const size_t c = line.find(": phony"); c != std::string_view::npos) name = std::string(line.substr(0, c));  // (Ninja's "name: phony")
        else return;
        // (CMake's own targets, and a file's object, preprocessed and assembly ones, are no target of the project's.)
        static constexpr std::string_view kOwn[] = {"all", "clean", "depend", "edit_cache", "rebuild_cache", "install", "list_install_components", "help", "test", "package", "package_source", "ZERO_CHECK", "ALL_BUILD", "RUN_TESTS"};
        const bool file = name.ends_with(".o") || name.ends_with(".obj") || name.ends_with(".i") || name.ends_with(".s");
        if (!name.empty() && name.find('/') == std::string::npos && !file && std::ranges::find(kOwn, name) == std::end(kOwn) && std::ranges::find(targets, name) == targets.end())
            targets.push_back(name);
    });
    // (Ninja's help lists phony names alone: an executable of the top
    // folder is a file it makes, which Ninja's own list of targets has.)
    each_line(made, [&](std::string_view line) {
        const size_t c = line.find(": ");
        if (c == std::string_view::npos || line.substr(c).find("_EXECUTABLE_LINKER__") == std::string_view::npos) return;
        const std::string name(line.substr(0, c));
        if (name.find('/') == std::string::npos && std::ranges::find(targets, name) == targets.end()) targets.push_back(name);
    });
    return targets;
}

}  // namespace

std::string App::build_root() const {
    const std::string* root = panel_root();
    return root ? *root : default_workdir();
}

// After each frame: the working folder is looked at when it changed (or
// when what it builds with may have).
void App::build_follow() {
    BuildUi& u = *buildui_;
    const std::string root = build_root();
    if (u.looking || (u.looked && root == u.root) || root.empty()) return;
    if (u.worker.joinable()) u.worker.join();
    u.looking = true;
    u.looking_at = root;
    std::vector<std::string> hidden = script_hidden_builds();
    u.worker = std::jthread([&u, root, hidden = std::move(hidden)] {
        BuildUi::Found f;
        f.root = root;
        f.files = fs_list(root);
        // (The folder's own tasks first, then its run configurations, the first
        // of which is the Run button's.)
        f.tasks = read_tasks(root, &f.problem);
        std::string run_problem;
        std::vector<BuildTask> configs = read_run_configs(root, &run_problem, &f.environments);
        if (f.problem.empty()) f.problem = std::move(run_problem);
        f.tasks.insert(f.tasks.end(), std::make_move_iterator(configs.begin()), std::make_move_iterator(configs.end()));
        std::vector<BuildTask> found = detect_builds(root, f.files, hidden);
        f.tasks.insert(f.tasks.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
        {
            std::lock_guard lock(u.mu);
            u.found = std::move(f);
        }
        wake();
    });
}

// What the worker found: and with it what init.lua's systems find. (And
// the build folder's targets, once listed: to choose from.)
void App::build_sync() {
    BuildUi& u = *buildui_;
    build_first_take();  // (a build that something waited for has ended)
    std::optional<BuildUi::Found> f;
    std::optional<BuildUi::Listed> l;
    std::optional<BuildUi::Read> r;
    {
        std::lock_guard lock(u.mu);
        f.swap(u.found);
        l.swap(u.listed);
        r.swap(u.read);
    }
    if (r) {
        u.reading = false;
        const std::string name = r->path.substr(r->path.rfind('/') + 1);
        if (!r->ok) {
            notify(!r->why.empty() ? "Cannot read " + r->path + ": " + r->why : name + " is " + r->kind, 1);
        } else {  // (a group of their own, after the runs', in place of a file read before)
            const size_t n = r->found.size();
            u.drop(u.held_read, &build_errors_, &build_warnings_);
            u.held_read.order = ++u.orders;
            u.read_from = r->path;
            u.take(u.held_read, r->found, &build_errors_, &build_warnings_);
            ++build_prob_gen_;
            ++build_gen_;
            dirty_ = true;
            notify(name + " (" + r->kind + "): " + (n ? std::to_string(n) + (n == 1 ? " problem" : " problems") : "no problems"));
            if (n) {
                if (!panel_px()) panel_toggle();
                panel_show_problems();
            }
        }
    }
    if (l) {
        u.listing = false;
        if (!l->missing.empty()) {
            notify(l->missing, 1);
        } else if (!l->ran) {
            notify("CMake did not list the targets");
        } else {
            find_choose_item("Target", l->targets, [this, targets = l->targets](int i) {
                if (i < 0 || static_cast<size_t>(i) >= targets.size()) return;
                buildui_->cmake_target = i == 0 ? std::string() : targets[static_cast<size_t>(i)];
                ++build_gen_;
                dirty_ = true;
            });
        }
    }
    if (!f) return;
    u.looking = false;
    script_build(f->root, f->files, &f->tasks);
    u.tasks = std::move(f->tasks);
    u.environments = std::move(f->environments);
    u.root = f->root;
    build_environment_apply();  // (the one chosen stays chosen while the file has it)
    u.looked = !u.again;  // (what the worker found may be from before the change)
    u.again = false;
    // The task chosen stays chosen if it is still there; else a default one.
    bool kept = false;
    for (const BuildTask& t : u.tasks) kept |= task_key(t) == u.current;
    if (!kept) {
        u.current.clear();
        for (const BuildTask& t : u.tasks)
            if (t.def) {
                u.current = task_key(t);
                break;
            }
        if (u.current.empty() && !u.tasks.empty()) u.current = task_key(u.tasks[0]);
    }
    u.sel = ~0u;  // (the chosen one, when the tab is drawn)
    if (!f->problem.empty()) notify(f->problem);
    run_config_take();
    ++build_gen_;
    dirty_ = true;
    if (u.looked)  // (what waited for the folder's tasks: each as it was asked for)
        for (std::function<void()>& go : std::exchange(u.when_ready, {})) go();
}

// Whether root's tasks and run configurations are read: a folder here is
// read now if it has not been (it is quick); one on another machine is not
// waited for (build_when_ready).
bool App::build_ready_for(const std::string& root) {
    BuildUi& u = *buildui_;
    if ((!u.looked || u.root != root) && !is_remote(root) && !(u.looking && is_remote(u.looking_at))) build_settle();
    return u.looked && u.root == root;
}

bool App::build_waiting() const { return buildui_ && !buildui_->when_ready.empty(); }

// Something to do once the folder's tasks have been read (a command given
// before they were: "Flash the board" as a folder on another machine is
// still being read).
void App::build_when_ready(std::function<void()> go) {
    buildui_->when_ready.push_back(std::move(go));
    dirty_ = true;
}

// The run configuration the Run button runs, if one is: its program,
// arguments, folder and environment are Debug's and Profile's too.
const BuildTask* App::run_config() const {
    if (!buildui_) return nullptr;
    for (const BuildTask& t : buildui_->tasks)
        if (t.config && task_key(t) == buildui_->current) return &t;
    return nullptr;
}

void App::run_config_take() {
    const BuildTask* t = run_config();
    if (!t || buildui_->taken == buildui_->current + "\n" + t->cmd) return;
    buildui_->taken = buildui_->current + "\n" + t->cmd;
    const std::string cwd = t->cwd.empty() ? buildui_->root : buildui_->root + "/" + t->cwd;
    debug_take(t->program, t->args, cwd, t->env, t->debug);
    prof_take(t->program, t->args, cwd, t->env);
}

// ---------------------------------------------------------------- a build first

void App::build_first(const std::string& what, std::function<void()> go, const BuildTask* config) {
    BuildUi& u = *buildui_;
    const std::string root = build_root();
    // (The folder's tasks, looked at now if it is here and has not been yet:
    // one there that has not been looked at yet builds nothing first.)
    if ((!u.looked || u.root != root) && !is_remote(root) && !(u.looking && is_remote(u.looking_at))) build_settle();
    if (!u.looked || u.root != root) return go();
    if (u.first && !u.first->ended && u.running && u.doc == u.first->doc) {  // (one under way: it is this that waits for it now)
        u.first->what = what;
        u.first->go = std::move(go);
        return;
    }
    const BuildTask* t = nullptr;
    if (const BuildTask* c = config ? config : run_config(); c && !c->before.empty()) {
        const auto it = std::ranges::find_if(u.tasks, [&](const BuildTask& b) { return !b.config && (b.name == c->before || b.system + ": " + b.name == c->before); });
        if (it == u.tasks.end()) return notify("No task called " + c->before + " to build before " + what);
        t = &*it;
    } else {  // (the Build tab's Build, or make's make: not a Run, nor autotools' Configure)
        for (const BuildTask& b : u.tasks)
            if (!b.config && b.def && (b.name == "Build" || (b.system == "Make" && b.name == "make"))) {
                t = &b;
                break;
            }
    }
    if (!t) return go();
    const BuildTask b = *t;
    if (!b.missing.empty()) return notify(b.missing + ": " + what + " did not start", 1);  // (its SDK is not there)
    const u64 doc = build_command(b.cwd.empty() ? u.root : u.root + "/" + b.cwd, with_setup(b, with_vars(b.system == "CMake" ? with_cmake(b.cmd, u.cmake_type, u.cmake_target) : b.cmd, build_vars())), b.name, b.problems, &b);
    if (!doc) return notify("Cannot run " + b.name + " before " + what);
    u.last_key = task_key(b);  // (how it goes shows on its row)
    if (const auto it = u.sizes.find(u.last_key); it != u.sizes.end()) u.sizes_before = it->second;
    u.first = BuildUi::First{what, b.name, std::move(go), doc, false, 0};
}

bool App::build_first_busy() const {
    const BuildUi* u = buildui_.get();
    return u && u->first && (u->first->ended || (u->running && u->doc == u->first->doc));
}

// What waited for a build that has ended: run if it went well.
void App::build_first_take() {
    BuildUi& u = *buildui_;
    if (!u.first || !u.first->ended) return;
    BuildUi::First f = std::move(*u.first);
    u.first.reset();
    if (f.status != 0) return notify(f.task + " failed: " + f.what + " did not start");
    f.go();
}

void App::build_first_settle() {
    if (!build_first_busy()) return;
    build_wait();
    build_first_take();
}

// The folder's run configurations in a tab: .blit/run.json, made the first
// time with one to fill in (the folder's name for its program, built first
// by the Build tab's Build when there is one).
void App::run_config_edit() {
    const std::string root = build_root();
    if (root.empty()) return notify("Open a folder first");
    const std::string path = root + "/" + kRunConfigs;
    if (fs_kind(path) != 1) {
        const std::string name = std::string(root.substr(root.find_last_of('/') + 1));
        const bool build = buildui_ && std::ranges::any_of(buildui_->tasks, [](const BuildTask& t) { return !t.config && t.name == "Build"; });
        const std::string text = "{\n  \"configurations\": [\n    {\n      \"name\": \"" + name + "\",\n      \"program\": \"build/" + name +
                                 "\",\n      \"args\": [],\n      \"env\": {},\n      \"cwd\": \".\"" + (build ? ",\n      \"build\": \"Build\"" : "") + "\n    }\n  ]\n}\n";
        std::string why;
        if (!fs_mkdirs(root + "/.blit", &why) || !fs_write(path, text, &why)) return notify("Cannot write " + path + ": " + why);
    }
    open_path(path.c_str());
}

void App::build_changed() {
    if (!buildui_) return;
    buildui_->looked = false;
    buildui_->again = buildui_->looking;
    dirty_ = true;
}

// (Tests and blit.build.tasks(): the folder looked at now, and waited for.)
void App::build_settle() {
    BuildUi& u = *buildui_;
    for (int round = 0; round < 2; ++round) {
        build_follow();
        if (u.worker.joinable()) u.worker.join();
        build_sync();
    }
}

std::vector<BuildTask> App::build_tasks() {
    build_settle();
    return buildui_->tasks;
}

// A command run in the build's output terminal: in the terminals' split
// below the text (appterm.cpp: shown again if it was hidden, made the first
// time), used again after (a run still going there is ended). A long
// task's has a tab of its own there, which the next run leaves alone. The
// keyboard stays where it was. Its output is read with init.lua's patterns
// that `problems` names (all of them, when it names none).
u64 App::build_command(const std::string& dir, const std::string& cmd, const std::string& title, const std::vector<std::string>& problems, const BuildTask* task) {
    BuildUi& u = *buildui_;
    const bool lasting = task && task->lasting;
    Pane* const editing = pane_;
    Pane* into = nullptr;
    const u64 near = u.term && find_doc(u.term) ? u.term : u.doc;  // (the last run's split, wherever it was moved to)
    if (!near || !find_doc(near, &into)) into = dock_show(editing);
    if (!into) into = editing;  // (no room for another split)
    const u64 old = !lasting && u.term && find_doc(u.term) ? u.term : 0;
    const BuildUi::Run* prev = u.run_of(old);
    const u64 old_order = prev ? prev->order : 0;  // (its problems' place, which this run's take once it has gone)
    if (u.first && !u.first->ended && u.first->doc == old) u.first.reset();  // (a build that something waited for, ended by this one: it is called off)
    pane_ = into;
    const CrashOptions crash = crash_options(task, cmd);
    const u64 doc = term_open(dir, cmd, title, {}, false, script_matchers(problems), &crash);
    if (!doc && into->docs.empty()) remove_pane(into);  // (a split made for it, and it could not start)
    if (old) {
        Pane* p = nullptr;
        size_t at = 0;
        if (find_doc(old, &p, &at) && p == into) tab_close(at, true);  // (and its program, if it is still going)
    }
    if (doc) {
        Pane* p = nullptr;
        size_t at = 0;
        if (find_doc(doc, &p, &at) && p == into) {
            pane_ = into;
            tab_show(at);
        }
    }
    bool editing_still = false;
    for (const std::unique_ptr<Pane>& p : panes_) editing_still |= p.get() == editing;
    focus_pane(editing_still ? editing : panes_.front().get());
    if (Pane* p = nullptr; doc && task && task->keys && find_doc(doc, &p)) focus_pane(p);  // (a program typed into: menuconfig)
    if (doc) {
        BuildUi::Run r;
        r.doc = doc;
        r.name = title;
        r.lasting = lasting;
        r.started_ns = now_ns();
        r.started_at = static_cast<i64>(std::time(nullptr));
        r.dir = dir;
        // (A task that runs a simulator is a simulation, said or not: simrun.hpp.)
        if (task) r.key = task_key(*task), r.kind = task->kind.empty() && !sim_tool(cmd).empty() ? "simulation" : task->kind, r.cmd = cmd;
        const std::string kind = r.kind;
        // (In the Problems tab, in the place of the run it replaced, if that
        // has gone: a long task's last run, or the terminal's; else last.)
        r.order = u.take_order ? u.take_order : old && !u.run_of(old) ? old_order : 0;
        if (!r.order) r.order = ++u.orders;
        u.runs.push_back(std::move(r));
        if (!lasting) u.term = doc;
        if (task) script_build_event(false, task->name, kind, cmd, 0, 0, 0, false, 0);
    }
    u.doc = doc;
    u.dir = dir;
    u.running = doc != 0;
    u.last_status = -1;
    u.last_missing.clear();
    u.last_name = title;
    u.last_key.clear();
    u.take_order = 0;
    u.summary.clear();
    u.progress = {};
    u.said.clear();
    u.unrun = false;
    u.sizes_before.clear();
    // (The problems of the run this one replaced went with its tab; a
    // file's read go now: this run's take their place.)
    u.drop(u.held_read, &build_errors_, &build_warnings_);
    u.read_from.clear();
    ++build_prob_gen_;
    ++build_gen_;
    dirty_ = true;
    return doc;
}

u64 App::build_run_on(const std::string& row, const std::string& dir, const BuildTask& task) {
    const u64 doc = build_command(dir, task.cmd, task.name, task.problems, &task);
    if (doc) buildui_->last_key = row;  // (how it goes shows on that row)
    return doc;
}

void App::build_note(const std::string& row, const std::string& words) {
    BuildUi& u = *buildui_;
    if (u.running) return notify(words);  // (the row and the button say how that run goes)
    u.last_key = row;
    u.summary = words;
    u.unrun = true;
    u.progress = {};
    u.said.clear();
    ++build_gen_;
    dirty_ = true;
}

const BuildTask* App::board_config() {
    BuildUi& u = *buildui_;
    const std::string root = build_root();
    if ((!u.looked || u.root != root) && !is_remote(root)) build_settle();
    if (const BuildTask* c = run_config(); c && c->board.set) return c;
    for (const BuildTask& t : u.tasks)
        if (t.config && t.board.set && u.root == root) return &t;
    // (None has one: an ESP-IDF or PlatformIO project's own, its Flash
    // task's, the Run button's if it is one.)
    const BuildTask* own = nullptr;
    for (const BuildTask& t : u.tasks)
        if (t.action == "flash" && t.board.set && u.root == root && (!own || task_key(t) == u.current)) own = &t;
    return own;
}

// (The Run button's first, if its board is on that port; the port as
// SerialTab::is takes it: the USB serial number, or the device a path is.)
const BuildTask* App::board_config_on(const SerialPortId& port) {
    BuildUi& u = *buildui_;
    const std::string root = build_root();
    if ((!u.looked || u.root != root) && !is_remote(root)) build_settle();
    if (u.root != root) return nullptr;
    std::string dev;
    auto on = [&](const Board& b) {
        if (!port.serial.empty() && b.probe == port.serial) return true;
        if (b.port.empty() || port.path.empty()) return false;
        if (b.port == port.path) return true;
        if (dev.empty()) dev = serial_device(port.path);
        return serial_device(b.port) == dev;
    };
    if (const BuildTask* c = run_config(); c && c->board.set && on(c->board)) return c;
    for (const BuildTask& t : u.tasks)
        if (t.config && t.board.set && on(t.board)) return &t;
    return nullptr;
}

// A task of the Build tab's (-1: the one the Run button runs).
void App::build_run(int index, const std::string& chosen) {
    BuildUi& u = *buildui_;
    if (index < 0)
        for (size_t i = 0; i < u.tasks.size(); ++i)
            if (task_key(u.tasks[i]) == u.current) index = static_cast<int>(i);
    if (index < 0 || static_cast<size_t>(index) >= u.tasks.size()) {  // nothing to run: the tab says why
        if (!panel_px()) panel_toggle();
        panel_show_build();
        return;
    }
    const BuildTask t = u.tasks[static_cast<size_t>(index)];
    u.current = task_key(t);
    // (What it needs is not there: said, on its row too, and nothing run.)
    if (!t.missing.empty()) {
        build_note(u.current, t.missing);
        return notify(t.missing, 1);
    }
    // (Blit's own: the board flashed, its serial monitor.)
    if (t.action == "flash") {
        ++build_gen_;
        dirty_ = true;
        return flash_board(FlashMode::kFlash, false, {}, &t);
    }
    if (t.action == "monitor") return firmware_monitor(t);
    // (One that asks which first: the command of the one chosen.)
    if (!t.choices.empty() && chosen.empty()) {
        std::vector<std::string> names;
        for (const auto& [name, cmd] : t.choices) names.push_back(name);
        return find_choose_item(t.name, std::move(names), [this, key = u.current, choices = t.choices](int i) {
            if (i < 0 || static_cast<size_t>(i) >= choices.size()) return;
            const std::vector<BuildTask>& tasks = buildui_->tasks;
            for (size_t k = 0; k < tasks.size(); ++k)
                if (task_key(tasks[k]) == key) return build_run(static_cast<int>(k), choices[static_cast<size_t>(i)].second);
        });
    }
    std::string cmd = !chosen.empty() ? chosen : t.system == "CMake" ? with_cmake(t.cmd, u.cmake_type, u.cmake_target) : t.cmd;
    if (!t.before.empty()) {  // a run configuration's build first: then, if it went well, the program
        const auto it = std::ranges::find_if(u.tasks, [&](const BuildTask& b) { return !b.config && (b.name == t.before || b.system + ": " + b.name == t.before); });
        if (it == u.tasks.end()) return notify("No task called " + t.before + " to run before " + t.name);
        std::string host, there;  // (on another machine the command runs there: its folders without ssh://host)
        const std::string root = split_remote(u.root, &host, &there) ? there : u.root;
        const std::string first = it->cwd.empty() ? root : root + "/" + it->cwd, then = t.cwd.empty() ? root : root + "/" + t.cwd;
        cmd = "cd " + sh_word(first) + " && " + with_setup(*it, it->cmd) + " && cd " + sh_word(then) + " && " + t.cmd;
    }
    if (t.lasting) {  // a long task run again: its last run's tab goes (one still going is ended, asked first once it has run a while)
        u64 was = 0;
        for (const BuildUi::Run& r : u.runs)
            if (r.lasting && r.key == u.current) {
                const u64 ran = now_ns() - r.started_ns;
                if (r.running && ran > kAskAfterNs && !confirm(r.name + " has been running for " + clock_text(ran) + ". End it and run it again?", "Run Again"))
                    return;
                was = r.doc;
            }
        const BuildUi::Run* last = u.run_of(was);
        const u64 order = last ? last->order : 0;  // (its problems' place in the Problems tab: the next run's)
        Pane* p = nullptr;
        size_t at = 0;
        if (was && find_doc(was, &p, &at)) {
            Pane* const keep = pane_;
            pane_ = p;
            tab_close(at, true);  // (and its program)
            bool still = false;
            for (const std::unique_ptr<Pane>& q : panes_) still |= q.get() == keep;
            pane_ = still ? keep : panes_.front().get();
        } else if (was) {
            term_stop(was, true);  // (its tab in the hidden split: ended there)
            build_forget(was);
        }
        if (!u.run_of(was)) u.take_order = order;
    }
    build_command(t.cwd.empty() || !t.before.empty() ? u.root : u.root + "/" + t.cwd, with_setup(t, with_vars(cmd, build_vars())), t.name, t.problems, &t);
    u.last_key = u.current;  // (how it goes shows on its row)
    if (const auto it = u.sizes.find(u.last_key); it != u.sizes.end()) u.sizes_before = it->second;
    run_config_take();
}

// The Run button's task, as a session keeps it; and one a session brings
// back. One for a folder not looked at yet waits there: build_sync() keeps
// it if the folder has it, and chooses as it always does if not.
std::string App::build_chosen() const { return buildui_ ? buildui_->current : std::string(); }

void App::build_choose(const std::string& key) {
    BuildUi& u = *buildui_;
    if (key.empty() || key == u.current) return;
    if (u.looked && u.root == build_root() && std::ranges::none_of(u.tasks, [&](const BuildTask& t) { return task_key(t) == key; })) return;
    u.current = key;
    u.sel = ~0u;
    run_config_take();
    ++build_gen_;
    dirty_ = true;
}

bool App::build_run_named(const std::string& name) {
    BuildUi& u = *buildui_;
    build_settle();
    if (name.empty()) {
        if (u.current.empty()) return false;
        build_run(-1);
        return true;
    }
    for (size_t i = 0; i < u.tasks.size(); ++i)
        if (u.tasks[i].name == name || u.tasks[i].system + ": " + u.tasks[i].name == name) {
            build_run(static_cast<int>(i));
            return true;
        }
    return false;
}

// Stop: the last run's program ended, its tab kept with what it wrote and
// the problems found in it (term_stop; build_ended once it has). A long
// task that has run a while is asked about first; a second Stop, for a
// program that would not end, kills it.
void App::build_stop() {
    BuildUi& u = *buildui_;
    BuildUi::Run* r = u.run_of(u.doc);
    if (!u.running || !r || !r->running) return;
    const u64 ran = now_ns() - r->started_ns;
    if (r->lasting && !r->stopped && ran > kAskAfterNs && !confirm(r->name + " has been running for " + clock_text(ran) + ". Stop it?", "Stop")) return;
    r = u.run_of(u.doc);  // (the box may have let anything happen meanwhile)
    if (!r || !r->running) return;
    const bool again = r->stopped;
    r->stopped = true;
    term_stop(u.doc, again);
    u.first.reset();  // (and what waited for it, called off)
    ++build_prob_gen_;  // (its heading in the Problems tab: stopping)
    ++build_gen_;
    dirty_ = true;
}

// The build's program has ended (term_sync tells): what it made may change
// what the folder builds with (a build folder configured), so it is looked
// at again; and what git sees there (files written, or a repository made by
// a new project's git init). On another machine, the files it wrote are
// looked for there too, and git with them (ran_there).
void App::build_ended(u64 doc, int status, const std::string& missing) {
    test_ended(doc);  // (a run of the tests: its reports read)
    BuildUi& u = *buildui_;
    // (A flash: remembered as done only if it went well.)
    if (const BuildUi::Run* r = u.run_of(doc); r && r->running) flash_ended(doc, status == 0 && !r->stopped && !(doc == u.doc && u.progress.failed));
    // How it went: for init.lua's BuildDone, and, behind another window, in
    // a notification of the system's.
    if (BuildUi::Run* r = u.run_of(doc); r && r->running) {
        r->running = false;
        r->status = status;
        r->missing = missing;
        ++build_prob_gen_;  // (its heading in the Problems tab says how it went; one that failed with no problem found has one now)
        const double took = static_cast<double>(now_ns() - r->started_ns) * 1e-9;
        if (!r->key.empty()) script_build_event(true, r->name, r->kind, r->cmd, status, r->errors, r->warnings, r->stopped, took);
        // (A simulation: the dump it wrote opened in the wave tab, or read again, as its output and command say which: appsim.cpp.)
        if (r->kind == "simulation") sim_ended(doc, r->dir, r->cmd, r->started_at);
        if (!front_ && cfg_.build_notify && !r->stopped) {
            char secs[32];
            std::snprintf(secs, sizeof secs, "%.1f s", took);
            std::string body = status == 0       ? std::string("Done in ") + secs
                               : !missing.empty() ? "Failed: " + missing
                                                  : "Failed with exit code " + std::to_string(status) + " after " + secs;
            if (r->errors) body += " \xC2\xB7 " + std::to_string(r->errors) + (r->errors == 1 ? " error" : " errors");
            if (r->warnings) body += std::string(r->errors ? ", " : " \xC2\xB7 ") + std::to_string(r->warnings) + (r->warnings == 1 ? " warning" : " warnings");
            if (const std::string none = notify_missing(); none.empty()) system_notify(r->name, body);
            else if (!std::exchange(u.said_no_notifier, true)) notify(not_installed(none, {}) + ": no system notification when a run ends (build.notify in Settings)", 1);
        }
    }
    if (u.first && u.first->doc == doc && !u.first->ended) {  // (what waits for it runs on the loop's next turn)
        u.first->ended = true;
        u.first->status = status;
        wake();
    }
    if (doc != u.doc) {
        ++build_gen_;  // (a long task's row)
        dirty_ = true;
        return;
    }
    u.running = false;
    u.last_status = status;
    // A flash stopped (or ended badly with no word of the tool's): what its
    // output came to is not what it did ("Flashed 23.2 KB" of three files,
    // the third cut short), but where it stopped.
    if (status != 0 && !u.progress.engine.empty() && !u.progress.failed && !u.progress.summary.empty()) {
        u.progress.summary = u.progress.cut.empty() ? "stopped" : "stopped in " + u.progress.cut + (u.progress.cut_percent >= 0 ? " at " + std::to_string(u.progress.cut_percent) + "%" : "");
        u.summary = u.progress.summary + (u.said.empty() ? "" : " \xC2\xB7 " + u.said);
    }
    u.last_missing = missing;
    u.looked = false;
    if (!ran_there(u.dir)) git_command("look", {});
    ++build_gen_;
    dirty_ = true;
}

// A run's tab has closed (forget_doc): it is no run of the tab's now. One
// still running was hung up on with its tab: it ends here, as Stop ends
// it, with no exit status to say (BuildDone's -1), and what waited for it
// is called off; the Run button can run again.
void App::build_forget(u64 doc) {
    if (!buildui_) return;
    BuildUi& u = *buildui_;
    if (BuildUi::Run* r = u.run_of(doc); r && r->running) {
        flash_ended(doc, false);
        r->running = false;
        r->stopped = true;
        if (!r->key.empty()) script_build_event(true, r->name, r->kind, r->cmd, -1, r->errors, r->warnings, true, static_cast<double>(now_ns() - r->started_ns) * 1e-9);
    }
    if (u.first && u.first->doc == doc && !u.first->ended) u.first.reset();
    if (doc == u.doc && u.running) {
        u.running = false;
        u.doc = 0;
        ++build_gen_;
        dirty_ = true;
    }
    if (BuildUi::Run* r = u.run_of(doc)) {  // (its problems go with it, from the Problems tab and the text)
        u.drop(*r, &build_errors_, &build_warnings_);
        std::erase_if(u.runs, [&](const BuildUi::Run& x) { return x.doc == doc; });
        ++build_prob_gen_;
        ++build_gen_;
        dirty_ = true;
    }
    if (BuildUi::Port* p = u.port_of(doc)) {  // (a serial tab's crashes, so too)
        u.drop(*p, &build_errors_, &build_warnings_);
        std::erase_if(u.ports, [&](const BuildUi::Port& x) { return x.doc == doc; });
        ++build_prob_gen_;
        ++build_gen_;
        dirty_ = true;
    }
    if (u.term == doc) u.term = 0;
}

// What a task's command may name (with_vars), from the file in front (or,
// with a terminal in front, the first split's that shows a file).
TaskVars App::build_vars() const {
    TaskVars v;
    std::string host, there;
    const std::string root = buildui_ && buildui_->looked ? buildui_->root : build_root();
    v.workdir = split_remote(root, &host, &there) ? there : root;
    const Doc* d = pane_ && pane_->doc && !pane_->doc->term ? pane_->doc : nullptr;
    for (size_t i = 0; !d && i < panes_.size(); ++i)
        if (panes_[i]->doc && !panes_[i]->doc->term) d = panes_[i]->doc;
    if (!d || d->ed.path.empty()) return v;
    const std::string full = is_remote(d->ed.path) ? d->ed.path : full_path(d->ed.path);
    v.file = split_remote(full, &host, &there) ? there : full;
    const Editor& ed = d->ed;
    const u64 at = ed.cursor();
    v.line = std::to_string(ed.buf.line_of(at) + 1);
    auto word = [&](u64 off) {
        char c = 0;
        return off < ed.buf.size() && ed.buf.read(off, &c, 1) == 1 && (std::isalnum(static_cast<unsigned char>(c)) || c == '_');
    };
    u64 lo = at, hi = at;
    while (lo > 0 && at - lo < 256 && word(lo - 1)) --lo;
    while (hi - at < 256 && word(hi)) ++hi;
    v.word = ed.buf.text(lo, hi - lo);
    lo = ed.sel_min(), hi = ed.sel_max();
    if (d == pane_->doc) vim_selection(&lo, &hi);  // (Vim's Visual mode's)
    if (hi > lo) v.selection = ed.buf.text(lo, std::min<u64>(hi - lo, 1u << 20));
    return v;
}

// Until a long task's time on show next changes, while blit is in front
// and it is on show (on the Run button, or in the Build tab): drawn again
// once a second then, and when a line comes; nothing ticks otherwise.
int App::build_wait_ms() const {
    if (!front_ || !buildui_) return -1;
    const BuildUi& u = *buildui_;
    const BuildUi::Run* r = u.ticking();
    if (!r || !((r->doc == u.doc && u.reserve) || (panel_px() > 0 && panel_tab() == 5))) return -1;  // (5: the Build tab)
    const u64 ran = now_ns() - r->started_ns;
    if (static_cast<i64>(ran / 1'000'000'000ull) != u.clock_shown) return 0;
    return static_cast<int>((1'000'000'000ull - ran % 1'000'000'000ull) / 1'000'000ull) + 1;
}

void App::build_older(int seconds) {
    for (BuildUi::Run& r : buildui_->runs)
        if (r.lasting && r.running) r.started_ns -= static_cast<u64>(seconds) * 1'000'000'000ull;
    ++build_gen_;
    dirty_ = true;
}

// ---------------------------------------------------------------- problems

// Problems a run's output reported (found on its terminal's reading
// thread): each after the others of its file in that run, the files in the
// order they came, the runs in theirs, as the Problems tab lists them and
// F8 goes through them.
void App::build_found(u64 doc, std::vector<BuildProblem> found) {
    BuildUi& u = *buildui_;
    BuildUi::Run* const run = u.run_of(doc);
    if (!run || found.empty()) return;
    u.take(*run, found, &build_errors_, &build_warnings_);
    ++build_prob_gen_;
    ++build_gen_;
    dirty_ = true;
}

// A serial tab's: its group made with its first, after those there are
// (it is no run, and takes no run's place); its heading follows its title.
void App::build_crashes(u64 doc, const std::string& title, std::vector<BuildProblem> found) {
    BuildUi& u = *buildui_;
    BuildUi::Port* port = u.port_of(doc);
    if (!port) {
        if (found.empty()) return;
        BuildUi::Port p;
        p.doc = doc;
        p.order = ++u.orders;
        u.ports.push_back(std::move(p));
        port = &u.ports.back();
    }
    if (port->title == title && found.empty()) return;
    port->title = title;
    if (!found.empty()) u.take(*port, found, &build_errors_, &build_warnings_);
    ++build_prob_gen_;
    ++build_gen_;
    dirty_ = true;
}

void App::BuildUi::take(Held& h, std::vector<BuildProblem>& come, u32* errors, u32* warnings) {
    const u32 was = at >= 0 ? problems[static_cast<size_t>(at)].seq : 0;
    const size_t had = problems.size();
    for (BuildProblem& p : come) {
        const auto file = h.files.find(p.path);
        if (p.count > 1 && file != h.files.end()) {  // (one said again, as a validation message every frame: the count on its row)
            const auto same = std::find_if(problems.rbegin(), problems.rend(), [&](const BuildProblem& q) {
                return q.group == file->second && q.severity == p.severity && q.line == p.line && q.col == p.col && q.code == p.code && q.message == p.message;
            });
            if (same != problems.rend()) {
                same->count = std::max(same->count, p.count);
                continue;
            }
        }
        if (file != h.files.end()) {
            p.group = file->second;
        } else {
            p.group = static_cast<u32>(group_order.size());
            group_order.push_back(h.order);
            h.files.emplace(p.path, p.group);
        }
        p.seq = seq++;
        (p.severity == 1 ? *errors : *warnings) += 1;
        (p.severity == 1 ? h.errors : h.warnings) += 1;
        problems.push_back(std::move(p));
    }
    // (The new ones in order, then merged in: a step through them, not a sort of them all for each piece of output.)
    const auto by_group = [this](const BuildProblem& a, const BuildProblem& b) { return before(a.group, b.group); };
    const auto mid = problems.begin() + static_cast<std::ptrdiff_t>(had);
    std::stable_sort(mid, problems.end(), by_group);
    std::inplace_merge(problems.begin(), mid, problems.end(), by_group);
    if (at >= 0)  // (F8 goes on from the one it went to)
        for (size_t i = 0; i < problems.size(); ++i)
            if (problems[i].seq == was) at = static_cast<i64>(i);
}

// A run's problems (or a file's read) taken out, and their marks from the
// text: the run's tab has closed, or it is being run again. False: it had none.
bool App::BuildUi::drop(Held& h, u32* errors, u32* warnings) {
    if (h.files.empty()) return false;
    for (const auto& [path, group] : h.files) group_order[group] = kGone;
    const u32 was = at >= 0 ? problems[static_cast<size_t>(at)].seq : 0;
    std::erase_if(problems, [&](const BuildProblem& p) { return group_order[p.group] == kGone; });
    for (auto& [id, m] : marks) std::erase_if(m->marks, [&](const Mark& k) { return group_order[k.group] == kGone; });
    if (at >= 0) {  // (F8 goes on from the one it went to, if that is still there)
        at = -1;
        for (size_t i = 0; i < problems.size(); ++i)
            if (problems[i].seq == was) at = static_cast<i64>(i);
    }
    *errors -= h.errors;
    *warnings -= h.warnings;
    h.files.clear();
    h.errors = h.warnings = 0;
    return true;
}

std::string App::BuildUi::state_of(const Run& r) {
    if (r.running) return r.stopped ? "stopping\xE2\x80\xA6" : "running\xE2\x80\xA6";
    if (r.stopped) return "stopped";
    if (r.status == 0) return "done";
    if (r.status > 0) return "failed (" + (r.missing.empty() ? "exit " + std::to_string(r.status) : r.missing.substr(0, r.missing.find(" on "))) + ")";
    return {};
}

std::string App::BuildUi::failure_of(const Run& r) {
    if (r.running || r.stopped || r.status <= 0 || r.errors) return {};  // (Stop ended it: it did not fail)
    if (!r.missing.empty()) return r.name + " failed: " + r.missing;
    return r.name + " failed with exit code " + std::to_string(r.status) + ": no " + (r.warnings ? "error" : "problem") + " found in its output";
}

// The groups, made again when the problems or a run's state have changed:
// the runs' (and the file's) in their order, each with how many problems it
// has, those with none left out unless they failed.
const std::vector<BuildGroup>& App::build_groups() {
    BuildUi& u = *buildui_;
    if (u.groups_gen == build_prob_gen_) return u.group_list;
    u.groups_gen = build_prob_gen_;
    u.group_list.clear();
    std::vector<std::pair<u64, const BuildUi::Held*>> held;
    for (const BuildUi::Run& r : u.runs) held.emplace_back(r.order, &r);
    if (!u.read_from.empty()) held.emplace_back(u.held_read.order, &u.held_read);
    std::unordered_map<const BuildUi::Held*, const std::string*> port_titles;  // (a serial tab's group: its title, and no run's state)
    for (const BuildUi::Port& p : u.ports) {
        held.emplace_back(p.order, &p);
        port_titles.emplace(&p, &p.title);
    }
    std::ranges::sort(held, {}, &std::pair<u64, const BuildUi::Held*>::first);
    size_t i = 0;
    for (const auto& [order, h] : held) {
        BuildGroup g;
        g.first = static_cast<u32>(i);
        while (i < u.problems.size() && u.group_order[u.problems[i].group] == order) ++i;
        g.count = static_cast<u32>(i) - g.first;
        if (h == &u.held_read) {
            g.title = u.read_from.substr(u.read_from.find_last_of('/') + 1);
        } else if (const auto port = port_titles.find(h); port != port_titles.end()) {
            g.title = *port->second;
        } else {
            const BuildUi::Run& r = static_cast<const BuildUi::Run&>(*h);
            g.title = r.name;
            g.state = BuildUi::state_of(r);
            g.failed = !r.running && !r.stopped && r.status > 0;
            if (!BuildUi::failure_of(r).empty())
                g.why = !r.missing.empty() ? r.missing : std::string("no ") + (r.warnings ? "error" : "problem") + " found in its output";
        }
        if (g.count || !g.why.empty()) u.group_list.push_back(std::move(g));
    }
    return u.group_list;
}

// What a task's run came to, and its tests' results, as its output gives
// them (another run's, or one that was no task's, has no row to show it on).
void App::build_outcome(u64 doc, std::vector<BuildScan::TestResult> results, const BuildScan::Outcome* outcome) {
    BuildUi& u = *buildui_;
    if (doc != u.doc || u.last_key.empty()) return;
    if (outcome) {
        if (!outcome->sizes.empty()) u.sizes[u.last_key] = outcome->sizes;
        u.said = BuildScan::summary_of(*outcome, &u.sizes_before);
        u.summary = u.progress.summary + (u.progress.summary.empty() || u.said.empty() ? "" : " \xC2\xB7 ") + u.said;
        ++build_gen_;
        dirty_ = true;
    }
    if (!results.empty()) test_output(u.last_name, u.last_key, results);
}

// How far an engine the run drives has got, as its output says (told only
// when the stage, its percent or the stages done change): the Run button
// and the task's row are drawn again; at the end, what it came to.
void App::build_progress(u64 doc, const BuildScan::Progress& p) {
    BuildUi& u = *buildui_;
    if (doc != u.doc) return;
    u.progress = p;
    u.summary = p.summary + (p.summary.empty() || u.said.empty() ? "" : " \xC2\xB7 ") + u.said;
    ++build_gen_;
    dirty_ = true;
}

const std::vector<BuildProblem>& App::build_problems() const { return buildui_->problems; }

// An edit to a file with the build's problems in it: a mark below what
// changed moves with its line (lines put in or taken out above it, a line
// opened after it), and one whose line's text was changed goes.
void App::BuildUi::Marks::on_edit(const TextEdit& e) {
    if (marks.empty() || marks.back().line < e.start_row) return;
    std::erase_if(marks, [&](Mark& m) {
        if (m.line < e.start_row || (m.line == e.start_row && e.start_col >= m.len)) return false;  // (before it, or after its text: a line opened under it)
        if (m.line > e.old_end_row || (m.line == e.old_end_row && e.old_end_col == 0 && e.new_end_col == 0)) {  // (after it, its text as it was)
            m.line = m.line - e.old_end_row + e.new_end_row;
            return false;
        }
        return true;
    });
}

// The build's problems in the text of the split with the keyboard, as the
// language server's are: a wavy underline in the colour of each kind, from
// its column over the word there (the line's text where it says no column),
// and the cursor's line's first error in words after the line's end (the
// server's, where it has one there). The marks of a file are made the first
// time it is drawn after problems come, and the underlines again only when
// the view, the text or the cursor's line changes: with no problems from
// the build this is one test of a frame. (Once every run's problems have
// gone, their files are no longer listened to.)
void App::build_marks(FrameDesc& fd) {
    BuildUi& u = *buildui_;
    if (u.problems.empty() && !u.marks.empty()) {
        for (auto& [id, m] : u.marks)
            if (Doc* d = m->listening ? find_doc(id) : nullptr) d->ed.unlisten(m.get());
        u.marks.clear();
    }
    if (u.problems.empty() || focus_view_ >= fd.view_count) return;
    Doc& d = *pane_->doc;
    if (d.term || d.ed.path.empty()) return;
    std::unique_ptr<BuildUi::Marks>& slot = u.marks[d.id];
    if (!slot) slot = std::make_unique<BuildUi::Marks>();
    BuildUi::Marks& m = *slot;
    const Buffer& b = d.ed.buf;
    if (m.gen != build_prob_gen_) {  // (problems came: those of this file not yet taken, from each run that has some there)
        m.gen = build_prob_gen_;
        const std::string full = is_remote(d.ed.path) ? d.ed.path : full_path(d.ed.path);
        const size_t had = m.marks.size();
        auto file_of = [&](const BuildUi::Held& h) {
            const auto g = h.files.find(full);
            if (g == h.files.end()) return;
            const auto by_group = [&u](const BuildProblem& p, u32 group) { return u.before(p.group, group); };
            for (auto it = std::lower_bound(u.problems.begin(), u.problems.end(), g->second, by_group); it != u.problems.end() && it->group == g->second; ++it) {
                if (it->seq < m.next_seq) continue;
                if (it->line == 0 || it->line > b.line_count()) continue;
                BuildUi::Mark k;
                k.line = it->line - 1;
                const u64 ls = b.line_start(k.line), le = d.ed.line_end(k.line);
                std::string text = b.text(ls, std::min<u64>(le - ls, 4096));
                k.len = static_cast<u32>(text.size());
                auto word = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };
                if (it->col) {
                    k.c0 = std::min<u32>(it->col - 1, k.len);
                    k.c1 = k.c0;
                    while (k.c1 < k.len && word(text[k.c1])) ++k.c1;
                    if (k.c1 == k.c0 && k.c1 < k.len) ++k.c1;
                } else {
                    while (k.c0 < k.len && (text[k.c0] == ' ' || text[k.c0] == '\t')) ++k.c0;
                    k.c1 = k.len;
                    while (k.c1 > k.c0 && (text[k.c1 - 1] == ' ' || text[k.c1 - 1] == '\t' || text[k.c1 - 1] == '\r')) --k.c1;
                }
                k.group = it->group;
                k.severity = it->severity;
                k.message = it->message.substr(0, std::min<size_t>(it->message.find('\n'), 600));
                m.marks.push_back(std::move(k));
            }
        };
        for (const BuildUi::Run& r : u.runs) file_of(r);
        for (const BuildUi::Port& p : u.ports) file_of(p);
        if (!u.read_from.empty()) file_of(u.held_read);
        if (m.marks.size() != had) std::stable_sort(m.marks.begin(), m.marks.end(), [](const BuildUi::Mark& x, const BuildUi::Mark& y) { return x.line < y.line; });
        m.next_seq = u.seq;
        if (!m.marks.empty() && !m.listening) {
            d.ed.listen(&m);
            m.listening = true;
        }
    }
    if (m.marks.empty()) return;
    GridSpec& grid = fd.views[focus_view_].grid;
    const i64 right = std::llround(pane_->scroll_x) - pad_x_ + (pane_->x1 - pane_->text_x);
    const u32 edge = right > 0 ? static_cast<u32>(right / font_->cell_w()) : 0;
    const u64 key[9] = {d.id, build_prob_gen_, d.ed.revision(), grid.top_line, grid.rows, grid.rows_key, pane_->cur_line, font_->generation(), edge};
    if (!std::equal(key, key + 9, u.deco_key)) {
        std::copy(key, key + 9, u.deco_key);
        u.decos.clear();
        u.noted = false;
        const u64 first = grid.top_line, last = std::min<u64>(b.line_count(), grid.bottom());
        const BuildUi::Mark* words = nullptr;
        for (auto it = std::ranges::lower_bound(m.marks, first, {}, &BuildUi::Mark::line); it != m.marks.end() && it->line < last; ++it) {
            const u64 ls = b.line_start(it->line);
            GridDeco deco;
            deco.line = it->line;
            deco.kind = it->severity;
            deco.col0 = d.ed.col_between(ls, ls + it->c0);
            deco.col1 = std::max(d.ed.col_between(ls, ls + it->c1), deco.col0 + 1);  // (a problem at a point still shows)
            u.decos.push_back(deco);
            if (!words && it->line == pane_->cur_line && it->severity == 1) words = &*it;
        }
        if (words && cfg_.lsp_inline) {  // (three columns after the code, cut short of the view's edge, as the server's)
            const u32 col = d.ed.col_between(b.line_start(words->line), d.ed.line_end(words->line)) + 3;
            const u32 room = edge > col + 1 ? edge - col - 1 : 0;
            if (room >= 8) {
                const std::string text = fit_text(words->message, room);
                TextBox box;
                box.resize(std::max<u32>(width_of(text), 1), 1);
                put(box, *font_, 0, 0, text, ink::kError);
                u.note_cells = std::move(box.cells);
                u.note = {words->line, col, u.note_cells.data(), static_cast<u32>(u.note_cells.size())};
                u.note_text = text;
                u.noted = true;
            }
        }
    }
    if (!u.decos.empty()) {
        u.decos_all.assign(grid.decos.begin(), grid.decos.end());
        u.decos_all.insert(u.decos_all.end(), u.decos.begin(), u.decos.end());
        grid.decos = u.decos_all;
    }
    if (u.noted && grid.notes.empty()) grid.notes = {&u.note, 1};
}

// (Tests.) The underlines the build's problems drew in the split in front
// at the next frame ("3:9-10E": line, columns from 1, kind), and the cursor
// line's words ("| 3: message"); "none".
std::string App::build_marks_dump() {
    frame();
    const BuildUi& u = *buildui_;
    const auto it = u.marks.find(pane_->doc->id);
    if (u.problems.empty() || it == u.marks.end() || it->second->marks.empty() || (u.decos.empty() && !u.noted)) return "none";
    std::string out;
    for (const GridDeco& d : u.decos)
        out += (out.empty() ? "" : " ") + std::to_string(d.line + 1) + ":" + std::to_string(d.col0 + 1) + "-" + std::to_string(d.col1) + (d.kind == 1 ? "E" : "W");
    if (u.noted) out += " | " + std::to_string(u.note.line + 1) + ": " + u.note_text;
    return out;
}

void App::build_go(size_t i) {
    BuildUi& u = *buildui_;
    if (i >= u.problems.size()) return;
    u.at = static_cast<i64>(i);
    const BuildProblem p = u.problems[i];  // (opening a file may run init.lua's handlers)
    panel_problem_seen(i);
    // (A place whose file is nowhere, as Metal's run-time "program_source", is
    // a problem all the same: counted and listed, with nowhere to go.)
    if (!is_remote(p.path) && !SDL_GetPathInfo(p.path.c_str(), nullptr)) return;
    go_to_place(p.path, p.line, p.col);
}

// F8 (Shift+F8, back): the next of the build's problems, round the ends,
// and which of how many it is on the bottom line.
bool App::build_step(int by) {
    BuildUi& u = *buildui_;
    const i64 n = static_cast<i64>(u.problems.size());
    if (!n) return false;
    const i64 at = u.at < 0 ? (by < 0 ? n - 1 : 0) : ((u.at + by) % n + n) % n;
    build_go(static_cast<size_t>(at));
    const std::string& m = u.problems[static_cast<size_t>(at)].message;
    notify("Problem " + std::to_string(at + 1) + " of " + std::to_string(n) + ": " + m.substr(0, m.find('\n')));
    return true;
}

// A file at a line and column, in a split that shows text: the one with
// the keyboard, unless that is a terminal's (the build's output), then the
// first that is not. Where the cursor was is somewhere to come back to.
bool App::go_to_place(const std::string& path, u32 line, u32 col) {
    if (!is_remote(path) && !SDL_GetPathInfo(path.c_str(), nullptr)) {  // (opening it would make a new, empty one)
        notify("No file " + path, 1);
        return false;
    }
    if (pane_->doc->term)
        for (const std::unique_ptr<Pane>& p : panes_)
            if (p->doc && !p->doc->term) {
                focus_pane(p.get());
                break;
            }
    remember_place();
    if (!open_path(path.c_str(), true)) return false;  // (another machine's: its tab goes if it is not there)
    show_place(line ? line - 1 : 0, col ? col - 1 : 0);
    return true;
}

// ---------------------------------------------------------------- the Build tab

void App::build_panel_draw(TextBox& box, bool focused) {
    BuildUi& u = *buildui_;
    Font& font = *font_;
    const u32 cols = box.cols, rows = box.rows;
    // The folder: the one looked at, or being looked at, or (before the
    // first look, made after the first frame) the one that will be.
    std::string where = u.looked ? u.root : u.looking ? u.looking_at : build_root();
    if (const char* home = SDL_getenv("HOME"); home && *home && where.starts_with(home)) where = "~" + where.substr(std::strlen(home));
    const bool looking = !u.looked || u.looking;
#if BLIT_DEV
    u.drawn_rows.clear();
    u.drawn_shown.clear();
#endif
    u.rows.clear();
    u.body = 0;
    size_t heading = 0;
    for (size_t i = 0; i < u.tasks.size(); ++i) {
        const BuildTask& t = u.tasks[i];
        if (i == 0 || u.tasks[i - 1].system != t.system) {
            heading = u.rows.size();
            u.rows.push_back({-1, t.system, 0});
            if (t.system == "CMake") {  // (its build type and target, chosen here)
                u.rows.push_back({kTypeRow, "Build type", 0});
                u.rows.push_back({kTargetRow, "Target", 0});
            }
            if (t.config && !u.environments.list.empty()) u.rows.push_back({kEnvRow, "Environment", 0});  // (the run configurations', chosen here)
        }
        ++u.rows[heading].count;
        u.rows.push_back({static_cast<int>(i), t.name, 0});
    }
    if (u.tasks.empty()) {
        if (looking) {
            put(box, font, 1, 0, "Looking at " + fit_text(where, cols > 12 ? cols - 12 : 1, true) + "\xE2\x80\xA6", ink::kMuted);
            return;
        }
        // Nothing found: where, and how to add what was not looked for.
        u32 r = 1;
        for (const std::string& line : wrapped("No build files in " + where + ".", cols))
            if (r < rows) put(box, font, r++, 0, line, ink::kMuted);
        ++r;
        for (const std::string& line : wrapped("A build system can be added in init.lua: blit.build.add(\xE2\x80\xA6)", cols))
            if (r < rows) put(box, font, r++, 0, line, ink::kMuted);
        static constexpr Hint kAgain[] = {{"Cmd+R", "look again"}};
        if (rows > r + 1 && hints_width(kAgain, font) <= cols) put_hints(box, font, rows - 1, 0, kAgain);
        return;
    }
    put(box, font, 1, 0, "In " + fit_text(where, cols > 3 ? cols - 3 : 1, true), ink::kMuted);
    // At the bottom, the keys: on one line where they fit, else on two (the
    // second said more briefly where the panel is narrow).
    Hint keys[2] = {{"Enter", "run"}, {"space", "make it the Run button's task"}};
    std::string brief;
    u32 foot = 1;
    if (hints_width(keys, font) > cols) {
        foot = 2;
        if (hints_width({keys + 1, 1}, font) > cols) {
            keys[1].second = "choose for the Run button";
            if (hints_width({keys + 1, 1}, font) > cols) keys[1].second = brief = fit_text(keys[1].second, cols > 6 ? cols - 6 : 1);
        }
    }
    // The chosen row: a task (the Run button's, after the folder was looked at).
    if (u.sel >= u.rows.size() || !choosable(u.rows[u.sel].task)) {
        u.sel = 0;
        for (u32 i = 0; i < u.rows.size(); ++i)
            if (u.rows[i].task >= 0 && task_key(u.tasks[static_cast<size_t>(u.rows[i].task)]) == u.current) u.sel = i;
        while (!choosable(u.rows[u.sel].task) && u.sel + 1 < u.rows.size()) ++u.sel;
    }
    u.body = rows > 3 + foot ? rows - 3 - foot : 0;  // (between the folder's line and a blank one above the keys)
    if (u.sel < u.top) u.top = u.sel;
    if (u.body && u.sel >= u.top + u.body) u.top = u.sel - u.body + 1;
    for (u32 r = 0; r < u.body && u.top + r < u.rows.size(); ++r) {
        const BuildUi::Row& row = u.rows[u.top + r];
        const u32 y = 2 + r;
        if (row.task == kTypeRow || row.task == kTargetRow || row.task == kEnvRow) {  // CMake's build type and target, the environment: what is chosen, or the project's own
            const std::string& v = row.task == kTypeRow ? u.cmake_type : row.task == kTargetRow ? u.cmake_target : u.environment;
            const u32 c = put(box, font, y, 2, row.text, ink::kText);
            if (c + 4 < cols)
                put(box, font, y, c + 2, fit_text(v.empty() ? (row.task == kTypeRow ? "the project's" : row.task == kEnvRow ? "none" : "all") : v, cols - c - 2), v.empty() ? ink::kMuted : ink::kAccent);
            if (u.top + r == u.sel) light_row(box, y, focused);
            continue;
        }
        if (row.task < 0) {
            const std::string count = std::to_string(row.count);
            const u32 c = put(box, font, y, 0, fit_text(row.text, cols > count.size() + 1 ? cols - static_cast<u32>(count.size()) - 1 : 1), ink::kHeading);
            put(box, font, y, c + 1, count, ink::kMuted);
            continue;
        }
        // The Run button's task marked, its name, and then how its last run
        // went (in words, not only colour), else its command.
        const BuildTask& t = u.tasks[static_cast<size_t>(row.task)];
        const bool cur = task_key(t) == u.current;
        if (cur) put(box, font, y, 0, "\xE2\x96\xB6", ink::kAccent);
        const u32 c = put(box, font, y, 2, fit_text(t.name, cols > 2 ? cols - 2 : 1), cur ? ink::kStrong : ink::kText);
        std::string after = t.system == "CMake" ? with_cmake(t.cmd, u.cmake_type, u.cmake_target) : t.cmd;
        Ink in = ink::kMuted;
        if (!t.missing.empty()) after = t.missing, in = ink::kError;  // (what it needs is not there: in place of its command)
        // (A long task's run whose tab is open says how it goes here too
        // while another run is the last: still running, with its time.)
        const std::string key = task_key(t);
        const BuildUi::Run* run = !u.last_key.empty() && key == u.last_key ? u.run_of(u.doc) : nullptr;
        for (size_t k = 0; !run && t.lasting && k < u.runs.size(); ++k)
            if (u.runs[k].lasting && u.runs[k].key == key) run = &u.runs[k];
        const bool last = !u.last_key.empty() && key == u.last_key && (!run || run->doc == u.doc);
        bool staged = false;  // (an engine's stage under way, which its words then end with)
        std::string ticking;  // (and a long task's time after them)
        if (last && u.unrun) {
            after = u.summary;  // (nothing ran, and why: "Not flashed: unchanged since the last flash")
        } else if (last || run) {
            const bool running = run ? run->running : u.running;
            const int status = run ? run->status : u.last_status;
            const std::string& missing = run ? run->missing : u.last_missing;
            bool said = true;
            if (running) {
                after = run && run->stopped ? "stopping\xE2\x80\xA6" : "running\xE2\x80\xA6";
                if (last && !(run && run->stopped) && (!u.progress.stage.empty() || !u.progress.done.empty())) {
                    after = BuildScan::progress_words(u.progress, true);  // ("Erase \xE2\x9C\x93 \xC2\xB7 Program 62% 25.4/41.2 KB")
                    staged = !u.progress.stage.empty();
                }
            } else if (run && run->stopped) {
                after = "stopped";
            } else if (status == 0) {
                after = "done";
            } else if (status > 0) {
                after = missing.empty() ? "failed: exited with code " + std::to_string(status) : missing.substr(0, missing.find(" on ")), in = ink::kError;  // (its machine is the folder's, named above: room for the program)
            } else {
                said = false;
            }
            if (said && running && run && run->lasting) {
                ticking = " " + clock_text(now_ns() - run->started_ns);
                after += ticking;
            }
            if (said && last && !u.summary.empty())  // (what it came to: "12 passed, 1 failed"; a flash stopped, where: "stopped in Program at 38%", said once)
                after = after == "stopped" && u.summary.starts_with("stopped") ? u.summary : after + " \xC2\xB7 " + u.summary;
        }
        const std::string kind = t.kind.empty() ? std::string() : t.kind + " \xC2\xB7 ";  // (the kind of work it is)
        after = kind + after;
        // As much as there is room for. While an engine's stage is under way
        // it stays, with its percent, whatever the room: the stages done are
        // said more briefly first ("\xE2\x9C\x93 2"), then left out, then the kind
        // of work, then the stage's sizes; only what the run has come to so
        // far, after them, is cut.
        const u32 room = c + 4 < cols ? cols - c - 2 : 0;
        std::string shown = after;
        if (staged && width_of(after) > room) {
            BuildScan::Progress p = u.progress;
            p.done.clear();
            const std::string now = BuildScan::progress_words(p, false) + ticking;
            p.bytes = p.total = 0;
            const std::string bare = BuildScan::progress_words(p, false) + ticking;
            const std::string done = u.progress.done.empty() ? std::string() : "\xE2\x9C\x93 " + std::to_string(u.progress.done.size()) + " \xC2\xB7 ";
            const std::string tail = u.summary.empty() ? std::string() : " \xC2\xB7 " + u.summary;
            shown = bare;
            for (const std::string& s : {kind + BuildScan::progress_words(u.progress, true) + ticking, kind + done + now, kind + now, now, bare})
                if (width_of(s) <= room) {
                    shown = s + tail;
                    break;
                }
        }
        shown = fit_text(shown, room);
        if (room) put(box, font, y, c + 2, shown, in);
#if BLIT_DEV
        u.drawn_rows.push_back(t.name + ": " + after);
        u.drawn_shown.push_back(t.name + ": " + (room ? shown : std::string()));
#endif
        if (u.top + r == u.sel) light_row(box, y, focused);
    }
    if (foot == 1) {
        put_hints(box, font, rows - 1, 0, keys);
    } else if (rows > 3) {
        put_hints(box, font, rows - 2, 0, {keys, 1});
        put_hints(box, font, rows - 1, 0, {keys + 1, 1});
    }
}

void App::build_panel_click(int row, int col, const SDL_MouseButtonEvent& b) {
    (void)col;
    BuildUi& u = *buildui_;
    if (b.button != SDL_BUTTON_LEFT || row < 2 || static_cast<u32>(row - 2) >= u.body) return;
    const u32 i = u.top + static_cast<u32>(row - 2);
    if (i >= u.rows.size() || !choosable(u.rows[i].task)) return;
    u.sel = i;
    if (u.rows[i].task < 0) {  // CMake's build type or target, the environment: changed
        build_option(u.rows[i].task);
        return;
    }
    u.current = task_key(u.tasks[static_cast<size_t>(u.rows[i].task)]);
    run_config_take();  // (a run configuration's program is Debug's and Profile's too)
    if (b.clicks >= 2) build_run(u.rows[i].task);
    ++build_gen_;
    dirty_ = true;
}

bool App::build_panel_key(const SDL_KeyboardEvent& k) {
    BuildUi& u = *buildui_;
    const bool cmd = k.mod & kPrimaryMod;
    if (cmd && k.key == SDLK_R) {  // the folder looked at again
        build_changed();
        ++build_gen_;
        return true;
    }
    if (cmd || u.rows.empty()) return false;
    const u32 n = static_cast<u32>(u.rows.size());
    auto step = [&](int by) {
        u32 at = u.sel < n ? u.sel : 0;
        for (u32 tries = 0; tries < n; ++tries) {
            at = static_cast<u32>((static_cast<int>(at) + by + static_cast<int>(n)) % static_cast<int>(n));
            if (choosable(u.rows[at].task)) break;
        }
        u.sel = at;
    };
    switch (k.key) {
        case SDLK_UP: step(-1); break;
        case SDLK_DOWN: step(1); break;
        case SDLK_HOME:  // the first task, or the last
        case SDLK_END:
            u.sel = k.key == SDLK_HOME ? n - 1 : 0;
            step(k.key == SDLK_HOME ? 1 : -1);
            break;
        case SDLK_SPACE:
            if (u.sel < n && u.rows[u.sel].task >= 0) {
                u.current = task_key(u.tasks[static_cast<size_t>(u.rows[u.sel].task)]);
                run_config_take();
            }
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (u.sel < n && u.rows[u.sel].task >= 0) build_run(u.rows[u.sel].task);
            else if (u.sel < n && choosable(u.rows[u.sel].task)) build_option(u.rows[u.sel].task);
            break;
        default: return false;
    }
    ++build_gen_;
    dirty_ = true;
    return true;
}

// The environment chosen laid over each run configuration's own variables
// (none, where the file no longer has it): what Run runs, and Debug,
// Profile and the tests take, from now.
void App::build_environment_apply() {
    BuildUi& u = *buildui_;
    const RunEnvironment* e = nullptr;
    for (const RunEnvironment& r : u.environments.list)
        if (r.name == u.environment) e = &r;
    if (!e) u.environment.clear();
    for (BuildTask& t : u.tasks)
        if (t.config) run_in_environment(t, e, u.root.empty() ? build_root() : u.root);
    run_config_take();
}

// The environment chosen, if any.
const RunEnvironment* App::run_environment() const {
    if (!buildui_) return nullptr;
    for (const RunEnvironment& r : buildui_->environments.list)
        if (r.name == buildui_->environment) return &r;
    return nullptr;
}

// The process the run of the Run button's configuration runs in, while it
// runs here (its terminal's shell, whose program it is): 0 when none.
i64 App::run_config_pid() {
    const BuildTask* c = run_config();
    if (!c) return 0;
    for (const BuildUi::Run& r : buildui_->runs)
        if (r.running && r.key == task_key(*c)) return term_pid(r.doc);
    return 0;
}

// CMake's build type, the next in turn; or its target, chosen from those
// the build folder has (CMake's own "help" target lists them, for make and
// Ninja alike): the folder of the CMake task the Run button runs, or else
// the first that builds.
void App::build_option(int row) {
    BuildUi& u = *buildui_;
    if (row == kEnvRow) {  // (none, then each in the file's order)
        size_t at = 0;
        for (size_t k = 0; k < u.environments.list.size(); ++k)
            if (u.environments.list[k].name == u.environment) at = k + 1;
        u.environment = at < u.environments.list.size() ? u.environments.list[at].name : std::string();
        build_environment_apply();
        ++build_gen_;
        dirty_ = true;
        return;
    }
    if (row == kTypeRow) {
        static const char* const kTypes[] = {"", "Debug", "Release", "RelWithDebInfo", "MinSizeRel"};
        size_t at = 0;
        for (size_t k = 0; k < std::size(kTypes); ++k)
            if (u.cmake_type == kTypes[k]) at = k;
        u.cmake_type = kTypes[(at + 1) % std::size(kTypes)];
        ++build_gen_;
        dirty_ = true;
        return;
    }
    std::string build;
    for (const BuildTask& t : u.tasks)
        if (t.system == "CMake" && t.cmd.find("cmake --build ") != std::string::npos && t.cmd.find("--target") == std::string::npos && (build.empty() || task_key(t) == u.current))
            build = t.cmd.substr(t.cmd.rfind("cmake --build "));
    if (build.empty()) return notify("Configure the project first: its targets are the build folder's");
    if (u.listing) return;  // (the picker opens when they are listed)
    // ("cmake --build DIR --parallel" or "cmake --build --preset P": its help.)
    if (const size_t par = build.find(" --parallel"); par != std::string::npos) build.resize(par);
    // One command for both (one round trip on another machine): CMake's
    // help, then, after a form feed, Ninja's own list of what it makes when
    // the help is Ninja's (make would take -t for touch).
    const std::string sh = "command -v cmake >/dev/null 2>&1 || { echo blit-no-cmake; exit 1; }; o=$(" + build +
                           " --target help); s=$?; printf '%s\\n\\f\\n' \"$o\"; case \"$o\" in *': phony'*) " + build + " -- -t targets all;; esac; exit $s";
    u.listing = true;
    std::thread([wp = std::weak_ptr<BuildUi>(buildui_), root = u.root, sh] {
        std::string out;
        const bool ok = fs_run(root, sh, &out, 60000);
        BuildUi::Listed l;
        bool said = false;
        l.targets = cmake_targets(out, &said);
        l.ran = ok || said;
        if (!ok && out.starts_with("blit-no-cmake")) {  // (the shell that would run it found none)
            std::string host, there;
            split_remote(root, &host, &there);
            l.missing = not_installed("cmake", host);
        }
        const std::shared_ptr<BuildUi> sp = wp.lock();
        if (!sp) return;
        {
            std::lock_guard lock(sp->mu);
            sp->listed = std::move(l);
        }
        wake();
    }).detach();
}

// A run that failed, with no error in what it printed: said so, wherever
// its problems are (the Run button, the Problems tab, F8), so that a red
// button never stands over "No problems".
int App::build_failed() const {
    const BuildUi::Run* r = buildui_->run_of(buildui_->doc);
    return r && !BuildUi::failure_of(*r).empty() ? r->status : 0;
}

std::string App::build_failure() const {
    const BuildUi::Run* first = nullptr;
    for (const BuildUi::Run& r : buildui_->runs)
        if (!BuildUi::failure_of(r).empty() && (!first || r.order < first->order)) first = &r;
    return first ? BuildUi::failure_of(*first) : std::string();
}

// ---------------------------------------------------------------- problems from a file

// The file in front to begin with (a SARIF file open in a tab is one
// Enter away), else the project's folder.
void App::build_read_choose() {
    const Doc& d = *pane_->doc;
    std::string start = !d.term && !d.ed.path.empty() ? (is_remote(d.ed.path) ? d.ed.path : full_path(d.ed.path)) : build_root() + "/";
    find_choose(kFindOpen, "Read the problems from", start, [this](const std::string& path) {
        if (!path.empty()) build_read(path);
    });
}

void App::build_read(const std::string& given) {
    BuildUi& u = *buildui_;
    const std::string path = is_remote(given) ? given : full_path(given);
    u.reading = true;
    std::thread([wp = std::weak_ptr<BuildUi>(buildui_), path] {
        BuildUi::Read r;
        r.path = path;
        std::string text;
        if (!fs_read_whole(path, &text, &r.why, 256ull << 20, std::stop_token())) {
            if (r.why.empty()) r.why = "it is not there";
        } else {
            std::function<bool(const std::string&)> exists;
            if (!is_remote(path))  // (not looked for on another machine: a round trip each)
                exists = [](const std::string& p) {
                    struct stat st;
                    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
                };
            r.ok = read_problems(text, path, exists, &r.found, &r.kind);
        }
        const std::shared_ptr<BuildUi> sp = wp.lock();
        if (!sp) return;
        {
            std::lock_guard lock(sp->mu);
            sp->read = std::move(r);
        }
        wake();
    }).detach();
}

// ---------------------------------------------------------------- the Run button

// Its words: the task (or Stop, while it runs), and how many errors its
// output has reported, else that it failed, else warnings
// ("\xE2\x96\xB6 Build (debug) \xC2\xB7 3 errors", "\xC2\xB7 failed (exit 2)").
std::string App::build_label() const {
    const BuildUi& u = *buildui_;
    std::string name = u.current.substr(u.current.find('\t') + 1);
    if (width_of(name) > 24) {
        size_t at = 0;
        for (u32 k = 0; k < 23 && at < name.size(); ++k) at += decode_utf8(reinterpret_cast<const u8*>(name.data()) + at, name.size() - at).len;
        name = name.substr(0, at) + "\xE2\x80\xA6";
    }
    std::string label = u.running ? "\xE2\x96\xA0 Stop" : "\xE2\x96\xB6 " + name;
    const bool staged = u.running && !u.progress.stage.empty();
    if (staged) label = "\xE2\x96\xA0 " + BuildScan::progress_words(u.progress, false);  // (an engine's stage: "Program 62% 25.4/41.2 KB")
    if (const BuildUi::Run* r = u.running ? u.run_of(u.doc) : nullptr; r && r->lasting)  // (a long task: which, unless its stage says, and its time)
        label += (staged ? std::string() : " " + fit_text(r->name, 24)) + " " + clock_text(now_ns() - r->started_ns);
    // (Its run's problems: the other runs' are theirs, in the Problems tab and the status bar.)
    const BuildUi::Run* const last = u.run_of(u.doc);
    const u32 errors = last ? last->errors : 0, warnings = last ? last->warnings : 0;
    const int failed = build_failed();
    if (errors) label += " \xC2\xB7 " + std::to_string(errors) + (errors == 1 ? " error" : " errors");
    else if (failed) label += " \xC2\xB7 failed (" + (u.last_missing.empty() ? "exit " + std::to_string(failed) : u.last_missing.substr(0, u.last_missing.find(" on "))) + ")";
    if (!errors && warnings) label += (failed ? ", " : " \xC2\xB7 ") + std::to_string(warnings) + (warnings == 1 ? " warning" : " warnings");
    // What the last run came to, where it leaves the tabs there room (a third of the window at most);
    // where it does not, without its time ("Flashed 170.5 KB, verified"), else its words before it.
    if (!u.running && !u.summary.empty() && font_ && font_->cell_w() > 0) {
        const u32 room = static_cast<u32>(px_w_ / font_->cell_w() / 3);
        const size_t time = u.summary.find(" in ");
        const size_t after = time == std::string::npos ? time : u.summary.find(',', time);
        for (const std::string& s : {u.summary, time == std::string::npos ? std::string() : u.summary.substr(0, time) + (after == std::string::npos ? "" : u.summary.substr(after)),
                                     time == std::string::npos ? std::string() : u.summary.substr(0, time)})
            if (!s.empty() && width_of(label) + 3 + width_of(s) <= room) {
                label += " \xC2\xB7 " + s;
                break;
            }
    }
    return label;
}

// At the window's top right, at the end of the tab bar there: the task
// chosen, to run it (or, while it runs, to stop it). Made before the
// splits are laid out, so the tab bar there knows to end before it.
void App::build_measure() {
    BuildUi& u = *buildui_;
    // A long task's time: what shows it is made again when its seconds have
    // changed (a frame comes when a line does, or from build_wait_ms).
    if (const BuildUi::Run* r = u.ticking()) {
        if (const i64 s = static_cast<i64>((now_ns() - r->started_ns) / 1'000'000'000ull); s != u.clock_shown) u.clock_shown = s, ++build_gen_;
    } else {
        u.clock_shown = -1;
    }
    u.reserve = 0;
    if (u.current.empty() || !font_ || !tabs_px() || start_shown()) return;
    Font& font = *font_;
    const int cw = font.cell_w();
    // (Its cells made again only when what they show has changed.)
    const u64 key = build_gen_ * 1000003ull ^ font.generation() * 31 ^ 1 ^ (u.summary.empty() ? 0 : static_cast<u64>(px_w_) << 40);  // (the window's width: whether the summary has room)
    if (key != u.drawn) {
        u.drawn = key;
        const std::string label = build_label();
        const u32 cols = width_of(label) + 2;
        u.box.resize(cols, 1);
        const BuildUi::Run* last = u.run_of(u.doc);  // (one Stop ended did not fail)
        u.box.put(font, 0, 1, label, u.running || (last && ((last->status > 0 && !last->stopped) || last->errors)) ? kInkDiagnostic : kInkAccent, Font::kBold);
        for (u32 c = 0; c < cols; ++c) u.box.cells[c] |= kCellHover;
    }
    u.reserve = static_cast<int>(u.box.cols) * cw + cw;
}

int App::build_reserve(const Pane& p) const {
    if (!buildui_ || !buildui_->reserve || p.y0 != 0 || p.x1 < px_w_ - 1) return 0;
    return buildui_->reserve;
}

void App::build_layout(int w, FrameDesc& fd) {
    BuildUi& u = *buildui_;
    u.bx0 = u.bx1 = 0;
    if (!u.reserve || fd.popup_count >= std::size(fd.popups)) return;
    Font& font = *font_;
    const int cw = font.cell_w(), ch = font.cell_h(), bar = tabs_px();
    const int bw = u.reserve - cw / 2;
    u.bx1 = w - cw / 2;
    u.bx0 = u.bx1 - bw;
    u.by0 = std::max(0, (bar - ch) / 2 - ch / 8);
    u.by1 = std::min(bar, u.by0 + ch + ch / 4);
    Popup& q = fd.popups[fd.popup_count++];
    q = Popup{};
    q.x = u.bx0, q.y = u.by0, q.w = u.bx1 - u.bx0, q.h = u.by1 - u.by0;
    q.pad_x = (bw - static_cast<int>(u.box.cols) * cw) / 2, q.pad_y = (q.h - ch) / 2;
    q.cols = u.box.cols, q.rows = 1;
    q.cells = u.box.cells.data();
    q.border_px = 0;
    q.bg = theme_.bg;
    q.pill = std::max(2.0f, look().pill);
}

bool App::build_mouse_down(const SDL_MouseButtonEvent& b) {
    BuildUi& u = *buildui_;
    const int px = static_cast<int>(b.x * density_), py = static_cast<int>(b.y * density_);
    if (u.bx1 <= u.bx0 || px < u.bx0 || px >= u.bx1 || py < u.by0 || py >= u.by1 || b.button != SDL_BUTTON_LEFT) return false;
    if (u.running) build_stop();
    else build_run(-1);
    return true;
}

// ---------------------------------------------------------------- for the tests

std::string App::build_dump(const std::string& what) {
    BuildUi& u = *buildui_;
#if BLIT_DEV
    if (what == "rows" || what == "shown") {  // the Build tab's task rows as drawn at the next frame (shown: as much as fitted), " | " between them
        build_settle();
        ++build_gen_;
        frame();
        std::string out;
        for (const std::string& r : what == "rows" ? u.drawn_rows : u.drawn_shown) out += (out.empty() ? "" : " | ") + r;
        return out.empty() ? "no rows" : out;
    }
#endif
    if (!what.empty()) {  // a run's output: the last run's ("text"), or a long task's by its name; its lines after "|"
        term_sync();
        u64 id = what == "text" ? u.doc : 0;
        for (const BuildUi::Run& r : u.runs)
            if (r.lasting && r.name == what) id = r.doc;
        const Doc* d = find_doc(id);
        TermLines lines;
        if (!d || !d->term || !term_lines(*d, &lines)) return "no output";
        std::string out;
        each_line(lines.text, [&](std::string_view line) {
            while (!line.empty() && line.back() == ' ') line.remove_suffix(1);
            if (!line.empty()) out += "|" + std::string(line);
        });
        return out;
    }
    build_settle();
    std::string out, system;
    for (const BuildTask& t : u.tasks) {
        if (t.system != system) out += (out.empty() ? "" : " | ") + (system = t.system) + ":";
        out += " " + std::string(task_key(t) == u.current ? ">" : "") + t.name;
    }
    if (out.empty()) out = "nothing";
    if (u.running) out += " (running)";
    if (u.last_status >= 0) out += " (last: " + std::to_string(u.last_status) + ")";
    if (const BuildUi::Run* r = u.run_of(u.doc); r && r->stopped) out += " (stopped)";
    if (!u.summary.empty()) out += " (" + u.summary + ")";
    for (const BuildUi::Run& r : u.runs)  // (the long tasks' runs whose tabs are open)
        if (r.lasting)
            out += " [" + r.name + ": " + (r.running ? "running " + clock_text(now_ns() - r.started_ns) : r.stopped ? std::string("stopped") : "ended " + std::to_string(r.status)) + "]";
    return out;
}

// The Run button's words, and the last run's task row's (as drawn), for the tests.
std::string App::build_progress_dump() {
    term_sync();
    const BuildUi& u = *buildui_;
    const BuildUi::Run* run = u.run_of(u.doc);
    std::string row = u.running                 ? "running\xE2\x80\xA6"
                      : run && run->stopped     ? "stopped"
                      : u.last_status == 0      ? "done"
                      : !u.last_missing.empty() ? u.last_missing.substr(0, u.last_missing.find(" on "))
                      : u.last_status > 0       ? "failed: exited with code " + std::to_string(u.last_status)
                                                : "";
    if (u.running && (!u.progress.stage.empty() || !u.progress.done.empty())) row = BuildScan::progress_words(u.progress, true);
    if (!row.empty() && !u.summary.empty()) row = row == "stopped" && u.summary.starts_with("stopped") ? u.summary : row + " \xC2\xB7 " + u.summary;  // (as the Build tab's row has it)
    if (u.unrun) row = u.summary;
    return "button: " + build_label() + " | row: " + row;
}

void App::build_wait() {  // the build's program has ended, and the targets being listed, or a file being read for problems, are (60 s at most)
    BuildUi& u = *buildui_;
    const u64 until = now_ns() + 60'000'000'000ull;
    while (u.running && now_ns() < until) {
        term_sync();
        SDL_Delay(5);
    }
    term_sync();
    build_first_take();
    while ((u.listing || u.reading) && now_ns() < until) {
        SDL_Delay(5);
        build_sync();
    }
}

}  // namespace blit
