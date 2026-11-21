#pragma once
// A project's tests, as its runners list them, and what a run of them
// reported: CTest (and so GoogleTest, Catch2 and doctest tests registered
// with it) and pytest. Only reading here: the Tests tab (apptest.cpp) runs
// the commands, on a thread and in the output terminal.
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common.hpp"

namespace blit {

struct TestCase {
    // kOutput: one a task's output gave (GoogleTest's, Catch2's, doctest's,
    // Unity's) that no runner here has: under the task's name and its
    // framework, run by running the task (its key, task_key's, as dir).
    enum Runner : u8 { kCTest, kPytest, kOutput };
    enum Result : u8 { kNotRun, kPassed, kFailed, kSkipped };
    Runner runner = kCTest;
    std::string id;     // what its runner calls it: CTest's name, pytest's node id (path::Class::name)
    std::string name;   // what the tab shows: the id's last part
    std::string group;  // what it is listed under: CTest's build folder, pytest's file
    std::string dir;    // CTest: the build folder it is run in
    std::vector<std::string> command;  // CTest: the program and its arguments (what is debugged)
    std::string cwd;                   // and where it runs (CTest's WORKING_DIRECTORY)
    Result result = kNotRun;
    std::string message;  // a failure's, as its report gave it
    double seconds = 0;
};

// What `ctest --show-only=json-v1` printed: its tests, in `dir`.
std::vector<TestCase> parse_ctest_json(std::string_view text, const std::string& dir);
// What `pytest --collect-only -q` printed: a node id a line.
std::vector<TestCase> parse_pytest_collect(std::string_view text);

// One test's outcome in a JUnit XML report (CTest's --output-junit,
// pytest's --junitxml; and pytest's for a module it could not collect: no
// classname, the module's dotted name, failed, why as its message).
struct TestOutcome {
    std::string name, classname;
    TestCase::Result result = TestCase::kPassed;
    std::string message;
    double seconds = 0;
};
std::vector<TestOutcome> parse_junit(std::string_view xml);
// Whether an outcome is that test's: CTest by name; pytest by its node id's
// module (and class) as the report writes them, dotted.
bool outcome_is(const TestOutcome& o, const TestCase& t);

// Which lines of a file a run ran and missed (from 1), and the share of its
// lines that ran.
struct FileCoverage {
    std::vector<u32> hit, missed;  // in order
    double percent = 0;
};
// GCC's gcov --json-format --stdout (a JSON document a line; paths from the
// build folder `dir` when not whole), and coverage.py's `coverage json`
// (paths from `dir`). Each file's coverage, by its whole path.
std::vector<std::pair<std::string, FileCoverage>> parse_gcov_json(std::string_view text, const std::string& dir);
std::vector<std::pair<std::string, FileCoverage>> parse_coverage_py(std::string_view text, const std::string& dir);

// Where tests are defined in a source file: each line (from 0) and the name
// its runner gives it. C and C++: TEST(A, B), TEST_F, TEST_P and TYPED_TEST
// ("A.B", as gtest_discover_tests names them), TEST_CASE("x") and
// TEST_CASE_METHOD(F, "x") ("x"), SCENARIO("x") ("Scenario: x"). Python:
// def test_x (in a class Test...: "Class::test_x").
std::vector<std::pair<u32, std::string>> find_test_defs(std::string_view text, bool python);

}  // namespace blit
