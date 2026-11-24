#pragma once
// Simulation runs (native-fpga.md 7): which simulator a task's command
// runs, and which dump a run of it wrote, for the wave tab to open when the
// run ends (appsim.cpp). Knows no window: the command, the run's output
// and the files it wrote are all it looks at.
//
// A run's dump is, best first:
//   1. the one its output names: Icarus's vvp says "VCD info: dumpfile X
//      opened for output." (or FST info), SymbiYosys "counterexample
//      trace: X" for a failed assertion, else "cover trace: X";
//   2. one the command names: a word ending .vcd or .fst (alone, or after
//      an = or a +: +dumpfile=x.vcd), or $dumpfile("X") in a Verilog or
//      SystemVerilog source it names;
//   3. the newest .vcd or .fst under the folder it ran in;
// each only if it was written during the run (a name in the output or the
// command is looked for there too, under that folder, for a command that
// went into another one first).
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit {

// The simulator a command line runs: "Verilator" (`verilator --binary`, or
// the program it built, obj_dir/V<top> or under its --Mdir), "Icarus"
// (`iverilog`, `vvp`) or "SymbiYosys" (`sby`); "" for none of them.
std::string_view sim_tool(std::string_view cmd);

// A command line's words as sh splits them (quotes taken off; ; && || |
// ( ) and & between commands are words of their own).
std::vector<std::string> sh_words(std::string_view cmd);

// The dumps a run's output names, best first (as written: from the folder
// it ran in, or whole paths).
std::vector<std::string> sim_dumps_said(std::string_view output);

// What a command names: dumps (words ending .vcd or .fst), and Verilog and
// SystemVerilog sources (.v .sv .vh .svh), for their $dumpfile.
void sim_named(std::string_view cmd, std::vector<std::string>* dumps, std::vector<std::string>* sources);

// The names $dumpfile("...") is given in a source's text.
std::vector<std::string> sim_dumpfiles(std::string_view text);

// A dump written under a folder: its path from the folder, and when it was
// last written (seconds since 1970).
struct SimDump {
    std::string path;
    i64 when = 0;
};

// Of the dumps found under the folder a run ran in (written since it
// started, newest first), the one it wrote: as said above, from what its
// output said and its command named (the names its sources give
// included). "" if there is none.
std::string sim_choose(const std::vector<SimDump>& found, const std::vector<std::string>& said, const std::vector<std::string>& named);

// The dumps under a folder of this machine written at or after `since`
// (seconds since 1970; 0: any), newest first: its folders to six deep,
// not hidden ones (.git, .blit) nor node_modules.
std::vector<SimDump> sim_find_here(const std::string& dir, i64 since);

// The shell command that lists them on another machine (a line each, "WHEN
// PATH", after a first with the machine's time now), for its find and stat,
// GNU's or BSD's; and what it printed, read (`ago`: how long the run took,
// in seconds, measured here, so that the two clocks need not agree).
std::string sim_find_command(i64 ago);
std::vector<SimDump> sim_find_read(std::string_view out, i64 ago);

}  // namespace blit
