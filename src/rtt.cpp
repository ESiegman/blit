#include "rtt.hpp"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "elf.hpp"
#include "flash.hpp"
#include "json.hpp"
#include "missing.hpp"

extern char** environ;

namespace blit {

namespace {

std::string hex_of(u64 v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

// A gdb server's port as boarddebug.cpp sets it: the board's, else the
// preset's own.
int gdb_port(const Board& b, const std::string& preset) {
    if (!b.debug_port.empty()) return std::atoi(b.debug_port.c_str());
    return preset == "jlink" ? 2331 : 3333;
}

}  // namespace

RttElf rtt_elf(const Elf& elf) {
    RttElf r;
    // (SEGGER's RTT, and the Rust crates that speak it, defmt-rtt and
    // rtt-target, name the control block `_SEGGER_RTT`: SEGGER_RTT.c, and
    // defmt-rtt 1.3.1 src/lib.rs's #[no_mangle] static.)
    for (const ElfSymbol& s : elf.symbols())
        if (s.name == "_SEGGER_RTT") {
            r.block = true;
            r.addr = s.value;
            r.size = s.size;
            break;
        }
    // (defmt's format strings are the symbols of the section its linker
    // script makes, `.defmt`: defmt 1.x defmt.x.in; defmt-print reads it,
    // decoder/src/elf2table/mod.rs.)
    r.defmt = elf.section(".defmt") != nullptr;
    return r;
}

BoardStreams board_streams(const Board& b, const std::string& p, const RttElf& elf, u16 tcl_port, const std::string& files, bool standalone) {
    BoardStreams s;
    if (!b.rtt && !b.semihosting) return s;
    const u16 base = b.rtt_port > 0 ? static_cast<u16>(b.rtt_port) : kRttPort;
    const u16 semi = b.semihosting_port > 0 ? static_cast<u16>(b.semihosting_port) : kSemihostingPort;
    u64 addr = elf.addr, size = elf.size;
    if (!b.rtt_address.empty()) {
        addr = std::strtoull(b.rtt_address.c_str(), nullptr, 0);
        if (!size) size = 0x400;  // (OpenOCD reads only the header there, and the channels it points to)
    }
    const bool have_addr = addr != 0;
    // The channels a tool is told of up front (those that do not list them:
    // pyOCD, J-Link), else channel 0.
    std::vector<int> chans = b.rtt_channels;
    if (chans.empty()) chans.push_back(0);
    auto is_defmt = [&](int ch, std::string_view name) {
        if (b.rtt_defmt == "off") return false;
        if (b.rtt_defmt == "on") return name.empty() ? ch == 0 : name == "defmt";
        return elf.defmt && (name == "defmt" || (name.empty() && ch == 0));
    };
    auto channel_tab = [&](int ch, u16 port, std::string greeting) {
        StreamPlan t;
        t.kind = "rtt";
        t.channel = ch;
        t.port = port;
        t.greeting = std::move(greeting);
        t.defmt = is_defmt(ch, {});
        t.name = "RTT " + std::to_string(ch) + (t.defmt ? ": defmt" : "");
        t.input = !t.defmt;
        s.tabs.push_back(std::move(t));
    };
    auto output_tab = [&](u16 port, bool listen) {
        StreamPlan t;
        t.kind = "semihosting";
        t.name = "Board output";
        t.port = port;
        t.listen = listen;
        s.tabs.push_back(std::move(t));
    };
    const bool arm = !esp_chip(b.chip);

    if (p == "openocd" || p == "esp") {
        if (b.rtt) {
            if (!have_addr) {
                s.notes.push_back("RTT through OpenOCD needs the control block's address: the ELF has no _SEGGER_RTT symbol (board.rtt.address gives it)");
            } else if (!tcl_port) {
                s.notes.push_back("RTT through OpenOCD needs a free port for its Tcl server, and none could be had");
            } else {
                // (OpenOCD's Tcl server on a port of its own; the RTT driver
                // sets RTT up through it once the board runs, apprtt.cpp.)
                s.tcl_port = tcl_port;
                s.rtt_base = base;
                s.openocd_words = " -c " + sh_word("tcl_port " + std::to_string(tcl_port));
            }
        }
        if (b.semihosting) {
            if (!arm) {
                s.notes.push_back("Semihosting through Espressif's OpenOCD is not one blit reaches: its output goes to OpenOCD's own console");
            } else {
                // (OpenOCD 0.12 src/target/semihosting_common.c lines 2041-2055:
                // `arm semihosting enable`, `arm semihosting_redirect tcp PORT
                // stdio`, both for a target that runs: after `init`.)
                const std::string on = "arm semihosting enable", to = "arm semihosting_redirect tcp " + std::to_string(semi) + " stdio";
                if (standalone) s.openocd_words += " -c init -c " + sh_word(on) + " -c " + sh_word(to);
                else s.after.push_back("monitor " + on), s.after.push_back("monitor " + to);
                output_tab(semi, false);
            }
        }
    } else if (p == "pyocd") {
        if (b.rtt) {
            // (pyOCD 0.46.1 utility/rtt_manager.py, RTTConfig: the `rtt`
            // option, a list of {control-block: {address, size} or
            // {auto-detect}, channel: [{number, mode: server, port}]}; the gdb
            // server looks for the block every 10 ms while the board runs,
            // gdbserver/gdbserver.py lines 299 and 490-500.)
            std::string y = "rtt:\n  - control-block:\n";
            if (have_addr) y += "      address: " + hex_of(addr) + "\n" + (size ? "      size: " + std::to_string(size) + "\n" : std::string());
            else y += "      auto-detect: true\n";
            y += "    channel:\n";
            for (const int ch : chans) {
                const u16 port = static_cast<u16>(base + ch);
                y += "      - number: " + std::to_string(ch) + "\n        mode: server\n        port: " + std::to_string(port) + "\n";
                channel_tab(ch, port, {});
            }
            s.pyocd_config = std::move(y);
            s.pyocd_path = files + ".yaml";
            s.rtt_base = base;
            s.server_words += " --config " + sh_word(s.pyocd_path);
        }
        if (b.semihosting) {
            // (-S, and its console on the telnet port, -T: v0.46.1
            // subcommands/gdbserver_cmd.py lines 62 and 85, utility/stdio.py's
            // StdioServer for semihost_console_type "telnet", the default.)
            s.server_words += " -S";
            output_tab(static_cast<u16>(gdb_port(b, p) + 1), false);
        }
    } else if (p == "jlink") {
        if (b.rtt) {
            // (SEGGER's J-Link GDB Server: -RTTTelnetPort, 19021 by default;
            // a channel other than 0 chosen by "$$SEGGER_TELNET_ConfigStr=
            // RTTCh;N$$" sent within 100 ms of connecting (kb.segger.com,
            // "J-Link RTT TELNET Channel"); the block where `SetRTTAddr` says,
            // else looked for in RAM.)
            s.server_words += " -RTTTelnetPort " + std::to_string(base);
            if (have_addr && !standalone) s.after.push_back("monitor exec SetRTTAddr " + hex_of(addr));
            for (const int ch : chans) channel_tab(ch, base, jlink_channel(ch));
        }
        if (b.semihosting) {
            if (standalone) {
                s.notes.push_back("J-Link's semihosting is turned on through gdb: it comes with a debug session");
            } else {
                // (`monitor semihosting enable`, and its output to the telnet
                // client: `monitor semihosting IOClient 1`, on -telnetport.)
                s.after.emplace_back("monitor semihosting enable");
                s.after.emplace_back("monitor semihosting IOClient 1");
                output_tab(static_cast<u16>(gdb_port(b, p) + 2), false);
            }
        }
    } else if (p == "qemu") {
        if (b.rtt) s.notes.push_back("QEMU has no RTT (no probe reads the board's memory); semihosting is its way out");
        if (b.semihosting && !standalone) {
            // (QEMU 11.1.3 semihosting/arm-compat-semi.c: a SYS_OPEN of ":tt"
            // is QEMU's own stdout, lines 417-432, and SYS_WRITEC and
            // SYS_WRITE0 go to the -semihosting-config chardev, lines 460-476
            // (else to its stderr: semihosting/console.c line 117). With
            // target=gdb, gdb 18.1's DAP sends none of what the program writes
            // as an event. So QEMU does the calls, beside gdb, its stdout and
            // a file chardev both appending to one file: boarddebug.cpp.)
            s.server_out = files + "-out.txt";
            s.qemu_words = " -semihosting-config enable=on,target=native,chardev=blitsh -chardev " + sh_word("file,id=blitsh,append=on,path=" + s.server_out);
            StreamPlan t;
            t.kind = "semihosting";
            t.name = "Board output";
            t.file = s.server_out;
            t.input = false;
            t.defmt = b.semihosting_defmt == "on";  // (defmt-semihosting's frames)
            s.tabs.push_back(std::move(t));
        }
    } else if (p == "probe-rs") {
        // (probe-rs 0.32.0's gdb server, cmd/gdb_server, has neither: RTT and
        // semihosting are its DAP server's, cmd/dap_server/server/core_data.rs.)
        if (!standalone) {
            if (b.rtt) s.notes.push_back("probe-rs's gdb server has no RTT, and one probe-rs at a time has the probe: RTT opens once the session ends (\"Open the board's RTT\"), or debug through the openocd preset");
            if (b.semihosting) s.notes.push_back("probe-rs's gdb server does not take semihosting: the openocd or pyocd preset does");
        }
    } else {
        if (b.rtt) s.notes.push_back("The " + p + " preset has no RTT blit reaches");
        if (b.semihosting) s.notes.push_back("The " + p + " preset's semihosting goes to gdb's console, not to a tab");
    }
    return s;
}

std::string jlink_channel(int n) { return n ? "$$SEGGER_TELNET_ConfigStr=RTTCh;" + std::to_string(n) + "$$" : std::string(); }

// ---------------------------------------------------------------- OpenOCD's Tcl port

std::string tcl_command(std::string_view line) { return std::string(line) + '\x1a'; }

std::vector<std::string> tcl_answers(std::string& buf) {
    std::vector<std::string> out;
    size_t at = 0;
    for (size_t z; (z = buf.find('\x1a', at)) != std::string::npos; at = z + 1) out.emplace_back(buf.substr(at, z - at));
    buf.erase(0, at);
    return out;
}

bool tcl_channels(std::string_view a, std::vector<RttChannel>* up, std::vector<RttChannel>* down) {
    up->clear();
    down->clear();
    if (a.find("Channels: up=") == std::string_view::npos) return false;
    std::vector<RttChannel>* into = nullptr;
    for (size_t at = 0; at < a.size();) {
        size_t nl = a.find('\n', at);
        if (nl == std::string_view::npos) nl = a.size();
        std::string_view line = a.substr(at, nl - at);
        at = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line == "Up-channels:") into = up;
        else if (line == "Down-channels:") into = down;
        else if (into && !line.empty() && line[0] >= '0' && line[0] <= '9') {
            // "I: NAME SIZE FLAGS" (a name may have spaces: the last two words are numbers)
            const size_t colon = line.find(": ");
            if (colon == std::string_view::npos) continue;
            RttChannel c;
            c.number = std::atoi(std::string(line.substr(0, colon)).c_str());
            std::string_view rest = line.substr(colon + 2);
            for (int k = 0; k < 2; ++k)
                if (const size_t sp = rest.rfind(' '); sp != std::string_view::npos) rest = rest.substr(0, sp);
            c.name = std::string(rest);
            into->push_back(std::move(c));
        }
    }
    return true;
}

// ---------------------------------------------------------------- defmt-print's records

const char* defmt_level_name(int level) {
    static const char* const kNames[] = {"", "TRACE", "DEBUG", "INFO", "WARN", "ERROR"};
    return level >= 0 && level <= 5 ? kNames[level] : "";
}

bool defmt_parse(std::string_view line, DefmtRecord* r, int* schema) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    if (line.empty() || line.front() != '{') return false;
    json::Doc d;
    if (!d.parse(std::string(line))) return false;
    const json::Value v = d.root();
    if (const json::Value s = v["schema_version"]; s.exists()) {
        *schema = static_cast<int>(s.integer());
        return false;
    }
    if (!v["data"].exists() && !v["level"].exists()) return false;
    *r = {};
    r->data = std::string(v["data"].str());
    r->host_ns = static_cast<u64>(v["host_timestamp"].integer());
    r->target_ts = std::string(v["target_timestamp"].str());
    std::string level(v["level"].str());
    for (char& c : level) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    for (int k = 1; k <= 5; ++k)
        if (level == defmt_level_name(k)) r->level = k;
    r->file = std::string(v["location"]["file"].str());
    r->line = static_cast<u32>(v["location"]["line"].integer());
    return true;
}

std::string defmt_line(const DefmtRecord& r) {
    // (defmt-print's own colours: defmt-print-v1.2.0 decoder/src/log/format/
    // mod.rs lines 754-758, error red, warn yellow, info green, debug bright
    // white, trace bright black.)
    static const char* const kInk[] = {"", "\x1b[90m", "\x1b[97m", "\x1b[32m", "\x1b[33m", "\x1b[31m"};
    std::string out;
    if (!r.target_ts.empty()) out += r.target_ts + " ";
    if (r.level) {
        std::string name = defmt_level_name(r.level);
        name.resize(5, ' ');
        out += std::string(kInk[r.level]) + name + "\x1b[0m ";
    }
    out += r.data;
    if (!r.file.empty()) out += "  \x1b[2m" + r.file + (r.line ? ":" + std::to_string(r.line) : std::string()) + "\x1b[0m";
    out += "\r\n";
    return out;
}

// ---------------------------------------------------------------- programs

Spawned spawn_piped(const std::vector<std::string>& argv, const std::string& dir, bool in, bool out, bool err, std::string* why) {
    Spawned s;
    int pin[2] = {-1, -1}, pout[2] = {-1, -1}, perr[2] = {-1, -1};
    auto make = [](int* p) {
        if (::pipe(p) != 0) return false;
        ::fcntl(p[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(p[1], F_SETFD, FD_CLOEXEC);
        return true;
    };
    auto shut = [](int* p) {
        for (int k = 0; k < 2; ++k)
            if (p[k] >= 0) ::close(p[k]), p[k] = -1;
    };
    if ((in && !make(pin)) || (out && !make(pout)) || (err && !make(perr))) {
        shut(pin), shut(pout), shut(perr);
        *why = std::strerror(errno);
        return s;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (in) posix_spawn_file_actions_adddup2(&fa, pin[0], 0);
    else posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (out) posix_spawn_file_actions_adddup2(&fa, pout[1], 1);
    else posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    if (err) posix_spawn_file_actions_adddup2(&fa, perr[1], 2);
    else posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    if (!dir.empty()) {
#if defined(__APPLE__) && defined(__MAC_26_0) && __MAC_OS_X_VERSION_MIN_REQUIRED >= __MAC_26_0
        posix_spawn_file_actions_addchdir(&fa, dir.c_str());
#else
        posix_spawn_file_actions_addchdir_np(&fa, dir.c_str());
#endif
    }
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    // (A group of its own: ending it ends what a shell line started.)
    short flags = POSIX_SPAWN_SETPGROUP;
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attr, flags);
    posix_spawnattr_setpgroup(&attr, 0);
    std::vector<std::string> words = argv;
    std::vector<char*> args;
    for (std::string& w : words) args.push_back(w.data());
    args.push_back(nullptr);
    pid_t pid = -1;
    const int rc = ::posix_spawnp(&pid, args[0], &fa, &attr, args.data(), environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&fa);
    if (pin[0] >= 0) ::close(pin[0]);
    if (pout[1] >= 0) ::close(pout[1]);
    if (perr[1] >= 0) ::close(perr[1]);
    if (rc != 0) {
        if (pin[1] >= 0) ::close(pin[1]);
        if (pout[0] >= 0) ::close(pout[0]);
        if (perr[0] >= 0) ::close(perr[0]);
        *why = rc == ENOENT && argv[0].find('/') == std::string::npos ? not_installed(argv[0], {}) : argv[0] + ": " + std::strerror(rc);
        return s;
    }
    s.pid = pid, s.in = pin[1], s.out = pout[0], s.err = perr[0];
    for (const int fd : {s.in, s.out, s.err})
        if (fd >= 0) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    return s;
}

void spawn_end(Spawned& s, int grace_ms) {
    for (int* fd : {&s.in, &s.out, &s.err})
        if (*fd >= 0) ::close(*fd), *fd = -1;
    if (s.pid <= 0) return;
    int st = 0;
    if (::waitpid(s.pid, &st, WNOHANG) == 0) {
        ::kill(-s.pid, SIGTERM);
        ::kill(s.pid, SIGTERM);
        for (int waited = 0; waited < grace_ms; waited += 10) {
            if (::waitpid(s.pid, &st, WNOHANG) != 0) {
                s.pid = -1;
                return;
            }
            ::usleep(10'000);
        }
        ::kill(-s.pid, SIGKILL);
        ::kill(s.pid, SIGKILL);
        ::waitpid(s.pid, &st, 0);
    }
    s.pid = -1;
}

}  // namespace blit
