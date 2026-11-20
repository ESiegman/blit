// Debugging in the editor (dap.hpp talks to the debugger): breakpoints in
// the gutter (F9; with a condition or a hit count, Shift+F9), which move
// with their lines as the file is edited; a session started with F5
// (lldb-dap on a Mac, gdb or lldb-dap on Linux, debugpy for Python; here,
// or on the machine an ssh:// program is on), the line it stopped on, and
// the side panel's Debug tab: the program, the controls, the threads, the
// call stack of the one chosen, the variables (opened a level at a time),
// the expressions watched, the breakpoints and what the program printed.
// And, in tabs of their own, the instructions where it stopped (stepped one
// at a time there, with breakpoints at an address) and memory, a page at a
// time.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include <unistd.h>

#include <cctype>

#include "app.hpp"
#include "base64.hpp"
#include "dap.hpp"
#include "fsio.hpp"
#include "highlight.hpp"
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
// A program with HIP's code for the GPU in it ($BLIT_DEBUG_PROGRAM has a
// .hip_fatbin) gets ROCm's rocgdb where there is one: a gdb that sees the
// GPU's waves as threads, its kernels' frames and variables.
// gdb's program writes where gdb's messages go, a pipe, and C's stdio holds
// back what is written to a pipe until it ends: stdbuf (coreutils) has it
// write a line at a time, as to a terminal, gdb and the program both.
const char* kNative =
    "if [ \"$(uname -s)\" = Darwin ]; then "
    "xcrun --find lldb-dap >/dev/null 2>&1 || { [ -d /Applications/Xcode.app/Contents/Developer ] && DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer && export DEVELOPER_DIR; }; "
    "exec xcrun lldb-dap; fi; "
    "if [ -n \"$BLIT_DEBUG_PROGRAM\" ] && grep -q -a -m1 hip_fatbin \"$BLIT_DEBUG_PROGRAM\" 2>/dev/null; then "
    "for d in rocgdb /opt/rocm/bin/rocgdb; do if command -v $d >/dev/null 2>&1; then echo 'blit-debugger: gdb (rocgdb)' >&2; command -v stdbuf >/dev/null 2>&1 && exec stdbuf -oL $d -q -i dap; exec $d -q -i dap; fi; done; fi; "
    "if command -v gdb >/dev/null 2>&1 && [ \"$(gdb --version | sed -n '1s/.* \\([0-9][0-9]*\\)\\..*/\\1/p')\" -ge 14 ] 2>/dev/null; then echo 'blit-debugger: gdb' >&2; command -v stdbuf >/dev/null 2>&1 && exec stdbuf -oL gdb -q -i dap; exec gdb -q -i dap; fi; "
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

// An address as the debugger writes it ("0x1000004FC") or a number: 0 where it is neither.
u64 address_of(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    u64 base = 10;
    if (s.starts_with("0x") || s.starts_with("0X")) s.remove_prefix(2), base = 16;
    u64 v = 0;
    for (const char c : s) {
        const int d = c >= '0' && c <= '9' ? c - '0' : base == 16 && c >= 'a' && c <= 'f' ? c - 'a' + 10 : base == 16 && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) break;
        v = v * base + static_cast<u64>(d);
    }
    return v;
}

// What gdb says as it puts a breakpoint somewhere ("Breakpoint 2 at
// 0x5555...: file count.c, line 7.", "Breakpoint 1 (-source ...) pending.").
bool gdb_breakpoint_said(std::string_view s) {
    if (!s.starts_with("Breakpoint ")) return false;
    s.remove_prefix(11);
    size_t n = 0;
    while (n < s.size() && std::isdigit(static_cast<unsigned char>(s[n]))) ++n;
    return n > 0 && (s.substr(n).starts_with(" at 0x") || s.substr(n).starts_with(" ("));
}

// Memory is read in pieces this big where a page cannot be read whole.
constexpr u64 kMemPiece = 256;

// The bytes a readMemory answer has, into those of the page from `addr`
// (from `at` in it, `count` of them): -1 where they could not be read.
void memory_take(std::vector<i32>& now, u64 addr, const Dap::Message& m, u64 at, u64 count) {
    const std::string data = from_base64(m.body()["data"].str());
    const u64 start = data.empty() ? addr + at : address_of(m.body()["address"].str());
    for (u64 i = at; i < at + count && i < now.size(); ++i)
        if (addr + i >= start && addr + i - start < data.size()) now[i] = static_cast<u8>(data[addr + i - start]);
}

std::string hex(u64 v, int width = 1) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%0*llx", width, static_cast<unsigned long long>(v));
    return b;
}

}  // namespace

// One debugging session: its debugger and what it says, the program's
// threads, stack, variables and output, and what it has of its own (data
// and instruction breakpoints, run to the cursor). The one shown is the
// Debug tab's (App::DebugUi is one); the others wait in DebugUi::others,
// each swapped into its place to be shown or to hear from its debugger.
struct DebugSession {
    std::string name;  // what it debugs: the program, a process, a core
    // The session, and what its debugger says it can do (initialize's answer).
    std::unique_ptr<Dap> dap;
    struct Can {
        bool data_breaks = false, set_variable = false, goto_targets = false, function_breaks = false, log_points = false;
        bool disassemble = false, read_memory = false, write_memory = false, instruction_breaks = false, granularity = false;
    } can;
    std::string how;  // what the session began with: "launch", "attach" (a process), "core"
    enum State : u8 { kIdle, kStarting, kRunning, kStopped } state = kIdle;
    std::string remote;  // "ssh://host" when the program is on another machine
    bool gdb = false;    // the debugger is gdb (its hit counts are from 0)
    bool rocm = false;   // and ROCm's rocgdb: the GPU's waves are threads, named by `info threads`
    bool going = false;  // the program has started (the "process" event): what gdb says before is its own
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
        u64 ip = 0;              // the address of its instruction
    };
    std::vector<Frame> frames;
    u32 frame = 0;
    struct Var {
        std::string name, value;
        i64 ref = 0;
        u32 depth = 0;
        bool open = false;
        i64 parent = 0;  // (the reference it was listed under: what setVariable and dataBreakpointInfo name it by)
        u64 addr = 0;    // where it is in memory, where the debugger says
    };
    // Data breakpoints: a variable's writes watched, by the debugger's id for it.
    struct Data {
        std::string id, name;
    };
    std::vector<Data> data;
    // Run to the cursor: a breakpoint there until the next stop.
    std::string run_to_path;
    u32 run_to_line = 0;
    std::set<u64> ibreaks;  // breakpoints at an instruction, by its address
    std::vector<Var> vars;
    u64 vars_gen = 0;  // (answers about variables from before the last stop are dropped)
    u64 watch_gen = 0;  // (answers from before the last evaluation are dropped)
    u32 asking = 0;    // questions about the stack and the variables not answered yet (debug_wait)
    std::vector<std::string> output;
    std::string partial;
    int exit_code = -1;
    bool end_soon = false;  // (the session ends once the debugger's answers have been run: not inside one)
    // A terminal's escape sequences (colours, mostly) are left out, as the
    // build's scanner leaves them out; one may be cut between two events.
    enum Esc : u8 { kText, kEsc, kEscMore, kCsi, kString, kStringEsc } esc = kText;
    void print(std::string_view text) {
        for (const char ch : text) {
            const u8 c = static_cast<u8>(ch);
            switch (esc) {
                case kText:
                    if (c == 0x1B) esc = kEsc;
                    else if (c != '\r') partial += ch;  // (a program on a terminal ends its lines with \r\n)
                    break;
                case kEsc:
                    esc = c == '[' ? kCsi : (c == ']' || c == 'P' || c == '_' || c == '^' || c == 'X') ? kString : (c >= 0x20 && c <= 0x2F) ? kEscMore : kText;
                    break;
                case kEscMore:  // ESC ( B and the like
                    if (c >= 0x30) esc = kText;
                    break;
                case kCsi:
                    if (c >= 0x40 && c <= 0x7E) esc = kText;
                    break;
                case kString:  // OSC (a title, a link) and the like: to BEL or ESC backslash
                    if (c == 0x07) esc = kText;
                    else if (c == 0x1B) esc = kStringEsc;
                    break;
                case kStringEsc: esc = c == '\\' ? kText : kString; break;
            }
        }
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

struct App::DebugUi : DebugSession {
    // What to debug.
    std::string program, args, cwd;
    std::vector<std::string> env;  // ("NAME=value": a run configuration's, given the debugger and so the program)
    // Breakpoints: by file (its full path, ssh:// for another machine's), by
    // line (from 0), each with what it waits for, if anything.
    struct Break {
        std::string cond;  // it stops only where this is true
        std::string hits;  // and only at this hit (a number: the Nth time it is reached)
        std::string log;   // a logpoint: this said (its {expressions} filled in) where it would stop, and no stop
        bool operator==(const Break&) const = default;
    };
    using Lines = std::map<u32, Break>;
    std::map<std::string, Lines> breaks;
    std::vector<std::string> functions;  // breakpoints by a function's name
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
    // The disassembly tab (its document's id; 0 when there is none): the
    // instructions around the frame shown's, a line each, and before those of
    // each source line a heading (its function, file and line, and its text).
    // The address of each line, 0 for a heading's.
    u64 disasm_doc = 0;
    std::vector<u64> disasm_addrs;
    u64 disasm_gen = 0;
    // The memory tab: a page of memory from mem_addr, 16 bytes a line, read
    // again at each stop; what it was asked for as.
    u64 mem_doc = 0, mem_addr = 0, mem_gen = 0;
    std::string mem_name;
    std::vector<i32> mem_last;  // (each byte as last read, -1 where it could not be)
    u64 mem_last_addr = 0;
    u32 mem_width = 8;  // (the hex digits of its addresses)
    static constexpr u32 kMemPage = 4096;
    // The expressions watched: each evaluated where it stopped, in the frame shown.
    struct Watch {
        std::string expr, value;
        bool failed = false;
    };
    std::vector<Watch> watches;
    // The panel: a row for each thing shown, and the one chosen.
    enum Act : u8 { kNone, kProgram, kArgs, kCwd, kStart, kContinue, kPause, kOver, kInto, kOut, kStop, kThread, kFrame, kVar, kWatch, kWatchAdd, kBreak, kFunction, kData, kInstr, kSession };
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
    // The other sessions, and whether one of them is in this one's place for
    // a moment (to hear from its debugger, or be sent breakpoints).
    std::vector<DebugSession> others;
    bool background = false;
    DebugSession& session() { return *this; }
    // The notes after the lines of each view (where it stopped, the values),
    // and what they were made for.
    struct Inline {
        u64 key[9] = {};
        std::vector<std::vector<u32>> cells;
        std::vector<GridNote> notes;
        std::vector<std::string> texts;  // (what each says: the tests)
    };
    Inline inline_[kMaxViews];

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

bool App::debug_running() const {
    if (!debugui_) return false;
    return debugui_->state != DebugUi::kIdle || std::ranges::any_of(debugui_->others, [](const DebugSession& s) { return s.state != DebugUi::kIdle; });
}

// ---------------------------------------------------------------- several sessions

// The session in front kept aside, among the others, and a fresh one in its place.
void App::debug_park() {
    DebugUi& u = *debugui_;
    if (!u.dap && u.state == DebugUi::kIdle) return;
    u.others.push_back(std::move(u.session()));
    u.session() = DebugSession{};
}

// Another session to the front (the one there among the others): its stack
// shown as it left it, its variables and watches asked for again.
void App::debug_switch(size_t i) {
    DebugUi& u = *debugui_;
    if (i >= u.others.size()) return;
    std::swap(u.session(), u.others[i]);
    if (u.state == DebugUi::kStopped && u.frame < u.frames.size()) debug_frame(u.frame, true);
    ++debug_gen_;
    dirty_ = true;
}

// Something done in each of the other sessions too (breakpoints sent), each in turn in the front one's place.
void App::debug_each_other(const std::function<void()>& fn) {
    DebugUi& u = *debugui_;
    if (u.background) return;
    for (DebugSession& other : u.others) {
        std::swap(u.session(), other);
        u.background = true;
        fn();
        u.background = false;
        std::swap(u.session(), other);
    }
}

// F5's session again, beside the one under way (which is kept aside).
void App::debug_start_another() {
    DebugUi& u = *debugui_;
    if (u.state != DebugUi::kIdle) debug_park();
    debug_start();
}

// ---------------------------------------------------------------- breakpoints

void App::debug_toggle_break() {
    DebugUi& u = *debugui_;
    Doc& d = *pane_->doc;
    if (debug_disasm_front()) {  // at the cursor's instruction
        const u64 line = d.ed.buf.line_of(d.ed.cursor());
        const u64 a = line < u.disasm_addrs.size() ? u.disasm_addrs[line] : 0;
        if (!a) return;
        if (!u.can.instruction_breaks) return notify("This debugger cannot break at an instruction");
        if (!u.ibreaks.erase(a)) u.ibreaks.insert(a);
        debug_send_instructions();
        ++debug_gen_;
        dirty_ = true;
        return;
    }
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
    find_choose(kFindInput, "Condition", was.cond, [this, path, line, hits = was.hits, log = was.log](const std::string& cond) {
        find_choose(kFindInput, "Hit count", hits, [this, path, line, log, cond = trimmed(cond)](const std::string& typed) {
            // Last, a message to log in place of stopping ({x} for x's value).
            find_choose(kFindInput, "Log message", log, [this, path, line, cond, hits = trimmed(typed)](const std::string& message) {
                DebugUi& v = *debugui_;
                v.breaks[path][line] = {cond, hits, trimmed(message)};
                if (Doc* d = pane_->doc; d && !d->ed.path.empty() && full_path(d->ed.path) == path) v.follow(*this, *d);
                debug_send_breaks(path);
                ++debug_gen_;
                dirty_ = true;
            });
        });
    });
}

// A file's breakpoints, to a session under way.
void App::debug_send_breaks(const std::string& path) {
    DebugUi& u = *debugui_;
    debug_each_other([&] { debug_send_breaks(path); });  // (every session has them)
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
            if (!b.log.empty()) a += ",\"logMessage\":" + quoted(b.log + "\n");
            a += "}";
            first = false;
        }
    }
    if (u.run_to_path == path) {  // (run to the cursor: there too, until it stops)
        const bool listed = u.breaks.contains(path) && u.breaks.at(path).contains(u.run_to_line);
        if (!listed) a += std::string(a.back() == '[' ? "" : ",") + "{\"line\":" + std::to_string(u.run_to_line + 1) + "}";
    }
    a += "]}";
    u.dap->request("setBreakpoints", a);
}

// The breakpoints by function name, to a session under way.
void App::debug_send_functions() {
    DebugUi& u = *debugui_;
    debug_each_other([&] { debug_send_functions(); });
    if (!u.dap || u.state == DebugUi::kStarting) return;  // (sent when the debugger is ready for them)
    std::string a = "{\"breakpoints\":[";
    for (size_t i = 0; i < u.functions.size(); ++i) a += std::string(i ? "," : "") + "{\"name\":" + quoted(u.functions[i]) + "}";
    u.dap->request("setFunctionBreakpoints", a + "]}");
}

// A breakpoint by a function's name, typed (the word at the cursor to start with).
void App::debug_break_function() {
    std::string word;
    if (const Doc& d = *pane_->doc; !d.ed.path.empty()) {
        u64 lo = d.ed.cursor(), hi = lo;
        char c = 0;
        auto ok = [&](u64 at) { return at < d.ed.buf.size() && d.ed.buf.read(at, &c, 1) == 1 && (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':'); };
        while (lo > 0 && hi - lo < 200 && ok(lo - 1)) --lo;
        while (hi - lo < 200 && ok(hi)) ++hi;
        word = d.ed.buf.text(lo, hi - lo);
    }
    find_choose(kFindInput, "Break at the function", word, [this](const std::string& typed) {
        DebugUi& v = *debugui_;
        const std::string name = trimmed(typed);
        if (name.empty() || std::ranges::find(v.functions, name) != v.functions.end()) return;
        v.functions.push_back(name);
        debug_send_functions();
        ++debug_gen_;
        dirty_ = true;
    });
}

// What goes in a line's first gutter cell: the line the program stopped
// on (an arrow), or a breakpoint (a dot); 0 for nothing.
u32 App::debug_gutter(const Doc& d, u64 line) {
    if (!debugui_) return 0;
    DebugUi& u = *debugui_;
    if (u.disasm_doc && d.id == u.disasm_doc) {  // the disassembly's: by address
        const u64 a = line < u.disasm_addrs.size() ? u.disasm_addrs[line] : 0;
        if (!a) return 0;
        if (u.state == DebugUi::kStopped && u.frame < u.frames.size() && u.frames[u.frame].ip == a)
            return (font_->glyph(0x25B6) & (Font::kWide - 1)) | (kInkAccent << kCellInkShift);
        if (u.ibreaks.contains(a)) return (font_->glyph(0x25CF) & (Font::kWide - 1)) | (kInkDiagnostic << kCellInkShift);
        return 0;
    }
    if (d.ed.path.empty()) return 0;
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
            const bool plain = b->second.cond.empty() && b->second.hits.empty();  // (a dot; one that waits for something, a diamond; a logpoint, a diamond in one)
            return (font_->glyph(!b->second.log.empty() ? 0x25C8 : plain ? 0x25CF : 0x25C6) & (Font::kWide - 1)) | (kInkDiagnostic << kCellInkShift);
        }
    return 0;
}

std::vector<u64> App::debug_break_lines(const Doc& d) {
    std::vector<u64> out;
    if (!debugui_) return out;
    const DebugUi& u = *debugui_;
    if (u.disasm_doc && d.id == u.disasm_doc) {
        for (size_t l = 0; l < u.disasm_addrs.size(); ++l)
            if (u.disasm_addrs[l] && u.ibreaks.contains(u.disasm_addrs[l])) out.push_back(l);
        return out;
    }
    if (d.ed.path.empty()) return out;
    if (u.breaks.empty()) return out;  // (none anywhere: the path is not even looked up)
    if (const auto it = u.breaks.find(full_path(d.ed.path)); it != u.breaks.end())
        for (const auto& [line, b] : it->second) out.push_back(line);
    return out;
}

// A run configuration chosen: what it runs is what is debugged.
void App::debug_take(const std::string& program, const std::string& args, const std::string& cwd, const std::vector<std::string>& env) {
    DebugUi& u = *debugui_;
    u.program = program;
    u.args = args;
    u.cwd = cwd;
    u.env = env;
    ++debug_gen_;
    dirty_ = true;
}

std::string App::debug_target() const {
    const DebugUi& u = *debugui_;
    auto last = [](const std::string& path) { return path.substr(path.find_last_of('/') + 1); };  // (a file's name, a folder's)
    std::string out = last(u.program) + (u.args.empty() ? "" : " " + u.args) + " in " + last(u.cwd);
    for (const std::string& e : u.env) out += " " + e;
    return out;
}

// ---------------------------------------------------------------- a session

// The debugger started (for a program at `near`: on its machine), the
// last session's state let go of, the Debug tab shown.
bool App::debug_open(const std::string& near, const std::string& dir, bool python) {
    DebugUi& u = *debugui_;
    std::string host, there;
    // (Attaching names no program: the machine is the folder's.)
    const bool remote = split_remote(near.empty() ? dir : near, &host, &there);
    u.remote = remote ? remote_prefix(host) : std::string();
    u.dap = std::make_unique<Dap>();
    u.gdb = u.rocm = false;
    u.going = false;
    u.frames.clear();
    u.threads.clear();
    u.vars.clear();
    u.output.clear();
    u.partial.clear();
    u.esc = DebugSession::kText;
    u.error.clear();
    u.reason.clear();
    u.exit_code = -1;
    u.asking = 0;
    // (On another machine the connection is waited for and the debugger started
    // there on the Dap's own thread: the tab says Starting… meanwhile, and what
    // is asked of it below is sent once it has started.)
    std::string err;
    std::string exports;  // (the debugger has them, and the program it starts)
    for (const std::string& e : u.env) exports += "export " + sh_word(e) + "; ";
    if (const std::string prog = near.empty() || !remote ? near : there; !prog.empty() && !python)
        exports += "BLIT_DEBUG_PROGRAM=" + sh_word(prog) + "; export BLIT_DEBUG_PROGRAM; ";  // (what it debugs: HIP's gets rocgdb)
    if (!u.dap->start({"/bin/sh", "-c", exports + (python ? kPython : kNative)}, dir, wake, &err)) {
        u.error = err;
        u.dap.reset();
        ++debug_gen_;
        dirty_ = true;
        return false;
    }
    u.state = DebugUi::kStarting;
    if (!panel_px()) panel_toggle();
    panel_show_debug();
    panel_focus(false);
    return true;
}

// Attaching to a process of the user's: the list of them, narrowed by name.
void App::debug_attach_pick() {
    std::string out;
    if (!fs_run(default_workdir(), "ps -x -o pid= -o comm=", &out, 5000)) return notify("Cannot list the processes");
    std::vector<std::pair<std::string, i64>> procs;
    const i64 self = static_cast<i64>(::getpid());
    for (size_t at = 0; at < out.size();) {
        size_t end = out.find('\n', at);
        if (end == std::string::npos) end = out.size();
        const std::string line = out.substr(at, end - at);
        at = end + 1;
        const size_t a = line.find_first_not_of(' ');
        if (a == std::string::npos) continue;
        const size_t sp = line.find(' ', a);
        if (sp == std::string::npos) continue;
        const i64 pid = std::atoll(line.c_str() + a);
        std::string name = line.substr(line.find_first_not_of(' ', sp));
        if (pid <= 0 || pid == self) continue;
        procs.emplace_back(name.substr(name.find_last_of('/') + 1) + "  " + std::to_string(pid) + "  " + name, pid);
    }
    std::ranges::sort(procs);
    std::vector<std::string> items;
    for (const auto& [text, pid] : procs) items.push_back(text);
    find_choose_item("Attach to", std::move(items), [this, procs](int i) {
        if (i >= 0 && static_cast<size_t>(i) < procs.size()) debug_attach(procs[static_cast<size_t>(i)].second);
    });
}

void App::debug_attach(i64 pid) {
    DebugUi& u = *debugui_;
    if (u.state != DebugUi::kIdle) debug_park();  // (beside the session under way)
    if (!debug_open({}, default_workdir(), false)) return;
    u.name = "process " + std::to_string(pid);
    debug_begin("attach", "{\"pid\":" + std::to_string(pid) + "}");
}

// A core file: the program it is of, then the core, each chosen.
void App::debug_core_pick() {
    DebugUi& u = *debugui_;
    const std::string* root = panel_root();
    std::string start = u.program.empty() ? (root ? *root : default_workdir()) + "/" : u.program;
    find_choose(kFindOpen, "Program the core is of", start, [this](const std::string& program) {
        find_choose(kFindOpen, "Core file", parent_of(program) + "/", [this, program](const std::string& core) { debug_core(program, core); });
    });
}

void App::debug_core(const std::string& program, const std::string& core) {
    DebugUi& u = *debugui_;
    if (u.state != DebugUi::kIdle) debug_park();
    u.program = full_path(program);
    if (!debug_open(u.program, parent_of(u.program), false)) return;
    u.name = "core of " + base_name(u.program);
    // (lldb-dap reads coreFile; gdb, core.)
    debug_begin("attach", "{\"program\":" + quoted(u.there(u.program)) + ",\"coreFile\":" + quoted(u.there(full_path(core))) + ",\"core\":" + quoted(u.there(full_path(core))) + "}");
}

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
    const std::string dir = !u.cwd.empty() ? u.cwd : parent_of(u.program);
    const bool python = u.program.ends_with(".py");
    if (!debug_open(u.program, dir, python)) return;
    u.name = base_name(u.program);
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
    debug_begin("launch", launch);
}

// The debugger told what to do once it has said what it can: launch the
// program, attach to a process, or open a core file.
void App::debug_begin(const std::string& request, const std::string& args_in) {
    DebugUi& u = *debugui_;
    // (lldb-dap: a value whose type has no summary of its own gets one made
    // from its members, "{first:"a", second:1}" for "std::pair<...> @ 0x..."
    // in a map. The other debuggers pass over what they do not know.)
    std::string arguments = args_in;
    if (arguments.starts_with("{")) arguments.insert(1, std::string("\"enableAutoVariableSummaries\":true") + (arguments.size() > 2 ? "," : ""));
    u.how = request == "launch" ? "launch" : arguments.find("coreFile") != std::string::npos ? "core" : "attach";
    u.dap->request("initialize",
                   "{\"clientID\":\"blit\",\"clientName\":\"blit\",\"adapterID\":\"blit\",\"linesStartAt1\":true,\"columnsStartAt1\":true,"
                   "\"pathFormat\":\"path\",\"supportsVariableType\":true,\"supportsRunInTerminalRequest\":false}",
                   [this, request, arguments](const Dap::Message& m) {
                       DebugUi& v = *debugui_;
                       if (!m.success) {
                           v.error = "the debugger would not start: " + m.message;
                           return;
                       }
                       const json::Value c = m.body();
                       v.can = {c["supportsDataBreakpoints"].boolean(), c["supportsSetVariable"].boolean(), c["supportsGotoTargetsRequest"].boolean(),
                                c["supportsFunctionBreakpoints"].boolean(), c["supportsLogPoints"].boolean(), c["supportsDisassembleRequest"].boolean(),
                                c["supportsReadMemoryRequest"].boolean(), c["supportsWriteMemoryRequest"].boolean(), c["supportsInstructionBreakpoints"].boolean(),
                                c["supportsSteppingGranularity"].boolean()};
                       v.dap->request(request, arguments, [this, request](const Dap::Message& r) {
                           if (!r.success) {
                               debugui_->error = (request == "launch" ? "cannot launch it: " : "cannot attach: ") + (r.message.empty() ? std::string("the debugger said no") : r.message);
                               debugui_->end_soon = true;
                           }
                       });
                   });
    ++debug_gen_;
    dirty_ = true;
}

// The session over: the debugger let go of (and the program with it, unless
// it was attached to: that goes on), on a thread of its own: nothing here
// waits for it to end.
void App::debug_end() {
    DebugUi& u = *debugui_;
    if (u.dap && !u.dap->ended()) {
        u.dap->request("disconnect", u.how == "attach" ? "{\"terminateDebuggee\":false}" : "{\"terminateDebuggee\":true}");
        u.dap->stop(300);  // (the adapter lets go of the program as it ends)
    }
    u.dap.reset();
    u.state = DebugUi::kIdle;
    u.data.clear();  // (the debugger's ids for them were this session's, as the program's addresses were)
    u.ibreaks.clear();
    u.run_to_path.clear();
    u.frames.clear();
    u.threads.clear();
    u.vars.clear();
    for (DebugUi::Watch& w : u.watches) w.value.clear(), w.failed = false;
    u.asking = 0;
    // Another session waiting takes its place (not while one is in it for a moment).
    if (!u.background && !u.others.empty()) {
        u.session() = std::move(u.others.back());
        u.others.pop_back();
        if (u.state == DebugUi::kStopped && u.frame < u.frames.size()) debug_frame(u.frame, true);
    }
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
    // continue, next, stepIn, stepOut; in the disassembly, a step is an instruction's
    const bool by_instruction = (what == "next" || what == "stepIn") && u.can.granularity && debug_disasm_front();
    u.dap->request(what, "{\"threadId\":" + std::to_string(u.thread) + (by_instruction ? ",\"granularity\":\"instruction\"" : "") + "}");
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
                d.goto_utf16 = false;
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
            // (The first open, the locals or the arguments; gdb has those two apart, and both are.)
            const std::string_view hint = s["presentationHint"].str();
            if ((first && hint != "registers") || hint == "arguments" || hint == "locals") debug_expand(static_cast<u32>(v.vars.size() - 1));
            first = false;
        }
        ++debug_gen_;
        dirty_ = true;
    });
    debug_watch_eval();  // (in this frame)
    debug_disasm_ask();  // (its instructions)
    debug_memory_ask(-1, -1);  // (as it is now)
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
        for (json::Value f : r.body()["stackFrames"]) {
            // (A frame without source of its own, a library's, is given one the debugger makes, its
            // disassembly, by reference: its path is no file's.)
            const bool own = f["source"]["sourceReference"].integer() == 0;
            v.frames.push_back({f["id"].integer(), std::string(f["name"].str()), own ? v.here(f["source"]["path"].str()) : std::string(), static_cast<u32>(f["line"].integer()),
                                address_of(f["instructionPointerReference"].str())});
        }
        // (Stepping in the disassembly, the frame that stopped, source or none, and the tab stays in front.)
        const bool instructions = debug_disasm_front();
        u32 first = 0;
        while (!instructions && first < v.frames.size() && v.frames[first].path.empty()) ++first;
        debug_frame(first < v.frames.size() ? first : 0, !instructions);
        ++debug_gen_;
        dirty_ = true;
    });
    ++debug_gen_;
    dirty_ = true;
}

// rocgdb's names for the GPU's waves among the threads: `info threads`
// says of each "AMDGPU Wave 1:1:1:1 (0,0,0)/0", its work-group and its
// number in it; the threads that are waves are named so ("GPU wave
// (0,0,0)/0"), after the program's own.
void App::debug_name_waves() {
    DebugUi& u = *debugui_;
    if (!u.dap || u.state != DebugUi::kStopped) return;
    ++u.asking;
    u.dap->request("evaluate", "{\"expression\":\"info threads\",\"context\":\"repl\"}", [this](const Dap::Message& m) {
        DebugUi& v = *debugui_;
        if (v.asking) --v.asking;
        if (!m.success || v.state != DebugUi::kStopped) return;
        const std::string text(m.body()["result"].str());
        std::map<i64, std::string> waves;
        for (size_t at = 0; at < text.size();) {
            size_t nl = text.find('\n', at);
            if (nl == std::string::npos) nl = text.size();
            std::string_view line = std::string_view(text).substr(at, nl - at);
            at = nl + 1;
            const size_t wave = line.find("AMDGPU Wave ");
            if (wave == std::string_view::npos) continue;
            size_t k = 0;
            while (k < line.size() && (line[k] == ' ' || line[k] == '*')) ++k;
            const i64 id = std::atoll(std::string(line.substr(k)).c_str());
            const size_t open = line.find('(', wave), slash = open == std::string_view::npos ? open : line.find('/', open);
            if (id <= 0 || slash == std::string_view::npos) continue;
            size_t end = slash + 1;
            while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end]))) ++end;
            waves[id] = "GPU wave " + std::string(line.substr(open, end - open));
        }
        if (waves.empty()) return;
        for (DebugUi::Thread& t : v.threads)
            if (const auto it = waves.find(t.id); it != waves.end()) t.name = it->second;
        std::ranges::stable_sort(v.threads, [&](const DebugUi::Thread& a, const DebugUi::Thread& b) { return !waves.contains(a.id) && waves.contains(b.id); });
        ++debug_gen_;
        dirty_ = true;
    });
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
            kids.push_back({std::string(x["name"].str()), std::move(value), x["variablesReference"].integer(), depth, false, ref, address_of(x["memoryReference"].str())});
        }
        // gdb gives a map's (libstdc++'s printer's) entries as keys and values
        // one after another, [0] a key, [1] its value: as pairs, [key] = value.
        const std::string& of = v.vars[at].value;
        const bool mapping = of.starts_with("std::map with") || of.starts_with("std::unordered_map with") || of.starts_with("std::multimap with") ||
                             of.starts_with("std::unordered_multimap with") || of.starts_with("std::__debug::map with");
        if (v.gdb && mapping && kids.size() % 2 == 0) {
            std::vector<DebugUi::Var> pairs;
            for (size_t k = 0; k + 1 < kids.size(); k += 2) {
                DebugUi::Var e = kids[k + 1];
                e.name = "[" + kids[k].value.substr(0, kids[k].value.find("  (")) + "]";
                pairs.push_back(std::move(e));
            }
            kids = std::move(pairs);
        }
        v.vars.insert(v.vars.begin() + static_cast<std::ptrdiff_t>(at + 1), kids.begin(), kids.end());
        ++debug_gen_;
        dirty_ = true;
    });
    ++debug_gen_;
    dirty_ = true;
}

// What the debugger has said: answers' callbacks run, then its events.
// What the debuggers have said: the front session's, then each other's in
// its place for a moment. One of them that has stopped comes to the front
// (the one there is kept aside in its place); one that has ended goes.
void App::debug_sync() {
    DebugUi& u = *debugui_;
    bool stopped = false;
    debug_sync_session(&stopped);
    for (size_t i = 0; i < u.others.size();) {
        std::swap(u.session(), u.others[i]);
        u.background = true;
        stopped = false;
        debug_sync_session(&stopped);
        u.background = false;
        if (!u.dap) {  // (ended)
            std::swap(u.session(), u.others[i]);
            u.others.erase(u.others.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (!stopped) std::swap(u.session(), u.others[i]);
        ++i;
    }
    ++debug_gen_;
    dirty_ = true;
}

void App::debug_sync_session(bool* stopped) {
    DebugUi& u = *debugui_;
    if (!u.dap) return;
    for (const Dap::Message& m : u.dap->poll()) {
        const json::Value b = m.body();
        if (m.type == "request") {  // (it asks for something blit does not do: running in a terminal, another session)
            u.dap->respond(m, false);
        } else if (m.name == "initialized") {
            u.state = DebugUi::kRunning;
            u.gdb = u.dap->errors().find(kGdbSays) != std::string::npos;  // (said before it started: here by now)
            u.rocm = u.dap->errors().find("blit-debugger: gdb (rocgdb)") != std::string::npos;
            for (const auto& [path, lines] : u.breaks) debug_send_breaks(path);
            debug_send_functions();
            debug_send_instructions();
            u.dap->request("setExceptionBreakpoints", "{\"filters\":[]}");
            u.dap->request("configurationDone");
            // (As other clients do, the threads are asked for once now: lldb-dap answers the
            // first such question with those it had at the start, whenever it is asked.)
            u.dap->request("threads");
        } else if (m.name == "stopped") {
            u.state = DebugUi::kStopped;
            *stopped = true;
            if (!u.run_to_path.empty()) {  // (run to the cursor: that breakpoint goes)
                const std::string path = std::exchange(u.run_to_path, {});
                debug_send_breaks(path);
            }
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
                if (v.rocm) debug_name_waves();  // (the GPU's waves, which rocgdb names as it lists them)
                ++debug_gen_;
                dirty_ = true;
            });
            debug_stack();
        } else if (m.name == "continued") {
            u.state = DebugUi::kRunning;
            u.frames.clear();
            u.threads.clear();
            u.vars.clear();
        } else if (m.name == "process") {
            u.going = true;
        } else if (m.name == "output") {
            const std::string_view cat = b["category"].str();
            // (gdb tells of breakpoints in a program it has not loaded yet,
            // "No source file named ...", as if the program had said it.)
            // (And after, its word that it has put a breakpoint somewhere: "Breakpoint 2 at 0x...: file f.c, line 7.")
            const bool gdb_own = u.gdb && cat == "stdout" && (!u.going || gdb_breakpoint_said(b["output"].str()));
            if (cat != "telemetry" && cat != "important" && !gdb_own) u.print(b["output"].str());
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

void App::debug_evaluate(const std::string& start) {
    DebugUi& u = *debugui_;
    if (!u.dap || u.state != DebugUi::kStopped) return;
    find_choose(kFindInput, "Evaluate", start, [this](const std::string& e) {
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
    find_on_tab([this](const std::string& typed) { debug_complete(typed); });  // (Tab: what the debugger would complete it with)
}

// What is typed to be evaluated, completed as the debugger would (its
// completions request): one answer goes in; several that share more than
// is typed, that much; else a list of them, the one chosen going in.
void App::debug_complete(const std::string& typed) {
    DebugUi& u = *debugui_;
    if (!u.dap || u.state != DebugUi::kStopped) return;
    const std::string frame = u.frame < u.frames.size() ? ",\"frameId\":" + std::to_string(u.frames[u.frame].id) : std::string();
    u.dap->request("completions", "{\"text\":" + quoted(typed) + ",\"column\":" + std::to_string(typed.size() + 1) + frame + "}", [this, typed](const Dap::Message& m) {
        if (!m.success || find_typed() != typed) return;  // (typed on meanwhile, or the box shut)
        // The word being typed, which a completion takes the place of (where the debugger says no other place).
        size_t word = typed.size();
        while (word > 0 && (std::isalnum(static_cast<unsigned char>(typed[word - 1])) || typed[word - 1] == '_')) --word;
        std::vector<std::string> all;
        for (const json::Value t : m.body()["targets"]) {
            const std::string label(t["label"].str());
            if (label.find(" -- ") != std::string::npos && label.ends_with(".")) continue;  // (lldb's own commands, with their help: not an expression)
            std::string text(t["text"].exists() ? t["text"].str() : t["label"].str());
            if (const size_t d = text.find(" -- "); d != std::string::npos) text.resize(d);
            std::string whole;
            if (t["start"].exists()) {
                const size_t at = std::min<size_t>(static_cast<size_t>(std::max<i64>(0, t["start"].integer() - 1)), typed.size());
                const size_t len = std::min<size_t>(static_cast<size_t>(std::max<i64>(0, t["length"].integer())), typed.size() - at);
                whole = typed.substr(0, at) + text + typed.substr(at + len);
            } else {
                whole = text.starts_with(typed.substr(word)) ? typed.substr(0, word) + text : typed + text;
            }
            if (std::ranges::find(all, whole) == all.end()) all.push_back(std::move(whole));
        }
        if (all.empty()) return;
        if (all.size() == 1) return find_set_typed(all[0]);
        std::string common = all[0];
        for (const std::string& c : all)
            while (!c.starts_with(common)) common.pop_back();
        if (common.size() > typed.size()) return find_set_typed(common);
        find_choose_item("Complete", all, [this, all](int i) {
            if (i >= 0 && static_cast<size_t>(i) < all.size()) debug_evaluate(all[static_cast<size_t>(i)]);
        });
    });
}

// A variable's value set, as typed (its value now to start with).
void App::debug_set_value(u32 i) {
    DebugUi& u = *debugui_;
    if (i >= u.vars.size() || !u.dap || u.state != DebugUi::kStopped || u.vars[i].parent <= 0) return;
    if (!u.can.set_variable) return notify("This debugger cannot set a value");
    const DebugUi::Var v = u.vars[i];
    find_choose(kFindInput, "Value of " + v.name, v.value.substr(0, v.value.find("  (")), [this, v](const std::string& typed) {
        DebugUi& w = *debugui_;
        if (!w.dap || trimmed(typed).empty()) return;
        ++w.asking;
        w.dap->request("setVariable", "{\"variablesReference\":" + std::to_string(v.parent) + ",\"name\":" + quoted(v.name) + ",\"value\":" + quoted(trimmed(typed)) + "}",
                       [this, v](const Dap::Message& m) {
                           DebugUi& x = *debugui_;
                           if (x.asking) --x.asking;
                           if (!m.success) return notify("Cannot set " + v.name + ": " + m.message);
                           for (DebugUi::Var& y : x.vars)
                               if (y.parent == v.parent && y.name == v.name) y.value = std::string(m.body()["value"].str());
                           debug_watch_eval();  // (what the watches say may have changed)
                           ++debug_gen_;
                           dirty_ = true;
                       });
    });
}

// A variable's writes watched: the debugger says what it calls it (and
// whether it can), then it is set with the others.
void App::debug_watch_writes(u32 i) {
    DebugUi& u = *debugui_;
    if (i >= u.vars.size() || !u.dap || u.state != DebugUi::kStopped || u.vars[i].parent <= 0) return;
    if (!u.can.data_breaks) return notify("This debugger cannot watch a variable's writes");
    const DebugUi::Var v = u.vars[i];
    ++u.asking;
    u.dap->request("dataBreakpointInfo", "{\"variablesReference\":" + std::to_string(v.parent) + ",\"name\":" + quoted(v.name) + "}", [this, v](const Dap::Message& m) {
        DebugUi& x = *debugui_;
        if (x.asking) --x.asking;
        const std::string id(m.body()["dataId"].str());
        if (!m.success || id.empty()) return notify("Cannot watch " + v.name + (m.body()["description"].str().empty() ? std::string() : ": " + std::string(m.body()["description"].str())));
        const auto had = std::ranges::find_if(x.data, [&](const DebugUi::Data& d) { return d.id == id; });
        if (had != x.data.end()) x.data.erase(had);  // (again: no longer)
        else x.data.push_back({id, v.name});
        debug_send_data();
        ++debug_gen_;
        dirty_ = true;
    });
}

void App::debug_send_data() {
    DebugUi& u = *debugui_;
    if (!u.dap) return;
    std::string a = "{\"breakpoints\":[";
    for (size_t i = 0; i < u.data.size(); ++i) a += std::string(i ? "," : "") + "{\"dataId\":" + quoted(u.data[i].id) + ",\"accessType\":\"write\"}";
    u.dap->request("setDataBreakpoints", a + "]}");
}

// Run to the cursor's line: a breakpoint there until it stops, then on.
void App::debug_run_to_cursor() {
    DebugUi& u = *debugui_;
    const Doc& d = *pane_->doc;
    if (!u.dap || u.state != DebugUi::kStopped || d.ed.path.empty()) return;
    u.run_to_path = full_path(d.ed.path);
    u.run_to_line = static_cast<u32>(d.ed.buf.line_of(d.ed.cursor()));
    debug_send_breaks(u.run_to_path);
    debug_command("continue");
}

// The program put back (or on) to the cursor's line, where the debugger
// can: what is between is not run.
void App::debug_jump_to_cursor() {
    DebugUi& u = *debugui_;
    const Doc& d = *pane_->doc;
    if (!u.dap || u.state != DebugUi::kStopped || d.ed.path.empty()) return;
    if (!u.can.goto_targets) return notify("This debugger cannot jump");
    const u32 line = static_cast<u32>(d.ed.buf.line_of(d.ed.cursor())) + 1;
    const i64 thread = u.thread;
    u.dap->request("gotoTargets", "{\"source\":{\"path\":" + quoted(u.there(full_path(d.ed.path))) + "},\"line\":" + std::to_string(line) + "}",
                   [this, thread](const Dap::Message& m) {
                       DebugUi& v = *debugui_;
                       const json::Value t = m.body()["targets"];
                       if (!m.success || !t.is_array() || t.size() == 0) return notify("Cannot jump there");
                       for (const json::Value target : t) {
                           v.dap->request("goto", "{\"threadId\":" + std::to_string(thread) + ",\"targetId\":" + std::to_string(target["id"].integer()) + "}");
                           break;
                       }
                   });
}

// ---------------------------------------------------------------- the disassembly and memory tabs

bool App::debug_disasm_front() const {
    return debugui_ && debugui_->disasm_doc && pane_ && pane_->doc && pane_->doc->id == debugui_->disasm_doc;
}

// One of the debugger's tabs (by its document's id, which is set when it is
// made) brought to the front; made, beside the one in front, if it is not open.
Doc* App::debug_tab(u64& id, const char* title) {
    Pane* p = nullptr;
    size_t at = 0;
    if (Doc* d = id ? find_doc(id, &p, &at) : nullptr) {
        focus_pane(p);
        tab_show(at);
        panel_focus(false);
        return d;
    }
    Doc* d = add_doc(pane_->tab + 1);
    d->title = title;
    d->read_only = true;
    id = d->id;
    tab_show(pane_->tab + 1);
    panel_focus(false);
    return d;
}

// Such a tab given its text, coloured in runs (from off[i] on, style[i]: 3 the
// accent, 4 quieter), its cursor on `line` (where it was, for -1), `col` along it.
void App::debug_tab_fill(Doc& d, std::string text, std::vector<u32> off, std::vector<u8> style, i64 line, u64 col_at) {
    const u64 was = d.ed.buf.line_of(d.ed.cursor()), col = d.ed.cursor() - d.ed.buf.line_start(was);
    if (!text.empty() && text.back() == '\n') text.pop_back();
    d.measurer = {};  // (it may be reading the text that goes)
    d.ed.open_bytes({}, text);
    d.ed.mark_saved();
    auto h = std::make_unique<Highlights>();
    h->base = 0;
    h->end = text.size();
    h->off = std::move(off);
    h->style = std::move(style);
    h->seal();
    d.marks = std::move(h);
    d.style_bits[3] = kInkAccent << kCellInkShift;
    d.style_bits[4] = kInkMuted << kCellInkShift;
    d.widest_rev = d.indent_rev = ~0ull;
    const u64 to = std::min<u64>(line >= 0 ? static_cast<u64>(line) : was, d.ed.buf.line_count() - 1);
    const u64 start = d.ed.buf.line_start(to);
    d.ed.move_to(std::min<u64>(start + (line >= 0 ? col_at : col), d.ed.line_end(to)), false);
    Pane* where = nullptr;
    find_doc(d.id, &where);
    if (where) {
        where->cache_rev = ~0ull;
        if (where->doc == &d) {
            Pane* const keep = pane_;
            pane_ = where;
            scroll_to_cursor();
            pane_ = keep;
        }
    }
    ++debug_gen_;
    dirty_ = true;
}

namespace {

// Runs of style as a tab's text is put together.
struct Runs {
    std::string text;
    std::vector<u32> off;
    std::vector<u8> style;
    void add(std::string_view s, u8 st) {
        if (s.empty()) return;
        const u32 at = static_cast<u32>(text.size());
        if (style.empty() ? st != 0 : style.back() != st) {
            if (!off.empty() && off.back() == at) style.back() = st;
            else off.push_back(at), style.push_back(st);
        }
        text += s;
    }
};

}  // namespace

void App::debug_disasm_open() {
    DebugUi& u = *debugui_;
    if (!u.dap || u.state != DebugUi::kStopped) return notify("The program is not stopped");
    if (!u.can.disassemble) return notify("This debugger cannot disassemble");
    debug_tab(u.disasm_doc, "Disassembly");
    debug_disasm_ask();
}

// The instructions around the frame shown's, asked for when the tab is open:
// a line each, and above those of each source line its file, line and text.
// (Those before it: lldb counts back by instructions; gdb cannot, and is
// asked from 128 bytes before, which it reads from the next it can. An answer
// that has not the frame's instruction in it is asked for again from there.)
void App::debug_disasm_ask(bool from_ip) {
    DebugUi& u = *debugui_;
    if (u.disasm_doc && !find_doc(u.disasm_doc)) u.disasm_doc = 0, u.disasm_addrs.clear();  // (closed)
    if (!u.disasm_doc || !u.dap || u.state != DebugUi::kStopped || u.frame >= u.frames.size() || !u.frames[u.frame].ip) return;
    const u64 ip = u.frames[u.frame].ip, gen = ++u.disasm_gen;
    const std::string frame_path = u.frames[u.frame].path;
    const char* const around = from_ip ? ",\"instructionOffset\":0,\"instructionCount\":64"
                               : u.gdb ? ",\"offset\":-128,\"instructionOffset\":0,\"instructionCount\":96"
                                       : ",\"instructionOffset\":-32,\"instructionCount\":96";
    ++u.asking;
    u.dap->request("disassemble", "{\"memoryReference\":" + quoted(hex(ip)) + around + ",\"resolveSymbols\":true}",
                   [this, gen, ip, from_ip, frame_path](const Dap::Message& m) {
                       DebugUi& v = *debugui_;
                       if (v.asking) --v.asking;
                       Doc* d = v.disasm_doc ? find_doc(v.disasm_doc) : nullptr;
                       if (gen != v.disasm_gen || !d) return;
                       if (!from_ip) {
                           bool has = false;
                           for (const json::Value x : m.body()["instructions"]) has |= address_of(x["address"].str()) == ip;
                           if (!has) return debug_disasm_ask(true);
                       }
                       v.disasm_addrs.clear();
                       if (!m.success) {
                           debug_tab_fill(*d, "The debugger could not disassemble here: " + why_not(m.message), {}, {}, 0, 0);
                           return;
                       }
                       const json::Value all = m.body()["instructions"];
                       u64 last = ip;
                       for (const json::Value x : all) last = std::max(last, address_of(x["address"].str()));
                       int width = 1;
                       for (u64 a = last; a >= 16; a /= 16) ++width;
                       std::map<std::string, std::vector<std::string>> sources;  // (each file's lines, read once)
                       auto source_line = [&](const std::string& path, i64 n) -> std::string {
                           if (!v.remote.empty()) return {};
                           auto [it, fresh] = sources.try_emplace(path);
                           if (fresh) {
                               std::string text;
                               // (gdb names it as the program was built with it: beside the frame's, for a name alone.)
                               const std::string full = path.starts_with('/') || frame_path.empty() ? path : parent_of(frame_path) + "/" + path;
                               if (fs_read(full, &text)) {
                                   size_t from = 0;
                                   for (size_t nl; (nl = text.find('\n', from)) != std::string::npos; from = nl + 1) it->second.push_back(text.substr(from, nl - from));
                                   it->second.push_back(text.substr(from));
                               }
                           }
                           if (n < 1 || static_cast<size_t>(n) > it->second.size()) return {};
                           std::string l = trimmed(it->second[static_cast<size_t>(n - 1)]);
                           for (char& c : l)
                               if (c == '\t') c = ' ';
                           return l;
                       };
                       Runs out;
                       std::string sym, file;
                       i64 line = -1, at = -1;
                       for (const json::Value x : all) {
                           const u64 a = address_of(x["address"].str());
                           if (const std::string_view s = x["symbol"].str(); !s.empty() && s != sym) {
                               sym = s;
                               out.add(sym + ":\n", 3);
                               v.disasm_addrs.push_back(0);
                           }
                           // (gdb names the file only where it changes.)
                           const std::string_view named = x["location"]["path"].str();
                           const std::string path = named.empty() ? file : std::string(named);
                           const i64 n = x["line"].integer(-1);
                           if (!path.empty() && n > 0 && (path != file || n != line)) {
                               file = path, line = n;
                               out.add("  " + base_name(file) + ":" + std::to_string(n), 4);
                               if (const std::string l = source_line(file, n); !l.empty()) out.add("  " + l, 0);
                               out.add("\n", 0);
                               v.disasm_addrs.push_back(0);
                           }
                           // (lldb pads the instruction, and names after a function's first the function, a heading here.)
                           std::string ins = trimmed(x["instruction"].str());
                           if (const size_t semi = ins.find(';'); semi != std::string::npos) {
                               std::string note = trimmed(std::string_view(ins).substr(semi + 1));
                               ins = trimmed(std::string_view(ins).substr(0, semi));
                               if (!note.empty() && note != sym) ins += "  ; " + note;
                           }
                           if (a == ip) at = static_cast<i64>(v.disasm_addrs.size());
                           out.add("    " + hex(a, width), 4);
                           out.add("  " + ins + "\n", 0);
                           v.disasm_addrs.push_back(a);
                       }
                       debug_tab_fill(*d, std::move(out.text), std::move(out.off), std::move(out.style), at >= 0 ? at : 0, 0);
                   });
}

// Breakpoints at instructions, to a session under way.
void App::debug_send_instructions() {
    DebugUi& u = *debugui_;
    if (!u.dap || u.state == DebugUi::kStarting || !u.can.instruction_breaks) return;
    std::string a = "{\"breakpoints\":[";
    for (const u64 at : u.ibreaks) a += std::string(a.back() == '[' ? "" : ",") + "{\"instructionReference\":" + quoted(hex(at)) + "}";
    u.dap->request("setInstructionBreakpoints", a + "]}");
}

// Memory at an address, or where an expression says (a pointer's value, or
// what the debugger says is its memory).
void App::debug_memory_pick() {
    DebugUi& u = *debugui_;
    if (!u.dap || u.state != DebugUi::kStopped) return notify("The program is not stopped");
    if (!u.can.read_memory) return notify("This debugger cannot read memory");
    find_choose(kFindInput, "Memory at", u.mem_name, [this](const std::string& typed) {
        DebugUi& v = *debugui_;
        const std::string e = trimmed(typed);
        if (e.empty() || !v.dap || v.state != DebugUi::kStopped) return;
        debug_memory_at(e, e);
    });
}

// Memory where an expression points: the debugger's memory reference for
// it, or its value (a pointer's: "(int *) 0x7ffe..." too).
void App::debug_memory_at(const std::string& expr, const std::string& name) {
    DebugUi& v = *debugui_;
    const std::string frame = v.frame < v.frames.size() ? ",\"frameId\":" + std::to_string(v.frames[v.frame].id) : std::string();
    ++v.asking;
    v.dap->request("evaluate", "{\"expression\":" + quoted(expr) + frame + ",\"context\":\"watch\"}", [this, expr, name](const Dap::Message& m) {
        DebugUi& w = *debugui_;
        if (w.asking) --w.asking;
        if (!m.success) return notify(expr + ": " + why_not(m.message));
        const std::string_view result = m.body()["result"].str();
        u64 a = address_of(m.body()["memoryReference"].str());
        if (!a) a = address_of(result);
        if (const size_t x = result.find("0x"); !a && x != std::string_view::npos) a = address_of(result.substr(x));
        if (!a) return notify(expr + " is no address");
        debug_memory_open(a, name);
    });
}

void App::debug_memory_open(u64 addr, const std::string& name) {
    DebugUi& u = *debugui_;
    u.mem_addr = addr & ~u64{15};
    if (!name.empty()) u.mem_name = name;
    u.mem_last.clear();
    debug_tab(u.mem_doc, "Memory");
    debug_memory_ask(0, static_cast<i32>(addr & 15));
}

void App::debug_memory_page(int by) {
    DebugUi& u = *debugui_;
    if (!u.mem_doc || !find_doc(u.mem_doc)) return;
    const u64 step = DebugUi::kMemPage;
    if (by < 0 && u.mem_addr < step) return;
    u.mem_addr = by < 0 ? u.mem_addr - step : u.mem_addr + step;
    u.mem_last.clear();
    debug_tab(u.mem_doc, "Memory");
    debug_memory_ask(0, -1);
}

// A page of memory from mem_addr, read (again).
void App::debug_memory_ask(i64 line, i32 byte) {
    DebugUi& u = *debugui_;
    if (u.mem_doc && !find_doc(u.mem_doc)) u.mem_doc = 0;  // (closed)
    if (!u.mem_doc || !u.dap || u.state != DebugUi::kStopped) return;
    const u64 addr = u.mem_addr, gen = ++u.mem_gen;
    ++u.asking;
    u.dap->request("readMemory", "{\"memoryReference\":" + quoted(hex(addr)) + ",\"count\":" + std::to_string(DebugUi::kMemPage) + "}",
                   [this, gen, addr, line, byte](const Dap::Message& m) {
                       DebugUi& v = *debugui_;
                       if (v.asking) --v.asking;
                       if (gen != v.mem_gen || !v.dap) return;
                       if (!m.success) {
                           // (gdb reads all of it or nothing: what it can, in pieces.)
                           struct Pieces {
                               std::vector<i32> now = std::vector<i32>(DebugUi::kMemPage, -1);
                               u32 left = DebugUi::kMemPage / kMemPiece;
                           };
                           auto pieces = std::make_shared<Pieces>();
                           for (u64 at = 0; at < DebugUi::kMemPage; at += kMemPiece) {
                               ++v.asking;
                               v.dap->request("readMemory", "{\"memoryReference\":" + quoted(hex(addr + at)) + ",\"count\":" + std::to_string(kMemPiece) + "}",
                                              [this, gen, addr, at, line, byte, pieces](const Dap::Message& r) {
                                                  DebugUi& w = *debugui_;
                                                  if (w.asking) --w.asking;
                                                  if (r.success) memory_take(pieces->now, addr, r, at, kMemPiece);
                                                  if (--pieces->left == 0 && gen == w.mem_gen) debug_memory_show(addr, std::move(pieces->now), line, byte);
                                              });
                           }
                           return;
                       }
                       std::vector<i32> now(DebugUi::kMemPage, -1);
                       memory_take(now, addr, m, 0, DebugUi::kMemPage);
                       debug_memory_show(addr, std::move(now), line, byte);
                   });
}

// The page shown: 16 bytes a line, as numbers and as characters, those that
// cannot be read ??, and those that have changed since it was last read in
// the accent; the cursor on `line` and the byte asked for.
void App::debug_memory_show(u64 addr, std::vector<i32> now, i64 line, i32 byte) {
    DebugUi& v = *debugui_;
    Doc* d = v.mem_doc ? find_doc(v.mem_doc) : nullptr;
    if (!d) return;
    const bool compare = v.mem_last.size() == now.size() && v.mem_last_addr == addr;
    int width = 8;
    for (u64 a = (addr + DebugUi::kMemPage - 1) >> 32; a; a >>= 4) ++width;
    v.mem_width = static_cast<u32>(width);
    Runs out;
    static const char kDigits[] = "0123456789abcdef";
    for (u64 row = 0; row < DebugUi::kMemPage / 16; ++row) {
        out.add(hex(addr + row * 16, width) + " ", 4);
        std::string chars;
        for (u64 j = 0; j < 16; ++j) {
            const i32 b = now[row * 16 + j];
            out.add(j == 8 ? "  " : " ", 0);
            if (b < 0) {
                out.add("??", 4);
                chars += ' ';
                continue;
            }
            const char two[2] = {kDigits[b >> 4], kDigits[b & 15]};
            out.add(std::string_view(two, 2), compare && v.mem_last[row * 16 + j] != b ? 3 : 0);
            chars += b >= 0x20 && b < 0x7F ? static_cast<char>(b) : '.';
        }
        out.add("  " + chars + "\n", 4);
    }
    v.mem_last = std::move(now);
    v.mem_last_addr = addr;
    // (The cursor on the byte asked for: after the address and its space, each byte is three wide, one more after the eighth.)
    const u64 col = byte < 0 ? 0 : 2 + static_cast<u64>(width) + 2 + 3 * static_cast<u64>(byte) + (byte >= 8 ? 1 : 0);
    debug_tab_fill(*d, std::move(out.text), std::move(out.off), std::move(out.style), line, col);
}

// Bytes written where the cursor is in the memory tab (typed as hex: "2a 00").
void App::debug_memory_write() {
    DebugUi& u = *debugui_;
    if (!u.mem_doc || !pane_->doc || pane_->doc->id != u.mem_doc || !u.dap || u.state != DebugUi::kStopped) return;
    if (!u.can.write_memory) return notify("This debugger cannot write memory");
    const Doc& d = *pane_->doc;
    const u64 line = d.ed.buf.line_of(d.ed.cursor()), col = d.ed.cursor() - d.ed.buf.line_start(line);
    // (The bytes start after the address, "0x" and its digits, and a space; each is three wide, with one more after the eighth; then the characters.)
    const u64 bytes = 2 + u.mem_width + 1, chars = bytes + 16 * 3 + 1 + 2;
    u64 j = col >= chars ? col - chars : col <= bytes ? 0 : (col - bytes - (col - bytes >= 25 ? 1 : 0)) / 3;
    j = std::min<u64>(j, 15);
    const u64 at = u.mem_addr + line * 16 + j;
    find_choose(kFindInput, "Bytes at " + hex(at), "", [this, at](const std::string& typed) {
        DebugUi& v = *debugui_;
        std::string raw;
        int half = -1;
        for (const char c : typed) {
            const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (c == ' ' || c == ',') continue;
            if (digit < 0) return notify("Not bytes: " + typed);
            if (half < 0) half = digit;
            else raw += static_cast<char>(half << 4 | digit), half = -1;
        }
        if (raw.empty() || half >= 0 || !v.dap || v.state != DebugUi::kStopped) return;
        ++v.asking;
        v.dap->request("writeMemory", "{\"memoryReference\":" + quoted(hex(at)) + ",\"data\":" + quoted(to_base64(raw)) + "}", [this, at](const Dap::Message& m) {
            DebugUi& w = *debugui_;
            if (w.asking) --w.asking;
            if (!m.success) return notify("Cannot write at " + hex(at) + ": " + why_not(m.message));
            debug_frame(w.frame, false);  // (the variables, the watches and the memory, as they are now)
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
    if (u.state == DebugUi::kStarting && !u.remote.empty()) status = "Starting on " + u.remote.substr(u.remote.find("://") + 3) + "\xE2\x80\xA6";  // (ssh://host, docker://name)
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
    if (!u.others.empty()) {  // (one alone is the tab's own)
        static const char* const kWord[] = {"not running", "starting", "running", "stopped"};
        heading("Sessions", 1 + u.others.size());
        Row& r = add(DebugUi::kSession, 0, u.name.empty() ? std::string("this one") : u.name);
        r.mark = "\xE2\x96\xB8", r.mark_ink = ink::kAccent;  // (the one shown)
        r.after = kWord[u.state];
        for (u32 i = 0; i < u.others.size(); ++i) {
            Row& o = add(DebugUi::kSession, i + 1, u.others[i].name.empty() ? std::string("another") : u.others[i].name);
            o.after = kWord[u.others[i].state];
        }
    }
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
                const bool plain = b.cond.empty() && b.hits.empty() && b.log.empty();
                r.mark = plain ? "\xE2\x97\x8F" : "\xE2\x97\x86";  // (the gutter's dot, or diamond)
                r.mark_ink = ink::kError;
                if (!b.cond.empty()) r.after = "if " + b.cond;
                if (!b.hits.empty()) r.after += (r.after.empty() ? "" : ", ") + std::string("hit ") + b.hits;
                if (!b.log.empty()) r.after += (r.after.empty() ? "" : ", ") + std::string("log \"") + b.log + "\"", r.mark = "\xE2\x97\x88";  // (a logpoint: a diamond in a diamond)
                if (focused && u.sel == out.size() - 1) r.key = key_symbols("Shift+F9", font);  // (the one chosen: what changes its condition)
            }
    }
    if (!u.data.empty()) {
        heading("Writes watched", u.data.size());
        for (u32 i = 0; i < u.data.size(); ++i) {
            Row& r = add(DebugUi::kData, i, u.data[i].name);
            r.mark = "\xE2\x97\x89";  // (a dot in a ring)
            r.mark_ink = ink::kError;
        }
    }
    if (!u.functions.empty()) {
        heading("Function breakpoints", u.functions.size());
        for (u32 i = 0; i < u.functions.size(); ++i) {
            Row& r = add(DebugUi::kFunction, i, u.functions[i]);
            r.mark = "\xC6\x92";  // (a small f)
            r.mark_ink = ink::kError;
        }
    }
    if (!u.ibreaks.empty()) {
        heading("Instruction breakpoints", u.ibreaks.size());
        u32 i = 0;
        for (const u64 a : u.ibreaks) {
            Row& r = add(DebugUi::kInstr, i++, hex(a));
            r.mark = "\xE2\x97\x8F";
            r.mark_ink = ink::kError;
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
        case DebugUi::kFunction: {  // its name changed (nothing: it goes)
            const u32 at = row.index;
            find_choose(kFindInput, "Function", at < u.functions.size() ? u.functions[at] : std::string(), [this, at](const std::string& typed) {
                DebugUi& v = *debugui_;
                if (at >= v.functions.size()) return;
                if (trimmed(typed).empty()) v.functions.erase(v.functions.begin() + at);
                else v.functions[at] = trimmed(typed);
                debug_send_functions();
                ++debug_gen_;
                dirty_ = true;
            });
            break;
        }
        case DebugUi::kData: break;
        case DebugUi::kSession:  // that one shown
            if (row.index > 0) debug_switch(row.index - 1);
            break;
        case DebugUi::kInstr: {  // its instruction, where the disassembly has it
            auto it = u.ibreaks.begin();
            std::advance(it, std::min<size_t>(row.index, u.ibreaks.size() - 1));
            Doc* d = u.disasm_doc ? find_doc(u.disasm_doc) : nullptr;
            const auto at = std::ranges::find(u.disasm_addrs, *it);
            if (!d || at == u.disasm_addrs.end()) break;
            debug_disasm_open();
            d->ed.move_to(d->ed.buf.line_start(static_cast<u64>(at - u.disasm_addrs.begin())), false);
            scroll_to_cursor();
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
        case SDLK_F2:  // a variable's value set
            if (u.sel < n && u.rows[u.sel].act == DebugUi::kVar) debug_set_value(u.rows[u.sel].index);
            return true;
        case SDLK_W:  // a variable's writes watched (again: no longer)
            if (u.sel < n && u.rows[u.sel].act == DebugUi::kVar) debug_watch_writes(u.rows[u.sel].index);
            return true;
        case SDLK_M:  // the memory a variable is in
            if (u.sel < n && u.rows[u.sel].act == DebugUi::kVar && u.rows[u.sel].index < u.vars.size()) {
                const DebugUi::Var& v = u.vars[u.rows[u.sel].index];
                if (!u.can.read_memory) return notify("This debugger cannot read memory"), true;
                if (v.addr) debug_memory_open(v.addr, {});
                else if (u.state == DebugUi::kStopped) debug_memory_at("&" + v.name, {});  // (gdb says where none is)
            }
            return true;
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
            if (u.sel < n && u.rows[u.sel].act == DebugUi::kData && u.rows[u.sel].index < u.data.size()) {
                u.data.erase(u.data.begin() + u.rows[u.sel].index);
                debug_send_data();
                ++debug_gen_;
                dirty_ = true;
                return true;
            }
            if (u.sel < n && u.rows[u.sel].act == DebugUi::kSession) {  // a session stopped
                const u32 at = u.rows[u.sel].index;
                if (at == 0) {
                    debug_command("stop");
                } else if (at - 1 < u.others.size()) {
                    std::swap(u.session(), u.others[at - 1]);
                    u.background = true;
                    debug_end();
                    u.background = false;
                    std::swap(u.session(), u.others[at - 1]);
                    u.others.erase(u.others.begin() + (at - 1));
                }
                ++debug_gen_;
                dirty_ = true;
                return true;
            }
            if (u.sel < n && u.rows[u.sel].act == DebugUi::kInstr && u.rows[u.sel].index < u.ibreaks.size()) {
                auto it = u.ibreaks.begin();
                std::advance(it, u.rows[u.sel].index);
                u.ibreaks.erase(it);
                debug_send_instructions();
                ++debug_gen_;
                dirty_ = true;
                return true;
            }
            if (u.sel < n && u.rows[u.sel].act == DebugUi::kFunction && u.rows[u.sel].index < u.functions.size()) {
                u.functions.erase(u.functions.begin() + u.rows[u.sel].index);
                debug_send_functions();
                ++debug_gen_;
                dirty_ = true;
                return true;
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

// Where it stopped, after the line's end; and after each line of the
// function it stopped in, from its start to there, the values of the
// variables the line names (by name, among the frame's: not in a comment or
// a string, as the colours have them, and not a member's, after . -> or ::).
// Made again only when something they are of changes.
void App::debug_marks(FrameDesc& fd) {
    if (!debugui_ || debugui_->state != DebugUi::kStopped) return;
    DebugUi& u = *debugui_;
    if (u.frame >= u.frames.size()) return;
    const DebugUi::Frame& f = u.frames[u.frame];
    for (u32 vi = 0; vi < fd.view_count && vi < kMaxViews; ++vi) {
        GridSpec& g = fd.views[vi].grid;
        Doc* d = find_doc(g.doc);
        if (!d) continue;
        u64 l = ~0ull;
        const bool instructions = u.disasm_doc && d->id == u.disasm_doc;
        if (instructions) {  // the disassembly: the instruction's line
            for (size_t k = 0; k < u.disasm_addrs.size() && f.ip; ++k)
                if (u.disasm_addrs[k] == f.ip) l = k;
        } else if (!d->ed.path.empty() && f.line && full_path(d->ed.path) == f.path) {
            l = f.line - 1;
        }
        if (l == ~0ull || l >= d->ed.buf.line_count()) continue;
        const Buffer& buf = d->ed.buf;
        const Highlights& hl = d->syntax.highlights();
        DebugUi::Inline& c = u.inline_[vi];
        const u64 key[] = {debug_gen_, d->id, d->ed.revision(), g.top_line, g.bottom(), hl.size(), hl.base, cfg_.debug_inline_values, font_->generation()};
        if (!std::equal(std::begin(key), std::end(key), std::begin(c.key))) {
            std::copy(std::begin(key), std::end(key), std::begin(c.key));
            c.notes.clear();
            c.cells.clear();
            c.texts.clear();
            std::vector<std::pair<u64, std::string>> lines;  // (each line's values)
            if (!instructions && cfg_.debug_inline_values) {
                // The frame's variables, by name (the first of a name: the innermost block's).
                std::map<std::string, std::string, std::less<>> values;
                for (const DebugUi::Var& v : u.vars) {
                    if (v.depth != 1 || v.name.empty()) continue;
                    std::string value = v.value.substr(0, v.value.find("  ("));
                    for (char& ch : value)
                        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
                    if (width_of(value) > 40) value = value.substr(0, 39) + "\xE2\x80\xA6";
                    values.try_emplace(v.name, std::move(value));
                }
                // From the start of the function it stopped in (or the top of the view) to the line.
                u64 first = g.top_line;
                const u64 at = buf.line_start(l);
                for (const SyntaxScope& sc : d->syntax.structure().scopes)
                    if ((sc.kind == 12 || sc.kind == 6) && sc.start <= at && at < sc.end) first = buf.line_of(sc.start);
                first = std::max(first, g.top_line);
                const std::vector<std::string>& names = d->syntax.style_names();
                auto quiet = [&](u64 pos) {  // (in a comment or a string)
                    if (hl.empty() || pos < hl.base || pos >= hl.end) return false;
                    const size_t i = hl.upper(pos);
                    const u8 st = i ? hl.style[i - 1] : 0;
                    return st < names.size() && (names[st].starts_with("comment") || names[st].starts_with("string"));
                };
                for (u64 line = first; !values.empty() && line <= l && line < g.bottom(); ++line) {
                    const u64 start = buf.line_start(line);
                    const std::string text = buf.text(start, d->ed.line_end(line) - start);
                    std::string said;
                    std::vector<std::string_view> seen;
                    for (size_t i = 0; i < text.size();) {
                        const auto word = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; };
                        if (!word(text[i]) || std::isdigit(static_cast<unsigned char>(text[i]))) {
                            while (i < text.size() && word(text[i])) ++i;  // (a number, all of it)
                            if (i < text.size() && !word(text[i])) ++i;
                            continue;
                        }
                        size_t e = i;
                        while (e < text.size() && word(text[e])) ++e;
                        const std::string_view name(text.data() + i, e - i);
                        const bool member = (i >= 1 && text[i - 1] == '.') || (i >= 2 && (text.compare(i - 2, 2, "->") == 0 || text.compare(i - 2, 2, "::") == 0));
                        if (const auto v = values.find(name); v != values.end() && !member && std::ranges::find(seen, name) == seen.end() && !quiet(start + i)) {
                            seen.push_back(name);
                            said += (said.empty() ? "" : ", ") + std::string(name) + " = " + v->second;
                        }
                        i = e;
                    }
                    if (!said.empty()) lines.emplace_back(line, std::move(said));
                }
            }
            Font& font = *font_;
            auto note = [&](u64 line, const std::string& stop, const std::string& said) {
                TextBox box;
                const std::string gap = stop.empty() || said.empty() ? "" : "   ";
                box.resize(std::min<u32>(width_of(stop) + width_of(gap) + width_of(said), 120), 1);
                const u32 col = put(box, font, 0, 0, stop, ink::kAccent);  // (upright: the font's italic may be a script)
                put(box, font, 0, col + width_of(gap), said, ink::kMuted);
                c.cells.push_back(std::move(box.cells));
                c.texts.push_back(std::to_string(line + 1) + ": " + stop + gap + said);
                const u64 start = buf.line_start(line);
                c.notes.push_back({line, d->ed.col_between(start, d->ed.line_end(line)) + 3, nullptr, static_cast<u32>(c.cells.back().size())});
            };
            const std::string stop = "\xE2\x97\x80 stopped" + (u.reason.empty() ? std::string() : ": " + u.reason);
            bool stopped_said = false;
            for (const auto& [line, said] : lines) {
                note(line, line == l ? stop : std::string(), said);
                stopped_said |= line == l;
            }
            if (!stopped_said && l >= g.top_line && l < g.bottom()) note(l, stop, {});
            for (size_t k = 0; k < c.notes.size(); ++k) c.notes[k].cells = c.cells[k].data();  // (once they have stopped moving)
        }
        if (c.notes.empty()) continue;
        // Each where nothing else is said (a problem's message comes first).
        std::vector<GridNote>& notes = debug_notes_[vi];
        notes.assign(g.notes.begin(), g.notes.end());
        for (const GridNote& n : c.notes)
            if (std::ranges::none_of(g.notes, [&](const GridNote& o) { return o.line == n.line; })) notes.push_back(n);
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
        if (state == "answers" ? !u.dap || u.dap->answered() : state == "stopped" ? u.state == DebugUi::kStopped && frames : u.state == DebugUi::kIdle) break;
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
    if (!u.others.empty()) {  // (the others first: "sessions: name state, ...", the one shown starred)
        std::string all = "sessions: *" + u.name + " " + kState[u.state];
        for (const DebugSession& o : u.others) all += ", " + o.name + " " + kState[o.state];
        out = all + "; " + out;
    }
    if (!u.reason.empty() && u.state == DebugUi::kStopped) out += " (" + u.reason + ")";
    if (!u.error.empty()) out += " error: " + u.error;
    if (u.threads.size() > 1) {
        u32 at = 0;
        for (u32 i = 0; i < u.threads.size(); ++i)
            if (u.threads[i].id == u.thread) at = i;
        out += "; threads: " + std::to_string(u.threads.size()) + ", #" + std::to_string(at + 1) + " shown";
        const size_t waves = static_cast<size_t>(std::ranges::count_if(u.threads, [](const DebugUi::Thread& t) { return t.name.starts_with("GPU wave"); }));
        if (waves) out += " (" + std::to_string(waves) + " GPU waves; shown: " + u.threads[at].name + ")";
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
std::string App::debug_inline_dump() {
    const DebugUi::Inline& c = debugui_->inline_[0];
    std::string out;
    for (const std::string& t : c.texts) out += (out.empty() ? "" : " | ") + t;
    return out.empty() ? "none" : out;
}

std::string App::debug_breaks_dump() {
    DebugUi& u = *debugui_;
    std::string out;
    for (const auto& [path, lines] : u.breaks)
        for (const auto& [l, b] : lines) {
            out += (out.empty() ? "" : "; ") + base_name(path) + ":" + std::to_string(l + 1);
            if (!b.cond.empty()) out += " if " + b.cond;
            if (!b.hits.empty()) out += " hit " + b.hits;
            if (!b.log.empty()) out += " log " + b.log;
        }
    for (const std::string& f : u.functions) out += (out.empty() ? "" : "; ") + f + "()";
    for (const DebugUi::Data& d : u.data) out += (out.empty() ? "" : "; ") + std::string("writes to ") + d.name;
    for (const u64 a : u.ibreaks) out += (out.empty() ? "" : "; ") + std::string("at ") + hex(a);
    return out.empty() ? "no breakpoints" : out;
}

// What Enter does on the Nth row (from 1; or, named, the variable's) of a section of the Debug tab as
// it was last drawn: threads, stack, watch, add (the row that adds an
// expression), or breaks. The row is chosen first, as the keys would.
bool App::debug_pick(const std::string& section, u32 n, const std::string& name) {
    DebugUi& u = *debugui_;
    const DebugUi::Act act = section == "threads" ? DebugUi::kThread : section == "stack" ? DebugUi::kFrame : section == "watch" ? DebugUi::kWatch
                             : section == "add"   ? DebugUi::kWatchAdd : section == "breaks" ? DebugUi::kBreak : section == "vars" ? DebugUi::kVar : section == "sessions" ? DebugUi::kSession : DebugUi::kNone;
    u32 seen = 0;
    for (u32 i = 0; i < u.rows.size(); ++i)
        if (act != DebugUi::kNone && u.rows[i].act == act && (name.empty() ? ++seen == std::max(1u, n) : u.rows[i].text.starts_with(name + " "))) {
            u.sel = i;
            debug_do(i);
            return true;
        }
    return false;
}

}  // namespace blit
