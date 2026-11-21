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
};

// Where a folder's run configurations are, from the folder.
inline constexpr const char* kRunConfigs = ".blit/run.json";
// The run configurations in root's .blit/run.json: { "configurations": [ {
// "name", "program", "args" (a list, or one line), "env" (an object),
// "cwd" (from the folder), "build" (a task to run first), "problems" (as
// BuildTask's: a list, or "none") } ] }. `problem`
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
