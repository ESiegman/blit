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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include <fcntl.h>
#include <signal.h>
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

// The debugger for a machine, chosen there: Xcode's lldb-dap on a Mac;
// lldb-dap where there is one elsewhere, else gdb (14 and later speak the
// protocol: gdb -i dap); debugpy for Python. A program with HIP's code for
// the GPU in it ($BLIT_DEBUG_PROGRAM has a .hip_fatbin) gets ROCm's rocgdb
// where there is one: a gdb that sees the GPU's waves as threads, its
// kernels' frames and variables. `want` (a run configuration's
// debug.adapter) is "lldb-dap" or "gdb" for that one alone, wherever it is.
// gdb's program writes where gdb's messages go, a pipe, and C's stdio holds
// back what is written to a pipe until it ends: stdbuf (coreutils) has it
// write a line at a time, as to a terminal, gdb and the program both.
std::string native(std::string_view want) {
    const std::string mac =
        "if [ \"$(uname -s)\" = Darwin ] && { [ -n \"$(xcrun --find lldb-dap 2>/dev/null)\" ] || [ -d /Applications/Xcode.app/Contents/Developer ]; }; then "
        "xcrun --find lldb-dap >/dev/null 2>&1 || { DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer && export DEVELOPER_DIR; }; "
        "exec xcrun lldb-dap; fi; ";
    const std::string rocm =
        "if [ -n \"$BLIT_DEBUG_PROGRAM\" ] && grep -q -a -m1 hip_fatbin \"$BLIT_DEBUG_PROGRAM\" 2>/dev/null; then "
        "for d in rocgdb /opt/rocm/bin/rocgdb; do if command -v $d >/dev/null 2>&1; then echo 'blit-debugger: gdb (rocgdb)' >&2; command -v stdbuf >/dev/null 2>&1 && exec stdbuf -oL $d -q -i dap; exec $d -q -i dap; fi; done; fi; ";
    const std::string lldb = "for d in lldb-dap lldb-vscode; do command -v $d >/dev/null 2>&1 && exec $d; done; ";
    const std::string gdb =
        "v=$(gdb --version 2>/dev/null | sed -n '1s/.* \\([0-9][0-9]*\\)\\..*/\\1/p'); "
        "if [ \"${v:-0}\" -ge 14 ] 2>/dev/null; then echo 'blit-debugger: gdb' >&2; command -v stdbuf >/dev/null 2>&1 && exec stdbuf -oL gdb -q -i dap; exec gdb -q -i dap; fi; ";
    if (want == "lldb-dap") return mac + lldb + "echo 'no lldb-dap here' >&2; exit 127";
    if (want == "gdb") return rocm + gdb + "if [ -n \"$v\" ]; then echo \"gdb $v is too old to be a debugger blit can use: 14 or newer speaks its protocol\" >&2; else echo 'no gdb here' >&2; fi; exit 127";
    return mac + rocm + lldb + gdb + "echo 'no debugger: install lldb, or gdb 14 or newer' >&2; exit 127";
}
// (debugpy's adapter is run with the project's Python, App::python_for(),
// and starts the program with the same.)
const char* kPython = " -m debugpy.adapter";
// On another machine: the Python python_for() would choose, chosen there in
// the project's folder (`root`, its path there: nothing is looked at over
// the link from here), and debugpy's adapter run with it.
std::string python_there(const std::string& root) {
    return "py=$(cd " + sh_word(root) + " 2>/dev/null && "
           "if [ -x .venv/bin/python ]; then echo \"$PWD/.venv/bin/python\"; "
           "elif [ -x venv/bin/python ]; then echo \"$PWD/venv/bin/python\"; "
           "elif [ -e uv.lock ] || grep -q -e uv_build -e '\\[tool\\.uv' pyproject.toml 2>/dev/null; then echo uv; fi); "
           "if [ \"$py\" = uv ]; then exec uv run python" + std::string(kPython) + "; fi; "
           "exec \"${py:-python3}\"" + kPython;
}
// What native() says on the error stream when the debugger it starts is gdb
// (which counts a breakpoint's hits from 0, where the others count from 1).
constexpr std::string_view kGdbSays = "blit-debugger: gdb";

// Why an expression could not be evaluated, in a line: a debugger's
// "error:" line, without where in the expression it was ("<user
// expression 1>:1:1: use of undeclared identifier 'x'" is the last part).
std::string why_not(std::string_view m) {
    std::string_view line;
    // (debugpy's is Python's traceback: its last line says what it was, "NameError: name 'x' is not defined".)
    if (m.starts_with("Traceback (most recent call last):")) {
        while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' ')) m.remove_suffix(1);
        if (const size_t nl = m.rfind('\n'); nl != std::string_view::npos) m.remove_prefix(nl + 1);
    }
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

// `over` laid over `base` (a run configuration's arguments over those blit
// sends): its members take the place of base's of the same name, an
// object's member by member, all the way down.
void laid_over(std::string& out, json::Value base, json::Value over) {
    if (!base.is_object() || !over.is_object()) return json::dump(out, over.exists() ? over : base);
    out += '{';
    bool first = true;
    auto member = [&](std::string_view key) {
        out += first ? "" : ",";
        first = false;
        json::quote(out, key);
        out += ':';
    };
    for (const json::Value m : base) {
        member(m.key());
        laid_over(out, m, over[m.key()]);
    }
    for (const json::Value m : over)
        if (!base[m.key()].exists()) {
            member(m.key());
            json::dump(out, m);
        }
    out += '}';
}

std::string laid_over(const std::string& base, const std::string& over) {
    json::Doc b, o;
    if (over.empty() || !b.parse(base) || !o.parse(over)) return base;
    std::string out;
    laid_over(out, b.root(), o.root());
    return out;
}

// Whether a program reads what is typed to it (its input given a terminal
// tab, where F5 starts it, unless its run configuration says otherwise):
// a Python file that calls input() or reads sys.stdin; a program (ELF or
// Mach-O, 64-bit) whose symbols from its libraries include C's stdin, C++'s
// std::cin, scanf, getchar or gets. Here alone (another machine's would be
// a round trip), and only the headers and the symbol table are read.
bool reads_input(const std::string& path) {
    if (path.empty() || is_remote(path)) return false;
    if (path.ends_with(".py")) {
        std::string text;
        if (!fs_read(path, &text, 1u << 20)) return false;
        for (size_t at = text.find("input("); at != std::string::npos; at = text.find("input(", at + 1))
            if (at == 0 || !(std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_' || text[at - 1] == '.')) return true;
        return text.find("sys.stdin") != std::string::npos;
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    auto at = [&](u64 off, void* into, u64 n) { return ::pread(fd, into, n, static_cast<off_t>(off)) == static_cast<ssize_t>(n); };
    auto u16_at = [](const u8* p) { return static_cast<u32>(p[0] | p[1] << 8); };
    auto u32_at = [](const u8* p) { return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 | static_cast<u32>(p[3]) << 24; };
    auto u64_at = [&](const u8* p) { return static_cast<u64>(u32_at(p)) | static_cast<u64>(u32_at(p + 4)) << 32; };
    auto reader = [](std::string_view name) {
        while (name.starts_with('_') && !name.starts_with("_Z") && !name.starts_with("__stdinp")) name.remove_prefix(1);  // (Mach-O's names start with _)
        if (name.starts_with("_Z")) return name == "_ZSt3cin" || name == "_ZSt4wcin" || name == "_ZNSt3__13cinE" || name == "_ZNSt3__14wcinE";
        if (const size_t v = name.find('@'); v != std::string_view::npos) name = name.substr(0, v);
        return name == "stdin" || name == "__stdinp" || name == "scanf" || name == "isoc99_scanf" || name == "isoc23_scanf" || name == "getchar" || name == "gets";
    };
    constexpr u64 kMost = 64u << 20;  // (a symbol table larger than this is not read)
    bool found = false;
    std::vector<u8> head(64);
    if (at(0, head.data(), 64) && std::memcmp(head.data(), "\x7f" "ELF\x02\x01", 6) == 0) {
        // ELF, 64-bit, little-endian: the dynamic symbols (else all of them), those not defined here.
        const u64 shoff = u64_at(&head[40]);
        const u32 shentsize = u16_at(&head[58]), shnum = u16_at(&head[60]);
        std::vector<u8> sh(static_cast<size_t>(shentsize) * shnum);
        if (shentsize >= 64 && shnum && at(shoff, sh.data(), sh.size())) {
            auto section = [&](u32 i) { return &sh[static_cast<size_t>(i) * shentsize]; };
            i64 sym = -1;
            for (u32 i = 0; i < shnum; ++i) {
                const u32 type = u32_at(section(i) + 4);
                if (type == 11) sym = i;  // SHT_DYNSYM
                else if (type == 2 && sym < 0) sym = i;  // SHT_SYMTAB
            }
            if (sym >= 0) {
                const u8* s = section(static_cast<u32>(sym));
                const u32 link = u32_at(s + 40);
                const u64 off = u64_at(s + 24), size = u64_at(s + 32);
                if (link < shnum && size <= kMost && u64_at(section(link) + 32) <= kMost) {
                    std::vector<u8> syms(size), strs(u64_at(section(link) + 32));
                    if (at(off, syms.data(), size) && at(u64_at(section(link) + 24), strs.data(), strs.size()))
                        for (u64 i = 0; i + 24 <= size && !found; i += 24) {
                            const u32 name = u32_at(&syms[i]);
                            if (u16_at(&syms[i + 6]) != 0 || name >= strs.size()) continue;  // (defined here: not one of its libraries')
                            const char* n = reinterpret_cast<const char*>(&strs[name]);
                            found = reader(std::string_view(n, ::strnlen(n, strs.size() - name)));
                        }
                }
            }
        }
    } else {
        // Mach-O, 64-bit (or a universal file's slices): LC_SYMTAB's symbols that are undefined and external.
        std::vector<u64> slices;
        if (u32_at(head.data()) == 0xBEBAFECA) {  // FAT_MAGIC, big-endian
            const u32 n = std::min<u32>(__builtin_bswap32(u32_at(&head[4])), 8);
            std::vector<u8> arch(static_cast<size_t>(n) * 20);
            if (at(8, arch.data(), arch.size()))
                for (u32 i = 0; i < n; ++i) slices.push_back(__builtin_bswap32(u32_at(&arch[i * 20 + 8])));
        } else {
            slices.push_back(0);
        }
        for (const u64 base : slices) {
            u8 mh[32];
            if (found || !at(base, mh, 32) || u32_at(mh) != 0xFEEDFACF) continue;
            const u32 ncmds = u32_at(mh + 16), sizeofcmds = u32_at(mh + 20);
            if (sizeofcmds > kMost) continue;
            std::vector<u8> cmds(sizeofcmds);
            if (!at(base + 32, cmds.data(), sizeofcmds)) continue;
            for (u32 i = 0, off = 0; i < ncmds && off + 8 <= sizeofcmds && !found; ++i) {
                const u32 cmd = u32_at(&cmds[off]), size = u32_at(&cmds[off + 4]);
                if (cmd == 2 && off + 24 <= sizeofcmds) {  // LC_SYMTAB
                    const u32 symoff = u32_at(&cmds[off + 8]), nsyms = u32_at(&cmds[off + 12]), stroff = u32_at(&cmds[off + 16]), strsize = u32_at(&cmds[off + 20]);
                    if (static_cast<u64>(nsyms) * 16 > kMost || strsize > kMost) break;
                    std::vector<u8> syms(static_cast<size_t>(nsyms) * 16), strs(strsize);
                    if (!at(base + symoff, syms.data(), syms.size()) || !at(base + stroff, strs.data(), strsize)) break;
                    for (u32 k = 0; k < nsyms && !found; ++k) {
                        const u8* s = &syms[static_cast<size_t>(k) * 16];
                        const u32 name = u32_at(s);
                        if ((s[4] & 0xE0) || (s[4] & 0x0E) != 0 || !(s[4] & 0x01) || name >= strsize) continue;  // (a debugger's entry, defined here, or not external)
                        const char* n = reinterpret_cast<const char*>(&strs[name]);
                        found = reader(std::string_view(n, ::strnlen(n, strsize - name)));
                    }
                }
                if (size < 8) break;
                off += size;
            }
        }
    }
    ::close(fd);
    return found;
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
        bool exception_options = false, exception_info = false;  // (conditions on its exception filters; what it says of an exception)
    } can;
    // The exceptions it can stop at (initialize's exceptionBreakpointFilters),
    // each as it has it unless chosen otherwise (DebugUi::exceptions).
    struct Filter {
        std::string id, label;
        bool on = false;         // (its default)
        bool condition = false;  // (it takes a condition)
    };
    std::vector<Filter> filters;
    // The exception it stopped at, as exceptionInfo says: what it is in a
    // line, then its details (the stack trace, the exceptions inside it).
    std::string exception;
    std::vector<std::string> exception_lines;
    std::string how;  // what the session began with: "launch", "attach" (a process), "core"
    enum State : u8 { kIdle, kStarting, kRunning, kStopped } state = kIdle;
    std::string remote;  // "ssh://host" when the program is on another machine
    bool gdb = false;    // the debugger is gdb (its hit counts are from 0)
    bool rocm = false;   // and ROCm's rocgdb: the GPU's waves are threads, named by `info threads`
    std::string python;  // a .py program's: the Python debugpy runs in (as words for a shell; there, the shell that chooses it)
    bool going = false;  // the program has started (the "process" event): what gdb says before is its own
    i64 thread = 0;      // the thread whose stack is shown (and which the steps step)
    i64 stopped_thread = 0;
    struct Thread {
        i64 id = 0;
        std::string name;
        bool wave = false;  // one of the GPU's waves (rocgdb's): those are after the program's own
    };
    std::vector<Thread> threads;  // (when it is stopped)
    // The program's own threads, those before the waves.
    size_t cpu_threads() const { return static_cast<size_t>(std::ranges::partition_point(threads, [](const Thread& t) { return !t.wave; }) - threads.begin()); }
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
    // The lines printed since the debug console was given them, each with
    // its style there (print_style's as it was printed: 2 an error's, 4 the
    // debugger's own).
    std::vector<std::pair<std::string, u8>> said;
    u8 print_style = 0;
    int exit_code = -1;
    bool end_soon = false;  // (the session ends once the debugger's answers have been run: not inside one)
    // What it began with, to restart it as it was: the request and its
    // arguments as sent (DAP's restart, where the debugger has it:
    // can_restart), else how to start it again; and whether F5 started it
    // (it is built again first).
    std::string began, began_args;
    std::function<void()> again;
    bool can_restart = false, by_f5 = false;
    // F5's: the program's environment given in the launch's arguments too,
    // the run configuration's own laid over them (`over`), its input and
    // output in a terminal tab (`terminal`), and the debugger's commands
    // once the session has started (`after`, run as initialized comes).
    bool configured = false, terminal = false;
    std::string over;
    std::vector<std::string> after;
    // That terminal, where blit gives the program one itself (a debugger
    // here, lldb-dap or gdb: the program started on its device, `tty`), not
    // asked for by the debugger (runInTerminal): its tab, whose program
    // holds the device open and reads nothing, ended with the session.
    u64 term_doc = 0;
    std::string tty;
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
            said.emplace_back(output.back(), print_style);
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
    BuildTask::Debug conf;         // (and its "debug": the debugger, the request, the arguments laid over blit's...)
    // Breakpoints: by file (its full path, ssh:// for another machine's), by
    // line (from 0), each with what it waits for, if anything.
    struct Break {
        std::string cond;  // it stops only where this is true
        std::string hits;  // and only at this hit (a number: the Nth time it is reached)
        std::string log;   // a logpoint: this said (its {expressions} filled in) where it would stop, and no stop
        bool off = false;  // disabled: kept (and drawn so), not given to the debugger
        // What the debugger has said of it, while a session has it: whether
        // it is verified (there is code there it can stop at; not yet, in a
        // library not loaded, or never, on a line with none), and if not why;
        // the line it was asked for (from 1) where it put it on another; its
        // id for it (and which debugger's), which its breakpoint events name.
        enum Said : u8 { kUnsaid, kVerified, kUnverified } said = kUnsaid;
        std::string why;
        u32 moved_from = 0;
        i64 id = 0;
        const Dap* by = nullptr;
        void unsaid() { said = kUnsaid, why.clear(), moved_from = 0, id = 0, by = nullptr; }
        // As the debugger describes it (an answer's, an event's).
        void take(const json::Value& x, const Dap* dap) {
            said = x["verified"].boolean() ? kVerified : kUnverified;
            // (Why not: its message where it gives one (gdb's, where it could
            // not make it: a condition that names what is not there); else the
            // reason the protocol has for it, gdb's "pending"; lldb-dap says
            // neither.)
            why = said == kVerified ? std::string() : std::string(x["message"].exists() ? x["message"].str() : x["reason"].str());
            while (!why.empty() && (why.back() == '.' || why.back() == '\n' || why.back() == ' ')) why.pop_back();  // (one of several said in a row)
            if (x["id"].exists()) id = x["id"].integer(), by = dap;
        }
        // Its mark, in the gutter and in the Debug tab: a dot; one that waits
        // for something, a diamond; a logpoint, a diamond in a diamond. One
        // not verified is hollow; one disabled, in the quieter ink.
        struct Mark {
            u32 cp;
            const char* utf8;
        };
        Mark mark() const {
            if (said == kUnverified && !off) return cond.empty() && hits.empty() && log.empty() ? Mark{0x25CB, "\xE2\x97\x8B"} : Mark{0x25C7, "\xE2\x97\x87"};
            if (!log.empty()) return {0x25C8, "\xE2\x97\x88"};
            return cond.empty() && hits.empty() ? Mark{0x25CF, "\xE2\x97\x8F"} : Mark{0x25C6, "\xE2\x97\x86"};
        }
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
    enum Act : u8 { kNone, kProgram, kArgs, kCwd, kStart, kContinue, kPause, kOver, kInto, kOut, kStop, kThread, kFrame, kVar, kWatch, kWatchAdd, kBreak, kFunction, kData, kInstr, kSession, kWaves, kException, kRestart };
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
    bool waves_open = false;  // the GPU's waves listed (below the variables), not one row
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
    // The exception filters chosen in the Debug tab, by the filter's id: on
    // or off, and the condition it stops on (where the debugger takes one).
    // Those not chosen are as the debugger has them.
    struct Choice {
        std::string cond;
        bool on = false;
    };
    std::map<std::string, Choice> exceptions;
    Choice choice(const Filter& f) const {
        if (const auto it = exceptions.find(f.id); it != exceptions.end()) return it->second;
        if (!python.empty() && (f.id == "uncaught" || f.id == "raised")) return {{}, f.id == "uncaught"};  // (debugpy: uncaught on, raised off, whatever it says)
        return {{}, f.on};
    }
    // The debug console: its tab (0: none), its lines (what the debuggers
    // printed, what was typed and the answers), what is typed on its last
    // line and where in it the cursor is, and the lines typed before.
    struct Said {
        std::string text;
        u8 style = 0;   // (as the tab's runs: 2 an error's, 3 the accent, 4 quieter)
        u32 depth = 0;  // (a member's, under its value)
        i64 ref = 0;    // a value with members (its variablesReference): opened a level at a time
        bool open = false;
        u64 id = 0;
    };
    std::vector<Said> con;
    std::weak_ptr<Doc> con_tab;  // (wherever it is: in the terminals' split while that is hidden too)
    u64 con_doc = 0, con_ids = 0;
    std::string con_input;
    size_t con_at = 0;
    bool con_typing = true;  // (with Vim's keys: Esc leaves the line for the lines above, i or a comes back to it)
    bool con_changed = false;
    std::vector<std::string> con_history;
    size_t con_hist = 0;
    std::string con_draft;  // (what was being typed when the history was gone into)
    u64 con_back = 0;       // the split the keyboard came from
    static constexpr size_t kConLines = 3000, kConHistory = 100;
    Said& say(std::string text, u8 style, i64 ref = 0, u32 depth = 0) {
        con.push_back({std::move(text), style, depth, ref, false, ++con_ids});
        con_changed = true;
        return con.back();
    }
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

// A breakpoint on the cursor's line (or the line given, from 0), or not.
void App::debug_toggle_break(i64 at) {
    DebugUi& u = *debugui_;
    Doc& d = *pane_->doc;
    if (debug_disasm_front()) {  // at the cursor's instruction
        const u64 line = at >= 0 ? static_cast<u64>(at) : d.ed.buf.line_of(d.ed.cursor());
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
    const u32 line = static_cast<u32>(at >= 0 ? static_cast<u64>(at) : d.ed.buf.line_of(d.ed.cursor()));
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
    // (An exception filter chosen there: the condition it stops on, where the debugger takes one; on, then.)
    if (panel_has_keyboard() && panel_tab() == 4 && u.sel < u.rows.size() && u.rows[u.sel].act == DebugUi::kException && u.rows[u.sel].index < u.filters.size()) {
        const DebugUi::Filter f = u.filters[u.rows[u.sel].index];
        if (!f.condition || !u.can.exception_options) return notify("This debugger takes no condition for " + f.label);
        return find_choose(kFindInput, "Condition", u.choice(f).cond, [this, f](const std::string& typed) {
            debugui_->exceptions[f.id] = {trimmed(typed), true};
            debug_send_exceptions();
            ++debug_gen_;
            dirty_ = true;
        });
    }
    if (!debug_chosen_break(-1, &path, &line)) return;
    DebugUi::Break was;
    if (auto f = u.breaks.find(path); f != u.breaks.end())
        if (auto b = f->second.find(line); b != f->second.end()) was = b->second;
    find_choose(kFindInput, "Condition", was.cond, [this, path, line, hits = was.hits, log = was.log](const std::string& cond) {
        find_choose(kFindInput, "Hit count", hits, [this, path, line, log, cond = trimmed(cond)](const std::string& typed) {
            // Last, a message to log in place of stopping ({x} for x's value).
            find_choose(kFindInput, "Log message", log, [this, path, line, cond, hits = trimmed(typed)](const std::string& message) {
                DebugUi& v = *debugui_;
                DebugUi::Break& b = v.breaks[path][line];
                b.cond = cond, b.hits = hits, b.log = trimmed(message);
                if (Doc* d = pane_->doc; d && !d->ed.path.empty() && full_path(d->ed.path) == path) v.follow(*this, *d);
                debug_send_breaks(path);
                ++debug_gen_;
                dirty_ = true;
            });
        });
    });
}

// The breakpoint chosen in the Debug tab while it has the keyboard; else
// the cursor's line (or the line given, from 0), whose file's breakpoints
// follow its lines from now.
bool App::debug_chosen_break(i64 at, std::string* path, u32* line) {
    DebugUi& u = *debugui_;
    if (at < 0 && panel_has_keyboard() && panel_tab() == 4 && u.sel < u.rows.size() && u.rows[u.sel].act == DebugUi::kBreak) {  // (4: the Debug tab)
        u32 n = 0;
        for (const auto& [p, lines] : u.breaks)
            for (const auto& [l, b] : lines)
                if (n++ == u.rows[u.sel].index) *path = p, *line = l;
        return !path->empty();
    }
    Doc& d = *pane_->doc;
    if (d.ed.path.empty() || d.term || d.prof || d.loading) return false;
    *path = full_path(d.ed.path);
    *line = static_cast<u32>(at >= 0 ? static_cast<u64>(at) : d.ed.buf.line_of(d.ed.cursor()));
    u.follow(*this, d);
    return true;
}

// A breakpoint disabled (kept, drawn so, and not given to the debugger),
// or enabled again: the one chosen in the Debug tab, or the cursor's line's.
void App::debug_enable_break(i64 at) {
    DebugUi& u = *debugui_;
    std::string path;
    u32 line = 0;
    if (!debug_chosen_break(at, &path, &line)) return;
    const auto f = u.breaks.find(path);
    const auto b = f == u.breaks.end() ? DebugUi::Lines::iterator() : f->second.find(line);
    if (f == u.breaks.end() || b == f->second.end()) return notify("No breakpoint on this line");
    b->second.off = !b->second.off;
    b->second.unsaid();
    debug_send_breaks(path);
    ++debug_gen_;
    dirty_ = true;
}

// A file's breakpoints, to a session under way (those disabled left out).
void App::debug_send_breaks(const std::string& path) {
    DebugUi& u = *debugui_;
    debug_each_other([&] { debug_send_breaks(path); });  // (every session has them)
    if (!u.dap || u.state == DebugUi::kStarting) return;  // (all are sent when the debugger is ready for them)
    std::string a = "{\"source\":{\"path\":" + quoted(u.there(path)) + "},\"breakpoints\":[";
    std::vector<u32> asked;  // (the lines, as the answer has them in turn)
    if (auto it = u.breaks.find(path); it != u.breaks.end()) {
        bool first = true;
        for (auto& [l, b] : it->second) {
            b.id = 0, b.by = nullptr;  // (the answer gives them their ids again: events meanwhile are of those there were)
            if (b.off) continue;
            asked.push_back(l);
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
        const bool listed = std::ranges::find(asked, u.run_to_line) != asked.end();
        if (!listed) a += std::string(a.back() == '[' ? "" : ",") + "{\"line\":" + std::to_string(u.run_to_line + 1) + "}";
    }
    a += "]}";
    u.dap->request("setBreakpoints", a, [this, path, asked](const Dap::Message& m) { debug_breaks_said(path, asked, m.success, m.body()["breakpoints"], m.message); });
}

// What the debugger says of a file's breakpoints (each, in the order they
// were sent): verified or not, and why; and where it put one on another line
// than it was asked for (a line with no code: the next that has some), it
// is on that line from now. An answer about breakpoints that have changed
// since (an edit moved them, one was added) is passed over: the next counts.
// (What the session in front says: those aside have them too, and say much
// the same, with ids of their own.)
void App::debug_breaks_said(const std::string& path, const std::vector<u32>& asked, bool ok, const json::Value& answers, const std::string& message) {
    DebugUi& u = *debugui_;
    const auto f = u.background ? u.breaks.end() : u.breaks.find(path);
    if (f == u.breaks.end()) return;
    DebugUi::Lines& lines = f->second;
    if (static_cast<size_t>(std::ranges::count_if(lines, [](const auto& l) { return !l.second.off; })) != asked.size()) return;
    for (const u32 l : asked)
        if (!lines.contains(l) || lines.at(l).off) return;
    std::vector<std::pair<u32, u32>> moves;
    size_t i = 0;
    for (const json::Value x : answers) {
        if (i == asked.size()) break;  // (run to the cursor's, after them)
        DebugUi::Break& b = lines.at(asked[i]);
        const i64 line = x["line"].integer();
        b.take(x, u.dap.get());
        if (b.said == DebugUi::Break::kVerified && line > 0 && static_cast<u32>(line - 1) != asked[i]) moves.emplace_back(asked[i], static_cast<u32>(line - 1));
        ++i;
    }
    for (; !ok && i < asked.size(); ++i) {  // (refused, all of them: why)
        DebugUi::Break& b = lines.at(asked[i]);
        b.said = DebugUi::Break::kUnverified;
        b.why = why_not(message);
    }
    debug_breaks_moved(path, moves);
    ++debug_gen_;
    dirty_ = true;
}

// Breakpoints the debugger has on other lines than they were asked for
// (from, to): each on its line now (in the session kept so), the line it
// was asked for said. One that comes to a line with one already is one with
// it (that one stays), and the debugger is given the file's again.
void App::debug_breaks_moved(const std::string& path, const std::vector<std::pair<u32, u32>>& moves) {
    DebugUi& u = *debugui_;
    if (moves.empty()) return;
    DebugUi::Lines& lines = u.breaks[path];
    std::vector<std::pair<u32, DebugUi::Break>> taken;
    for (const auto& [from, to] : moves) {
        auto node = lines.extract(from);
        if (node.empty()) continue;
        if (!node.mapped().moved_from) node.mapped().moved_from = from + 1;
        taken.emplace_back(to, std::move(node.mapped()));
    }
    bool merged = false;
    for (auto& [to, b] : taken) {
        if (b.moved_from == to + 1) b.moved_from = 0;  // (back where it was asked for)
        merged |= !lines.try_emplace(to, std::move(b)).second;
    }
    if (merged) debug_send_breaks(path);
}

// A breakpoint event: one the debugger has changed (verified once its
// library is loaded, put on another line), made (gdb's `break` typed to it)
// or taken away.
void App::debug_break_event(const json::Value& body) {
    DebugUi& u = *debugui_;
    if (u.background) return;  // (the session in front's, as its answers)
    const json::Value x = body["breakpoint"];
    const std::string_view reason = body["reason"].str();
    const i64 id = x["id"].integer();
    std::string path;
    u32 at = 0;
    DebugUi::Break* b = nullptr;
    for (auto& [p, lines] : u.breaks)
        for (auto& [l, one] : lines)
            if (id && one.id == id && one.by == u.dap.get()) path = p, at = l, b = &one;
    const i64 line = x["line"].integer();
    if (reason == "removed" && b) {
        u.breaks[path].erase(at);
        if (u.breaks[path].empty()) {
            u.breaks.erase(path);
            u.unfollow(*this, path);
        }
    } else if (reason == "changed" && b) {
        b->take(x, u.dap.get());
        if (b->said == DebugUi::Break::kVerified && line > 0 && static_cast<u32>(line - 1) != at) debug_breaks_moved(path, {{at, static_cast<u32>(line - 1)}});
    } else if (reason == "new" && !b && line > 0 && !x["source"]["path"].str().empty()) {
        const std::string where = u.here(x["source"]["path"].str());
        DebugUi::Break& made = u.breaks[where][static_cast<u32>(line - 1)];
        made.take(x, u.dap.get());
        for (const std::unique_ptr<Pane>& p : panes_)  // (its file's lines followed, where it is open)
            for (const std::shared_ptr<Doc>& d : p->docs)
                if (!d->ed.path.empty() && full_path(d->ed.path) == where) u.follow(*this, *d);
    } else {
        return;
    }
    ++debug_gen_;
    dirty_ = true;
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
        if (const auto b = u.gutter_breaks->find(static_cast<u32>(line)); b != u.gutter_breaks->end())
            return (font_->glyph(b->second.mark().cp) & (Font::kWide - 1)) | ((b->second.off ? kInkMuted : kInkDiagnostic) << kCellInkShift);
    return 0;
}

// A click in that cell sets a breakpoint on its line, or takes it away. A
// test's mark there runs the test and a fold's opens it, as before (where
// a breakpoint is drawn in their place, the click is the breakpoint's).
bool App::debug_gutter_click(const SDL_MouseButtonEvent& b) {
    if (!debugui_ || b.button != SDL_BUTTON_LEFT || !gutter_px()) return false;
    Doc& d = *pane_->doc;
    const bool disasm = debugui_->disasm_doc && d.id == debugui_->disasm_doc;
    if (!disasm && (d.ed.path.empty() || d.term || d.prof || d.loading)) return false;
    const int px = static_cast<int>(b.x * density_), py = static_cast<int>(b.y * density_), cw = font_->cell_w(), ch = font_->cell_h();
    if (px < pane_->x0 || px >= pane_->x0 + cw + look().edge || py < pane_->text_y || py >= pane_->y1) return false;
    const i64 y = py - pane_->text_y + std::llround(pane_->scroll_y);
    if (y < 0 || static_cast<u64>(y / ch) >= view_line_count()) return false;
    const u64 line = line_of_row(static_cast<u64>(y / ch));
    if (line >= d.ed.buf.line_count()) return false;
    if (!debug_gutter(d, line)) {
        if (test_gutter(d, line)) return false;
        if (const Folds* f = folds_on(); f && f->header(line)) return false;
    }
    // (With Alt, a breakpoint there disabled or enabled again.)
    if (!disasm && (SDL_GetModState() & SDL_KMOD_ALT) && std::ranges::contains(debug_break_lines(d), line)) return debug_enable_break(static_cast<i64>(line)), true;
    debug_toggle_break(static_cast<i64>(line));
    return true;
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

// ---------------------------------------------------------------- in the folder's session

namespace {

// A condition, a hit count, a message or an expression in a session's line:
// its tabs, line ends and backslashes written as \t, \n and \\.
std::string session_field(std::string_view s) {
    std::string out;
    for (const char c : s) out += c == '\t' ? "\\t" : c == '\n' ? "\\n" : c == '\\' ? "\\\\" : std::string(1, c);
    return out;
}

std::string session_unfield(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 == s.size()) {
            out += s[i];
            continue;
        }
        const char c = s[++i];
        out += c == 't' ? '\t' : c == 'n' ? '\n' : c;
    }
    return out;
}

}  // namespace

// The breakpoints (where, and what each waits for), the functions' and the
// expressions watched, as lines of the folder's session (appsession.cpp):
//   break LINE (from 0) CONDITION HITS MESSAGE PATH (last: it may have tabs)
//   breakoff ...       (the same, of one disabled)
//   function NAME
//   watch EXPRESSION
//   exception FILTER ON (1 or 0) CONDITION   (an exception filter chosen in the Debug tab)
//   console LINE   (typed in the debug console, the oldest first)
std::string App::debug_session_text() const {
    if (!debugui_) return {};
    const DebugUi& u = *debugui_;
    std::string out;
    for (const auto& [path, lines] : u.breaks) {
        if (path.find('\n') != std::string::npos) continue;
        for (const auto& [l, b] : lines)
            out += (b.off ? "breakoff\t" : "break\t") + std::to_string(l) + "\t" + session_field(b.cond) + "\t" + session_field(b.hits) + "\t" + session_field(b.log) + "\t" + path + "\n";
    }
    for (const std::string& f : u.functions) out += "function\t" + session_field(f) + "\n";
    for (const DebugUi::Watch& w : u.watches) out += "watch\t" + session_field(w.expr) + "\n";
    for (const auto& [id, c] : u.exceptions) out += "exception\t" + session_field(id) + "\t" + (c.on ? "1" : "0") + "\t" + session_field(c.cond) + "\n";
    for (const std::string& line : u.con_history) out += "console\t" + session_field(line) + "\n";
    return out;
}

// A folder's session come back: its breakpoints and watches take the place
// of those there were (a session under way is given them).
void App::debug_session_take(const std::vector<std::string>& lines) {
    DebugUi& u = *debugui_;
    std::set<std::string> paths;
    for (const auto& [path, l] : u.breaks) paths.insert(path);
    u.breaks.clear();
    u.functions.clear();
    u.watches.clear();
    u.exceptions.clear();
    u.con_history.clear();
    for (const std::string& line : lines) {
        std::vector<std::string_view> f;
        std::string_view rest = line;
        while (f.size() < 5) {  // (break's first five fields; the path is the rest)
            const size_t tab = rest.find('\t');
            if (tab == std::string_view::npos) break;
            f.push_back(rest.substr(0, tab));
            rest.remove_prefix(tab + 1);
        }
        if (f.empty()) continue;
        if ((f[0] == "break" || f[0] == "breakoff") && f.size() == 5 && !rest.empty()) {
            const std::string path(rest);
            DebugUi::Break& b = u.breaks[path][static_cast<u32>(std::max(0, std::atoi(std::string(f[1]).c_str())))];
            b = {};
            b.cond = session_unfield(f[2]), b.hits = session_unfield(f[3]), b.log = session_unfield(f[4]);
            b.off = f[0] == "breakoff";
            paths.insert(path);
        } else if (f[0] == "function" && f.size() == 1 && !rest.empty()) {
            u.functions.push_back(session_unfield(rest));
        } else if (f[0] == "watch" && f.size() == 1 && !rest.empty()) {
            u.watches.push_back({session_unfield(rest), {}, false});
        } else if (f[0] == "exception" && f.size() == 3 && !f[1].empty()) {
            u.exceptions[session_unfield(f[1])] = {session_unfield(rest), f[2] == "1"};
        } else if (f[0] == "console" && f.size() == 1 && !rest.empty()) {
            u.con_history.push_back(session_unfield(rest));
        }
    }
    if (u.con_history.size() > DebugUi::kConHistory) u.con_history.erase(u.con_history.begin(), u.con_history.end() - DebugUi::kConHistory);
    u.con_hist = u.con_history.size();
    debug_send_exceptions();
    for (const std::string& path : paths) {
        if (!u.breaks.contains(path)) u.unfollow(*this, path);
        debug_send_breaks(path);
    }
    // (The files open whose breakpoints these are follow their lines from now.)
    for (const std::unique_ptr<Pane>& p : panes_)
        for (const std::shared_ptr<Doc>& d : p->docs)
            if (!d->ed.path.empty() && !u.breaks.empty() && u.breaks.contains(full_path(d->ed.path))) u.follow(*this, *d);
    debug_send_functions();
    debug_watch_eval();
    ++debug_gen_;
    dirty_ = true;
}

// A run configuration chosen: what it runs is what is debugged.
void App::debug_take(const std::string& program, const std::string& args, const std::string& cwd, const std::vector<std::string>& env, const BuildTask::Debug& how) {
    DebugUi& u = *debugui_;
    u.program = program;
    u.args = args;
    u.cwd = cwd;
    u.env = env;
    u.conf = how;
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
bool App::debug_open(const std::string& near, const std::string& dir, bool python, const std::string& adapter) {
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
    u.exception.clear();
    u.exception_lines.clear();
    u.exit_code = -1;
    u.asking = 0;
    u.again = {};  // (what started it says how to start it again, and the rest of what F5 gives)
    u.can_restart = u.by_f5 = u.configured = u.terminal = false;
    u.over.clear();
    u.after.clear();
    u.term_doc = 0;
    u.tty.clear();
    // (On another machine the connection is waited for and the debugger started
    // there on the Dap's own thread: the tab says Starting… meanwhile, and what
    // is asked of it below is sent once it has started.)
    std::string err;
    std::string exports;  // (the debugger has them, and the program it starts)
    for (const std::string& e : u.env) exports += "export " + sh_word(e) + "; ";
    if (const std::string prog = near.empty() || !remote ? near : there; !prog.empty() && !python)
        exports += "BLIT_DEBUG_PROGRAM=" + sh_word(prog) + "; export BLIT_DEBUG_PROGRAM; ";  // (what it debugs: HIP's gets rocgdb)
    // (On another machine, chosen there: in the project's folder if that is
    // on the same machine, else in the program's.)
    std::string launch = native(adapter);
    if (!python && !adapter.empty() && adapter != "lldb-dap" && adapter != "gdb") {  // a command line of the run configuration's
        const std::string first = adapter.substr(0, adapter.find(' '));
        launch = (base_name(first).find("gdb") != std::string::npos ? "echo 'blit-debugger: gdb' >&2; " : "") + adapter;  // (arm-none-eabi-gdb, gdb-multiarch: a gdb)
    }
    u.python.clear();
    if (python && u.remote.empty()) {
        u.python = python_for(build_root(), false);
        launch = "exec " + u.python + kPython;
    } else if (python) {
        std::string root_host, root_there;
        u.python = launch = python_there(split_remote(build_root(), &root_host, &root_there) && root_host == host ? root_there : parent_of(there));
    }
    if (!u.dap->start({"/bin/sh", "-c", exports + launch}, dir, wake, &err)) {
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
    u.again = [this, pid] { debug_attach(pid); };
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
    u.again = [this, program, core] { debug_core(program, core); };
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
    build_first("Debug", [this] { debug_launch(); });  // (the folder's build first, where it has one)
}

void App::debug_launch() {
    DebugUi& u = *debugui_;
    if (u.state != DebugUi::kIdle || u.program.empty()) return;  // (started meanwhile, another way)
    const BuildTask::Debug& how = u.conf;
    const std::string dir = !u.cwd.empty() ? u.cwd : parent_of(u.program);
    const bool python = how.adapter == "debugpy" || (how.adapter.empty() && u.program.ends_with(".py"));
    const bool attach = how.request == "attach";
    // (Its input and output in a terminal tab: as the run configuration says,
    // else where the program reads what is typed to it.)
    const bool terminal = !attach && (how.console == "terminal" || (how.console.empty() && reads_input(u.program)));
    if (!debug_open(u.program, dir, python, python ? std::string() : how.adapter)) return;
    u.name = base_name(u.program);
    u.again = [this] { debug_launch(); };  // (built again first by debug_restart)
    u.by_f5 = true;
    u.configured = !attach;
    u.terminal = terminal;
    u.over = how.arguments;
    u.after = how.after;
    // A terminal for it: with a debugger of the machine's here, a tab whose
    // device the program is started on (lldb's target.input-path and the
    // like, gdb's inferior-tty: lldb-dap's runInTerminal has the program
    // started by a launcher it attaches to, which macOS does not let it
    // attach to), its program a shell that holds the device open and reads
    // nothing (what is typed is the program's; Ctrl+C is not the shell's);
    // else (debugpy, another machine, a debugger of the configuration's
    // own) the debugger asked to run it in one, which it asks of blit.
    if (terminal && !python && u.remote.empty() && (how.adapter.empty() || how.adapter == "lldb-dap" || how.adapter == "gdb")) {
        u.term_doc = debug_terminal(dir, "exec /bin/sh -c 'trap \"\" INT QUIT TSTP; trap \"exit 0\" TERM HUP; while :; do sleep 86400 & wait $!; done'", "Debug: " + u.name);
        u.tty = term_tty(u.term_doc);
    }
    if (attach) {  // (to what the run configuration's arguments name: a process, a gdb server)
        debug_begin("attach", "{\"program\":" + quoted(u.there(u.program)) + ",\"cwd\":" + quoted(u.there(dir)) + "}");
        return;
    }
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
                               (python ? std::string(",\"type\":\"python\",\"request\":\"launch\",\"console\":") + (terminal ? "\"integratedTerminal\"" : "\"internalConsole\"") + ",\"justMyCode\":true" : "") +
                               "}";
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
    // (F5's launch: the program's environment as each debugger takes it, gdb
    // and debugpy an object, lldb-dap a list of NAME=value.)
    std::string env_object = "{", env_list = "[";
    for (const std::string& e : u.env) {
        const size_t eq = e.find('=');
        env_object += (env_object.size() > 1 ? "," : "") + quoted(e.substr(0, eq)) + ":" + quoted(eq == std::string::npos ? std::string() : e.substr(eq + 1));
        env_list += (env_list.size() > 1 ? "," : "") + quoted(e);
    }
    env_object += "}", env_list += "]";
    u.dap->request("initialize",
                   "{\"clientID\":\"blit\",\"clientName\":\"blit\",\"adapterID\":\"blit\",\"linesStartAt1\":true,\"columnsStartAt1\":true,"
                   "\"pathFormat\":\"path\",\"supportsVariableType\":true,\"supportsRunInTerminalRequest\":true}",
                   [this, request, arguments, env_object, env_list](const Dap::Message& m) {
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
                       v.can.exception_options = c["supportsExceptionFilterOptions"].boolean();
                       v.can.exception_info = c["supportsExceptionInfoRequest"].boolean();
                       v.filters.clear();
                       for (const json::Value f : c["exceptionBreakpointFilters"])
                           if (!f["filter"].str().empty())
                               v.filters.push_back({std::string(f["filter"].str()), std::string(f["label"].str().empty() ? f["filter"].str() : f["label"].str()), f["default"].boolean(),
                                                    f["supportsCondition"].boolean()});
                       v.can_restart = c["supportsRestartRequest"].boolean();
                       // What it is told to do, as this debugger takes it (which it is
                       // has been said by now: native() says so before it starts it),
                       // and a run configuration's own arguments laid over that.
                       v.gdb = v.dap->errors().find(kGdbSays) != std::string::npos;
                       std::string args = arguments;
                       if (v.configured && args.ends_with('}')) {
                           std::string more;
                           if (env_list.size() > 2) more += ",\"env\":" + (v.gdb || !v.python.empty() ? env_object : env_list);
                           if (v.terminal && !v.tty.empty() && v.gdb) {  // (the device blit's terminal tab is: gdb's own word for it, before it runs the program)
                               v.after.insert(v.after.begin(), "set inferior-tty " + v.tty);
                           } else if (v.terminal && !v.tty.empty()) {  // (lldb's, before it launches it)
                               more += ",\"preRunCommands\":[";
                               for (const char* s : {"input", "output", "error"}) more += quoted("settings set target." + std::string(s) + "-path " + v.tty) + (*s == 'e' ? "]" : ",");
                           } else if (v.terminal && !v.gdb && v.python.empty()) {  // (the debugger asked to run it in one: lldb-dap's old name for that and its new)
                               more += ",\"runInTerminal\":true,\"console\":\"integratedTerminal\"";
                           }
                           args.insert(args.size() - 1, more);
                       }
                       args = laid_over(args, v.over);
                       v.began = request;
                       v.began_args = args;
                       v.dap->request(request, args, [this, request](const Dap::Message& r) {
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
    if (u.dap && !u.remote.empty()) ran_there(u.remote);  // (on another machine: what the program wrote there, looked for)
    for (auto& [path, lines] : u.breaks)  // (its ids for the breakpoints were its own)
        for (auto& [l, b] : lines)
            if (b.by && b.by == u.dap.get()) b.id = 0, b.by = nullptr;
    if (u.dap && !u.dap->ended()) {
        u.dap->request("disconnect", u.how == "attach" ? "{\"terminateDebuggee\":false}" : "{\"terminateDebuggee\":true}");
        u.dap->stop(300);  // (the adapter lets go of the program as it ends)
    }
    u.dap.reset();
    u.state = DebugUi::kIdle;
    if (const i64 pid = u.term_doc ? term_pid(std::exchange(u.term_doc, 0)) : 0; pid > 0) ::kill(static_cast<pid_t>(pid), SIGTERM);  // (the terminal's shell ends: its tab says it is done)
    u.data.clear();  // (the debugger's ids for them were this session's, as the program's addresses were)
    u.ibreaks.clear();
    u.run_to_path.clear();
    u.exception.clear(), u.exception_lines.clear();
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
    if (!u.background && u.state == DebugUi::kIdle && u.others.empty())  // (no session has them now: as they were asked for)
        for (auto& [path, lines] : u.breaks)
            for (auto& [l, b] : lines) b.unsaid();
    ++debug_gen_;
    dirty_ = true;
}

// The session ended and started again as it began: F5's built again first
// (and nothing happens to it if that build fails); then DAP's restart where
// the debugger has it, the program started again in the same session (its
// breakpoints kept), else the session ended and another begun in its place.
void App::debug_restart() {
    DebugUi& u = *debugui_;
    if (u.state == DebugUi::kIdle || !u.dap) return notify("Not debugging");
    auto restart = [this] {
        DebugUi& v = *debugui_;
        if (v.state == DebugUi::kIdle || !v.dap) return;  // (ended meanwhile)
        if (v.can_restart && !v.began.empty()) {
            v.dap->request("restart", "{\"arguments\":" + v.began_args + "}", [this](const Dap::Message& r) {
                if (!r.success) debugui_->print("cannot restart: " + (r.message.empty() ? std::string("the debugger said no") : r.message) + "\n");
            });
            v.state = DebugUi::kRunning;
            v.going = false;
            v.frames.clear();
            v.threads.clear();
            v.vars.clear();
            v.output.clear();
            v.partial.clear();
            v.reason.clear();
            v.error.clear();
            v.exit_code = -1;
            ++v.vars_gen;
            ++v.watch_gen;
            v.data.clear();  // (the program's addresses, and the debugger's ids for its variables, were the last run's)
            debug_send_data();
            ++debug_gen_;
            dirty_ = true;
            return;
        }
        std::function<void()> again = std::move(v.again);  // (kept here: what starts it again makes the session's anew)
        v.background = true;  // (its place is not given to another session meanwhile)
        debug_end();
        v.background = false;
        if (again) again();
    };
    if (u.by_f5) build_first("Restart", restart);
    else restart();
}

// The debugger's runInTerminal: its program (`args`, in `cwd`, with `env`
// after the run configuration's environment) in a terminal tab of the
// terminals' split, as the build's output is (the keyboard stays where it
// was), here or on the session's machine; the answer, its process (the
// shell's, which gives way to it). "" where it could not be started.
std::string App::debug_in_terminal(const std::string& text) {
    DebugUi& u = *debugui_;
    json::Doc doc;
    if (!doc.parse(text)) return {};
    const json::Value a = doc.root();
    std::string cmd;
    for (const std::string& e : u.env) cmd += "export " + sh_word(e) + "; ";
    for (const json::Value e : a["env"]) {
        const std::string_view name = e.key();
        if (name.empty() || name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::string_view::npos) continue;
        cmd += e.type() == json::Type::Null ? "unset " + std::string(name) + "; " : "export " + std::string(name) + "=" + sh_word(e.str()) + "; ";
    }
    // (Each word as it is, unless the debugger says they are a shell's to read.)
    const bool shell = a["argsCanBeInterpretedByShell"].boolean();
    std::string words;
    for (const json::Value w : a["args"]) words += " " + (shell ? std::string(w.str()) : sh_word(w.str()));
    if (words.empty()) return {};
    cmd += "exec" + words;
    const std::string_view cwd = a["cwd"].str();
    const std::string dir = !cwd.empty() ? u.here(cwd) : !u.cwd.empty() ? u.cwd : parent_of(u.program);
    const std::string_view title = a["title"].str();
    const u64 id = debug_terminal(dir, cmd, title.empty() ? "Debug: " + u.name : std::string(title));
    if (!id) return {};
    const i64 pid = term_pid(id);
    return pid > 0 ? "{\"processId\":" + std::to_string(pid) + ",\"shellProcessId\":" + std::to_string(pid) + "}" : std::string("{}");
}

// A terminal tab for the program's input and output: in the terminals'
// split, as the build's output is; the keyboard stays where it was.
u64 App::debug_terminal(const std::string& dir, const std::string& command, const std::string& title) {
    Pane* const editing = pane_;
    Pane* into = dock_show(editing);
    if (!into) into = editing;  // (no room for another split)
    pane_ = into;
    const u64 id = term_open(dir, command, title);
    bool editing_still = false;
    for (const std::unique_ptr<Pane>& p : panes_) editing_still |= p.get() == editing;
    focus_pane(editing_still ? editing : panes_.front().get());
    return id;
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
    u.exception.clear(), u.exception_lines.clear();
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
        // (From the debug console, or another tab of the terminals' split, the
        // source is shown in the split the keyboard came from, and the
        // keyboard stays where it is.)
        Pane* const keep = pane_;
        if (pane_ == dock_pane() || (pane_->doc && debug_console_doc(*pane_->doc)))
            for (const std::unique_ptr<Pane>& p : panes_)
                if (p.get() != keep && p.get() != dock_pane() && !p->floating && (pane_ == keep || p->id == u.con_back)) pane_ = p.get();
        const bool moved = pane_ != keep;
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
        if (moved) {
            for (const std::unique_ptr<Pane>& p : panes_)
                if (p.get() == keep) pane_ = keep;
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
            if (const auto it = waves.find(t.id); it != waves.end()) t.name = it->second, t.wave = true;
        std::ranges::stable_partition(v.threads, [](const DebugUi::Thread& t) { return !t.wave; });
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
    if (u.con_changed) debug_console_fill(false);
    ++debug_gen_;
    dirty_ = true;
}

void App::debug_sync_session(bool* stopped) {
    DebugUi& u = *debugui_;
    if (!u.dap) return;
    for (const Dap::Message& m : u.dap->poll()) {
        const json::Value b = m.body();
        if (m.type == "request" && m.name == "runInTerminal") {  // the program, in a terminal tab
            std::string arguments;
            if (m.doc) json::dump(arguments, m.doc->root()["arguments"]);
            const std::string answer = debug_in_terminal(arguments);
            u.dap->respond(m, !answer.empty(), answer.empty() ? std::string_view("{}") : std::string_view(answer));
        } else if (m.type == "request") {  // (it asks for something blit does not do: another session)
            u.dap->respond(m, false);
        } else if (m.name == "initialized") {
            u.state = DebugUi::kRunning;
            u.gdb = u.dap->errors().find(kGdbSays) != std::string::npos;  // (said before it started: here by now)
            u.rocm = u.dap->errors().find("blit-debugger: gdb (rocgdb)") != std::string::npos;
            // A run configuration's commands for the debugger, in its console's
            // context, before the breakpoints and the program's start (what
            // each says, or why it could not, among what the program printed).
            for (const std::string& a : u.after)
                u.dap->request("evaluate", "{\"expression\":" + quoted(a) + ",\"context\":\"repl\"}", [this, a](const Dap::Message& r) {
                    DebugUi& v = *debugui_;
                    std::string said = r.success ? std::string(r.body()["result"].str()) : a + ": " + why_not(r.message);
                    if (!said.empty() && !said.ends_with('\n')) said += '\n';
                    v.print(said);
                    ++debug_gen_;
                    dirty_ = true;
                });
            for (const auto& [path, lines] : u.breaks) debug_send_breaks(path);
            debug_send_functions();
            debug_send_instructions();
            debug_send_exceptions();
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
            u.exception.clear(), u.exception_lines.clear();
            if (b["reason"].str() == "exception") debug_exception_info(u.thread);
            // Every thread there is (each one's stack can be chosen), and the stack of the one that stopped.
            u.threads.clear();
            ++u.asking;
            u.dap->request("threads", "{}", [this](const Dap::Message& r) {
                DebugUi& v = *debugui_;
                if (v.asking) --v.asking;
                if (!r.success || v.state != DebugUi::kStopped) return;
                v.threads.clear();
                // (An adapter that names the GPU's waves so has them after the program's threads too.)
                for (json::Value t : r.body()["threads"]) v.threads.push_back({t["id"].integer(), std::string(t["name"].str()), t["name"].str().find("AMDGPU Wave") != std::string_view::npos});
                std::ranges::stable_partition(v.threads, [](const DebugUi::Thread& t) { return !t.wave; });
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
            u.exception.clear(), u.exception_lines.clear();
        } else if (m.name == "breakpoint") {
            debug_break_event(b);
        } else if (m.name == "process") {
            u.going = true;
        } else if (m.name == "capabilities") {  // (what it can do, said again: lldb-dap says it can restart once it has launched)
            if (const json::Value r = b["capabilities"]["supportsRestartRequest"]; r.exists()) u.can_restart = r.boolean();
        } else if (m.name == "output") {
            const std::string_view cat = b["category"].str();
            // (gdb tells of breakpoints in a program it has not loaded yet,
            // "No source file named ...", as if the program had said it.)
            // (And after, its word that it has put a breakpoint somewhere: "Breakpoint 2 at 0x...: file f.c, line 7.")
            const bool gdb_own = u.gdb && cat == "stdout" && (!u.going || gdb_breakpoint_said(b["output"].str()));
            // (Nor lldb's word of the commands blit gave it for the program's terminal, as it runs them.)
            const std::string_view out = b["output"].str();
            const bool for_tty = !u.tty.empty() && cat == "console" &&
                                 ((out == "Running preRunCommands:\n" && u.over.find("preRunCommands") == std::string::npos) ||
                                  (out.starts_with("(lldb) settings set target.") && out.find(u.tty) != std::string_view::npos));
            // (In the debug console, the program's errors and what the debugger
            // says as an error stand out, and the debugger's own word is quieter.)
            u.print_style = cat == "stderr" || cat == "important" ? 2 : cat == "console" || gdb_own ? 4 : 0;
            if (cat == "important" || gdb_own) {  // (the debugger's own: the console's alone)
                const std::string_view text = b["output"].str();
                for (size_t at = 0; at < text.size();) {
                    const size_t nl = std::min(text.find('\n', at), text.size());
                    u.said.emplace_back(std::string(text.substr(at, nl - at)), u.print_style);
                    at = nl + 1;
                }
            } else if (cat != "telemetry" && !for_tty) {
                u.print(out);
            }
            u.print_style = 0;
        } else if (m.name == "exited") {
            u.exit_code = static_cast<int>(b["exitCode"].integer(-1));
            u.print("[exited with code " + std::to_string(u.exit_code) + "]\n");
        } else if (m.name == "terminated") {
            u.end_soon = true;
        }
    }
    for (auto& [text, style] : u.said) {  // (to the debug console: every session's)
        if (!text.empty() && text.back() == '\r') text.pop_back();
        u.say(std::move(text), style);
    }
    u.said.clear();
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
            if (!u.python.empty() && (why.find("No module named 'debugpy'") != std::string::npos || why.find("No module named debugpy") != std::string::npos)) {
                // (There, the Python it was is the one the error names: Python
                // starts its message with its own path.)
                const std::string py = python_named(u.remote.empty() ? u.python : why.substr(0, why.find(": ")), build_root());
                why ="debugpy is not installed for " + py + (py.starts_with("uv run") ? " (uv add --dev debugpy)" : "");
            }
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

namespace {

// What a completions answer makes of what is typed: each completion, the
// whole of what would be typed with it in. lldb's own commands (with their
// help, "frame -- Commands for ...") only where a command can be typed.
std::vector<std::string> completions_of(const std::string& typed, const json::Value& targets, bool commands) {
    // The word being typed, which a completion takes the place of (where the debugger says no other place).
    size_t word = typed.size();
    while (word > 0 && (std::isalnum(static_cast<unsigned char>(typed[word - 1])) || typed[word - 1] == '_')) --word;
    std::vector<std::string> all;
    for (const json::Value t : targets) {
        const std::string label(t["label"].str());
        if (!commands && label.find(" -- ") != std::string::npos && label.ends_with(".")) continue;  // (not an expression)
        std::string text(t["text"].exists() ? t["text"].str() : t["label"].str());
        if (const size_t d = text.find(" -- "); d != std::string::npos) text.resize(d);
        std::string whole;
        if (t["start"].exists()) {
            // (Its start is counted from 1, as blit asked; debugpy counts
            // from 0 all the same. blit asks at the end of what is typed,
            // so a range counted from 0 ends just there and one counted
            // from 1 never does: that settles it, even for a length of 0
            // (Tab straight after a dot). Else what a completion takes the
            // place of is the start of it ("desc" of "describe"): where
            // that is so only counted from 0, it is counted from 0.)
            const i64 start = std::max<i64>(0, t["start"].integer()), length = std::max<i64>(0, t["length"].integer());
            auto begins = [&](size_t at) {
                if (at > typed.size()) return false;
                const std::string_view was = std::string_view(typed).substr(at, static_cast<size_t>(length));
                return text.starts_with(was);
            };
            size_t at = static_cast<size_t>(std::max<i64>(0, start - 1));
            if (start > 0 && (static_cast<size_t>(start + length) == typed.size() || (!begins(at) && begins(static_cast<size_t>(start))))) at = static_cast<size_t>(start);
            at = std::min(at, typed.size());
            const size_t len = std::min<size_t>(static_cast<size_t>(length), typed.size() - at);
            whole = typed.substr(0, at) + text + typed.substr(at + len);
        } else {
            whole = text.starts_with(typed.substr(word)) ? typed.substr(0, word) + text : typed + text;
        }
        if (std::ranges::find(all, whole) == all.end()) all.push_back(std::move(whole));
    }
    return all;
}

// What several completions share (more than is typed, or nothing).
std::string completions_share(const std::vector<std::string>& all) {
    std::string common = all.empty() ? std::string() : all[0];
    for (const std::string& c : all)
        while (!c.starts_with(common)) common.pop_back();
    return common;
}

}  // namespace

// What is typed to be evaluated, completed as the debugger would (its
// completions request): one answer goes in; several that share more than
// is typed, that much; else a list of them, the one chosen going in.
void App::debug_complete(const std::string& typed) {
    DebugUi& u = *debugui_;
    if (!u.dap || u.state != DebugUi::kStopped) return;
    const std::string frame = u.frame < u.frames.size() ? ",\"frameId\":" + std::to_string(u.frames[u.frame].id) : std::string();
    u.dap->request("completions", "{\"text\":" + quoted(typed) + ",\"column\":" + std::to_string(typed.size() + 1) + frame + "}", [this, typed](const Dap::Message& m) {
        if (!m.success || find_typed() != typed) return;  // (typed on meanwhile, or the box shut)
        const std::vector<std::string> all = completions_of(typed, m.body()["targets"], false);
        if (all.empty()) return;
        if (all.size() == 1) return find_set_typed(all[0]);
        if (const std::string common = completions_share(all); common.size() > typed.size()) return find_set_typed(common);
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
        control(DebugUi::kRestart, "\xE2\x86\xBB", "Restart", chord_text(SDLK_F5, static_cast<SDL_Keymod>(kPrimaryMod | SDL_KMOD_SHIFT)).c_str());
        control(DebugUi::kStop, "\xE2\x96\xA0", "Stop", "Shift+F5");
    } else {
        control(DebugUi::kPause, "\xE2\x80\x96", "Pause", "F6");
        control(DebugUi::kRestart, "\xE2\x86\xBB", "Restart", chord_text(SDLK_F5, static_cast<SDL_Keymod>(kPrimaryMod | SDL_KMOD_SHIFT)).c_str());
        control(DebugUi::kStop, "\xE2\x96\xA0", "Stop", "Shift+F5");
    }
    // A section: its heading, with a count where it is a list.
    auto heading = [&](const char* title, size_t count) {
        add(DebugUi::kNone, 0, "");
        Row& r = add(DebugUi::kNone, 0, title, ink::kHeading);
        if (count) r.after = std::to_string(count);
    };
    if (u.state == DebugUi::kStopped && !u.exception.empty()) {  // the exception it stopped at, and what the debugger says of it
        heading("Exception", 0);
        add(DebugUi::kNone, 0, u.exception, ink::kError);
        for (size_t i = 0; i < u.exception_lines.size() && i < 40; ++i) add(DebugUi::kNone, 0, u.exception_lines[i], ink::kMuted, 2);
    }
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
    // A thread's row. (A wave's name is put in as it is drawn: of thousands, only those on show.)
    auto thread_row = [&](u32 i, u32 indent) {
        const DebugUi::Thread& t = u.threads[i];
        Row& r = add(DebugUi::kThread, i, t.wave ? std::string() : t.name.empty() ? "Thread " + std::to_string(t.id) : t.name, t.id == u.thread ? ink::kStrong : ink::kText, indent);
        r.mark = t.id == u.thread ? "\xE2\x96\xB8" : " ";  // (the one whose stack is shown)
        r.mark_ink = ink::kAccent;
        if (t.id == u.stopped_thread && !u.reason.empty()) r.after = u.reason;
    };
    // The GPU's waves (there may be thousands) are one row, which opens to
    // list them below the variables; the one that stopped, and the one
    // whose stack is shown, are under it all the same.
    const u32 cpu = static_cast<u32>(u.cpu_threads()), all = static_cast<u32>(u.threads.size());
    if (all > 1) {  // (one alone is the stack's own)
        heading("Threads", all);
        for (u32 i = 0; i < cpu; ++i) thread_row(i, 0);
        if (cpu < all) {
            Row& r = add(DebugUi::kWaves, 0, "GPU waves");
            r.mark = u.waves_open ? "\xE2\x96\xBE" : "\xE2\x96\xB8";
            r.after = std::to_string(all - cpu);
            for (const i64 id : {u.stopped_thread, u.thread})
                for (u32 i = cpu; i < all; ++i)
                    if (u.threads[i].id == id) {
                        if (out.back().act != DebugUi::kThread || out.back().index != i) thread_row(i, 2);
                        break;
                    }
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
    if (all > 1 && cpu < all && u.waves_open) {
        heading("GPU waves", all - cpu);
        for (u32 i = cpu; i < all; ++i) thread_row(i, 0);
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
                // (Its mark as the gutter's; one disabled quiet, and what the debugger said of it first.)
                Row& r = add(DebugUi::kBreak, n++, base_name(path) + ":" + std::to_string(l + 1), b.off ? ink::kMuted : ink::kText);
                r.mark = b.mark().utf8;
                r.mark_ink = b.off ? ink::kMuted : ink::kError;
                auto then = [&](const std::string& s) { r.after += (r.after.empty() ? "" : ", ") + s; };
                if (b.off) then("disabled");
                else if (b.said == DebugUi::Break::kUnverified) then(b.why.empty() ? std::string("not verified") : "not verified: " + b.why);
                if (b.moved_from) then("moved from line " + std::to_string(b.moved_from));
                if (!b.cond.empty()) then("if " + b.cond);
                if (!b.hits.empty()) then("hit " + b.hits);
                if (!b.log.empty()) then("log \"" + b.log + "\"");
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
    if (!u.filters.empty()) {  // the exceptions it can stop at, each on or off (Enter), with its condition (Shift+F9)
        heading("Exceptions", 0);
        for (u32 i = 0; i < u.filters.size(); ++i) {
            const DebugUi::Choice c = u.choice(u.filters[i]);
            Row& r = add(DebugUi::kException, i, u.filters[i].label, c.on ? ink::kText : ink::kMuted);
            r.mark = c.on ? "\xE2\x97\x89" : "\xE2\x97\x8B";  // (a dot in a ring, or a ring)
            r.mark_ink = c.on ? ink::kError : ink::kMuted;
            if (!c.cond.empty()) r.after = "if " + c.cond;
            if (focused && u.sel == out.size() - 1 && u.filters[i].condition && u.can.exception_options) r.key = key_symbols("Shift+F9", font);
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
        const std::string& text = row.act == DebugUi::kThread && row.text.empty() && row.index < u.threads.size() ? u.threads[row.index].name : row.text;
        c = put(box, font, 2 + r, c, fit_text(text, room), row.ink);
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
        case DebugUi::kRestart: debug_restart(); break;
        case DebugUi::kThread: debug_thread(row.index); break;
        case DebugUi::kWaves: u.waves_open = !u.waves_open; break;
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
        case DebugUi::kException:  // stopped at, or not
            if (row.index < u.filters.size()) {
                DebugUi::Choice c = u.choice(u.filters[row.index]);
                c.on = !c.on;
                u.exceptions[u.filters[row.index].id] = c;
                debug_send_exceptions();
            }
            break;
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
        case SDLK_SPACE:  // (on a breakpoint: disabled, or enabled again)
            if (u.sel < n && u.rows[u.sel].act == DebugUi::kBreak) return debug_enable_break(), true;
            debug_do(u.sel);
            return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: debug_do(u.sel); return true;
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
        case SDLK_LEFT:  // a variable opened or shut, or the GPU's waves listed or not
            if (u.sel < n && u.rows[u.sel].act == DebugUi::kWaves) u.waves_open = k.key == SDLK_RIGHT;
            if (u.sel < n && u.rows[u.sel].act == DebugUi::kThread && k.key == SDLK_LEFT && u.waves_open && u.rows[u.sel].index < u.threads.size() && u.threads[u.rows[u.sel].index].wave) {
                u.waves_open = false;  // (Left on a wave of the list: shut, back on the waves' row)
                for (u32 i = 0; i < n; ++i)
                    if (u.rows[i].act == DebugUi::kWaves) u.sel = i;
            }
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
                const u32 col = put(box, font, 0, 0, stop, u.exception.empty() ? ink::kAccent : ink::kError);  // (upright: the font's italic may be a script)
                put(box, font, 0, col + width_of(gap), said, ink::kMuted);
                c.cells.push_back(std::move(box.cells));
                c.texts.push_back(std::to_string(line + 1) + ": " + stop + gap + said);
                const u64 start = buf.line_start(line);
                c.notes.push_back({line, d->ed.col_between(start, d->ed.line_end(line)) + 3, nullptr, static_cast<u32>(c.cells.back().size())});
            };
            // (At an exception, what it is, as the debugger says: the Debug tab has the rest.)
            const std::string stop = !u.exception.empty() ? "\xE2\x97\x80 exception: " + u.exception : "\xE2\x97\x80 stopped" + (u.reason.empty() ? std::string() : ": " + u.reason);
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

// ---------------------------------------------------------------- exceptions

// The exceptions it stops at (and on what condition), to a session under
// way: every session's, each of those its own debugger has.
void App::debug_send_exceptions() {
    DebugUi& u = *debugui_;
    debug_each_other([&] { debug_send_exceptions(); });
    if (!u.dap || u.state == DebugUi::kStarting) return;  // (sent when the debugger is ready for them)
    std::string filters, options;
    for (const DebugUi::Filter& f : u.filters) {
        const DebugUi::Choice c = u.choice(f);
        if (!c.on) continue;
        if (!c.cond.empty() && f.condition && u.can.exception_options) options += std::string(options.empty() ? "" : ",") + "{\"filterId\":" + quoted(f.id) + ",\"condition\":" + quoted(c.cond) + "}";
        else filters += std::string(filters.empty() ? "" : ",") + quoted(f.id);
    }
    u.dap->request("setExceptionBreakpoints", "{\"filters\":[" + filters + "]" + (u.can.exception_options ? ",\"filterOptions\":[" + options + "]" : std::string()) + "}");
}

// What the debugger says of the exception it stopped at, where it can say:
// what it is, in a line; then its message, its stack trace and the
// exceptions inside it (debugpy's), a line each.
void App::debug_exception_info(i64 thread) {
    DebugUi& u = *debugui_;
    if (!u.dap || !u.can.exception_info) return;
    ++u.asking;
    u.dap->request("exceptionInfo", "{\"threadId\":" + std::to_string(thread) + "}", [this, thread](const Dap::Message& m) {
        DebugUi& v = *debugui_;
        if (v.asking) --v.asking;
        if (!m.success || v.state != DebugUi::kStopped || v.stopped_thread != thread) return;
        const json::Value b = m.body();
        const std::string id(b["exceptionId"].str()), what(b["description"].str());
        // (lldb-dap's id is its filter's, or "signal", and its description says it all, "C++ Throw",
        // "signal SIGABRT"; debugpy's is the exception's type, its description the message.)
        const bool filter = std::ranges::any_of(v.filters, [&](const DebugUi::Filter& f) { return f.id == id; });
        v.exception = what.empty() ? id : id.empty() || filter || what.find(id) != std::string::npos ? what : id + ": " + what;
        for (char& c : v.exception)
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        v.exception_lines.clear();
        auto lines = [&](std::string_view text, const std::string& lead) {
            for (size_t at = 0; at < text.size();) {
                const size_t nl = std::min(text.find('\n', at), text.size());
                std::string l(text.substr(at, nl - at));
                at = nl + 1;
                while (!l.empty() && (l.back() == ' ' || l.back() == '\r')) l.pop_back();
                for (char& c : l)
                    if (c == '\t') c = ' ';
                if (!l.empty()) v.exception_lines.push_back(lead + l);
            }
        };
        auto details = [&](auto&& self, const json::Value d, int depth) -> void {
            if (!d.is_object() || depth > 4) return;
            const std::string_view message = d["message"].str(), type = d["typeName"].str();
            if (depth > 0) lines("caused by " + std::string(type) + (type.empty() || message.empty() ? "" : ": ") + std::string(message), {});
            else if (!message.empty() && message != what) lines(message, {});
            lines(d["stackTrace"].str(), {});
            for (const json::Value inner : d["innerException"]) self(self, inner, depth + 1);
        };
        details(details, b["details"], 0);
        ++debug_gen_;
        dirty_ = true;
    });
}

// ---------------------------------------------------------------- the debug console

namespace {

constexpr std::string_view kPrompt = "\xE2\x80\xBA ";  // (before what is typed: a ›)

}  // namespace

bool App::debug_console_doc(const Doc& d) const { return debugui_ && debugui_->con_doc && d.id == debugui_->con_doc; }

// The debug console's tab, the keyboard on it: in the terminals' split
// (made, or brought back as it was, as for a terminal) unless it was moved
// elsewhere. From there (toggle), the keyboard goes back to the split it
// came from.
void App::debug_console(bool toggle) {
    DebugUi& u = *debugui_;
    Pane* const from = pane_;
    if (toggle && from->doc && debug_console_doc(*from->doc) && !panel_has_keyboard() && !find_open_) {
        for (const std::unique_ptr<Pane>& p : panes_)
            if (p->id == u.con_back && p.get() != from) focus_pane(p.get());
        dirty_ = true;
        return;
    }
    Pane* where = nullptr;
    size_t at = 0;
    Doc* d = u.con_doc ? find_doc(u.con_doc, &where, &at) : nullptr;
    if (!d) {
        Pane* const dock = dock_show(from);  // (and the console with it, if it is there)
        d = u.con_doc ? find_doc(u.con_doc, &where, &at) : nullptr;
        if (!d) {
            where = dock ? dock : from;
            pane_ = where;
            at = where->docs.size();
            d = add_doc(at);
            d->title = "Debug console";
            d->read_only = true;
            u.con_doc = d->id;
            u.con_tab = where->docs[at];
            pane_ = from;
        }
    }
    if (where != from) u.con_back = from->id;
    pane_ = where;
    tab_show(at);
    pane_ = from;
    place_panes(px_w_, px_h_);  // (a split just made had no tab to place its text by)
    focus_pane(where);
    panel_focus(false);
    u.con_typing = true;
    debug_console_fill(true);
    dirty_ = true;
}

// The console's text made again from its lines (the oldest let go of past
// kConLines): each a line, a value with members marked as the Variables
// view marks one, then the line typed on. The cursor stays where it was,
// unless it was on that line or `to_input`: there, where what is typed is.
void App::debug_console_fill(bool to_input) {
    DebugUi& u = *debugui_;
    u.con_changed = false;
    if (u.con.size() > DebugUi::kConLines) u.con.erase(u.con.begin(), u.con.end() - DebugUi::kConLines);
    const std::shared_ptr<Doc> d = u.con_tab.lock();
    if (!d) return;
    const bool on_line = d->ed.buf.line_of(d->ed.cursor()) + 1 >= d->ed.buf.line_count();
    Runs out;
    for (const DebugUi::Said& s : u.con) {
        if (s.depth) out.add(std::string(2 * s.depth, ' '), 0);
        if (s.ref > 0) out.add(s.open ? "\xE2\x96\xBE " : "\xE2\x96\xB8 ", 4);  // (a small triangle, down or right)
        out.add(s.text, s.style);
        out.add("\n", 0);
    }
    out.add(kPrompt, 3);
    out.add(u.con_input, 0);
    d->style_bits[2] = kInkDiagnostic << kCellInkShift;
    debug_tab_fill(*d, std::move(out.text), std::move(out.off), std::move(out.style), to_input || on_line ? static_cast<i64>(u.con.size()) : -1, kPrompt.size() + u.con_at);
}

// The line typed on, again (what is typed, and the cursor in it), alone.
void App::debug_console_line() {
    DebugUi& u = *debugui_;
    const std::shared_ptr<Doc> d = u.con_tab.lock();
    if (!d) return;
    Editor& ed = d->ed;
    const u64 start = ed.buf.line_start(ed.buf.line_count() - 1) + kPrompt.size();
    if (start > ed.buf.size() || ed.buf.line_count() != u.con.size() + 1) return debug_console_fill(true);
    ed.replace(start, ed.buf.size(), u.con_input);
    ed.mark_saved();
    if (d->marks) d->marks->end = ed.buf.size();  // (what is typed is plain: no run of its own to move)
    ed.move_to(start + u.con_at, false);
    Pane* where = nullptr;
    find_doc(d->id, &where);
    if (where && where->doc == d.get()) {
        Pane* const keep = pane_;
        pane_ = where;
        scroll_to_cursor();
        pane_ = keep;
    }
    dirty_ = true;
}

// Enter: the line to the debugger as its own command line takes it (an
// evaluate in the "repl" context: lldb-dap takes an expression or one of
// its commands, gdb its commands, debugpy Python), in the frame shown; its
// answer under it, a value with members opened a level.
void App::debug_console_send() {
    DebugUi& u = *debugui_;
    const std::string line = u.con_input;
    u.con_input.clear();
    u.con_at = 0;
    u.con_draft.clear();
    if (trimmed(line).empty()) return debug_console_fill(true);
    if (u.con_history.empty() || u.con_history.back() != line) u.con_history.push_back(line);
    if (u.con_history.size() > DebugUi::kConHistory) u.con_history.erase(u.con_history.begin());
    u.con_hist = u.con_history.size();
    u.say(std::string(kPrompt) + line, 3);
    if (!u.dap || u.state == DebugUi::kIdle || u.state == DebugUi::kStarting) {
        u.say("No program is being debugged", 4);
        return debug_console_fill(true);
    }
    // A gdb server's own command (OpenOCD's, a probe's: "monitor reset
    // halt") goes to it as it is: gdb takes "monitor" as typed; lldb-dap as
    // its command for it, after the prefix that makes a line a command.
    std::string expr = line;
    if (const std::string t = trimmed(line); !u.gdb && u.python.empty() && (t == "monitor" || t.starts_with("monitor "))) expr = "`process plugin packet monitor" + t.substr(7);
    const std::string frame = u.state == DebugUi::kStopped && u.frame < u.frames.size() ? ",\"frameId\":" + std::to_string(u.frames[u.frame].id) : std::string();
    ++u.asking;
    u.dap->request("evaluate", "{\"expression\":" + quoted(expr) + frame + ",\"context\":\"repl\"}", [this](const Dap::Message& m) {
        DebugUi& v = *debugui_;
        if (v.asking) --v.asking;
        if (!m.success) {
            v.say(why_not(m.message), 2);
            return;
        }
        const std::string_view result = m.body()["result"].str();
        const i64 ref = m.body()["variablesReference"].integer();
        u64 last = 0;
        for (size_t at = 0; at < result.size();) {
            const size_t nl = std::min(result.find('\n', at), result.size());
            std::string l(result.substr(at, nl - at));
            at = nl + 1;
            if (!l.empty() && l.back() == '\r') l.pop_back();
            last = v.say(std::move(l), 0).id;
        }
        if (ref > 0) {  // (on its last line)
            if (!last) last = v.say({}, 0).id;
            v.con.back().ref = ref;
            debug_console_value(last);
        }
    });
    debug_console_fill(true);
}

// A value's members, a level under it (as the Variables view opens it), or
// shut again.
void App::debug_console_value(u64 id) {
    DebugUi& u = *debugui_;
    const auto it = std::ranges::find_if(u.con, [&](const DebugUi::Said& s) { return s.id == id; });
    if (it == u.con.end() || it->ref <= 0) return;
    if (it->open) {
        it->open = false;
        auto end = it + 1;
        while (end != u.con.end() && end->depth > it->depth) ++end;
        u.con.erase(it + 1, end);
        return debug_console_fill(false);
    }
    if (!u.dap) return;
    it->open = true;
    const u32 depth = it->depth + 1;
    ++u.asking;
    u.dap->request("variables", "{\"variablesReference\":" + std::to_string(it->ref) + "}", [this, id, depth](const Dap::Message& m) {
        DebugUi& v = *debugui_;
        if (v.asking) --v.asking;
        const auto at = std::ranges::find_if(v.con, [&](const DebugUi::Said& s) { return s.id == id; });
        if (at == v.con.end() || !at->open) return;
        std::vector<DebugUi::Said> kids;
        if (!m.success) kids.push_back({why_not(m.message), 4, depth, 0, false, ++v.con_ids});  // (gone: the program has gone on since)
        for (const json::Value x : m.success ? m.body()["variables"] : json::Value()) {
            std::string value(x["value"].str());
            if (const std::string_view type = x["type"].str(); !type.empty() && value.size() < 40) value += "  (" + std::string(type) + ")";
            std::string text = std::string(x["name"].str()) + " = " + value;
            for (char& c : text)
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';  // (a line each)
            kids.push_back({std::move(text), 0, depth, x["variablesReference"].integer(), false, ++v.con_ids});
        }
        v.con.insert(at + 1, kids.begin(), kids.end());
        v.con_changed = true;
    });
    debug_console_fill(false);
}

// Tab: what is typed (up to the cursor) completed as the debugger would,
// its commands too: one answer goes in, several that share more than is
// typed that much, else a list of them, the one chosen going in.
void App::debug_console_complete() {
    DebugUi& u = *debugui_;
    if (!u.dap || u.state == DebugUi::kIdle || u.state == DebugUi::kStarting) return;
    const std::string typed = u.con_input.substr(0, u.con_at), rest = u.con_input.substr(u.con_at);
    const std::string frame = u.state == DebugUi::kStopped && u.frame < u.frames.size() ? ",\"frameId\":" + std::to_string(u.frames[u.frame].id) : std::string();
    u.dap->request("completions", "{\"text\":" + quoted(typed) + ",\"column\":" + std::to_string(typed.size() + 1) + frame + "}", [this, typed, rest](const Dap::Message& m) {
        DebugUi& v = *debugui_;
        if (!m.success || v.con_input != typed + rest || v.con_at != typed.size()) return;  // (typed on meanwhile)
        const std::vector<std::string> all = completions_of(typed, m.body()["targets"], true);
        auto take = [this, rest](const std::string& s) {
            DebugUi& w = *debugui_;
            w.con_input = s + rest;
            w.con_at = s.size();
            debug_console_line();
        };
        if (all.empty()) return;
        if (all.size() == 1) return take(all[0]);
        if (const std::string common = completions_share(all); common.size() > typed.size()) return take(common);
        find_choose_item("Complete", all, [all, take](int i) {
            if (i >= 0 && static_cast<size_t>(i) < all.size()) take(all[static_cast<size_t>(i)]);
        });
    });
}

// Keys in the console: on the line typed on, they edit it (Up and Down go
// through the lines typed before, Tab completes, Enter sends it); Enter on
// a value with members opens it or shuts it. The editor's shortcuts (Cmd+F
// finds in the lines), the function keys (the debugger's) and Esc stay the
// editor's; with Vim's keys, Esc leaves the line, and Vim's keys move over
// the lines until i or a (or Enter) comes back to it.
bool App::debug_console_key(const SDL_KeyboardEvent& k) {
    if (!debugui_ || !pane_->doc || !debug_console_doc(*pane_->doc)) return false;
    DebugUi& u = *debugui_;
    const Editor& ed = pane_->doc->ed;
    const bool primary = k.mod & kPrimaryMod, ctrl = k.mod & SDL_KMOD_CTRL, alt = k.mod & SDL_KMOD_ALT, shift = k.mod & SDL_KMOD_SHIFT;
    if ((k.key >= SDLK_F1 && k.key <= SDLK_F12) || (k.key >= SDLK_F13 && k.key <= SDLK_F24)) return false;
    if (k.key == SDLK_ESCAPE) {
        if (cfg_.modal) u.con_typing = false;
        return false;
    }
    const u64 line = ed.buf.line_of(ed.cursor());
    const bool on_line = line + 1 >= ed.buf.line_count() && !ed.has_selection();
    if (primary && !alt && !shift && k.key == SDLK_V) {  // pasted where the cursor is on the line (a line of it: its line ends as spaces)
        std::string text;
        if (char* c = SDL_GetClipboardText()) {
            text = c;
            SDL_free(c);
        }
        for (char& c : text)
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        u.con_typing = true;
        u.con_input.insert(u.con_at, text);
        u.con_at += text.size();
        debug_console_line();
        return true;
    }
    if (primary || ctrl) return false;
    const bool enter = k.key == SDLK_RETURN || k.key == SDLK_KP_ENTER;
    if (enter && !on_line && line < u.con.size() && u.con[line].ref > 0) {
        debug_console_value(u.con[line].id);
        return true;
    }
    if (cfg_.modal && !u.con_typing) {
        if (!enter) return false;
        u.con_typing = true;
        debug_console_line();
        return true;
    }
    auto back = [&](size_t at) {  // (the start of the character before)
        while (at > 0 && (static_cast<u8>(u.con_input[--at]) & 0xC0) == 0x80) {
        }
        return at;
    };
    auto on = [&](size_t at) {  // (the start of the character after)
        if (at < u.con_input.size()) ++at;
        while (at < u.con_input.size() && (static_cast<u8>(u.con_input[at]) & 0xC0) == 0x80) ++at;
        return at;
    };
    auto word = [&](size_t at) {  // (the start of the word before)
        while (at > 0 && u.con_input[at - 1] == ' ') --at;
        while (at > 0 && u.con_input[at - 1] != ' ') --at;
        return at;
    };
    switch (k.key) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER: debug_console_send(); return true;
        case SDLK_UP:
        case SDLK_DOWN:
            if (shift || !on_line) return false;  // (over the lines above)
            if (k.key == SDLK_UP && u.con_hist > 0) {
                if (u.con_hist >= u.con_history.size()) u.con_draft = u.con_input;
                u.con_input = u.con_history[--u.con_hist];
            } else if (k.key == SDLK_DOWN && u.con_hist < u.con_history.size()) {
                u.con_input = ++u.con_hist < u.con_history.size() ? u.con_history[u.con_hist] : u.con_draft;
            }
            u.con_at = u.con_input.size();
            break;
        case SDLK_LEFT:
        case SDLK_RIGHT:
        case SDLK_HOME:
        case SDLK_END:
            if (shift || !on_line) return false;
            u.con_at = k.key == SDLK_HOME ? 0 : k.key == SDLK_END ? u.con_input.size() : k.key == SDLK_LEFT ? (alt ? word(u.con_at) : back(u.con_at)) : on(u.con_at);
            break;
        case SDLK_BACKSPACE: {
            const size_t from = alt ? word(u.con_at) : back(u.con_at);
            u.con_input.erase(from, u.con_at - from);
            u.con_at = from;
            break;
        }
        case SDLK_DELETE: u.con_input.erase(u.con_at, on(u.con_at) - u.con_at); break;
        case SDLK_TAB: debug_console_complete(); return true;
        case SDLK_PAGEUP:
        case SDLK_PAGEDOWN: return false;
        default: return !alt || kPrimaryMod == SDL_KMOD_GUI;  // (keys that type: their text comes as text; Alt+key elsewhere is the editor's)
    }
    debug_console_line();
    return true;
}

// Text typed in the console goes on the line typed on, where its cursor is
// (wherever the editor's cursor was in the lines above). With Vim's keys,
// off the line, i, a, A or I in Normal mode come back to it; the rest is Vim's.
bool App::debug_console_text(const char* text) {
    if (!debugui_ || !pane_->doc || !debug_console_doc(*pane_->doc)) return false;
    DebugUi& u = *debugui_;
    const std::string_view t = text;
    if (cfg_.modal && !u.con_typing) {
        if (!(t == "i" || t == "a" || t == "A" || t == "I") || vim_.mode() != Vim::kNormal || vim_.pending() || !vim_.showcmd().empty()) return false;
        u.con_typing = true;
        if (t == "I") u.con_at = 0;
        if (t == "A") u.con_at = u.con_input.size();
        debug_console_line();
        return true;
    }
    std::string s(t);
    for (char& c : s)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    u.con_input.insert(u.con_at, s);
    u.con_at += s.size();
    debug_console_line();
    return true;
}

// ---------------------------------------------------------------- for the tests

void App::debug_set(const std::string& what, const std::string& value) {
    DebugUi& u = *debugui_;
    if (what == "program") u.program = full_path(value);
    else if (what == "args") u.args = value;
    else if (what == "cwd") u.cwd = full_path(value);
    else if (what == "adapter") u.conf.adapter = value;  // (and a run configuration's debug: adapter, request, console, arguments, after, a command each time)
    else if (what == "request") u.conf.request = value;
    else if (what == "console") u.conf.console = value;
    else if (what == "arguments") u.conf.arguments = value;
    else if (what == "after") u.conf.after.push_back(value);
    ++debug_gen_;
}

// Waits (20 s at most) until the session is stopped, or over.
void App::debug_wait(const std::string& state) {
    build_first_settle();  // (what F5 builds first, and the session after it)
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
    if (!u.exception.empty()) {  // (and its details, a line each)
        out += "; exception: " + u.exception;
        for (const std::string& l : u.exception_lines) out += " | " + l;
    }
    if (u.threads.size() > 1) {
        u32 at = 0;
        for (u32 i = 0; i < u.threads.size(); ++i)
            if (u.threads[i].id == u.thread) at = i;
        out += "; threads: " + std::to_string(u.threads.size()) + ", #" + std::to_string(at + 1) + " shown";
        const size_t waves = u.threads.size() - u.cpu_threads();
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
            if (b.off) out += " off";
            if (b.said == DebugUi::Break::kUnverified) out += " unverified" + (b.why.empty() ? std::string() : " (" + b.why + ")");
            if (b.moved_from) out += " moved from " + std::to_string(b.moved_from);
            if (!b.cond.empty()) out += " if " + b.cond;
            if (!b.hits.empty()) out += " hit " + b.hits;
            if (!b.log.empty()) out += " log " + b.log;
        }
    for (const std::string& f : u.functions) out += (out.empty() ? "" : "; ") + f + "()";
    for (const DebugUi::Data& d : u.data) out += (out.empty() ? "" : "; ") + std::string("writes to ") + d.name;
    for (const u64 a : u.ibreaks) out += (out.empty() ? "" : "; ") + std::string("at ") + hex(a);
    // (The exceptions it stops at, of those the debugger has: by their names.)
    for (const DebugUi::Filter& f : u.filters)
        if (const DebugUi::Choice c = u.choice(f); c.on) out += (out.empty() ? "" : "; ") + std::string("exception ") + f.id + (c.cond.empty() ? "" : " if " + c.cond);
    return out.empty() ? "no breakpoints" : out;
}

// The Debug tab's rows as last drawn, from the heading `from`, `n` of them
// (those with something in them): each its mark, text and what is after it.
std::string App::debug_rows_dump(const std::string& from, u32 n) {
    const DebugUi& u = *debugui_;
    std::string out;
    bool on = false;
    for (const DebugUi::Row& r : u.rows) {
        on |= r.ink.ink == ink::kHeading.ink && r.ink.style == ink::kHeading.style && r.text == from;
        if (!on || (r.text.empty() && r.act != DebugUi::kThread)) continue;
        if (n-- == 0) break;
        const std::string& text = r.act == DebugUi::kThread && r.text.empty() && r.index < u.threads.size() ? u.threads[r.index].name : r.text;
        out += (out.empty() ? "" : " | ") + (r.mark.empty() || r.mark == " " ? std::string() : r.mark + " ") + text + (r.after.empty() ? "" : " " + r.after);
    }
    return out.empty() ? "no " + from : out;
}

// What Enter does on the Nth row (from 1; or, named, the variable's) of a section of the Debug tab as
// it was last drawn: threads, stack, watch, add (the row that adds an
// expression), or breaks. The row is chosen first, as the keys would.
bool App::debug_pick(const std::string& section, u32 n, const std::string& name) {
    DebugUi& u = *debugui_;
    const DebugUi::Act act = section == "threads" ? DebugUi::kThread : section == "stack" ? DebugUi::kFrame : section == "watch" ? DebugUi::kWatch
                             : section == "add"   ? DebugUi::kWatchAdd : section == "breaks" ? DebugUi::kBreak : section == "vars" ? DebugUi::kVar : section == "sessions" ? DebugUi::kSession : section == "waves" ? DebugUi::kWaves
                             : section == "exceptions" ? DebugUi::kException : DebugUi::kNone;
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
