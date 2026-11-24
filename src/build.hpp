#pragma once
// What a folder builds with: the build systems of C, C++ and HIP (CMake,
// Meson, make, autotools, Ninja) and Python's (uv, pip), found from the
// files in it, and the tasks each offers (configure, build, test, run, its
// own targets). The Build tab shows them (appbuild.cpp); any other is
// init.lua's to add, with blit.build.add.
#include <string>
#include <vector>

#include "fsio.hpp"

namespace blit {

// A run configuration's board (its "board" object in .blit/run.json, data
// as written there; "Choose the probe..." writes probe and serial,
// appprobe.cpp): the probe it is flashed and debugged through, its serial
// port, and what the flashing and debugging parts take from it. Paths are
// as written (from the folder).
struct Board {
    bool set = false;        // the configuration has one
    std::string probe;       // probe.serial: its serial number (or "probe": "SERIAL")
    std::string probe_kind;  // probe.kind: "stlink", "jlink", "cmsis-dap", "esp-usb-jtag", "blackmagic", "ftdi", ... (probes.cpp's table)
    std::string probe_usb;   // probe.usb: "0483:374e", its vendor and product ids
    std::string probe_gdb;   // probe.gdb: a probe's own GDB server port (Black Magic's)
    std::string port;        // serial.port (or "serial": "PORT"): its console
    int baud = 0;            // serial.baud
    std::string chip, elf, svd;
    std::string flash;  // flash.tool (or "flash": "TOOL"): "esptool", "openocd", "probe-rs", "jlink", "openfpgaloader"
    std::string debug;  // debug.preset (or "debug": "PRESET")
    // The rest of the debug object (boarddebug.hpp): the gdb (else the one
    // the ELF's machine says), the gdb server's port, QEMU's machine or
    // Renode's platform (else the chip), Renode's script, and the function
    // F5 runs to (else main; app_main on an ESP32).
    std::string debug_gdb, debug_port, debug_machine, debug_script, debug_main;
    // The rest of the flash object (flash.cpp): where a raw .bin goes
    // (address), openFPGALoader's board (-b) or cable (-c), OpenOCD's
    // interface and target scripts, J-Link's device (else the chip), the
    // flash part Vivado writes an FPGA's flash as (cfgmem: its name in
    // get_cfgmem_parts); and an FPGA's bitstream (else the newest the build made).
    std::string flash_address, flash_board, flash_cable, flash_interface, flash_target, flash_device, flash_cfgmem;
    std::string bitstream;
};

struct BuildTask {
    std::string system;  // "CMake", "Make", ...
    std::string name;    // "Build", "Test (build)", "make install"
    std::string cmd;     // run by sh in the folder
    std::string cwd;     // relative to the folder; empty: the folder itself
    bool def = false;    // the one the Run button runs, until another is chosen
    // A run configuration's (.blit/run.json): what it runs, for Debug,
    // Profile and the tests to take as theirs.
    bool config = false;
    std::string program;           // its full path (from the folder's, when written relative)
    std::string args;              // as one line, as a shell splits it
    std::vector<std::string> env;  // "NAME=value" (a run configuration's: its own, then the environment chosen's)
    std::vector<std::string> own_env;  // (a run configuration's own, before the environment chosen)
    std::string before;           // a task of the Build tab's to run first (its name)
    // init.lua's patterns its output is read with (blit.build.matcher): those
    // named; empty, all of them; {"none"}, nothing in it is a problem.
    std::vector<std::string> problems;
    // Its "debug" (F5's): the debugger ("lldb-dap", "gdb", "debugpy", or a
    // command line; empty: the machine's own), "launch" or "attach", the
    // arguments laid over those blit sends (an object's JSON text), commands
    // the debugger runs once the session has started, and where the
    // program's input and output go ("terminal", "debug"; empty: decided).
    struct Debug {
        std::string adapter, request, arguments, console;
        std::vector<std::string> after;
    } debug;
    // A task of .blit/tasks.json's: one that runs long (a simulation, a
    // synthesis: its own output tab, which other runs leave alone, asked
    // about before anything ends it, its time on the Run button), and the
    // kind of work it is (one of kTaskKinds; empty: none said).
    bool lasting = false;
    std::string kind;
    Board board;  // (its "board", if it has one)
};

// The folder's own tasks, from the folder.
inline constexpr const char* kTasks = ".blit/tasks.json";
// What a task may say it is: flashing a board, programming an FPGA, a
// synthesis, a simulation, a GPU profile.
inline constexpr const char* kTaskKinds[] = {"flash", "program", "synthesis", "simulation", "gpu-profile"};
// The tasks in root's .blit/tasks.json: { "tasks": [ { "label", "command"
// (a line for sh), "args" (a list of words, each quoted as it needs), "cwd"
// (from the folder), "env" (an object), "problems" (as BuildTask's: a list,
// or "none"), "long" (true or false), "kind" (one of kTaskKinds) } ] } (or
// that list alone). `problem` says what is wrong with the file, if anything is.
std::vector<BuildTask> read_tasks(const std::string& root, std::string* problem);
// What a task's command may name, as ${file}, ${line}, ${word},
// ${selection} and ${workdir} (and ${fileDirname}, ${fileBasename} and
// ${fileBasenameNoExtension}, from the file): the file in front and its
// cursor's line (from 1), the word there, what is selected, the folder
// (on another machine, the paths there).
struct TaskVars {
    std::string file, line, word, selection, workdir;
};
// A command line with those put in, each quoted for sh as the place it is
// in needs (bare, in '...' or in "..."); \${file} and names not among
// them (${HOME}) are left for sh.
std::string with_vars(std::string_view cmd, const TaskVars& vars);

// Where a folder's run configurations are, from the folder.
inline constexpr const char* kRunConfigs = ".blit/run.json";
// The run configurations in root's .blit/run.json: { "configurations": [ {
// "name", "program", "args" (a list, or one line), "env" (an object),
// "cwd" (from the folder), "build" (a task to run first), "problems" (as
// BuildTask's: a list, or "none"), "debug" (as BuildTask::Debug: "adapter"
// a name or a command line, or a list of its words; "request"; "arguments",
// an object; "after", a list; "console"), "board" (as Board) } ] }. `problem`
// says what is wrong with the file, if anything is.
struct RunEnvironments;
std::vector<BuildTask> read_run_configs(const std::string& root, std::string* problem, RunEnvironments* environments = nullptr);

// run.json's "environments": { NAME: { VARIABLE: value, ... } }, each a
// set of variables laid over every configuration's own (for Run, Debug,
// Profile and the tests), one chosen at a time in the Build tab, so that
// validation, a HUD or a trace is a choice rather than a copy of each
// configuration. A capture preset among them: "capture": "metal" (the
// program run with MTL_CAPTURE_ENABLED=1, and "Capture a frame" takes one
// with gpucapture) or "renderdoc" (the program run by `renderdoccmd
// capture`, whose captures, as the program takes them, go to
// .blit/captures).
struct RunEnvironment {
    std::string name, capture;
    std::vector<std::string> env;  // "NAME=value"
};
struct RunEnvironments {
    std::vector<RunEnvironment> list;  // (in the file's order)
};
inline constexpr const char* kCaptures = ".blit/captures";  // where captures go, from the folder
// A run configuration in an environment (null: none): its env, its own and
// then the environment's, and what the Run button runs, as
// read_run_configs makes it (`root`: the folder, for the captures' place).
void run_in_environment(BuildTask& t, const RunEnvironment* e, const std::string& root);
// A word as sh reads it back (in single quotes when it needs them).
std::string sh_word(std::string_view word);

// The built-in systems' names, in the order they are looked for.
const std::vector<std::string>& build_systems();
// The tasks of every system found in root, whose entries are `files`
// (files may be read; on another machine that is a round trip each).
// Systems named in `hidden` are not looked for.
std::vector<BuildTask> detect_builds(const std::string& root, const std::vector<FsEntry>& files, const std::vector<std::string>& hidden = {});

}  // namespace blit
