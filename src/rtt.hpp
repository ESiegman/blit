#pragma once
// A board's RTT channels, semihosting output and defmt logs (the embedded
// design's E6): what each debug preset's server offers, and how blit
// reaches it. The tabs themselves are linktab.hpp's; apprtt.cpp opens them.
//
// The transports, as each tool has them (sources cited where they are used,
// in rtt.cpp):
//
//   - OpenOCD (0.12): `rtt setup ADDR SIZE ID` with the address of
//     `_SEGGER_RTT` from the ELF, `rtt start`, then `rtt server start PORT
//     CHANNEL` for each up-channel; a TCP server per channel, whose input
//     goes to the down-channel of the same number. `rtt start` looks once
//     and gives up when the program has not set the block up yet, and gdb
//     cannot pass `monitor` commands while the board runs, so blit drives
//     these through OpenOCD's Tcl port (a loopback link) until the block is
//     found. Semihosting: `arm semihosting enable` and `arm
//     semihosting_redirect tcp PORT stdio`, through gdb once attached.
//   - pyOCD (0.46): its gdb server's `rtt` session option (a YAML config:
//     the control block and a TCP server per channel), which it looks for
//     on its own while the board runs. Semihosting: `-S`, its console on
//     the telnet port (`-T`).
//   - J-Link's GDB server: channel 0 on its RTT telnet port
//     (`-RTTTelnetPort`, 19021), another chosen by a config string sent in
//     the first 100 ms; the block's address by `monitor exec SetRTTAddr`.
//     Semihosting: `monitor semihosting enable`, its output on the telnet
//     port (`-telnetport`) with `monitor semihosting IOClient 1`.
//   - probe-rs (0.32): its gdb server has no RTT and no semihosting; its
//     DAP server sends both as events (`probe-rs-rtt-channel-config`,
//     answered with `rttWindowOpened`, then `probe-rs-rtt-data`), so RTT on
//     a running board goes through `probe-rs dap-server` (one probe-rs at
//     a time has the probe: not during its gdb session).
//   - QEMU: no RTT; semihosting done by QEMU (target=native), its output
//     on its own stdout: a SYS_WRITE to ":tt" goes nowhere else (a chardev
//     takes only SYS_WRITEC and SYS_WRITE0), so QEMU runs as a server
//     beside gdb (not through gdb's pipe, which is its stdout otherwise),
//     its stdout a file the tab reads as it grows.
//
// defmt: when the ELF has a `.defmt` section, a channel named "defmt" (or
// channel 0 of a tool that does not name channels) goes through
// `defmt-print -e ELF --json stdin`, one JSON record a line after a schema
// line (defmt-print 1.2.0, decoder/defmt-json-schema), and blit draws each
// record: level in its colour, timestamp, message, file:line.
#include <string>
#include <string_view>
#include <vector>

#include "build.hpp"
#include "common.hpp"

namespace blit {

class Elf;

// What the ELF says of RTT and defmt.
struct RttElf {
    bool block = false;    // it has `_SEGGER_RTT`
    u64 addr = 0, size = 0;
    bool defmt = false;    // it has a `.defmt` section
};
RttElf rtt_elf(const Elf& elf);

// A tab a board's tool serves: what it is, where blit reaches it.
struct StreamPlan {
    std::string kind;   // "rtt", "semihosting"
    std::string name;   // "RTT 0", "RTT 1: defmt", "Board output"
    int channel = -1;   // an RTT channel's number
    bool listen = false;  // blit listens and the tool connects (QEMU's chardev)
    u16 port = 0;
    std::string greeting;  // sent once connected (J-Link's channel string)
    bool input = true;     // typing goes to the board (a down-channel, semihosting's input)
    bool defmt = false;    // through defmt-print
    bool pushed = false;   // fed by a session's events (probe-rs's DAP server), not a port
    std::string file;      // fed by a file a tool writes (QEMU's own output), read as it grows
};

// What a board's debug session (or an RTT server started for a running
// board) adds for its RTT and semihosting, by preset.
struct BoardStreams {
    std::vector<StreamPlan> tabs;     // the tabs known before it starts
    u16 tcl_port = 0;                 // OpenOCD's Tcl port: the RTT driver finds the channels through it
    u16 rtt_base = 0;                 // channel n's server at rtt_base + n (OpenOCD, pyOCD)
    std::string server_words;         // put on the gdb server's command line (sh words, each with a space before it)
    std::string openocd_words;        // put on OpenOCD's (the Tcl port, in place of "disabled")
    std::string qemu_words;           // put on QEMU's
    std::vector<std::string> after;   // gdb's commands once attached
    std::string pyocd_config;         // the YAML pyOCD is given (--config), "" none
    std::string pyocd_path;           // (and where it is written)
    std::string server_out;           // the server's output to this file, not to gdb's errors (QEMU's semihosting)
    std::vector<std::string> notes;   // what this preset cannot give, in words, said once
    bool any() const { return !tabs.empty() || tcl_port || !pyocd_config.empty(); }
};

// The streams for a board's preset: `elf` what its ELF says (RTT needs the
// block's address but for J-Link, which can look for it); `tcl_port` a free
// port for OpenOCD's Tcl server; `files` where files of its own go (a path
// and the start of a name: pyOCD's config is files + ".yaml", QEMU's output
// files + "-out.txt"). `standalone`: a server started for a running board,
// with no gdb (no `after`).
BoardStreams board_streams(const Board& b, const std::string& preset, const RttElf& elf, u16 tcl_port, const std::string& files, bool standalone);

// What J-Link's RTT telnet port is sent first for channel n ("" for 0).
std::string jlink_channel(int n);

// The default ports: RTT channel 0's server (19021, J-Link's own; channel
// n at 19021 + n for OpenOCD and pyOCD), and the semihosting port blit
// listens on for QEMU or connects to for OpenOCD (19020).
constexpr u16 kRttPort = 19021, kSemihostingPort = 19020;

// ---------------------------------------------------------------- OpenOCD's Tcl port

// A command for OpenOCD's Tcl server: the line, then 0x1a (OpenOCD 0.12
// src/server/tcl_server.c lines 216-240: each command ends at 0x1a, each
// answer is its result then 0x1a).
std::string tcl_command(std::string_view line);
// The answers in what came, each without its 0x1a; what is left of an
// answer not ended yet stays in `buf`.
std::vector<std::string> tcl_answers(std::string& buf);
// `rtt channels`' answer (src/rtt/tcl.c lines 100-150): "Channels: up=N,
// down=M", "Up-channels:", "I: NAME SIZE FLAGS" ..., "Down-channels:", ...
// The up-channels (number, name), or none when the block is not found.
struct RttChannel {
    int number = 0;
    std::string name;
};
bool tcl_channels(std::string_view answer, std::vector<RttChannel>* up, std::vector<RttChannel>* down);

// ---------------------------------------------------------------- defmt-print's records

struct DefmtRecord {
    u64 host_ns = 0;        // defmt-print's host_timestamp (Unix ns)
    std::string target_ts;  // the target's, as defmt formats it ("" none)
    int level = 0;          // 0 none (println), 1 trace .. 5 error
    std::string data;       // the message
    std::string file;       // where (relative to defmt-print's folder, as it prints it)
    u32 line = 0;
};
// One line of `defmt-print --json`: a record (true), the schema line
// (*schema set), or neither.
bool defmt_parse(std::string_view line, DefmtRecord* r, int* schema);
// A record as a line on a tab: the target's time, the level in its colour
// (ANSI), the message, the place; ending CR LF.
std::string defmt_line(const DefmtRecord& r);
const char* defmt_level_name(int level);  // "TRACE" .. "ERROR"

// ---------------------------------------------------------------- programs

// A program started with pipes to its input, output and errors (each -1 if
// not asked for), in `dir`, by `sh -c LINE` or by argv. -1 and why when it
// cannot be started (why: it is not installed on this machine).
struct Spawned {
    int pid = -1, in = -1, out = -1, err = -1;
};
Spawned spawn_piped(const std::vector<std::string>& argv, const std::string& dir, bool in, bool out, bool err, std::string* why);
// Ended (TERM, then KILL after `grace_ms`) and reaped; the descriptors closed.
void spawn_end(Spawned& s, int grace_ms = 500);

}  // namespace blit
