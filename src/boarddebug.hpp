#pragma once
// A board debugged through a preset: its run configuration's board.debug
// (else the one its probe's kind says) turned into what the debug door
// takes (appdebug.cpp): the debugger to start, as a command line for sh
// (the cross gdb, found by the ELF's machine, and the gdb server beside it,
// ended with it), DAP's attach and its arguments, and gdb's commands once
// attached. F5 resets the board (flashed first by its flash tool, or loaded
// by gdb where it has none) and runs to main; "Attach to the running board"
// connects as it is, with no reset and no flash, to look at it.
//
// The presets (kDebugPresets): probe-rs (its gdb server), openocd (started
// by gdb through a pipe), jlink (SEGGER's GDB server), esp (Espressif's
// OpenOCD through a pipe, and its gdb), blackmagic (the probe's own GDB
// port, as extended-remote), pyocd (its gdb server), qemu (the simulator
// started by gdb through a pipe) and renode (its GDB server).
#include <string>
#include <vector>

#include "build.hpp"

namespace blit {

struct BoardDebug {
    std::string preset;   // its name
    std::string adapter;  // the debugger: a command line for sh
    std::string arguments;            // attach's (a JSON object)
    std::vector<std::string> after;   // gdb's commands once attached
    std::string run_to;    // F5's: where a temporary breakpoint takes it ("main"); "" when attaching
    bool flash = false;    // F5's: the board flashed first (by its flash tool; else `load` is among `after`)
    bool simulator = false;  // the target is a simulator started for the session, ended with it
    std::string why;       // it cannot be debugged so: why
    std::string shown;     // what it starts, in a line: the gdbs looked for, the server, the target (the tests')
};

// The preset a board is debugged with: its debug, else the one its probe's
// kind says (ST-LINK and CMSIS-DAP: probe-rs; J-Link: jlink; Espressif's
// USB-Serial/JTAG: esp; Black Magic: blackmagic); "" when nothing says.
std::string board_preset(const Board& b);

// What a board's preset starts: `elf` the ELF's path as the debugger's
// machine has it; `attach`, the running board as it is (no reset, no
// flash, no run to main); `load`, gdb loads the image itself, for a board
// with no flash tool.
BoardDebug board_debug(const Board& b, const std::string& elf, bool attach, bool load);

// The programs a preset's command line may say are missing ("a|b|c": the
// first of those looked for, then the others), as words: "a is not
// installed on lab (nor b, c)".
std::string board_missing(std::string_view names, std::string_view host);

}  // namespace blit
