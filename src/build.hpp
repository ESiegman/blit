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
};

// The built-in systems' names, in the order they are looked for.
const std::vector<std::string>& build_systems();
// The tasks of every system found in root, whose entries are `files`
// (files may be read; on another machine that is a round trip each).
// Systems named in `hidden` are not looked for.
std::vector<BuildTask> detect_builds(const std::string& root, const std::vector<FsEntry>& files, const std::vector<std::string>& hidden = {});

}  // namespace blit
