#pragma once
// Problems from a file a tool wrote ("Read the problems from this file",
// appbuild.cpp): SARIF 2.1 (each result at its first location, its rule as
// the code, its related locations and code flows as notes), GCC's JSON
// diagnostics (-fdiagnostics-format=json, and json-file; its children as
// notes), else a log, read as a build's output is (BuildScan). Run on a
// worker thread; nothing here knows about windows.
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "buildscan.hpp"

namespace blit {

// text: the file's; path: its full path (here, or ssh://host/...), whose
// folder relative paths in it are from (SARIF's originalUriBaseIds first,
// where exists says the file is there; with no exists, wherever they say).
// kind: what it was read as ("SARIF", "GCC's JSON", "a log"); false, and
// why in kind, when it is JSON that is neither.
bool read_problems(std::string_view text, const std::string& path, const std::function<bool(const std::string&)>& exists, std::vector<BuildProblem>* out,
                   std::string* kind);

}  // namespace blit
