#pragma once
// init.lua's patterns for a build's output (blit.build.matcher), made once
// into a table that the thread reading the output runs (buildmatch.cpp):
// Lua patterns, whose captures are a problem's file, line, column, how bad,
// message and code. Made on the main thread, never changed after: a run's
// scan keeps the table it was given.
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace blit {

// A pattern, and which of its captures (from 1; 0: none) are what.
struct BuildPattern {
    std::string pattern;  // empty: none
    // Text every line it takes has, looked for first, so that a line
    // without it costs a find and not the pattern (at the line's start
    // when at_start); empty when the pattern has none.
    std::string literal;
    bool at_start = false;
    u8 captures = 0;
    u8 file = 0, line = 0, col = 0, severity = 0, message = 0, code = 0;
};

struct BuildMatcher {
    std::string name;
    BuildPattern first;
    // The lines after the first that go on with it (a simulator's message,
    // with its place on the line under it): what each takes fills in what
    // was not taken yet, and its message goes after the first's.
    BuildPattern next;
    std::string file;  // the file each problem is in, for a tool that names none; "": a capture's
    u8 severity = 1;   // when no capture says: 1 an error, 2 a warning
};

// What a run's output is read with: init.lua's matchers for it, or nothing
// at all (none: the task's output is no problems).
struct BuildMatchers {
    std::vector<BuildMatcher> list;
    bool none = false;
};

// A pattern made ready (its literal text found), or false and why it is
// not one.
bool build_pattern(std::string pattern, BuildPattern* out, std::string* why);
// How bad a tool's word says it is: "error", "Fatal", "E", "UVM_ERROR" 1;
// "warning", "W", "CRITICAL WARNING" 2; 0 neither ("Note", "INFO": no
// problem).
u8 build_severity(std::string_view word);

}  // namespace blit
