// Debugging in the editor (dap.hpp talks to the debugger): breakpoints in
// the gutter (F9; with a condition or a hit count, Shift+F9), which move
// with their lines as the file is edited; a session started with F5
// (lldb-dap on a Mac, gdb or lldb-dap on Linux, debugpy for Python; here,
// or on the machine an ssh:// program is on), the line it stopped on, and
// the side panel's Debug tab: the program, the controls, the threads, the
// call stack of the one chosen, the variables (opened a level at a time),
// the expressions watched, the breakpoints and what the program printed.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <map>

#include "app.hpp"
#include "dap.hpp"
#include "look.hpp"
#include "remote.hpp"
#include "textbox.hpp"
#include "utf8.hpp"

namespace blit {

namespace {

void wake() {
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = 15;
    SDL_PushEvent(&ev);
}

std::string quoted(std::string_view s) {
    std::string out;
    json::quote(out, s);
    return out;
}

std::string base_name(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// A row that is picked: lit as selected while the panel has the keyboard,
// in the quieter tone of the mouse's while it does not.
void light_row(TextBox& box, u32 row, bool focused) {
    if (focused) return box.select_row(row);
    if (row >= box.rows) return;
    for (u32 c = 0; c < box.cols; ++c) box.cells[static_cast<size_t>(row) * box.cols + c] |= kCellHover;
}

// The debugger for a machine, chosen there: Xcode's lldb-dap on a Mac; gdb
// (14 and later speak the protocol) or lldb-dap elsewhere; debugpy for Python.
const char* kNative =
    "if [ \"$(uname -s)\" = Darwin ]; then "
    "xcrun --find lldb-dap >/dev/null 2>&1 || { [ -d /Applications/Xcode.app/Contents/Developer ] && DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer && export DEVELOPER_DIR; }; "
    "exec xcrun lldb-dap; fi; "
    "if command -v gdb >/dev/null 2>&1 && [ \"$(gdb --version | sed -n '1s/.* \\([0-9][0-9]*\\)\\..*/\\1/p')\" -ge 14 ] 2>/dev/null; then echo 'blit-debugger: gdb' >&2; exec gdb -q -i dap; fi; "
    "for d in lldb-dap lldb-vscode; do command -v $d >/dev/null 2>&1 && exec $d; done; "
    "echo 'no debugger: install gdb 14 or newer, or lldb' >&2; exit 127";
const char* kPython = "exec python3 -m debugpy.adapter";
// What kNative says on the error stream when the debugger it starts is gdb
// (which counts a breakpoint's hits from 0, where the others count from 1).
constexpr std::string_view kGdbSays = "blit-debugger: gdb";

// Why an expression could not be evaluated, in a line: a debugger's
// "error:" line, without where in the expression it was ("<user
// expression 1>:1:1: use of undeclared identifier 'x'" is the last part).
std::string why_not(std::string_view m) {
    std::string_view line;
    for (size_t at = 0; at <= m.size();) {
        const size_t nl = std::min(m.find('\n', at), m.size());
        const std::string_view l = m.substr(at, nl - at);
        if (const size_t e = l.find("error: "); e != std::string_view::npos) {
            line = l.substr(e + 7);
            break;
        }
        if (line.empty()) line = l;
        at = nl + 1;
    }
    if (line.starts_with('<'))
        if (const size_t close = line.find('>'); close != std::string_view::npos)
            if (const size_t sp = line.find(": ", close); sp != std::string_view::npos) line = line.substr(sp + 2);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.remove_suffix(1);
    return line.empty() ? std::string("not available") : std::string(line);
}

std::string trimmed(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return std::string(s);
}

}  // namespace

struct App::DebugUi {
    // What to debug.
    std::string program, args, cwd;
    // Breakpoints: by file (its full path, ssh:// for another machine's), by
    // line (from 0), each with what it waits for, if anything.
    struct Break {
        std::string cond;  // it stops only where this is true
        std::string hits;  // and only at this hit (a number: the Nth time it is reached)
        bool operator==(const Break&) const = default;
    };
    using Lines = std::map<u32, Break>;
    std::map<std::string, Lines> breaks;
    // The file whose gutter was drawn last: its full path and breakpoints, kept
    // while debug_gen_ stands (the gutter asks about each line).
    std::string gutter_of, gutter_full;
    const Lines* gutter_breaks = nullptr;
    u64 gutter_gen = 0;
    // A document whose breakpoints follow its text as it is edited, as Vim's
    // marks do: each breakpoint is a place at the start of its line, and
    // moves as such a place would (a line put above it takes it down, its
    // line joined to the one above takes it up). One for each document that
    // has had breakpoints, by its id.
    struct Mover final : EditListener {
        App* app = nullptr;
        Editor* ed = nullptr;
        std::string raw, full;  // the document's path, and its full path
        void on_edit(const TextEdit& e) override;
    };
    std::map<u64, std::unique_ptr<Mover>> movers;
    void follow(App& a, Doc& d) {
        if (movers.contains(d.id)) return;
        auto m = std::make_unique<Mover>();
        m->app = &a;
        m->ed = &d.ed;
        m->raw = d.ed.path;
        m->full = full_path(d.ed.path);
        d.ed.listen(m.get());
        movers[d.id] = std::move(m);
    }
    // The documents of a file that has no breakpoints left stop being followed (and those closed are forgotten).
    void unfollow(App& a, const std::string& full) {
        for (auto it = movers.begin(); it != movers.end();) {
            Doc* d = a.find_doc(it->first);
            if (d && it->second->full != full) {
                ++it;
                continue;
            }
            if (d) d->ed.unlisten(it->second.get());
            it = movers.erase(it);
        }
    }
    // The session.
    std::unique_ptr<Dap> dap;
    enum State : u8 { kIdle, kStarting, kRunning, kStopped } state = kIdle;
    std::string remote;  // "ssh://host" when the program is on another machine
    bool gdb = false;    // the debugger is gdb (its hit counts are from 0)
    i64 thread = 0;      // the thread whose stack is shown (and which the steps step)
    i64 stopped_thread = 0;
    struct Thread {
        i64 id = 0;
        std::string name;
    };
    std::vector<Thread> threads;  // (when it is stopped)
    std::string reason, error;
    struct Frame {
        i64 id = 0;
        std::string name, path;  // (the path as blit knows it)
        u32 line = 0;            // from 1
    };
    std::vector<Frame> frames;
    u32 frame = 0;
    struct Var {
        std::string name, value;
        i64 ref = 0;
        u32 depth = 0;
        bool open = false;
    };
    std::vector<Var> vars;
    u64 vars_gen = 0;  // (answers about variables from before the last stop are dropped)
    // The expressions watched: each evaluated where it stopped, in the frame shown.
    struct Watch {
        std::string expr, value;
        bool failed = false;
    };
    std::vector<Watch> watches;
    u64 watch_gen = 0;  // (answers from before the last evaluation are dropped)
    u32 asking = 0;    // questions about the stack and the variables not answered yet (debug_wait)
    std::vector<std::string> output;
    std::string partial;
    int exit_code = -1;
    bool end_soon = false;  // (the session ends once the debugger's answers have been run: not inside one)
    // The panel: a row for each thing shown, and the one chosen.
    enum Act : u8 { kNone, kProgram, kArgs, kCwd, kStart, kContinue, kPause, kOver, kInto, kOut, kStop, kThread, kFrame, kVar, kWatch, kWatchAdd, kBreak };
    // A row: a mark (a triangle, an arrow) before its text, in an ink of its
    // own; the text; something quieter after it; and on the right its key.
    struct Row {
        Act act = kNone;
        u32 index = 0;
        std::string text;
        Ink ink = ink::kText;
        u32 indent = 0;
        std::string mark, after, key;
        Ink mark_ink = ink::kMuted;
    };
    std::vector<Row> rows;
    u32 sel = 0, top = 0;

    void print(std::string_view text) {
        for (const char c : text)
            if (c != '\r') partial += c;  // (a program on a terminal ends its lines with \r\n)
        size_t nl;
        while ((nl = partial.find('\n')) != std::string::npos) {
            output.push_back(partial.substr(0, nl));
            partial.erase(0, nl + 1);
        }
        if (output.size() > 500) output.erase(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(output.size() - 500));
    }
    // A path as the debugger knows it (another machine's: without ssh://host), and back.
    std::string there(const std::string& p) const { return !remote.empty() && p.starts_with(remote) ? p.substr(remote.size()) : p; }
    std::string here(std::string_view p) const { return p.empty() ? std::string() : remote + std::string(p); }
};

void App::DebugUi::Mover::on_edit(const TextEdit& e) {
    // (Typing within a line, the most of what is done, moves no line.)
    if (e.start_row == e.old_end_row && e.old_end_row == e.new_end_row) return;
    DebugUi& u = *app->debugui_;
    if (ed->path != raw) raw = ed->path, full = full_path(raw);  // (saved under another name)
    const auto it = u.breaks.find(full);
    if (it == u.breaks.end() || it->second.empty() || it->second.rbegin()->first < e.start_row) return;
    Lines moved;
    for (const auto& [line, b] : it->second) {
        u32 to = line;
        if (line > e.old_end_row || (line == e.old_end_row && e.old_end_col == 0)) to = line - e.old_end_row + e.new_end_row;  // after what changed
        else if (line > e.start_row) to = e.start_row;  // in what was taken away: where it was
        moved.try_emplace(to, b);  // (two that come to one line are one)
    }
    if (moved == it->second) return;
    it->second = std::move(moved);
    ++app->debug_gen_;
}

void App::debug_create() { debugui_ = std::make_shared<DebugUi>(); }

bool App::debug_running() const { return debugui_ && debugui_->state != DebugUi::kIdle; }

// ---------------------------------------------------------------- breakpoints

void App::debug_toggle_break() {
    DebugUi& u = *debugui_;
    Doc& d = *pane_->doc;
    if (d.ed.path.empty() || d.term || d.prof) return;
    const std::string path = full_path(d.ed.path);
    const u32 line = static_cast<u32>(d.ed.buf.line_of(d.ed.cursor()));
    DebugUi::Lines& lines = u.breaks[path];
    if (!lines.erase(line)) lines.emplace(line, DebugUi::Break{});
    if (lines.empty()) {
        u.breaks.erase(path);
        u.unfollow(*this, path);
    } else {
        u.follow(*this, d);
    }
    debug_send_breaks(path);
    ++debug_gen_;
    dirty_ = true;
}

// Shift+F9: a breakpoint's condition and hit count, asked for in turn (each
// as it is now; nothing typed in both is a plain breakpoint). Of the
// breakpoint chosen in the Debug tab while it has the keyboard; else of the
// cursor's line, which gets one if it has none.
void App::debug_edit_break() {
    DebugUi& u = *debugui_;
    std::string path;
    u32 line = 0;
    if (panel_has_keyboard() && panel_tab() == 4 && u.sel < u.rows.size() && u.rows[u.sel].act == DebugUi::kBreak) {  // (4: the Debug tab)
        u32 n = 0;
        for (const auto& [p, lines] : u.breaks)
            for (const auto& [l, b] : lines)
                if (n++ == u.rows[u.sel].index) path = p, line = l;
    } else {
        Doc& d = *pane_->doc;
        if (d.ed.path.empty() || d.term || d.prof || d.loading) return;
        path = full_path(d.ed.path);
        line = static_cast<u32>(d.ed.buf.line_of(d.ed.cursor()));
        u.follow(*this, d);
    }
    if (path.empty()) return;
    DebugUi::Break was;
    if (auto f = u.breaks.find(path); f != u.breaks.end())
        if (auto b = f->second.find(line); b != f->second.end()) was = b->second;
    find_choose(kFindInput, "Condition", was.cond, [this, path, line, hits = was.hits](const std::string& cond) {
        find_choose(kFindInput, "Hit count", hits, [this, path, line, cond = trimmed(cond)](const std::string& typed) {
            DebugUi& v = *debugui_;
            v.breaks[path][line] = {cond, trimmed(typed)};
            if (Doc* d = pane_->doc; d && !d->ed.path.empty() && full_path(d->ed.path) == path) v.follow(*this, *d);
            debug_send_breaks(path);
            ++debug_gen_;
            dirty_ = true;
        });
    });
}

// A file's breakpoints, to a session under way.
void App::debug_send_breaks(const std::string& path) {
    DebugUi& u = *debugui_;
    if (!u.dap || u.state == DebugUi::kStarting) return;  // (all are sent when the debugger is ready for them)
    std::string a = "{\"source\":{\"path\":" + quoted(u.there(path)) + "},\"breakpoints\":[";
    if (auto it = u.breaks.find(path); it != u.breaks.end()) {
        bool first = true;
        for (const auto& [l, b] : it->second) {
            a += std::string(first ? "" : ",") + "{\"line\":" + std::to_string(l + 1);
            if (!b.cond.empty()) a += ",\"condition\":" + quoted(b.cond);
            if (!b.hits.empty()) {
                // (gdb takes the hits to pass over, the others the hit to stop at.)
                std::string hits = b.hits;
                if (u.gdb && std::ranges::all_of(hits, [](char c) { return c >= '0' && c <= '9'; }) && hits.size() < 9)
                    hits = std::to_string(std::max(1, std::atoi(hits.c_str())) - 1);
                a += ",\"hitCondition\":" + quoted(hits);
            }
            a += "}";
            first = false;
        }
    }
    a += "]}";
    u.dap->request("setBreakpoints", a);
}

// What goes in a line's first gutter cell: the line the program stopped
// on (an arrow), or a breakpoint (a dot); 0 for nothing.
u32 App::debug_gutter(const Doc& d, u64 line) {
    if (!debugui_ || d.ed.path.empty()) return 0;
    DebugUi& u = *debugui_;
    if (u.breaks.empty() && u.state != DebugUi::kStopped) return 0;
    if (u.gutter_gen != debug_gen_ || u.gutter_of != d.ed.path) {
        u.gutter_of = d.ed.path;
        u.gutter_full = full_path(d.ed.path);
        const auto it = u.breaks.find(u.gutter_full);
        u.gutter_breaks = it == u.breaks.end() ? nullptr : &it->second;
        u.gutter_gen = debug_gen_;
        if (u.gutter_breaks)  // (its breakpoints follow its lines: a file opened again is followed again)
            if (Doc* doc = find_doc(d.id)) u.follow(*this, *doc);
    }
    if (u.state == DebugUi::kStopped && u.frame < u.frames.size() && u.frames[u.frame].line == line + 1 && u.frames[u.frame].path == u.gutter_full)
        return (font_->glyph(0x25B6) & (Font::kWide - 1)) | (kInkAccent << kCellInkShift);
    if (u.gutter_breaks)
        if (const auto b = u.gutter_breaks->find(static_cast<u32>(line)); b != u.gutter_breaks->end()) {
            const bool plain = b->second.cond.empty() && b->second.hits.empty();  // (a dot; one that waits for something, a diamond)
            return (font_->glyph(plain ? 0x25CF : 0x25C6) & (Font::kWide - 1)) | (kInkDiagnostic << kCellInkShift);
        }
    return 0;
}

// ---------------------------------------------------------------- a session

void App::debug_start() {
    DebugUi& u = *debugui_;
    if (u.state == DebugUi::kStopped) return debug_command("continue");
    if (u.state == DebugUi::kStarting) return debug_end();  // (F5 again while it starts: called off)
    if (u.state != DebugUi::kIdle) return;
    if (u.program.empty()) {  // first, what to debug
        if (!panel_px()) panel_toggle();
        panel_show_debug();
        const std::string* root = panel_root();
        std::string start = (root ? *root : default_workdir()) + "/";
        if (const char* home = SDL_getenv("HOME"); home && *home && start.starts_with(home)) start = "~" + start.substr(std::strlen(home));
        find_choose(kFindOpen, "Program to debug", start, [this](const std::string& p) {
            debugui_->program = p;
            debug_start();
        });
        return;
    }
    std::string host, there;
    u.remote = split_remote(u.program, &host, &there) ? "ssh://" + host : std::string();
    const std::string dir = !u.cwd.empty() ? u.cwd : parent_of(u.program);
    const bool python = u.program.ends_with(".py");
    u.dap = std::make_unique<Dap>();
    u.gdb = false;
    u.frames.clear();
    u.threads.clear();
    u.vars.clear();
    u.output.clear();
    u.partial.clear();
    u.error.clear();
    u.reason.clear();
    u.exit_code = -1;
    u.asking = 0;
    // (On another machine the connection is waited for and the debugger started
    // there on the Dap's own thread: the tab says Starting… meanwhile, and what
    // is asked of it below is sent once it has started.)
    std::string err;
    if (!u.dap->start({"/bin/sh", "-c", python ? kPython : kNative}, dir, wake, &err)) {
        u.error = err;
        u.dap.reset();
        ++debug_gen_;
        dirty_ = true;
        return;
    }
    u.state = DebugUi::kStarting;
    if (!panel_px()) panel_toggle();
    panel_show_debug();
    panel_focus(false);
    // The program's arguments, as a shell would split them.
    std::string args = "[";
    {
        std::string word;
        bool any = false;
        char quote = 0;
        auto end_word = [&] {
            if (any || !word.empty()) args += (args.size() > 1 ? "," : "") + quoted(word);
            word.clear();
            any = false;
        };
        for (const char c : u.args) {
            if (quote) c == quote ? void(quote = 0) : void(word += c);
            else if (c == '"' || c == '\'') quote = c, any = true;
            else if (c == ' ' || c == '\t') end_word();
            else word += c;
        }
        end_word();
    }
    args += "]";
    const std::string launch = "{\"program\":" + quoted(u.there(u.program)) + ",\"args\":" + args + ",\"cwd\":" + quoted(u.there(dir)) +
                               (python ? ",\"type\":\"python\",\"request\":\"launch\",\"console\":\"internalConsole\",\"justMyCode\":true" : "") + "}";
    u.dap->request("initialize",
                   "{\"clientID\":\"blit\",\"clientName\":\"blit\",\"adapterID\":\"blit\",\"linesStartAt1\":true,\"columnsStartAt1\":true,"
                   "\"pathFormat\":\"path\",\"supportsVariableType\":true,\"supportsRunInTerminalRequest\":false}",
                   [this, launch](const Dap::Message& m) {
                       DebugUi& v = *debugui_;
                       if (!m.success) {
                           v.error = "the debugger would not start: " + m.message;
                           return;
                       }
                       v.dap->request("launch", launch, [this](const Dap::Message& r) {
                           if (!r.success) {
                               debugui_->error = "cannot launch it: " + (r.message.empty() ? std::string("the debugger said no") : r.message);
                               debugui_->end_soon = true;
                           }
                       });
                   });
    ++debug_gen_;
    dirty_ = true;
}

// The session over: the debugger let go of (and the program with it), on
// a thread of its own: nothing here waits for it to end.
void App::debug_end() {
    DebugUi& u = *debugui_;
    if (u.dap && !u.dap->ended()) {
        u.dap->request("disconnect", "{\"terminateDebuggee\":true}");
        u.dap->stop(300);  // (the adapter lets go of the program as it ends)
    }
    u.dap.reset();
    u.state = DebugUi::kIdle;
    u.frames.clear();
    u.threads.clear();
    u.vars.clear();
    for (DebugUi::Watch& w : u.watches) w.value.clear(), w.failed = false;
    u.asking = 0;
    ++debug_gen_;
    dirty_ = true;
}

void App::debug_command(const std::string& what) {
    DebugUi& u = *debugui_;
    if (!u.dap) return;
    if (what == "stop") return debug_end();
    if (what == "pause") {
        if (u.state == DebugUi::kRunning) u.dap->request("pause", "{\"threadId\":" + std::to_string(u.thread ? u.thread : 1) + "}");
        return;
    }
    if (u.state != DebugUi::kStopped) return;
    u.dap->request(what, "{\"threadId\":" + std::to_string(u.thread) + "}");  // continue, next, stepIn, stepOut
    u.state = DebugUi::kRunning;
    u.frames.clear();
    u.threads.clear();
    u.vars.clear();
    ++u.vars_gen;
    ++u.watch_gen;
    ++debug_gen_;
    dirty_ = true;
}

// A frame of the call stack: its source shown, its variables asked for.
void App::debug_frame(u32 i, bool show) {
    DebugUi& u = *debugui_;
    if (i >= u.frames.size() || !u.dap) return;
    u.frame = i;
    const DebugUi::Frame& f = u.frames[i];
    if (show && !f.path.empty()) {
        if (open_path(f.path.c_str())) {
            Doc& d = *pane_->doc;
            if (d.loading) {
                d.goto_line = f.line ? f.line - 1 : 0;
                d.goto_col = 0;
            } else {
                d.ed.move_to(d.ed.buf.line_start(std::min<u64>(f.line ? f.line - 1 : 0, d.ed.buf.line_count() - 1)), false);
                scroll_to_cursor();
            }
        }
    }
    u.vars.clear();
    const u64 gen = ++u.vars_gen;
    ++u.asking;
    u.dap->request("scopes", "{\"frameId\":" + std::to_string(f.id) + "}", [this, gen](const Dap::Message& m) {
        DebugUi& v = *debugui_;
        if (v.asking) --v.asking;
        if (gen != v.vars_gen || !m.success) return;
        bool first = true;
        for (json::Value s : m.body()["scopes"]) {
            if (s["expensive"].boolean()) continue;
            v.vars.push_back({std::string(s["name"].str()), {}, s["variablesReference"].integer(), 0, false});
            if (first) debug_expand(static_cast<u32>(v.vars.size() - 1));  // (the first, the locals or arguments, open)
            first = false;
        }
        ++debug_gen_;
        dirty_ = true;
    });
    debug_watch_eval();  // (in this frame)
    ++debug_gen_;
    dirty_ = true;
}

// The expressions watched, evaluated where it has stopped, in the frame shown.
void App::debug_watch_eval() {
    DebugUi& u = *debugui_;
    if (!u.dap || u.state != DebugUi::kStopped) return;
    const u64 gen = ++u.watch_gen;
    const std::string frame = u.frame < u.frames.size() ? ",\"frameId\":" + std::to_string(u.frames[u.frame].id) : std::string();
    for (size_t i = 0; i < u.watches.size(); ++i) {
        const std::string expr = u.watches[i].expr;
        ++u.asking;
        u.dap->request("evaluate", "{\"expression\":" + quoted(expr) + frame + ",\"context\":\"watch\"}", [this, gen, i, expr](const Dap::Message& m) {
            DebugUi& v = *debugui_;
            if (v.asking) --v.asking;
            if (gen != v.watch_gen || i >= v.watches.size() || v.watches[i].expr != expr) return;
            DebugUi::Watch& w = v.watches[i];
            w.failed = !m.success;
            w.value = m.success ? std::string(m.body()["result"].str()) : why_not(m.message);
            for (char& c : w.value)
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';  // (one row)
            ++debug_gen_;
            dirty_ = true;
        });
    }
}

// The call stack of the thread chosen; the first frame with source to show
// (the program's own, not a library's) is shown, and its variables asked for.
void App::debug_stack() {
    DebugUi& u = *debugui_;
    if (!u.dap || u.state != DebugUi::kStopped) return;
    u.frames.clear();
    u.vars.clear();
    ++u.vars_gen;
    ++u.asking;
    const i64 thread = u.thread;
    u.dap->request("stackTrace", "{\"threadId\":" + std::to_string(thread) + ",\"startFrame\":0,\"levels\":64}", [this, thread](const Dap::Message& r) {
        DebugUi& v = *debugui_;
        if (v.asking) --v.asking;
        if (!r.success || v.state != DebugUi::kStopped || v.thread != thread) return;  // (another thread chosen meanwhile: its own answer counts)
        v.frames.clear();
        for (json::Value f : r.body()["stackFrames"])
            v.frames.push_back({f["id"].integer(), std::string(f["name"].str()), v.here(f["source"]["path"].str()), static_cast<u32>(f["line"].integer())});
        u32 first = 0;
        while (first < v.frames.size() && v.frames[first].path.empty()) ++first;
        debug_frame(first < v.frames.size() ? first : 0, true);
        ++debug_gen_;
        dirty_ = true;
    });
    ++debug_gen_;
    dirty_ = true;
}

// A thread chosen in the Debug tab: its stack shown (and the steps are its).
void App::debug_thread(u32 i) {
    DebugUi& u = *debugui_;
    if (i >= u.threads.size() || u.state != DebugUi::kStopped) return;
    u.thread = u.threads[i].id;
    debug_stack();
}

// A variable (or a scope) opened: its members, a level below it.
void App::debug_expand(u32 i) {
    DebugUi& u = *debugui_;
    if (i >= u.vars.size() || !u.dap || u.vars[i].ref <= 0) return;
    DebugUi::Var& var = u.vars[i];
    if (var.open) {  // shut: what is under it goes
        var.open = false;
        size_t end = i + 1;
        while (end < u.vars.size() && u.vars[end].depth > var.depth) ++end;
        u.vars.erase(u.vars.begin() + i + 1, u.vars.begin() + static_cast<std::ptrdiff_t>(end));
        ++debug_gen_;
        dirty_ = true;
        return;
    }
    var.open = true;
    const u64 gen = u.vars_gen;
    const i64 ref = var.ref;
    const u32 depth = var.depth + 1;
    ++u.asking;
    u.dap->request("variables", "{\"variablesReference\":" + std::to_string(ref) + "}", [this, gen, ref, depth](const Dap::Message& m) {
        DebugUi& v = *debugui_;
        if (v.asking) --v.asking;
        if (gen != v.vars_gen || !m.success) return;
        // (Where its parent is now: the list may have changed meanwhile.)
        size_t at = 0;
        while (at < v.vars.size() && !(v.vars[at].ref == ref && v.vars[at].depth + 1 == depth && v.vars[at].open)) ++at;
        if (at == v.vars.size()) return;
        std::vector<DebugUi::Var> kids;
        for (json::Value x : m.body()["variables"]) {
            std::string value(x["value"].str());
            if (const std::string_view type = x["type"].str(); !type.empty() && value.size() < 40) value += "  (" + std::string(type) + ")";
            kids.push_back({std::string(x["name"].str()), std::move(value), x["variablesReference"].integer(), depth, false});
        }
        v.vars.insert(v.vars.begin() + static_cast<std::ptrdiff_t>(at + 1), kids.begin(), kids.end());
        ++debug_gen_;
        dirty_ = true;
    });
    ++debug_gen_;
    dirty_ = true;
}

// What the debugger has said: answers' callbacks run, then its events.
void App::debug_sync() {
    DebugUi& u = *debugui_;
    if (!u.dap) return;
    for (const Dap::Message& m : u.dap->poll()) {
        const json::Value b = m.body();
        if (m.type == "request") {  // (it asks for something blit does not do: running in a terminal, another session)
            u.dap->respond(m, false);
        } else if (m.name == "initialized") {
            u.state = DebugUi::kRunning;
            u.gdb = u.dap->errors().find(kGdbSays) != std::string::npos;  // (said before it started: here by now)
            for (const auto& [path, lines] : u.breaks) debug_send_breaks(path);
            u.dap->request("setExceptionBreakpoints", "{\"filters\":[]}");
            u.dap->request("configurationDone");
            // (As other clients do, the threads are asked for once now: lldb-dap answers the
            // first such question with those it had at the start, whenever it is asked.)
            u.dap->request("threads");
        } else if (m.name == "stopped") {
            u.state = DebugUi::kStopped;
            u.thread = u.stopped_thread = b["threadId"].integer(u.thread);
            u.reason = std::string(b["reason"].str());
            if (const std::string_view text = b["text"].str(); !text.empty()) u.reason += ": " + std::string(text);
            // Every thread there is (each one's stack can be chosen), and the stack of the one that stopped.
            u.threads.clear();
            ++u.asking;
            u.dap->request("threads", "{}", [this](const Dap::Message& r) {
                DebugUi& v = *debugui_;
                if (v.asking) --v.asking;
                if (!r.success || v.state != DebugUi::kStopped) return;
                v.threads.clear();
                for (json::Value t : r.body()["threads"]) v.threads.push_back({t["id"].integer(), std::string(t["name"].str())});
                ++debug_gen_;
                dirty_ = true;
            });
            debug_stack();
        } else if (m.name == "continued") {
            u.state = DebugUi::kRunning;
            u.frames.clear();
            u.threads.clear();
            u.vars.clear();
        } else if (m.name == "output") {
            const std::string_view cat = b["category"].str();
            if (cat != "telemetry" && cat != "important") u.print(b["output"].str());
        } else if (m.name == "exited") {
            u.exit_code = static_cast<int>(b["exitCode"].integer(-1));
            u.print("[exited with code " + std::to_string(u.exit_code) + "]\n");
        } else if (m.name == "terminated") {
            u.end_soon = true;
        }
    }
    if (u.end_soon) {
        u.end_soon = false;
        debug_end();
        return;
    }
    if (u.dap && u.dap->ended()) {  // the debugger itself went
        if (u.state == DebugUi::kStarting || u.exit_code < 0) {
            std::string why = u.dap->errors();
            if (const size_t said = why.find(kGdbSays); said != std::string::npos) why.erase(said, kGdbSays.size() + 1);  // (with its new line)
            while (!why.empty() && (why.back() == '\n' || why.back() == '\r')) why.pop_back();
            if (const size_t nl = why.rfind('\n'); nl != std::string::npos) why = why.substr(nl + 1);
            if (why.find("No module named debugpy") != std::string::npos) why = "debugpy is not installed (pip install debugpy)";
            if (u.error.empty() && !why.empty()) u.error = why;
        }
        debug_end();
    }
    ++debug_gen_;
    dirty_ = true;
}

void App::debug_evaluate() {
    DebugUi& u = *debugui_;
    if (!u.dap || u.state != DebugUi::kStopped) return;
    find_choose(kFindInput, "Evaluate", "", [this](const std::string& e) {
        DebugUi& v = *debugui_;
        if (!v.dap || e.empty()) return;
        const std::string frame = v.frame < v.frames.size() ? ",\"frameId\":" + std::to_string(v.frames[v.frame].id) : std::string();
        ++v.asking;  // (debug_wait waits for the answer too)
        v.dap->request("evaluate", "{\"expression\":" + quoted(e) + frame + ",\"context\":\"watch\"}", [this, e](const Dap::Message& m) {
            DebugUi& w = *debugui_;
            if (w.asking) --w.asking;
            w.print(e + " = " + (m.success ? std::string(m.body()["result"].str()) : "(" + m.message + ")") + "\n");
            ++debug_gen_;
            dirty_ = true;
        });
    });
}

// ---------------------------------------------------------------- the Debug tab

void App::debug_panel_draw(TextBox& box, bool focused) {
    DebugUi& u = *debugui_;
    Font& font = *font_;
    const u32 cols = box.cols, rows = box.rows;
    using Row = DebugUi::Row;
    std::vector<Row>& out = u.rows;
    out.clear();
    auto add = [&](DebugUi::Act act, u32 index, std::string text, Ink in = ink::kText, u32 indent = 0) -> Row& {
        out.push_back({act, index, std::move(text), in, indent, {}, {}, {}, ink::kMuted});
        return out.back();
    };
    // What it is doing.
    static const char* const kState[] = {"Not running", "Starting\xE2\x80\xA6", "Running", "Stopped"};
    std::string status = kState[u.state];
    if (u.state == DebugUi::kStarting && !u.remote.empty()) status = "Starting on " + u.remote.substr(6) + "\xE2\x80\xA6";  // (ssh://host)
    if (u.state == DebugUi::kStopped && !u.reason.empty()) status += " (" + u.reason + ")";
    if (u.state == DebugUi::kIdle && u.exit_code >= 0) status += ": exited with code " + std::to_string(u.exit_code);
    put(box, font, 1, 0, fit_text(status, cols), u.state == DebugUi::kStopped ? ink::kStrong : ink::kMuted);
    auto shown = [](const std::string& p) {
        std::string s = p;
        if (const char* home = SDL_getenv("HOME"); home && *home && s.starts_with(home)) s = "~" + s.substr(std::strlen(home));
        return s;
    };
    // The program, its arguments and its working directory: each a heading
    // and its value, or (quietly) what it is when not set.
    add(DebugUi::kProgram, 0, "Program", ink::kHeading);
    add(DebugUi::kProgram, 0, u.program.empty() ? "Choose a program\xE2\x80\xA6" : fit_text(shown(u.program), cols > 2 ? cols - 2 : 1, true), u.program.empty() ? ink::kMuted : ink::kText, 2);
    add(DebugUi::kArgs, 0, "Arguments", ink::kHeading);
    add(DebugUi::kArgs, 0, u.args.empty() ? "None" : u.args, u.args.empty() ? ink::kMuted : ink::kText, 2);
    add(DebugUi::kCwd, 0, "Working directory", ink::kHeading);
    add(DebugUi::kCwd, 0, u.cwd.empty() ? "The program's folder" : fit_text(shown(u.cwd), cols > 2 ? cols - 2 : 1, true), u.cwd.empty() ? ink::kMuted : ink::kText, 2);
    add(DebugUi::kNone, 0, "");
    if (!u.error.empty()) add(DebugUi::kNone, 0, u.error, ink::kError);
    // The controls, each with its key; starting (or going on) is the button.
    auto control = [&](DebugUi::Act act, const char* mark, const char* label, const char* key, Ink in = ink::kText) {
        Row& r = add(act, 0, label, in);
        r.mark = mark;
        r.mark_ink = in.ink == ink::kText.ink ? ink::kMuted : in;
        r.key = key_symbols(key, font);
    };
    if (u.state == DebugUi::kIdle) {
        control(DebugUi::kStart, "\xE2\x96\xB6", "Start", "F5", ink::kAccentStrong);
    } else if (u.state == DebugUi::kStarting) {  // (F5 again, or Shift+F5, calls it off)
        control(DebugUi::kStop, "\xE2\x96\xA0", "Cancel", "F5");
    } else if (u.state == DebugUi::kStopped) {
        control(DebugUi::kContinue, "\xE2\x96\xB6", "Continue", "F5", ink::kAccentStrong);
        control(DebugUi::kOver, "\xE2\x86\xB7", "Step over", "F10");
        control(DebugUi::kInto, "\xE2\x86\x93", "Step into", "F7");
        control(DebugUi::kOut, "\xE2\x86\x91", "Step out", "Shift+F11");
        control(DebugUi::kStop, "\xE2\x96\xA0", "Stop", "Shift+F5");
    } else {
        control(DebugUi::kPause, "\xE2\x80\x96", "Pause", "F6");
        control(DebugUi::kStop, "\xE2\x96\xA0", "Stop", "Shift+F5");
    }
    // A section: its heading, with a count where it is a list.
    auto heading = [&](const char* title, size_t count) {
        add(DebugUi::kNone, 0, "");
        Row& r = add(DebugUi::kNone, 0, title, ink::kHeading);
        if (count) r.after = std::to_string(count);
    };
    if (u.threads.size() > 1) {  // (one alone is the stack's own)
        heading("Threads", u.threads.size());
        for (u32 i = 0; i < u.threads.size(); ++i) {
            const DebugUi::Thread& t = u.threads[i];
            Row& r = add(DebugUi::kThread, i, t.name.empty() ? "Thread " + std::to_string(t.id) : t.name, t.id == u.thread ? ink::kStrong : ink::kText);
            r.mark = t.id == u.thread ? "\xE2\x96\xB8" : " ";  // (the one whose stack is shown)
            r.mark_ink = ink::kAccent;
            if (t.id == u.stopped_thread && !u.reason.empty()) r.after = u.reason;
        }
    }
    if (!u.frames.empty()) {
        heading("Call stack", u.frames.size());
        for (u32 i = 0; i < u.frames.size(); ++i) {
            const DebugUi::Frame& f = u.frames[i];
            Row& r = add(DebugUi::kFrame, i, f.name, i == u.frame ? ink::kStrong : f.path.empty() ? ink::kMuted : ink::kText);
            r.mark = i == u.frame ? "\xE2\x96\xB8" : " ";  // (the one shown: where you are)
            r.mark_ink = ink::kAccent;
            if (!f.path.empty()) r.after = base_name(f.path) + ":" + std::to_string(f.line);
        }
    }
    if (!u.vars.empty()) {
        heading("Variables", 0);
        for (u32 i = 0; i < u.vars.size(); ++i) {
            const DebugUi::Var& v = u.vars[i];
            Row& r = add(DebugUi::kVar, i, v.name + (v.value.empty() ? "" : " = " + v.value), v.depth == 0 ? ink::kMuted : ink::kText, 2 * v.depth);
            r.mark = v.ref > 0 ? (v.open ? "\xE2\x96\xBE" : "\xE2\x96\xB8") : " ";  // a small triangle, down or right
        }
    }
    // The expressions watched (always there: the last row adds one).
    heading("Watch", u.watches.size());
    for (u32 i = 0; i < u.watches.size(); ++i) {
        const DebugUi::Watch& w = u.watches[i];
        const bool known = u.state == DebugUi::kStopped && !w.value.empty();
        Row& r = add(DebugUi::kWatch, i, known && !w.failed ? w.expr + " = " + w.value : w.expr, known ? ink::kText : ink::kMuted);
        r.mark = " ";  // (under the add row's +)
        if (known && w.failed) r.after = w.value;
    }
    {
        Row& r = add(DebugUi::kWatchAdd, 0, "Add expression\xE2\x80\xA6", ink::kMuted);
        r.mark = "+";
    }
    if (!u.breaks.empty()) {
        size_t count = 0;
        for (const auto& [path, lines] : u.breaks) count += lines.size();
        heading("Breakpoints", count);
        u32 n = 0;
        for (const auto& [path, lines] : u.breaks)
            for (const auto& [l, b] : lines) {
                Row& r = add(DebugUi::kBreak, n++, base_name(path) + ":" + std::to_string(l + 1));
                const bool plain = b.cond.empty() && b.hits.empty();
                r.mark = plain ? "\xE2\x97\x8F" : "\xE2\x97\x86";  // (the gutter's dot, or diamond)
                r.mark_ink = ink::kError;
                if (!b.cond.empty()) r.after = "if " + b.cond;
                if (!b.hits.empty()) r.after += (r.after.empty() ? "" : ", ") + std::string("hit ") + b.hits;
                if (focused && u.sel == out.size() - 1) r.key = key_symbols("Shift+F9", font);  // (the one chosen: what changes its condition)
            }
    }
    if (!u.output.empty() || !u.partial.empty()) {
        heading("Output", 0);
        const size_t from = u.output.size() > 200 ? u.output.size() - 200 : 0;
        for (size_t i = from; i < u.output.size(); ++i) add(DebugUi::kNone, 0, u.output[i]);
        if (!u.partial.empty()) add(DebugUi::kNone, 0, u.partial);
    }
    // The chosen row: one that does something.
    if (u.sel >= out.size() || out[u.sel].act == DebugUi::kNone) {
        u.sel = 0;
        for (u32 i = 0; i < out.size(); ++i)
            if (out[i].act != DebugUi::kNone) {
                u.sel = i;
                break;
            }
    }
    const u32 body = rows > 2 ? rows - 2 : 0;
    if (u.sel < u.top) u.top = u.sel;
    if (body && u.sel >= u.top + body) u.top = u.sel - body + 1;
    if (out.size() <= body) u.top = 0;
    // (A field's heading and value are lit together.)
    const DebugUi::Act chosen = u.sel < out.size() ? out[u.sel].act : DebugUi::kNone;
    const bool field = chosen == DebugUi::kProgram || chosen == DebugUi::kArgs || chosen == DebugUi::kCwd;
    for (u32 r = 0; r < body && u.top + r < out.size(); ++r) {
        const Row& row = out[u.top + r];
        u32 c = row.indent;
        if (!row.mark.empty()) c = put(box, font, 2 + r, c, row.mark, row.mark_ink) + 1;
        const u32 key_w = row.key.empty() ? 0 : width_of(row.key) + 2;
        const u32 room = cols > c + key_w ? cols - c - key_w : 0;
        c = put(box, font, 2 + r, c, fit_text(row.text, room), row.ink);
        if (!row.after.empty() && c + 3 < cols - key_w) put(box, font, 2 + r, c + 1, fit_text(row.after, cols - key_w - c - 1), ink::kMuted);
        if (key_w && key_w < cols) put(box, font, 2 + r, cols - key_w + 2, row.key, ink::kMuted);
        if (u.top + r == u.sel || (field && row.act == chosen)) light_row(box, 2 + r, focused);
    }
}

// What a row of the Debug tab does.
void App::debug_do(u32 i) {
    DebugUi& u = *debugui_;
    if (i >= u.rows.size()) return;
    const DebugUi::Row row = u.rows[i];
    u.sel = i;
    ++debug_gen_;
    dirty_ = true;
    auto field = [&](const char* prompt, std::string* into, int mode) {
        std::string start = *into;
        if (mode != kFindInput && start.empty()) {
            const std::string* root = panel_root();
            start = (root ? *root : default_workdir()) + "/";
        }
        if (const char* home = SDL_getenv("HOME"); mode != kFindInput && home && *home && start.starts_with(home)) start = "~" + start.substr(std::strlen(home));
        find_choose(mode, prompt, start, [this, into](const std::string& v) {
            *into = v;
            ++debug_gen_;
            dirty_ = true;
        });
    };
    switch (row.act) {
        case DebugUi::kProgram: field("Program to debug", &u.program, kFindOpen); break;
        case DebugUi::kArgs: field("Arguments", &u.args, kFindInput); break;
        case DebugUi::kCwd: field("Working directory", &u.cwd, kFindFolder); break;
        case DebugUi::kStart:
        case DebugUi::kContinue: debug_start(); break;
        case DebugUi::kPause: debug_command("pause"); break;
        case DebugUi::kOver: debug_command("next"); break;
        case DebugUi::kInto: debug_command("stepIn"); break;
        case DebugUi::kOut: debug_command("stepOut"); break;
        case DebugUi::kStop: debug_command("stop"); break;
        case DebugUi::kThread: debug_thread(row.index); break;
        case DebugUi::kFrame: debug_frame(row.index, true); break;
        case DebugUi::kVar: debug_expand(row.index); break;
        case DebugUi::kWatch:
        case DebugUi::kWatchAdd: {  // an expression to watch, or one watched changed (nothing typed: it goes)
            const bool fresh = row.act == DebugUi::kWatchAdd;
            const u32 at = row.index;
            find_choose(kFindInput, "Watch", fresh || at >= u.watches.size() ? std::string() : u.watches[at].expr, [this, fresh, at](const std::string& typed) {
                DebugUi& v = *debugui_;
                const std::string e = trimmed(typed);
                if (fresh && !e.empty()) v.watches.push_back({e, {}, false});
                else if (!fresh && at < v.watches.size() && e.empty()) v.watches.erase(v.watches.begin() + at);
                else if (!fresh && at < v.watches.size()) v.watches[at] = {e, {}, false};
                debug_watch_eval();
                ++debug_gen_;
                dirty_ = true;
            });
            break;
        }
        case DebugUi::kBreak: {  // the breakpoint's line
            u32 n = 0;
            for (const auto& [path, lines] : u.breaks)
                for (const auto& [l, b] : lines)
                    if (n++ == row.index && open_path(path.c_str()) && !pane_->doc->loading) {
                        pane_->doc->ed.move_to(pane_->doc->ed.buf.line_start(std::min<u64>(l, pane_->doc->ed.buf.line_count() - 1)), false);
                        scroll_to_cursor();
                    }
            break;
        }
        case DebugUi::kNone: break;
    }
}

void App::debug_panel_click(int row, int col, const SDL_MouseButtonEvent& b) {
    (void)col;
    DebugUi& u = *debugui_;
    if (b.button != SDL_BUTTON_LEFT || row < 2) return;
    debug_do(u.top + static_cast<u32>(row - 2));
}

bool App::debug_panel_key(const SDL_KeyboardEvent& k) {
    DebugUi& u = *debugui_;
    const u32 n = static_cast<u32>(u.rows.size());
    auto step = [&](int by) {
        u32 at = u.sel;
        for (u32 tries = 0; tries < n; ++tries) {
            at = static_cast<u32>((static_cast<int>(at) + by + static_cast<int>(n)) % static_cast<int>(n));
            const DebugUi::Row& r = u.rows[at];
            const bool same_field = r.act == u.rows[u.sel].act && (r.act == DebugUi::kProgram || r.act == DebugUi::kArgs || r.act == DebugUi::kCwd);
            if (r.act != DebugUi::kNone && !same_field) break;
        }
        u.sel = at;
    };
    if (n == 0) return false;
    switch (k.key) {
        case SDLK_UP: step(-1); break;
        case SDLK_DOWN: step(1); break;
        case SDLK_HOME:  // the first row that does something, or the last
        case SDLK_END:
            u.sel = k.key == SDLK_HOME ? n - 1 : 0;
            step(k.key == SDLK_HOME ? 1 : -1);
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE: debug_do(u.sel); return true;
        case SDLK_RIGHT:
        case SDLK_LEFT:  // a variable opened or shut
            if (u.sel < n && u.rows[u.sel].act == DebugUi::kVar) {
                const DebugUi::Var& v = u.vars[u.rows[u.sel].index];
                if (v.open == (k.key == SDLK_LEFT)) debug_expand(u.rows[u.sel].index);
            }
            break;
        case SDLK_DELETE:
        case SDLK_BACKSPACE:  // a breakpoint taken away, or an expression no longer watched
            if (u.sel < n && u.rows[u.sel].act == DebugUi::kWatch && u.rows[u.sel].index < u.watches.size()) {
                u.watches.erase(u.watches.begin() + u.rows[u.sel].index);
                break;
            }
            if (u.sel < n && u.rows[u.sel].act == DebugUi::kBreak) {
                u32 i = 0;
                for (auto it = u.breaks.begin(); it != u.breaks.end(); ++it)
                    for (const auto& [l, b] : it->second)
                        if (i++ == u.rows[u.sel].index) {
                            const std::string path = it->first;
                            it->second.erase(l);
                            if (it->second.empty()) {
                                u.breaks.erase(it);
                                u.unfollow(*this, path);
                            }
                            debug_send_breaks(path);
                            ++debug_gen_;
                            dirty_ = true;
                            return true;
                        }
            }
            return true;
        default: return false;
    }
    ++debug_gen_;
    dirty_ = true;
    return true;
}

// ---------------------------------------------------------------- the stopped line, in the text

void App::debug_marks(FrameDesc& fd) {
    if (!debugui_ || debugui_->state != DebugUi::kStopped) return;
    DebugUi& u = *debugui_;
    if (u.frame >= u.frames.size()) return;
    const DebugUi::Frame& f = u.frames[u.frame];
    Font& font = *font_;
    for (u32 vi = 0; vi < fd.view_count && vi < kMaxViews; ++vi) {
        GridSpec& g = fd.views[vi].grid;
        Doc* d = find_doc(g.doc);
        if (!d || d->ed.path.empty() || full_path(d->ed.path) != f.path || f.line == 0) continue;
        const u64 l = f.line - 1;
        if (l < g.top_line || l >= g.top_line + g.rows || l >= d->ed.buf.line_count()) continue;
        TextBox box;
        const std::string text = "\xE2\x97\x80 stopped" + (u.reason.empty() ? std::string() : ": " + u.reason);
        box.resize(std::min<u32>(width_of(text), 80), 1);
        put(box, font, 0, 0, text, ink::kAccent);  // (upright: the font's italic may be a script)
        debug_note_cells_[vi] = std::move(box.cells);
        std::vector<GridNote>& notes = debug_notes_[vi];
        notes.clear();
        bool taken = false;
        for (const GridNote& n : g.notes) {
            notes.push_back(n);
            taken |= n.line == l;
        }
        if (!taken) {
            const u64 start = d->ed.buf.line_start(l), end = d->ed.line_end(l);
            notes.push_back({l, d->ed.col_between(start, end) + 3, debug_note_cells_[vi].data(), static_cast<u32>(debug_note_cells_[vi].size())});
        }
        g.notes = notes;
    }
}

// ---------------------------------------------------------------- for the tests

void App::debug_set(const std::string& what, const std::string& value) {
    DebugUi& u = *debugui_;
    if (what == "program") u.program = full_path(value);
    else if (what == "args") u.args = value;
    else if (what == "cwd") u.cwd = full_path(value);
    ++debug_gen_;
}

// Waits (20 s at most) until the session is stopped, or over.
void App::debug_wait(const std::string& state) {
    const u64 until = now_ns() + 20'000'000'000ull;
    DebugUi& u = *debugui_;
    while (now_ns() < until) {
        debug_sync();
        const bool frames = !u.frames.empty() && !u.vars.empty() && u.asking == 0;  // (the stack, and the first scope's variables)
        if (state == "stopped" ? u.state == DebugUi::kStopped && frames : u.state == DebugUi::kIdle) break;
        SDL_Delay(5);
    }
    // (A little longer for the variables' answers.)
    for (int i = 0; i < 20; ++i) {
        debug_sync();
        SDL_Delay(5);
    }
}

std::string App::debug_dump() {
    DebugUi& u = *debugui_;
    static const char* const kState[] = {"idle", "starting", "running", "stopped"};
    std::string out = std::string(kState[u.state]);
    if (!u.reason.empty() && u.state == DebugUi::kStopped) out += " (" + u.reason + ")";
    if (!u.error.empty()) out += " error: " + u.error;
    if (u.threads.size() > 1) {
        u32 at = 0;
        for (u32 i = 0; i < u.threads.size(); ++i)
            if (u.threads[i].id == u.thread) at = i;
        out += "; threads: " + std::to_string(u.threads.size()) + ", #" + std::to_string(at + 1) + " shown";
    }
    if (!u.frames.empty()) {
        out += "; stack:";
        for (size_t i = 0; i < u.frames.size() && i < 2; ++i) out += " " + u.frames[i].name + (u.frames[i].path.empty() ? "" : "@" + std::to_string(u.frames[i].line));
    }
    if (!u.vars.empty()) {
        out += "; vars:";
        for (const DebugUi::Var& v : u.vars)
            if (v.depth > 0) out += " " + v.name + "=" + v.value.substr(0, v.value.find("  ("));
    }
    if (!u.watches.empty()) {
        out += "; watch:";
        for (size_t i = 0; i < u.watches.size(); ++i)
            out += (i ? " | " : " ") + u.watches[i].expr + (u.watches[i].value.empty() ? "" : u.watches[i].failed ? " (" + u.watches[i].value + ")" : " = " + u.watches[i].value);
    }
    if (!u.output.empty()) {  // (its last four lines)
        out += "; output:";
        for (size_t i = u.output.size() > 4 ? u.output.size() - 4 : 0; i < u.output.size(); ++i) out += " " + u.output[i] + (i + 1 < u.output.size() ? " |" : "");
    }
    return out;
}

// The breakpoints: each file's name and line, with what it waits for.
std::string App::debug_breaks_dump() {
    DebugUi& u = *debugui_;
    std::string out;
    for (const auto& [path, lines] : u.breaks)
        for (const auto& [l, b] : lines) {
            out += (out.empty() ? "" : "; ") + base_name(path) + ":" + std::to_string(l + 1);
            if (!b.cond.empty()) out += " if " + b.cond;
            if (!b.hits.empty()) out += " hit " + b.hits;
        }
    return out.empty() ? "no breakpoints" : out;
}

// What Enter does on the Nth row (from 1) of a section of the Debug tab as
// it was last drawn: threads, stack, watch, add (the row that adds an
// expression), or breaks. The row is chosen first, as the keys would.
bool App::debug_pick(const std::string& section, u32 n) {
    DebugUi& u = *debugui_;
    const DebugUi::Act act = section == "threads" ? DebugUi::kThread : section == "stack" ? DebugUi::kFrame : section == "watch" ? DebugUi::kWatch
                             : section == "add"   ? DebugUi::kWatchAdd : section == "breaks" ? DebugUi::kBreak : DebugUi::kNone;
    u32 seen = 0;
    for (u32 i = 0; i < u.rows.size(); ++i)
        if (act != DebugUi::kNone && u.rows[i].act == act && ++seen == std::max(1u, n)) {
            u.sel = i;
            debug_do(i);
            return true;
        }
    return false;
}

}  // namespace blit
