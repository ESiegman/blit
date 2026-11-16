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

#include "app.hpp"
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

}  // namespace

struct App::BuildUi {
    std::vector<BuildTask> tasks;
    std::string root;          // the folder they were found in
    bool looked = false;       // tasks are root's (else: look, when the window is free to)
    std::string current;       // the task the Run button runs (task_key)
    // The worker looking at a folder, and what it found.
    struct Found {
        std::string root;
        std::vector<FsEntry> files;
        std::vector<BuildTask> tasks;
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
    // The Build tab: a row for each system and for each task, and the one chosen.
    struct Row {
        int task = -1;  // -1: a system's heading
        std::string text;
    };
    std::vector<Row> rows;
    u32 sel = 0, top = 0;
    // The Run button, where it was drawn.
    TextBox box;
    int reserve = 0;  // its width and margin: the top right tab bar ends before it
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    // Last, so that it is joined first, while all it uses is still there.
    std::jthread worker;
};

void App::build_create() { buildui_ = std::make_shared<BuildUi>(); }

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
        f.tasks = detect_builds(root, f.files, hidden);
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
    ++build_gen_;
    dirty_ = true;
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

// A command run in the build's output terminal: in a split below the
// text, made the first time and used again after (a run still going there
// is ended). The keyboard stays where it was.
u64 App::build_command(const std::string& dir, const std::string& cmd, const std::string& title) {
    BuildUi& u = *buildui_;
    Pane* const editing = pane_;
    Pane* into = nullptr;
    size_t old_at = 0;
    const u64 old = u.doc && find_doc(u.doc, &into, &old_at) ? u.doc : 0;
    if (!into) {
        into = split_pane(editing, kEdgeBottom);
        if (!into) {
            into = editing;  // (no room for another split)
        } else {
            pane_ = into;
            for (int k = 0; k < 4; ++k) split_grow(false, -1);  // (the output a third of the height)
        }
    }
    pane_ = into;
    const u64 doc = term_open(dir, cmd, title);
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
    build_command(t.cwd.empty() ? u.root : u.root + "/" + t.cwd, t.cmd, t.name);
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
// what the folder builds with (a build folder configured), so it is looked at again.
void App::build_ended(u64 doc, int status) {
    BuildUi& u = *buildui_;
    if (doc != u.doc) return;
    u.running = false;
    u.last_status = status;
    u.looked = false;
    ++build_gen_;
    dirty_ = true;
}

// ---------------------------------------------------------------- the Build tab

void App::build_panel_draw(TextBox& box, bool focused) {
    BuildUi& u = *buildui_;
    Font& font = *font_;
    const u32 cols = box.cols, rows = box.rows;
    std::string where = u.looked ? u.root : u.looking_at;
    if (const char* home = SDL_getenv("HOME"); home && *home && where.starts_with(home)) where = "~" + where.substr(std::strlen(home));
    const bool looking = !u.looked || u.looking;
    box.put(font, 1, 0, looking && u.tasks.empty() ? "Looking at " + where + "\xE2\x80\xA6" : u.tasks.empty() ? "Nothing to build in " + where : "In " + where, kInkMuted);
    u.rows.clear();
    std::string system;
    for (size_t i = 0; i < u.tasks.size(); ++i) {
        const BuildTask& t = u.tasks[i];
        if (t.system != system) {
            system = t.system;
            u.rows.push_back({-1, t.system});
        }
        u.rows.push_back({static_cast<int>(i), t.name});
    }
    if (u.tasks.empty()) {
        if (looking) return;
        static const char* const kHelp[] = {"Found from the folder's files: CMake,", "Meson, make, autotools and Ninja", "(C, C++, HIP), and uv and pip (Python).",
                                            "", "Others can be added in init.lua", "with blit.build.add.", "", "Cmd+R looks again."};
        for (u32 r = 0; r < std::size(kHelp) && 3 + r < rows; ++r) box.put(font, 3 + r, 1, kHelp[r], kInkMuted);
        return;
    }
    // The chosen row: a task (the Run button's, after the folder was looked at).
    if (u.sel >= u.rows.size() || u.rows[u.sel].task < 0) {
        u.sel = 0;
        for (u32 i = 0; i < u.rows.size(); ++i)
            if (u.rows[i].task >= 0 && task_key(u.tasks[static_cast<size_t>(u.rows[i].task)]) == u.current) u.sel = i;
        if (u.rows[u.sel].task < 0 && u.sel + 1 < u.rows.size()) ++u.sel;
    }
    const u32 body = rows > 4 ? rows - 4 : 0;
    if (u.sel < u.top) u.top = u.sel;
    if (body && u.sel >= u.top + body) u.top = u.sel - body + 1;
    for (u32 r = 0; r < body && u.top + r < u.rows.size(); ++r) {
        const BuildUi::Row& row = u.rows[u.top + r];
        const u32 y = 2 + r;
        if (row.task < 0) {
            box.put(font, y, 0, row.text, kInkMuted, Font::kBold);
            continue;
        }
        const BuildTask& t = u.tasks[static_cast<size_t>(row.task)];
        const bool cur = task_key(t) == u.current;
        u32 c = box.put(font, y, 1, cur ? "\xE2\x96\xB6 " : "  ", kInkAccent);
        c = box.put(font, y, c, t.name, 0, cur ? Font::kBold : 0);
        if (c + 4 < cols) {  // the command, as much of it as fits
            std::string cmd = t.cmd;
            const u32 room = cols - c - 2;
            if (width_of(cmd) > room) {
                size_t at = 0;
                for (u32 k = 0; k + 1 < room && at < cmd.size(); ++k) at += decode_utf8(reinterpret_cast<const u8*>(cmd.data()) + at, cmd.size() - at).len;
                cmd = cmd.substr(0, at) + "\xE2\x80\xA6";
            }
            box.put(font, y, c + 2, cmd, kInkMuted);
        }
        if (focused && u.top + r == u.sel) box.select_row(y);
    }
    if (rows > 2) {
        std::string last;
        if (u.running) last = u.last_name + ": running";
        else if (u.last_status == 0) last = u.last_name + ": done";
        else if (u.last_status > 0) last = u.last_name + ": failed (" + std::to_string(u.last_status) + ")";
        if (!last.empty()) box.put(font, rows - 2, 0, last, u.last_status > 0 ? kInkDiagnostic : kInkMuted);
        box.put(font, rows - 1, 0, "Enter runs \xC2\xB7 Space sets \xE2\x96\xB6", kInkMuted);
    }
}

void App::build_panel_click(int row, int col, const SDL_MouseButtonEvent& b) {
    (void)col;
    BuildUi& u = *buildui_;
    if (b.button != SDL_BUTTON_LEFT || row < 2) return;
    const u32 i = u.top + static_cast<u32>(row - 2);
    if (i >= u.rows.size() || u.rows[i].task < 0) return;
    u.sel = i;
    u.current = task_key(u.tasks[static_cast<size_t>(u.rows[i].task)]);
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
            if (u.rows[at].task >= 0) break;
        }
        u.sel = at;
    };
    switch (k.key) {
        case SDLK_UP: step(-1); break;
        case SDLK_DOWN: step(1); break;
        case SDLK_SPACE:
            if (u.sel < n && u.rows[u.sel].task >= 0) u.current = task_key(u.tasks[static_cast<size_t>(u.rows[u.sel].task)]);
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (u.sel < n && u.rows[u.sel].task >= 0) build_run(u.rows[u.sel].task);
            break;
        default: return false;
    }
    ++build_gen_;
    dirty_ = true;
    return true;
}

// ---------------------------------------------------------------- the Run button

// At the window's top right, at the end of the tab bar there: the task
// chosen, to run it (or, while it runs, to stop it). Made before the
// splits are laid out, so the tab bar there knows to end before it.
void App::build_measure() {
    BuildUi& u = *buildui_;
    u.reserve = 0;
    if (u.current.empty() || !font_ || !tabs_px() || start_shown()) return;
    Font& font = *font_;
    const int cw = font.cell_w();
    std::string name = u.current.substr(u.current.find('\t') + 1);
    if (width_of(name) > 24) {
        size_t at = 0;
        for (u32 k = 0; k < 23 && at < name.size(); ++k) at += decode_utf8(reinterpret_cast<const u8*>(name.data()) + at, name.size() - at).len;
        name = name.substr(0, at) + "\xE2\x80\xA6";
    }
    const std::string label = u.running ? "\xE2\x96\xA0 Stop" : "\xE2\x96\xB6 " + name;
    const u32 cols = width_of(label) + 2;
    u.box.resize(cols, 1);
    u.box.put(font, 0, 1, label, u.running || u.last_status > 0 ? kInkDiagnostic : kInkAccent, Font::kBold);
    for (u32 c = 0; c < cols; ++c) u.box.cells[c] |= kCellHover;
    u.reserve = static_cast<int>(cols) * cw + cw;
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
