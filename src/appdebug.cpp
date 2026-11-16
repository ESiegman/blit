// Debugging in the editor (dap.hpp talks to the debugger): breakpoints in
// the gutter (F9), a session started with F5 (lldb-dap on a Mac, gdb or
// lldb-dap on Linux, debugpy for Python; here, or on the machine an
// ssh:// program is on), the line it stopped on, and the side panel's Debug
// tab: the program, the controls, the call stack, the variables (opened a
// level at a time), the breakpoints and what the program printed.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

#include "app.hpp"
#include "dap.hpp"
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

// The debugger for a machine, chosen there: Xcode's lldb-dap on a Mac; gdb
// (14 and later speak the protocol) or lldb-dap elsewhere; debugpy for Python.
const char* kNative =
    "if [ \"$(uname -s)\" = Darwin ]; then "
    "xcrun --find lldb-dap >/dev/null 2>&1 || { [ -d /Applications/Xcode.app/Contents/Developer ] && DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer && export DEVELOPER_DIR; }; "
    "exec xcrun lldb-dap; fi; "
    "if command -v gdb >/dev/null 2>&1 && [ \"$(gdb --version | sed -n '1s/.* \\([0-9][0-9]*\\)\\..*/\\1/p')\" -ge 14 ] 2>/dev/null; then exec gdb -q -i dap; fi; "
    "for d in lldb-dap lldb-vscode; do command -v $d >/dev/null 2>&1 && exec $d; done; "
    "echo 'no debugger: install gdb 14 or newer, or lldb' >&2; exit 127";
const char* kPython = "exec python3 -m debugpy.adapter";

}  // namespace

struct App::DebugUi {
    // What to debug.
    std::string program, args, cwd;
    // Breakpoints: by file (its full path, ssh:// for another machine's), lines from 0.
    std::map<std::string, std::set<u32>> breaks;
    // The session.
    std::unique_ptr<Dap> dap;
    enum State : u8 { kIdle, kStarting, kRunning, kStopped } state = kIdle;
    std::string remote;  // "ssh://host" when the program is on another machine
    i64 thread = 0;
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
    std::vector<std::string> output;
    std::string partial;
    int exit_code = -1;
    bool end_soon = false;  // (the session ends once the debugger's answers have been run: not inside one)
    // The panel: a row for each thing shown, and the one chosen.
    enum Act : u8 { kNone, kProgram, kArgs, kCwd, kStart, kContinue, kPause, kOver, kInto, kOut, kStop, kFrame, kVar, kBreak };
    struct Row {
        Act act = kNone;
        u32 index = 0;
        std::string text;
        u32 ink = 0, style = 0, indent = 0;
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

void App::debug_create() { debugui_ = std::make_shared<DebugUi>(); }

bool App::debug_running() const { return debugui_ && debugui_->state != DebugUi::kIdle; }

// ---------------------------------------------------------------- breakpoints

void App::debug_toggle_break() {
    DebugUi& u = *debugui_;
    Doc& d = *pane_->doc;
    if (d.ed.path.empty() || d.term || d.prof) return;
    const std::string path = full_path(d.ed.path);
    const u32 line = static_cast<u32>(d.ed.buf.line_of(d.ed.cursor()));
    std::set<u32>& lines = u.breaks[path];
    if (!lines.erase(line)) lines.insert(line);
    if (lines.empty()) u.breaks.erase(path);
    debug_send_breaks(path);
    ++debug_gen_;
    dirty_ = true;
}

// A file's breakpoints, to a session under way.
void App::debug_send_breaks(const std::string& path) {
    DebugUi& u = *debugui_;
    if (!u.dap || u.state == DebugUi::kStarting) return;  // (all are sent when the debugger is ready for them)
    std::string a = "{\"source\":{\"path\":" + quoted(u.there(path)) + "},\"breakpoints\":[";
    if (auto it = u.breaks.find(path); it != u.breaks.end()) {
        bool first = true;
        for (const u32 l : it->second) a += std::string(first ? "" : ",") + "{\"line\":" + std::to_string(l + 1) + "}", first = false;
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
    const std::string path = full_path(d.ed.path);
    if (u.state == DebugUi::kStopped && u.frame < u.frames.size() && u.frames[u.frame].path == path && u.frames[u.frame].line == line + 1)
        return (font_->glyph(0x25B6) & (Font::kWide - 1)) | (kInkAccent << kCellInkShift);
    if (auto it = u.breaks.find(path); it != u.breaks.end() && it->second.contains(static_cast<u32>(line)))
        return (font_->glyph(0x25CF) & (Font::kWide - 1)) | (kInkDiagnostic << kCellInkShift);
    return 0;
}

// ---------------------------------------------------------------- a session

void App::debug_start() {
    DebugUi& u = *debugui_;
    if (u.state == DebugUi::kStopped) return debug_command("continue");
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
    u.frames.clear();
    u.vars.clear();
    u.output.clear();
    u.partial.clear();
    u.error.clear();
    u.reason.clear();
    u.exit_code = -1;
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

// The session over: the debugger let go of (and the program with it).
void App::debug_end() {
    DebugUi& u = *debugui_;
    if (u.dap && !u.dap->ended()) {
        u.dap->request("disconnect", "{\"terminateDebuggee\":true}");
        u.dap->stop();
    }
    u.dap.reset();
    u.state = DebugUi::kIdle;
    u.frames.clear();
    u.vars.clear();
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
    u.vars.clear();
    ++u.vars_gen;
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
    u.dap->request("scopes", "{\"frameId\":" + std::to_string(f.id) + "}", [this, gen](const Dap::Message& m) {
        DebugUi& v = *debugui_;
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
    ++debug_gen_;
    dirty_ = true;
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
    u.dap->request("variables", "{\"variablesReference\":" + std::to_string(ref) + "}", [this, gen, ref, depth](const Dap::Message& m) {
        DebugUi& v = *debugui_;
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
            for (const auto& [path, lines] : u.breaks) debug_send_breaks(path);
            u.dap->request("setExceptionBreakpoints", "{\"filters\":[]}");
            u.dap->request("configurationDone");
        } else if (m.name == "stopped") {
            u.state = DebugUi::kStopped;
            u.thread = b["threadId"].integer(u.thread);
            u.reason = std::string(b["reason"].str());
            if (const std::string_view text = b["text"].str(); !text.empty()) u.reason += ": " + std::string(text);
            u.frames.clear();
            u.vars.clear();
            ++u.vars_gen;
            u.dap->request("stackTrace", "{\"threadId\":" + std::to_string(u.thread) + ",\"startFrame\":0,\"levels\":64}", [this](const Dap::Message& r) {
                DebugUi& v = *debugui_;
                if (!r.success || v.state != DebugUi::kStopped) return;
                v.frames.clear();
                for (json::Value f : r.body()["stackFrames"])
                    v.frames.push_back({f["id"].integer(), std::string(f["name"].str()), v.here(f["source"]["path"].str()), static_cast<u32>(f["line"].integer())});
                // The first frame with source to show (the program's own, not a library's).
                u32 first = 0;
                while (first < v.frames.size() && v.frames[first].path.empty()) ++first;
                debug_frame(first < v.frames.size() ? first : 0, true);
            });
        } else if (m.name == "continued") {
            u.state = DebugUi::kRunning;
            u.frames.clear();
            u.vars.clear();
        } else if (m.name == "output") {
            const std::string_view cat = b["category"].str();
            if (cat != "telemetry" && cat != "important") u.print(b["output"].str());
        } else if (m.name == "exited") {
            u.exit_code = static_cast<int>(b["exitCode"].integer(-1));
            u.print("[exited with " + std::to_string(u.exit_code) + "]\n");
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
        v.dap->request("evaluate", "{\"expression\":" + quoted(e) + frame + ",\"context\":\"watch\"}", [this, e](const Dap::Message& m) {
            DebugUi& w = *debugui_;
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
    auto add = [&](DebugUi::Act act, u32 index, std::string text, u32 ink = 0, u32 style = 0, u32 indent = 0) { out.push_back({act, index, std::move(text), ink, style, indent}); };
    // What it is doing.
    static const char* const kState[] = {"Not running", "Starting\xE2\x80\xA6", "Running", "Stopped"};
    std::string status = kState[u.state];
    if (u.state == DebugUi::kStopped && !u.reason.empty()) status += " (" + u.reason + ")";
    if (u.state == DebugUi::kIdle && u.exit_code >= 0) status += ": it exited with " + std::to_string(u.exit_code);
    box.put(font, 1, 0, status, u.state == DebugUi::kStopped ? kInkAccent : kInkMuted);
    auto shown = [](const std::string& p) {
        std::string s = p;
        if (const char* home = SDL_getenv("HOME"); home && *home && s.starts_with(home)) s = "~" + s.substr(std::strlen(home));
        return s;
    };
    add(DebugUi::kProgram, 0, "Program", kInkMuted);
    add(DebugUi::kProgram, 0, u.program.empty() ? "Choose a program\xE2\x80\xA6" : shown(u.program), u.program.empty() ? kInkMuted : 0, u.program.empty() ? Font::kItalic : 0, 2);
    add(DebugUi::kArgs, 0, "Arguments", kInkMuted);
    add(DebugUi::kArgs, 0, u.args.empty() ? "None" : u.args, u.args.empty() ? kInkMuted : 0, u.args.empty() ? Font::kItalic : 0, 2);
    add(DebugUi::kCwd, 0, "Working directory", kInkMuted);
    add(DebugUi::kCwd, 0, u.cwd.empty() ? "The program's folder" : shown(u.cwd), u.cwd.empty() ? kInkMuted : 0, u.cwd.empty() ? Font::kItalic : 0, 2);
    add(DebugUi::kNone, 0, "");
    if (!u.error.empty()) add(DebugUi::kNone, 0, u.error, kInkDiagnostic, Font::kItalic, 1);
    if (u.state == DebugUi::kIdle) {
        add(DebugUi::kStart, 0, "\xE2\x96\xB6 Start  (F5)", kInkAccent, Font::kBold, 1);
    } else if (u.state == DebugUi::kStopped) {
        add(DebugUi::kContinue, 0, "\xE2\x96\xB6 Continue  (F5)", kInkAccent, Font::kBold, 1);
        add(DebugUi::kOver, 0, "\xE2\x86\xB7 Step over  (F10)", 0, 0, 1);
        add(DebugUi::kInto, 0, "\xE2\x86\x93 Step into  (F11)", 0, 0, 1);
        add(DebugUi::kOut, 0, "\xE2\x86\x91 Step out  (Shift+F11)", 0, 0, 1);
        add(DebugUi::kStop, 0, "\xE2\x96\xA0 Stop  (Shift+F5)", kInkDiagnostic, 0, 1);
    } else {
        add(DebugUi::kPause, 0, "\xE2\x9D\x9A\xE2\x9D\x9A Pause  (F6)", 0, 0, 1);
        add(DebugUi::kStop, 0, "\xE2\x96\xA0 Stop  (Shift+F5)", kInkDiagnostic, 0, 1);
    }
    if (!u.frames.empty()) {
        add(DebugUi::kNone, 0, "");
        add(DebugUi::kNone, 0, "Call stack", kInkMuted);
        for (u32 i = 0; i < u.frames.size(); ++i) {
            const DebugUi::Frame& f = u.frames[i];
            add(DebugUi::kFrame, i, (i == u.frame ? "\xE2\x96\xB8 " : "  ") + f.name + (f.path.empty() ? "" : "  " + base_name(f.path) + ":" + std::to_string(f.line)),
                f.path.empty() ? kInkMuted : 0, i == u.frame ? Font::kBold : 0, 1);
        }
    }
    if (!u.vars.empty()) {
        add(DebugUi::kNone, 0, "");
        add(DebugUi::kNone, 0, "Variables", kInkMuted);
        for (u32 i = 0; i < u.vars.size(); ++i) {
            const DebugUi::Var& v = u.vars[i];
            const std::string mark = v.ref > 0 ? (v.open ? "\xE2\x96\xBE " : "\xE2\x96\xB8 ") : "  ";
            add(DebugUi::kVar, i, mark + v.name + (v.value.empty() ? "" : " = " + v.value), v.depth == 0 ? kInkMuted : 0, 0, 1 + 2 * v.depth);
        }
    }
    if (!u.breaks.empty()) {
        add(DebugUi::kNone, 0, "");
        add(DebugUi::kNone, 0, "Breakpoints", kInkMuted);
        u32 n = 0;
        for (const auto& [path, lines] : u.breaks)
            for (const u32 l : lines) add(DebugUi::kBreak, n++, "\xE2\x97\x8F " + base_name(path) + ":" + std::to_string(l + 1), 0, 0, 1);
    }
    if (!u.output.empty() || !u.partial.empty()) {
        add(DebugUi::kNone, 0, "");
        add(DebugUi::kNone, 0, "Output", kInkMuted);
        const size_t from = u.output.size() > 200 ? u.output.size() - 200 : 0;
        for (size_t i = from; i < u.output.size(); ++i) add(DebugUi::kNone, 0, u.output[i], 0, 0, 1);
        if (!u.partial.empty()) add(DebugUi::kNone, 0, u.partial, 0, 0, 1);
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
    for (u32 r = 0; r < body && u.top + r < out.size(); ++r) {
        const Row& row = out[u.top + r];
        std::string text = row.text;
        const u32 room = cols > row.indent ? cols - row.indent : 1;
        if (width_of(text) > room) {
            size_t at = 0;
            for (u32 k = 0; k + 1 < room && at < text.size(); ++k) at += decode_utf8(reinterpret_cast<const u8*>(text.data()) + at, text.size() - at).len;
            text = text.substr(0, at) + "\xE2\x80\xA6";
        }
        box.put(font, 2 + r, row.indent, text, row.ink, row.style);
        if (focused && u.top + r == u.sel) box.select_row(2 + r);
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
        case DebugUi::kFrame: debug_frame(row.index, true); break;
        case DebugUi::kVar: debug_expand(row.index); break;
        case DebugUi::kBreak: {  // the breakpoint's line
            u32 n = 0;
            for (const auto& [path, lines] : u.breaks)
                for (const u32 l : lines)
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
        case SDLK_BACKSPACE:  // a breakpoint taken away
            if (u.sel < n && u.rows[u.sel].act == DebugUi::kBreak) {
                u32 i = 0;
                for (auto it = u.breaks.begin(); it != u.breaks.end(); ++it)
                    for (const u32 l : it->second)
                        if (i++ == u.rows[u.sel].index) {
                            const std::string path = it->first;
                            it->second.erase(l);
                            if (it->second.empty()) u.breaks.erase(it);
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
        box.put(font, 0, 0, text, kInkAccent, Font::kItalic);
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
        const bool frames = !u.frames.empty() && !u.vars.empty() && u.vars.size() > 1;
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
    if (!u.frames.empty()) {
        out += "; stack:";
        for (size_t i = 0; i < u.frames.size() && i < 2; ++i) out += " " + u.frames[i].name + (u.frames[i].path.empty() ? "" : "@" + std::to_string(u.frames[i].line));
    }
    if (!u.vars.empty()) {
        out += "; vars:";
        for (const DebugUi::Var& v : u.vars)
            if (v.depth > 0) out += " " + v.name + "=" + v.value.substr(0, v.value.find("  ("));
    }
    if (!u.output.empty()) {  // (its last four lines)
        out += "; output:";
        for (size_t i = u.output.size() > 4 ? u.output.size() - 4 : 0; i < u.output.size(); ++i) out += " " + u.output[i] + (i + 1 < u.output.size() ? " |" : "");
    }
    return out;
}

}  // namespace blit
