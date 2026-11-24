#include "boarddebug.hpp"

#include <cstdlib>

#include "flash.hpp"
#include "json.hpp"
#include "missing.hpp"

namespace blit {

namespace {

// A chip's name as the tables have it: "ESP32-S3" is "esp32s3".
std::string key_of(std::string_view chip) {
    std::string out;
    for (const char c : chip)
        if (c != '-' && c != '_' && c != ' ') out += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    return out;
}

// The ESP32s whose cores are Xtensa (the rest are RISC-V).
bool xtensa_esp(std::string_view key) { return key == "esp32" || key.starts_with("esp32s"); }

// What the command line starts with: a program found among some (the first
// on the PATH of the machine it runs on; NAME:ARCH only if that gdb can
// set that architecture), else said missing, by a marker the session's
// failure reads (board_missing).
constexpr std::string_view kHelpers =
    "pick() { for p; do n=${p%%:*}; a=${p#\"$n\"}; a=${a#:}; command -v \"$n\" >/dev/null 2>&1 || continue; "
    "[ -z \"$a\" ] || \"$n\" -nx -batch -ex \"set architecture $a\" >/dev/null 2>&1 || continue; printf '%s' \"$n\"; return 0; done; return 1; }; "
    "missing() { echo \"blit-no-debugger: $1\" >&2; exit 127; }; ";

// A gdb command (Python) that has gdb's DAP say a new thread without
// taking the program to be running again (where gdb is started, below).
constexpr std::string_view kThreadsSaid =
    "python import gdb; from gdb.dap import events as e; gdb.events.new_thread.disconnect(e._new_thread); "
    "gdb.events.new_thread.connect(lambda ev: e.thread_event(ev, 'started'))";

// The gdbs for a machine, in the order looked for. ELF's e_machine: 40 Arm,
// 94 Xtensa, 243 RISC-V. Espressif's Xtensa gdbs are one for each chip
// (ESP-IDF 6.1's xtensa-esp-elf-gdb 17.1 has xtensa-esp32-elf-gdb,
// -esp32s2- and -esp32s3-, each choosing the build for the Python it
// finds); its RISC-V one is for all of its RISC-V chips. Last, the plain
// gdb, NAME:ARCH: taken only if it is built for that architecture (Arch
// Linux's and Fedora's are built for them all; a Debian's or Homebrew's
// for the machine's own alone).
std::string gdbs_for(int machine, std::string_view key) {
    if (machine == 94) return xtensa_esp(key) ? "xtensa-" + std::string(key) + "-elf-gdb" : std::string("xtensa-esp32s3-elf-gdb xtensa-esp32-elf-gdb xtensa-esp32s2-elf-gdb");
    if (machine == 243) return "riscv32-esp-elf-gdb riscv64-unknown-elf-gdb riscv-none-elf-gdb riscv64-elf-gdb gdb-multiarch gdb:riscv:rv32";
    return "arm-none-eabi-gdb gdb-multiarch gdb:arm";
}

std::string with_bars(std::string list) {
    for (char& c : list)
        if (c == ' ') c = '|';
    return list;
}

std::string json_of(std::string_view s) {
    std::string out;
    json::quote(out, s);
    return out;
}

}  // namespace

std::string board_preset(const Board& b) {
    if (!b.debug.empty()) return b.debug;
    // (An ST-LINK board whose RTT or semihosting is on: OpenOCD, which serves
    // them while gdb debugs; probe-rs's gdb server has neither: rtt.hpp.)
    if (b.probe_kind == "stlink" && (b.rtt || b.semihosting)) return "openocd";
    if (b.probe_kind == "stlink" || b.probe_kind == "cmsis-dap") return "probe-rs";
    if (b.probe_kind == "jlink") return "jlink";
    if (b.probe_kind == "esp-usb-jtag") return "esp";
    if (b.probe_kind == "blackmagic") return "blackmagic";
    return {};
}

std::string board_missing(std::string_view names, std::string_view host) {
    // (NAME:ARCH, a program taken only if built for that architecture: "a
    // gdb built for Arm".)
    auto said = [](std::string_view name) {
        const size_t colon = name.find(':');
        if (colon == std::string_view::npos) return std::string(name);
        const std::string_view arch = name.substr(colon + 1);
        return "a " + std::string(name.substr(0, colon)) + " built for " + (arch == "arm" ? std::string("Arm") : arch.starts_with("riscv") ? std::string("RISC-V") : std::string(arch));
    };
    std::string list;
    for (size_t at = 0; at <= names.size();) {
        size_t bar = names.find('|', at);
        if (bar == std::string_view::npos) bar = names.size();
        list += (list.empty() ? "" : "|") + said(names.substr(at, bar - at));
        at = bar + 1;
    }
    const size_t bar = list.find('|');
    if (bar == std::string::npos) return not_installed(list, host);
    std::string others = list.substr(bar + 1);
    for (size_t at; (at = others.find('|')) != std::string::npos;) others.replace(at, 1, ", ");
    return not_installed(list.substr(0, bar), host) + " (nor " + others + ")";
}

BoardDebug board_debug(const Board& b, const std::string& elf, bool attach, bool load, const BoardStreams* streams) {
    BoardDebug d;
    if (streams) d.streams = *streams;
    const BoardStreams& st = d.streams;
    d.preset = board_preset(b);
    const std::string& p = d.preset;
    if (p.empty()) return d.why = "The board names no debug preset (board.debug), and its probe says none", d;
    const std::string key = key_of(b.chip);
    const bool sim = p == "qemu" || p == "renode";
    d.simulator = sim && !attach;
    d.flash = !attach && !sim;
    const std::string port = !b.debug_port.empty()                                     ? b.debug_port
                             : p == "probe-rs"                                         ? "1337"   // (probe-rs 0.32.0 cmd/gdb_server/mod.rs line 61)
                             : p == "jlink"                                            ? "2331"   // (SEGGER's UM08001, the GDB Server's -port)
                             : p == "qemu"                                             ? "1234"   // (QEMU's -s)
                             : p == "pyocd" || p == "renode" || p == "openocd" || p == "esp" ? "3333"
                                                                                       : std::string();
    const std::string here = "127.0.0.1:" + port;

    // The gdb: the board's, else the one for the ELF's machine (read from
    // its header there, by od: an ELF not built yet says nothing, and the
    // chip decides), at 14 or later (which speaks DAP: gdb -i dap).
    std::string sh(kHelpers);
    const int guess = xtensa_esp(key) ? 94 : esp_chip(b.chip) ? 243 : 40;
    if (!b.debug_gdb.empty()) {
        sh += "G=" + sh_word(b.debug_gdb) + "; ";
    } else {
        sh += "case $(od -An -tu2 -j18 -N2 " + sh_word(elf) + " 2>/dev/null | tr -d ' \\n') in 94) G='" + gdbs_for(94, key) + "' ;; 243) G='" + gdbs_for(243, key) +
              "' ;; 40) G='" + gdbs_for(40, key) + "' ;; *) G='" + gdbs_for(guess, key) + "' ;; esac; ";
    }
    sh += "g=$(pick $G) || missing \"$(echo $G | tr ' ' '|')\"; "
          "v=$(\"$g\" --version 2>/dev/null | sed -n '1s/.* \\([0-9][0-9]*\\)\\..*/\\1/p'); "
          "if [ \"${v:-0}\" -lt 14 ] 2>/dev/null; then echo \"$g $v is too old to be a debugger blit can use: 14 or newer speaks its protocol\" >&2; exit 127; fi; "
          "echo \"blit-debugger: gdb ($g)\" >&2; ";

    // What the session attaches to, and the gdb server, if any, started
    // beside gdb (its output where gdb's errors go).
    std::string target, server, extra;
    std::string reset = "monitor reset halt";
    std::string servers;  // (the server's programs: the first there is run)
    auto server_named = [&](std::string_view programs) {  // ($BLIT_SERVER: the first there)
        servers = with_bars(std::string(programs));
        sh +="BLIT_SERVER=$(pick " + std::string(programs) + ") || missing '" + with_bars(std::string(programs)) + "'; export BLIT_SERVER; ";
    };
    auto words = [](std::initializer_list<std::string> list) {
        std::string out;
        for (const std::string& w : list)
            if (!w.empty()) out += " " + sh_word(w);
        return out;
    };
    if (p == "probe-rs") {
        // probe-rs's gdb server (`probe-rs gdb`, 0.32.0): --chip, --probe
        // VID:PID:SERIAL, --gdb-connection-string; `monitor reset halt` is
        // its own (cmd/gdb_server/target/monitor.rs lines 24-38). Its DAP
        // server has no function breakpoints (debugger.rs, its initialize
        // answer) and its console's `break` takes only `*address` or
        // file:line (repl_commands/breakpoint.rs line 31), so run to main,
        // "Break at a function" and gdb's commands are had through gdb.
        if (b.chip.empty()) return d.why = "probe-rs needs the board's chip", d;
        const std::string probe = b.probe_usb.size() == 9 ? (b.probe.empty() ? b.probe_usb : b.probe_usb + ":" + b.probe) : std::string();
        server_named("probe-rs");
        server = "\"$BLIT_SERVER\" gdb" + words({"--chip", b.chip}) + (probe.empty() ? std::string() : words({"--probe", probe})) + words({"--non-interactive", "--gdb-connection-string", here});
        target = here;
    } else if (p == "openocd" || p == "esp") {
        // OpenOCD started by gdb itself through a pipe (gdb's manual,
        // "Connecting to a Remote Target": target remote | COMMAND), with
        // `gdb_port pipe` (OpenOCD's manual, "GDB and OpenOCD"); its own
        // Tcl and telnet ports off, so that another OpenOCD is not in its
        // way. It ends when gdb lets go of it. Espressif's fork for an
        // ESP32, with its scripts (interface/esp_usb_jtag.cfg,
        // target/esp32s3.cfg: board/esp32s3-builtin.cfg is those two).
        const std::string iface = openocd_interface(b), tgt = openocd_target(b);
        if (iface.empty()) return d.why = "OpenOCD needs the probe's interface: flash.interface, or a probe of a kind it knows", d;
        if (tgt.empty()) return d.why = b.chip.empty() ? "OpenOCD needs the board's chip, or flash.target" : "OpenOCD has no target script blit knows for " + b.chip + ": flash.target", d;
        const bool esp = p == "esp" || esp_chip(b.chip);
        server_named(esp ? "openocd-esp32 openocd" : "openocd");
        // (Attaching to an ESP32 as it runs: its flash's size said to be
        // 0, as Espressif's gdb-attach handler otherwise resets the chip
        // when the program has turned memory protection on, as ESP-IDF's
        // do: v0.12.0-esp32-20260831 target/esp_common.cfg lines 313-330.
        // Breakpoints are then the core's own, two on an ESP32-S3.)
        // (Its Tcl port open when RTT is driven through it: rtt.hpp.)
        target = "| exec \"$BLIT_SERVER\"" + words({"-c", "gdb_port pipe"}) + (st.openocd_words.empty() ? words({"-c", "tcl_port disabled"}) : st.openocd_words) +
                 words({"-c", "telnet_port disabled"}) +
                 (esp && attach ? words({"-c", "set ESP_FLASH_SIZE 0"}) : std::string()) + words({"-f", iface}) +
                 (openocd_adapter(b).empty() ? std::string() : words({"-c", openocd_adapter(b)})) + words({"-f", tgt}) +
                 // (Let go of, the board runs on: Espressif's left the ESP32-S3
                 // halted where it stopped when gdb detached, on the lab box.)
                 words({"-c", "[target current] configure -event gdb-detach { resume }"});
        // (ESP-IDF's own gdbinit for its chips: two hardware watchpoints,
        // the register cache flushed after a reset; esp-idf v6.1
        // docs/en/api-guides/jtag-debugging/using-debugger.rst.)
        if (esp && (key.empty() || xtensa_esp(key))) d.after.emplace_back("set remote hardware-watchpoint-limit 2");
    } else if (p == "jlink") {
        // SEGGER's J-Link GDB Server, the command-line one (UM08001, "J-Link
        // GDB Server", its command line options): -device, -if, -speed,
        // -port and the two beside it, -select USB=SERIAL, -singlerun (it
        // ends when gdb lets go). `monitor reset` resets and halts.
        const std::string device = !b.flash_device.empty() ? b.flash_device : b.chip;
        if (device.empty()) return d.why = "The J-Link GDB Server needs the board's chip (its device), or flash.device", d;
        const int n = std::atoi(port.c_str());
        server_named("JLinkGDBServerCLExe JLinkGDBServer");
        server = "\"$BLIT_SERVER\"" + words({"-device", device, "-if", "SWD", "-speed", "4000", "-port", port, "-swoport", std::to_string(n + 1), "-telnetport", std::to_string(n + 2)}) +
                 (b.probe.empty() ? std::string() : words({"-select", "USB=" + b.probe})) + words({"-singlerun", "-nogui"}) + st.server_words;
        target = here;
        reset = "monitor reset";
    } else if (p == "pyocd") {
        // pyOCD's gdb server (v0.46.1 subcommands/gdbserver_cmd.py lines
        // 59-71, base.py lines 82-88): -t the target type (pyocd list
        // --targets), -u the probe, -p and -T its ports; without --persist
        // it ends when gdb lets go.
        if (b.chip.empty()) return d.why = "pyOCD needs the board's chip (its target type, as pyocd list --targets names it)", d;
        std::string type;
        for (const char c : b.chip) type += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        server_named("pyocd");
        server = "\"$BLIT_SERVER\" gdbserver" + words({"-t", type}) + (b.probe.empty() ? std::string() : words({"-u", b.probe})) +
                 words({"-p", port, "-T", std::to_string(std::atoi(port.c_str()) + 1)}) + st.server_words;
        target = here;
    } else if (p == "blackmagic") {
        // The probe is the gdb server: its GDB port, as extended-remote,
        // which DAP's attach cannot say (gdb 18.1's dap/launch.py, lines
        // 153-160: its target is `target remote`'s), so it goes on gdb's
        // command line, the probe's targets scanned (`monitor swd_scan`,
        // Black Magic v2.0.0 src/command.c line 98), and the attach is
        // to its first (`attach 1`, attach's pid). Loading puts the pc at
        // the image's entry; there is no reset that leaves it halted.
        if (b.probe_gdb.empty()) return d.why = "Black Magic's GDB port is not known: board.probe.gdb (\"Choose the probe\xE2\x80\xA6\" writes it)", d;
        extra = words({"-iex", "target extended-remote " + b.probe_gdb, "-iex", "monitor swd_scan"});
        reset.clear();
    } else if (p == "qemu") {
        // QEMU started by gdb through a pipe (-gdb stdio), halted before its
        // first instruction (-S), the ELF loaded (-kernel); the machine is
        // debug.machine, else the chip (qemu-system-arm -M help: mps2-an385,
        // netduinoplus2, olimex-stm32-h405, ...). Attaching: to a QEMU
        // already running with its gdb port (-s, or -gdb tcp::PORT).
        if (attach) {
            target = here;
        } else {
            const std::string machine = !b.debug_machine.empty() ? b.debug_machine : b.chip;
            if (machine.empty()) return d.why = "QEMU needs the board's machine: debug.machine (as qemu-system-arm -M help names it), or the chip", d;
            sh += "case $(od -An -tu2 -j18 -N2 " + sh_word(elf) + " 2>/dev/null | tr -d ' \\n') in 243) Q='qemu-system-riscv32' ;; 94) Q='qemu-system-xtensa' ;; *) Q='qemu-system-arm' ;; esac; ";
            sh += "BLIT_SERVER=$(pick $Q) || missing \"$Q\"; export BLIT_SERVER; ";
            servers = "qemu-system-*";
            if (!st.server_out.empty()) {
                // (Its output a file of its own, the semihosting tab's (rtt.hpp):
                // a server beside gdb on its gdb port, which gdb connects to,
                // trying again while it starts: gdb's `tcp auto-retry`.)
                server = "\"$BLIT_SERVER\"" + words({"-M", machine, "-nographic", "-monitor", "none", "-serial", "none", "-kernel", elf, "-S", "-gdb", "tcp:" + here}) + st.qemu_words;
                target = here;
            } else {
                target = "| exec \"$BLIT_SERVER\"" + words({"-M", machine, "-nographic", "-monitor", "none", "-serial", "none", "-kernel", elf, "-S", "-gdb", "stdio"}) + st.qemu_words;
            }
        }
        reset.clear();
    } else if (p == "renode") {
        // Renode (v1.17.1 README lines 155-170: --disable-gui, -e COMMAND
        // after the script) with its GDB server (its documentation,
        // "Debugging with GDB": machine StartGdbServer PORT; `monitor start`
        // starts the emulation). The script loads the ELF; else the
        // platform debug.machine names (a .repl) and the ELF loaded into it.
        if (attach) {
            target = here;
        } else {
            std::string run;
            if (!b.debug_script.empty()) run = words({b.debug_script});
            else if (!b.debug_machine.empty())
                run = words({"-e", "mach create", "-e", "machine LoadPlatformDescription @" + b.debug_machine, "-e", "sysbus LoadELF @" + elf});
            else return d.why = "Renode needs the board's debug.script (a .resc that loads the ELF), or debug.machine (a platform's .repl)", d;
            server_named("renode");
            server = "\"$BLIT_SERVER\" --disable-gui" + run + words({"-e", "machine StartGdbServer " + port});
            target = here;
            d.after.emplace_back("monitor start");
        }
        reset.clear();
    } else {
        return d.why = "No debug preset called " + p, d;
    }

    // Once attached: for F5, the board reset and halted (and its image
    // loaded by gdb, when no flash tool flashed it), then run to main.
    if (!attach && !sim) {
        if (!reset.empty()) d.after.push_back(reset), d.after.emplace_back("maintenance flush register-cache");
        if (load) d.after.emplace_back("load");
    }
    d.after.insert(d.after.end(), st.after.begin(), st.after.end());  // (RTT's and semihosting's, once the board is reset and loaded)
    if (!attach) d.run_to = !b.debug_main.empty() ? b.debug_main : esp_chip(b.chip) || p == "esp" ? "app_main" : "main";

    // The command line: the server started (its input none, its output with
    // gdb's errors), then gdb, whose input and output are the session's;
    // whichever way the session ends (gdb's own end, or a signal to this
    // shell), the server and what gdb started through a pipe end with it.
    // (An asynchronous command's input is /dev/null unless it says another:
    // the session's is kept as 3.)
    // After a flash, the probe waited for (10 s at most): a chip whose own USB
    // is its probe (an ESP32's USB-Serial/JTAG) comes back only once the
    // program flashed has started, and a server started before then finds
    // none (Espressif's OpenOCD, after esptool on the lab's ESP32-S3).
    // Found by its ids and serial number: /sys/bus/usb on Linux, IOKit's
    // USB plane (ioreg) on a Mac.
    if (!attach && !sim && b.probe_usb.size() == 9) {
        const std::string vid = b.probe_usb.substr(0, 4), pid = b.probe_usb.substr(5);
        const std::string mac = !b.probe.empty() ? "ioreg -p IOUSB -l -w0 | grep -q " + sh_word("\"USB Serial Number\" = \"" + b.probe + "\"")
                                                 : "ioreg -p IOUSB -l -w0 | grep -q " + sh_word("\"idProduct\" = " + std::to_string(std::strtoul(pid.c_str(), nullptr, 16)));
        sh += "here() { if [ -d /sys/bus/usb/devices ]; then for d in /sys/bus/usb/devices/*; do "
              "[ \"$(cat \"$d/idVendor\" 2>/dev/null):$(cat \"$d/idProduct\" 2>/dev/null)\" = " + sh_word(vid + ":" + pid) + " ] || continue; " +
              (b.probe.empty() ? std::string("return 0; ") : "[ \"$(cat \"$d/serial\" 2>/dev/null)\" = " + sh_word(b.probe) + " ] && return 0; ") +
              "done; return 1; else " + mac + "; fi; }; n=0; until here; do n=$((n + 1)); [ $n -gt 100 ] && break; sleep 0.1; done; ";
    }
    sh += "exec 3<&0; ";
    if (!server.empty()) sh += server + " </dev/null " + (st.server_out.empty() ? std::string("1>&2") : "1>>" + sh_word(st.server_out)) + " & sp=$!; ";
    sh += "trap 'pkill -TERM -P \"$gp\" 2>/dev/null; kill $gp $sp 2>/dev/null' EXIT; trap 'exit 143' TERM HUP INT; ";
    // (gdb's DAP takes a thread it is told of as the program running again
    // (GNU gdb 18.1 and esp-gdb 17.1, gdb/dap/events.py lines 93-96), which
    // holds for a process but not for a gdb server that lists an RTOS's
    // tasks as threads while the board is stopped (OpenOCD's FreeRTOS, as
    // Espressif's has it on by default; pyOCD's, J-Link's plug-ins): every
    // request after says "notStopped". So here a thread it is told of is
    // said and nothing more; its going on is told of as ever (_cont).)
    sh += "\"$g\" -q -i dap -iex " + sh_word(kThreadsSaid) + extra + " <&3 3<&- & gp=$!; wait $gp";
    d.adapter = std::move(sh);
    auto named = [&](std::string line) {  // ($BLIT_SERVER as the programs it is the first of)
        if (const size_t at = line.find("\"$BLIT_SERVER\""); at != std::string::npos) line.replace(at, 14, "{" + servers + "}");
        return line;
    };
    d.shown = "gdb: " + (!b.debug_gdb.empty() ? b.debug_gdb : gdbs_for(guess, key)) + (server.empty() ? std::string() : " | server: " + named(server)) +
              (p == "blackmagic" ? std::string() : " | target: " + named(target)) + (extra.empty() ? std::string() : " | gdb:" + extra);
    d.arguments = "{\"program\":" + json_of(elf) + (p == "blackmagic" ? std::string(",\"pid\":1") : ",\"target\":" + json_of(target)) + "}";
    return d;
}

}  // namespace blit
