#pragma once
// What a build says is wrong, read from its output as it comes: GCC's and
// Clang's "path:line:col: error: message" (and warning, fatal error),
// CMake's "CMake Error at path:line (command):" with the lines under it,
// make's "path:line: *** message", Python's tracebacks (the innermost
// frame of the program's own, and the exception), its warnings and
// pytest's "path.py:line: AssertionError"; and the sanitizers' reports
// (AddressSanitizer, LeakSanitizer, ThreadSanitizer, MemorySanitizer: at
// the first frame of a stack in the program's own files, what was wrong;
// UndefinedBehaviorSanitizer's "path:line:col: runtime error: ..."). The bytes are a terminal's: the
// escape sequences are dropped, and a line a carriage return goes back
// over (Ninja's progress) is the line written over it.
//
// Relative paths are the folder the build runs in, or one make or Ninja
// says it entered ("Entering directory"): the one the file is in. It runs
// on the thread that reads the build's output (appterm.cpp); nothing here
// knows about windows.
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common.hpp"

namespace blit {

// A place a problem goes on to name: a compiler's "note:", a header in an
// include chain, a frame of a sanitizer's stack, the other access of a race.
struct BuildNote {
    std::string path;  // as BuildProblem's (empty: a line of text with no place)
    u32 line = 0, col = 0;
    std::string message;
};

struct BuildProblem {
    std::string path;   // the file: a full path, where one could be made
    u32 line = 1;       // from 1
    u32 col = 0;        // from 1; 0: not said
    u8 severity = 1;    // 1 an error, 2 a warning (the language server's numbers)
    std::string message;
    std::string code;   // the tool's own name for it, where it gives one (a lint rule, a VUID); "" if none
    std::vector<BuildNote> notes;  // what it goes on to name, in order
    u32 count = 1;      // the times the output said it (a validation message, every frame: one row); more than 1 from the scan: that many so far, for the row already found
    u32 group = 0, seq = 0;  // (the editor's: its file, by the order the files came in, and its own place in that order)
};

class BuildScan {
public:
    // dir: the folder the build runs in, a path here or "ssh://host/path"
    // there. exists: whether a file is there, for a relative path that
    // could be from more than one folder (none: the latest folder entered).
    // by_name: the full path of the one file of the project with a name, for
    // a bare name that is in none of those folders (ASan's on macOS,
    // symbolised by atos: "ring.cpp:16"); empty when it is not one file's.
    explicit BuildScan(std::string dir, std::function<bool(const std::string&)> exists = {}, std::function<std::string(const std::string&)> by_name = {});

    void feed(std::string_view bytes);
    void finish();  // the end: the last line, and what waited on lines after it

    // What was found since this was last emptied (the caller takes them).
    std::vector<BuildProblem>& found() { return found_; }
    // The folders entered, as full paths, the latest last; and a count that
    // goes up when they change.
    const std::vector<std::string>& dirs() const { return seen_; }
    u64 dirs_gen() const { return dirs_gen_; }
    u64 lines() const { return lines_; }
    // What the run came to, in a few words, for its task's row: the tests'
    // count ("12 passed, 1 failed"), a firmware's size ("text 14.2 KB"),
    // timing's worst slack; "" when its output said none of that.
    const std::string& summary() const { return summary_; }

    static constexpr size_t kMaxProblems = 5000;  // a run's, at most
    static constexpr size_t kMaxLine = 4096;      // bytes of a line looked at

private:
    void line(std::string_view text);
    // Each kind of output in a file of its own, taking a line if it is one
    // of its kind (true), and told when the output has ended (what a kind
    // was holding for the lines after it, it adds then):
    bool validation_line(std::string_view text);  // buildvalidation.cpp: Vulkan's and Metal's validation messages
    void validation_end();
    bool matcher_line(std::string_view text);     // buildmatch.cpp: init.lua's patterns (blit.build.matcher)
    void matcher_end();
    bool tool_line(std::string_view text);        // buildtools.cpp: linkers, assemblers, HDL tools, shader compilers, Python's tools
    void tool_end();
    bool test_line(std::string_view text);        // buildtests.cpp: test frameworks' results, sizes, timing
    void test_end();
    bool cmake_line(std::string_view text);
    bool python_line(std::string_view text);
    bool compiler_line(std::string_view text);
    bool sanitizer_line(std::string_view text);
    void entered(std::string_view text);
    void wrapped_join();
    void wrapped_end();
    void cmake_end();
    // A problem found (none if it was found before, or there are too many):
    // the one added, to put its code and notes on, until the next is added.
    BuildProblem* add(std::string_view path, u32 line, u32 col, u8 severity, std::string_view message);
    std::string resolve(std::string_view path);

    std::string prefix_;  // "ssh://host" for a folder there; empty here
    std::string base_;    // the folder the build runs in (a path, after the prefix)
    std::function<bool(const std::string&)> exists_;
    std::function<std::string(const std::string&)> by_name_;
    std::vector<std::string> dirs_;  // entered and not yet left
    std::vector<std::string> seen_;  // every folder entered, with the prefix
    std::vector<std::string> hints_; // folders "make -C" was told to go into
    u64 dirs_gen_ = 0;
    std::unordered_map<std::string, std::string> resolved_;  // (for the folders as they are now)

    // The bytes: a line being put together, and where in an escape sequence they are.
    enum : u8 { kText, kEsc, kEscMore, kCsi, kString, kStringEsc } esc_ = kText;
    bool cr_ = false;  // a carriage return: what comes next is written over the line
    std::string line_;
    u64 lines_ = 0;

    // A compiler's problem, kept for a line: Clang writing to a terminal
    // wraps its message to the terminal's width, onto lines under it.
    bool wrap_ = false;
    BuildProblem wrap_problem_;  // (its path as written)
    // The last line six spaces in under it: its message, unless carets come
    // next (then it was the source line, printed with no line number).
    bool wrap_held_ = false;
    std::string wrap_line_;
    // CMake's problem, whose message is the lines under it; one outside the
    // project (in a module of CMake's), held for its call stack's frames.
    bool cmake_ = false;
    bool cmake_outside_ = false, cmake_held_ = false, cmake_stack_ = false;
    BuildProblem cmake_problem_;
    // A sanitizer's report: what it says is wrong (and what was read or
    // written, or leaked), and whether a frame of its stack has been taken.
    bool san_ = false, san_taken_ = false;
    std::string san_kind_, san_what_;
    // Python's traceback: its innermost frame of a file's own, so far.
    bool py_ = false;
    std::string py_path_;
    u32 py_line_ = 0;

    // (Each kind's own state, below its name.)
    // buildvalidation.cpp:
    // A validation message being read (its lines go on to its end), what it
    // is so far (its path as written; none: no place), whether it is one to
    // show (not the layer's Information), where in it the lines are; and
    // each message's first problem, with the times it has been said.
    u8 val_ = 0;
    bool val_show_ = false, val_said_ = false, val_spec_ = false, val_more_ = false;
    BuildProblem val_problem_;
    std::unordered_map<std::string, BuildProblem> val_seen_;
    // buildmatch.cpp:
    // buildtools.cpp:
    // buildtests.cpp:
    std::string summary_;

    std::vector<BuildProblem> found_;
    size_t count_ = 0;
    std::unordered_set<std::string> seen_problems_;  // (the same one again is not: a header's error, from each file that includes it)
};

// A place named in a line of text around byte `at`: "path:line[:col]" as
// compilers, CMake and make write them, or Python's File "path", line N.
// begin and end: the bytes it takes.
struct TextPlace {
    std::string path;
    u32 line = 0, col = 0;
    size_t begin = 0, end = 0;
};
bool place_in_text(std::string_view text, size_t at, TextPlace* out);

// "a/./b/../c" as "a/c"; a leading "/" kept.
std::string tidy_path(std::string_view path);

}  // namespace blit
