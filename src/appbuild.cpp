// Building: the side panel's Build tab lists what the working folder can
// be built with (CMake, make, Cargo, npm ... found by build.cpp, and what
// init.lua adds with blit.build.add), and the Run button at the window's
// top right runs the task chosen in an output terminal below the text
// (Cmd+Shift+B). The folder is looked at on a thread of its own: on
// another machine each file read is a round trip.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include "app.hpp"
#include "look.hpp"
#include "remote.hpp"
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

}  // namespace

struct App::BuildUi {
    std::vector<BuildTask> tasks;
    std::string root;          // the folder they were found in
    bool looked = false;       // tasks are root's (else: look, when the window is free to)
    std::string current;       // the task the Run button runs (task_key)
    std::string taken;         // the run configuration Debug and Profile last took (and its command)
    // The worker looking at a folder, and what it found.
    struct Found {
        std::string root;
        std::vector<FsEntry> files;
        std::vector<BuildTask> tasks;
        std::string problem;  // what is wrong with .blit/run.json, if anything
    };
    std::mutex mu;
    std::optional<Found> found;  // (mu)
    bool looking = false;
    std::string looking_at;
    // The output terminal of the last run.
    u64 doc = 0;
    bool running = false;
    int last_status = -1;
    std::string last_name;
    std::string last_key;  // the task it ran (task_key), when it was one of the tab's
    // What its output said was wrong: a file's problems together, the files
    // in the order they came; how many of each kind; the one F8 last went to.
    std::vector<BuildProblem> problems;
    std::unordered_map<std::string, u32> groups;  // a file's place in that order
    u32 seq = 0;
    i64 at = -1;
    // CMake's build type and target, chosen in the Build tab (empty: the
    // project's own, and all), put into its tasks as they run.
    std::string cmake_type, cmake_target;
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
    // Last, so that it is joined first, while all it uses is still there.
    std::jthread worker;
};

void App::build_create() { buildui_ = std::make_shared<BuildUi>(); }

namespace {

constexpr int kTypeRow = -2, kTargetRow = -3;  // (the Build tab's rows for CMake's build type and target)
bool choosable(int task) { return task >= 0 || task == kTypeRow || task == kTargetRow; }

// A CMake task's command with the build type and target chosen: a
// configure given -DCMAKE_BUILD_TYPE (and, where a build is all it does, a
// configure first, so that the type is the folder's), a build given
// --target. A preset's says its own.
std::string with_cmake(std::string cmd, const std::string& type, const std::string& target) {
    if (cmd.find("--preset") != std::string::npos) return cmd;
    auto word_end = [&](size_t from) {
        size_t e = cmd.find(' ', from);
        return e == std::string::npos ? cmd.size() : e;
    };
    if (!type.empty()) {
        const size_t conf = cmd.find("cmake -S . -B ");
        if (conf != std::string::npos) {
            cmd.insert(word_end(conf + 14), " -DCMAKE_BUILD_TYPE=" + type);
        } else if (const size_t build = cmd.find("cmake --build "); build != std::string::npos && cmd.find("--target clean") == std::string::npos) {
            const std::string dir = cmd.substr(build + 14, word_end(build + 14) - build - 14);
            cmd = "cmake -S . -B " + dir + " -DCMAKE_BUILD_TYPE=" + type + " && " + cmd;
        }
    }
    if (!target.empty() && cmd.find("--target") == std::string::npos)
        if (const size_t build = cmd.find("cmake --build "); build != std::string::npos) cmd.insert(word_end(build + 14), " --target " + sh_word(target));
    return cmd;
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
        f.tasks = read_run_configs(root, &f.problem);  // (the folder's own run configurations first: the first is the Run button's)
        std::vector<BuildTask> found = detect_builds(root, f.files, hidden);
        f.tasks.insert(f.tasks.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
        {
            std::lock_guard lock(u.mu);
            u.found = std::move(f);
        }
        wake();
    });
}

// What the worker found: and with it what init.lua's systems find.
void App::build_sync() {
    BuildUi& u = *buildui_;
    std::optional<BuildUi::Found> f;
    {
        std::lock_guard lock(u.mu);
        f.swap(u.found);
    }
    if (!f) return;
    u.looking = false;
    script_build(f->root, f->files, &f->tasks);
    u.tasks = std::move(f->tasks);
    u.root = f->root;
    u.looked = true;
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
    debug_take(t->program, t->args, cwd, t->env);
    prof_take(t->program, t->args, cwd, t->env);
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
// time), used again after (a run still going there is ended). The keyboard
// stays where it was.
u64 App::build_command(const std::string& dir, const std::string& cmd, const std::string& title) {
    BuildUi& u = *buildui_;
    Pane* const editing = pane_;
    Pane* into = nullptr;
    if (!u.doc || !find_doc(u.doc, &into)) into = dock_show(editing);  // (the last run's split, wherever it was moved to)
    if (!into) into = editing;  // (no room for another split)
    const u64 old = u.doc && find_doc(u.doc) ? u.doc : 0;
    pane_ = into;
    const u64 doc = term_open(dir, cmd, title);
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
    u.doc = doc;
    u.running = doc != 0;
    u.last_status = -1;
    u.last_name = title;
    u.last_key.clear();
    u.problems.clear();  // (the last run's go: this one's come as it goes)
    u.groups.clear();
    build_errors_ = build_warnings_ = 0;
    u.at = -1;
    ++build_prob_gen_;
    ++build_gen_;
    dirty_ = true;
    return doc;
}

// A task of the Build tab's (-1: the one the Run button runs).
void App::build_run(int index) {
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
    std::string cmd = t.system == "CMake" ? with_cmake(t.cmd, u.cmake_type, u.cmake_target) : t.cmd;
    if (!t.before.empty()) {  // a run configuration's build first: then, if it went well, the program
        const auto it = std::ranges::find_if(u.tasks, [&](const BuildTask& b) { return !b.config && (b.name == t.before || b.system + ": " + b.name == t.before); });
        if (it == u.tasks.end()) return notify("No task called " + t.before + " to run before " + t.name);
        std::string host, there;  // (on another machine the command runs there: its folders without ssh://host)
        const std::string root = split_remote(u.root, &host, &there) ? there : u.root;
        const std::string first = it->cwd.empty() ? root : root + "/" + it->cwd, then = t.cwd.empty() ? root : root + "/" + t.cwd;
        cmd = "cd " + sh_word(first) + " && " + it->cmd + " && cd " + sh_word(then) + " && " + t.cmd;
    }
    build_command(t.cwd.empty() || !t.before.empty() ? u.root : u.root + "/" + t.cwd, cmd, t.name);
    u.last_key = u.current;  // (how it goes shows on its row)
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

void App::build_stop() {
    BuildUi& u = *buildui_;
    Pane* p = nullptr;
    size_t at = 0;
    if (!u.running || !u.doc || !find_doc(u.doc, &p, &at)) return;
    Pane* const keep = pane_;
    pane_ = p;
    tab_close(at, true);  // (its program hung up on)
    bool still = false;
    for (const std::unique_ptr<Pane>& q : panes_) still |= q.get() == keep;
    pane_ = still ? keep : panes_.front().get();
    focus_pane(pane_);
    u.running = false;
    u.doc = 0;
    ++build_gen_;
    dirty_ = true;
}

// The build's program has ended (term_sync tells): what it made may change
// what the folder builds with (a build folder configured), so it is looked
// at again; and what git sees there (files written, or a repository made by
// a new project's git init).
void App::build_ended(u64 doc, int status) {
    test_ended(doc);  // (a run of the tests: its reports read)
    BuildUi& u = *buildui_;
    if (doc != u.doc) return;
    u.running = false;
    u.last_status = status;
    u.looked = false;
    git_command("look", {});
    ++build_gen_;
    dirty_ = true;
}

// ---------------------------------------------------------------- problems

// Problems the run's output reported (found on its terminal's reading
// thread): each after the others of its file, the files in the order they
// came, as the Problems tab lists them and F8 goes through them.
void App::build_found(u64 doc, std::vector<BuildProblem> found) {
    BuildUi& u = *buildui_;
    if (doc != u.doc || found.empty()) return;
    const u32 was = u.at >= 0 ? u.problems[static_cast<size_t>(u.at)].seq : 0;
    const size_t had = u.problems.size();
    for (BuildProblem& p : found) {
        p.group = u.groups.try_emplace(p.path, static_cast<u32>(u.groups.size())).first->second;
        p.seq = u.seq++;
        (p.severity == 1 ? build_errors_ : build_warnings_) += 1;
        u.problems.push_back(std::move(p));
    }
    // (The new ones in order, then merged in: a step through them, not a sort of them all for each piece of output.)
    const auto by_file = [](const BuildProblem& a, const BuildProblem& b) { return a.group < b.group; };
    const auto mid = u.problems.begin() + static_cast<std::ptrdiff_t>(had);
    std::stable_sort(mid, u.problems.end(), by_file);
    std::inplace_merge(u.problems.begin(), mid, u.problems.end(), by_file);
    if (u.at >= 0)  // (F8 goes on from the one it went to)
        for (size_t i = 0; i < u.problems.size(); ++i)
            if (u.problems[i].seq == was) u.at = static_cast<i64>(i);
    ++build_prob_gen_;
    ++build_gen_;
    dirty_ = true;
}

const std::vector<BuildProblem>& App::build_problems() const { return buildui_->problems; }

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
        if (row.task == kTypeRow || row.task == kTargetRow) {  // CMake's build type and target: what is chosen, or the project's own
            const std::string& v = row.task == kTypeRow ? u.cmake_type : u.cmake_target;
            const u32 c = put(box, font, y, 2, row.text, ink::kText);
            if (c + 4 < cols) put(box, font, y, c + 2, fit_text(v.empty() ? (row.task == kTypeRow ? "the project's" : "all") : v, cols - c - 2), v.empty() ? ink::kMuted : ink::kAccent);
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
        if (!u.last_key.empty() && task_key(t) == u.last_key) {
            if (u.running) after = "running\xE2\x80\xA6";
            else if (u.last_status == 0) after = "done";
            else if (u.last_status > 0) after = "failed: exited with code " + std::to_string(u.last_status), in = ink::kError;
        }
        if (c + 4 < cols) put(box, font, y, c + 2, fit_text(after, cols - c - 2), in);
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
    if (u.rows[i].task < 0) {  // CMake's build type or target: changed
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

// CMake's build type, the next in turn; or its target, chosen from those
// the build folder has (CMake's own "help" target lists them, for make and
// Ninja alike): the folder of the CMake task the Run button runs, or else
// the first that builds.
void App::build_option(int row) {
    BuildUi& u = *buildui_;
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
    // ("cmake --build DIR --parallel" or "cmake --build --preset P": its help.)
    if (const size_t par = build.find(" --parallel"); par != std::string::npos) build.resize(par);
    std::string out;
    if (!fs_run(u.root, build + " --target help", &out, 30000) && out.empty()) return notify("CMake did not list the targets");
    std::vector<std::string> targets{"all"};
    for (size_t at = 0; at < out.size();) {
        size_t nl = out.find('\n', at);
        if (nl == std::string::npos) nl = out.size();
        std::string_view line = std::string_view(out).substr(at, nl - at);
        at = nl + 1;
        std::string name;
        if (line.starts_with("... ")) name = std::string(line.substr(4, line.find(' ', 4) - 4));  // (make's "... name")
        else if (const size_t c = line.find(": phony"); c != std::string_view::npos) name = std::string(line.substr(0, c));  // (Ninja's "name: phony")
        else continue;
        // (CMake's own targets, and a file's object, preprocessed and assembly ones, are no target of the project's.)
        static constexpr std::string_view kOwn[] = {"all", "clean", "depend", "edit_cache", "rebuild_cache", "install", "list_install_components", "help", "test", "package", "package_source", "ZERO_CHECK", "ALL_BUILD", "RUN_TESTS"};
        const bool file = name.ends_with(".o") || name.ends_with(".obj") || name.ends_with(".i") || name.ends_with(".s");
        if (!name.empty() && name.find('/') == std::string::npos && !file && std::ranges::find(kOwn, name) == std::end(kOwn) && std::ranges::find(targets, name) == targets.end())
            targets.push_back(name);
    }
    // (Ninja's help lists phony names alone: an executable of the top
    // folder is a file it makes, which Ninja's own list of targets has.)
    if (out.find(": phony") != std::string::npos && fs_run(u.root, build + " -- -t targets all", &out, 30000))
        for (size_t at = 0; at < out.size();) {
            size_t nl = out.find('\n', at);
            if (nl == std::string::npos) nl = out.size();
            const std::string_view line = std::string_view(out).substr(at, nl - at);
            at = nl + 1;
            const size_t c = line.find(": ");
            if (c == std::string_view::npos || line.substr(c).find("_EXECUTABLE_LINKER__") == std::string_view::npos) continue;
            const std::string name(line.substr(0, c));
            if (name.find('/') == std::string::npos && std::ranges::find(targets, name) == targets.end()) targets.push_back(name);
        }
    find_choose_item("Target", targets, [this, targets](int i) {
        if (i < 0 || static_cast<size_t>(i) >= targets.size()) return;
        buildui_->cmake_target = i == 0 ? std::string() : targets[static_cast<size_t>(i)];
        ++build_gen_;
        dirty_ = true;
    });
}

// ---------------------------------------------------------------- the Run button

// Its words: the task (or Stop, while it runs), and how many errors its
// output has reported, else warnings ("\xE2\x96\xB6 Build (debug) \xC2\xB7 3 errors").
std::string App::build_label() const {
    const BuildUi& u = *buildui_;
    std::string name = u.current.substr(u.current.find('\t') + 1);
    if (width_of(name) > 24) {
        size_t at = 0;
        for (u32 k = 0; k < 23 && at < name.size(); ++k) at += decode_utf8(reinterpret_cast<const u8*>(name.data()) + at, name.size() - at).len;
        name = name.substr(0, at) + "\xE2\x80\xA6";
    }
    std::string label = u.running ? "\xE2\x96\xA0 Stop" : "\xE2\x96\xB6 " + name;
    if (build_errors_) label += " \xC2\xB7 " + std::to_string(build_errors_) + (build_errors_ == 1 ? " error" : " errors");
    else if (build_warnings_) label += " \xC2\xB7 " + std::to_string(build_warnings_) + (build_warnings_ == 1 ? " warning" : " warnings");
    return label;
}

// At the window's top right, at the end of the tab bar there: the task
// chosen, to run it (or, while it runs, to stop it). Made before the
// splits are laid out, so the tab bar there knows to end before it.
void App::build_measure() {
    BuildUi& u = *buildui_;
    u.reserve = 0;
    if (u.current.empty() || !font_ || !tabs_px() || start_shown()) return;
    Font& font = *font_;
    const int cw = font.cell_w();
    // (Its cells made again only when what they show has changed.)
    const u64 key = build_gen_ * 1000003ull ^ font.generation() * 31 ^ 1;
    if (key != u.drawn) {
        u.drawn = key;
        const std::string label = build_label();
        const u32 cols = width_of(label) + 2;
        u.box.resize(cols, 1);
        u.box.put(font, 0, 1, label, u.running || u.last_status > 0 || build_errors_ ? kInkDiagnostic : kInkAccent, Font::kBold);
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

std::string App::build_dump() {
    BuildUi& u = *buildui_;
    build_settle();
    std::string out, system;
    for (const BuildTask& t : u.tasks) {
        if (t.system != system) out += (out.empty() ? "" : " | ") + (system = t.system) + ":";
        out += " " + std::string(task_key(t) == u.current ? ">" : "") + t.name;
    }
    if (out.empty()) out = "nothing";
    if (u.running) out += " (running)";
    if (u.last_status >= 0) out += " (last: " + std::to_string(u.last_status) + ")";
    return out;
}

void App::build_wait() {  // the build's program has ended (60 s at most)
    BuildUi& u = *buildui_;
    const u64 until = now_ns() + 60'000'000'000ull;
    while (u.running && now_ns() < until) {
        term_sync();
        SDL_Delay(5);
    }
    term_sync();
}

}  // namespace blit
