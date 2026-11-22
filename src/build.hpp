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
    std::vector<std::string> env;  // "NAME=value"
    std::string before;            // a task of the Build tab's to run first (its name)
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
    Board board;  // (its "board", if it has one)
};

// Where a folder's run configurations are, from the folder.
inline constexpr const char* kRunConfigs = ".blit/run.json";
// The run configurations in root's .blit/run.json: { "configurations": [ {
// "name", "program", "args" (a list, or one line), "env" (an object),
// "cwd" (from the folder), "build" (a task to run first), "problems" (as
// BuildTask's: a list, or "none"), "debug" (as BuildTask::Debug: "adapter"
// a name or a command line, or a list of its words; "request"; "arguments",
// an object; "after", a list; "console"), "board" (as Board) } ] }. `problem`
// says what is wrong with the file, if anything is.
std::vector<BuildTask> read_run_configs(const std::string& root, std::string* problem);
// A word as sh reads it back (in single quotes when it needs them).
std::string sh_word(std::string_view word);

// The built-in systems' names, in the order they are looked for.
const std::vector<std::string>& build_systems();
// The tasks of every system found in root, whose entries are `files`
// (files may be read; on another machine that is a round trip each).
// Systems named in `hidden` are not looked for.
std::vector<BuildTask> detect_builds(const std::string& root, const std::vector<FsEntry>& files, const std::vector<std::string>& hidden = {});

}  // namespace blit
