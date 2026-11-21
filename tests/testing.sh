#!/bin/sh
# The Tests tab: a project's tests found (CTest's in a configured build
# folder; pytest's, where pytest is installed), run all, one, or the failed
# ones again, how each went read from the reports, and the marks where
# tests are defined. Needs cmake and a C++ compiler.
#
#   tests/testing.sh path/to/blit            run the cases
#   tests/testing.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/testing.sh path/to/blit [--print]}
print=${2:-}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
export BLIT_RECENT_FILE="$dir/recent"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\nblit.opt.editor.restore = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua"

# A project: two CTest tests (one fails), and two pytest ones (one fails).
P="$dir/p"
mkdir -p "$P/tests"
cat >"$P/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.21)
project(tp CXX)
enable_testing()
add_executable(check check.cpp)
add_test(NAME adds COMMAND check adds)
add_test(NAME breaks COMMAND check breaks)
EOF
cat >"$P/check.cpp" <<'EOF'
#include <cstdio>
#include <cstring>
// TEST(Not, Registered) is a test the runner does not have: no mark.
int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "breaks") == 0) return std::puts("2 + 2 is not 5"), 1;
    return std::puts("ok"), 0;
}
EOF
cat >"$P/tests/test_math.py" <<'EOF'
def test_adds():
    assert 1 + 1 == 2


class TestMore:
    def test_fails(self):
        assert 2 + 2 == 5
EOF
cmake -S "$P" -B "$P/build" >/dev/null 2>&1 && cmake --build "$P/build" >/dev/null 2>&1 || { echo "testing: cannot build the project"; exit 1; }
pytest=no
if command -v pytest >/dev/null 2>&1 || python3 -m pytest --version >/dev/null 2>&1; then pytest=yes; fi

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run FILE SCRIPT: the last line the script prints.
run() {
    got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$P;frame;$2" "$1" 2>&1 >/dev/null | grep -v '^blit: ' | tail -1)
}
# (Where pytest is not installed, its tests are not found: the CTest part alone.)
py() { if [ "$pytest" = yes ]; then printf '%s' "$1"; fi; }

run "$P/check.cpp" "testdump"
check "the tests found, by folder and file, none run yet" "build: .adds .breaks$(py ' | tests/test_math.py: .test_adds .test_fails')"
run "$P/check.cpp" "command:Run the tests;testdump"
check "all run: each as its report says" "build: +adds xbreaks$(py ' | tests/test_math.py: +test_adds xtest_fails')"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.restore = false\n' >"$dir/vim.lua"
BLIT_INIT="$dir/vim.lua"
run "$P/check.cpp" "vim:gX;testdump"
BLIT_INIT="$dir/init.lua"
check "and with gX in Normal mode" "build: +adds xbreaks$(py ' | tests/test_math.py: +test_adds xtest_fails')"
run "$P/check.cpp" "testdump;command:Show Tests;frame;key:Down;key:Return;testdump"
check "one run from the tab" "build: .adds xbreaks$(py ' | tests/test_math.py: .test_adds .test_fails')"
got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$P;frame;testdump;command:Run the failed tests again" "$P/check.cpp" 2>&1 >/dev/null | grep '^blit: ' | tail -1)
check "the failed ones again, none yet" 'blit: No test has failed'
if [ "$pytest" = yes ]; then
    run "$P/tests/test_math.py" "testdump"
    check "the marks where they are defined, in the file in front" 'build: .adds .breaks | tests/test_math.py: .test_adds .test_fails | here: 1. 6.'
    run "$P/tests/test_math.py" "key:Down;key:Down;key:Down;key:Down;key:Down;key:Down;command:Run the test at the cursor;testdump"
    check "the test at the cursor run: its mark too" 'build: .adds .breaks | tests/test_math.py: .test_adds xtest_fails | here: 1. 6x'
    run "$P/tests/test_math.py" "command:Run the tests;testdump;command:Run the failed tests again;testdump"
    check "the failed ones again" 'build: +adds xbreaks | tests/test_math.py: +test_adds xtest_fails | here: 1+ 6x'
fi
# pytest and coverage.py as the project's Python has them (its .venv's,
# here one that answers as they would), not the PATH's; the coverage
# report made in the run configuration's environment too.
V="$dir/venv"
mkdir -p "$V/.venv/bin" "$V/tests" "$V/.blit"
printf 'def test_one():\n    pass\n\n\ndef test_two():\n    pass\n\n\n@pytest.mark.parametrize("n", [1, 2])\ndef test_p(n):\n    assert n\n' >"$V/tests/test_v.py"
printf '{ "configurations": [ { "name": "t", "program": "tests/test_v.py", "env": { "FAKE_ENV": "yes" } } ] }\n' >"$V/.blit/run.json"
cat >"$V/.venv/bin/python" <<'EOF'
#!/bin/sh
[ "$1" = -m ] || exit 1
module=$2
shift 2
if [ "$module" = coverage ]; then
    if [ "$1" = json ]; then  # json -q -o FILE
        [ "$FAKE_ENV" = yes ] && printf '{"files": {"tests/test_v.py": {"executed_lines": [1, 2, 5], "missing_lines": [6]}}}' >"$4"
        exit 0
    fi
    shift 3  # run -m pytest
fi
set -f  # (the ids are not patterns)
ids=
for a in "$@"; do
    case $a in
        --collect-only) printf 'tests/test_v.py::test_one\ntests/test_v.py::test_two\ntests/test_v.py::test_p[1]\ntests/test_v.py::test_p[2]\n\n4 tests collected in 0.01s\n'; exit 0 ;;
        --junitxml=*) report=${a#--junitxml=} ;;
        *::*) ids="$ids ${a#*::}" ;;
    esac
done
if [ -e tests/test_bad.py ]; then  # (as pytest reports a module it cannot import: and nothing runs)
    printf '<testsuites><testsuite><testcase classname="" name="tests.test_bad" time="0"><error message="collection failure">tests/test_bad.py:1: in &lt;module&gt;\nE   ModuleNotFoundError: No module named &apos;nosuch&apos;</error></testcase></testsuite></testsuites>' >"$report"
    exit 2
fi
{
    printf '<testsuites><testsuite>'
    for t in ${ids:-test_one test_two test_p[1] test_p[2]}; do  # (those asked for; none: all, test_two failing)
        if [ "$t" = test_two ]; then printf '<testcase classname="tests.test_v" name="%s" time="0.1"><failure message="no"/></testcase>' "$t"
        else printf '<testcase classname="tests.test_v" name="%s" time="0.1"/>' "$t"; fi
    done
    printf '</testsuite></testsuites>'
} >"$report"
EOF
chmod +x "$V/.venv/bin/python"
got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$V;frame;builddump;command:Run the tests;testdump" "$V/tests/test_v.py" 2>&1 >/dev/null | tail -1)
check "pytest in the project's .venv: found and run" 'tests/test_v.py: +test_one xtest_two +test_p[1] +test_p[2] | here: 1+ 5x 10+ 10+'
got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$V;frame;testdump;vim:11G;command:Run the test at the cursor;testdump" "$V/tests/test_v.py" 2>&1 >/dev/null | tail -1)
check "a parametrised test at the cursor: its cases run, not the test before it" 'tests/test_v.py: .test_one .test_two +test_p[1] +test_p[2] | here: 1. 5. 10+ 10+'
# One name at a module's level and in its Test class: each def is its own
# test, and the cursor on one runs that one alone.
K="$dir/same"
mkdir -p "$K/.venv/bin" "$K/tests"
printf 'def test_x():\n    pass\n\n\nclass TestK:\n    def test_x(self):\n        pass\n' >"$K/tests/test_k.py"
cat >"$K/.venv/bin/python" <<'EOF'
#!/bin/sh
shift 2  # -m pytest
set -f
ids=
for a in "$@"; do
    case $a in
        --collect-only) printf 'tests/test_k.py::test_x\ntests/test_k.py::TestK::test_x\n\n2 tests collected in 0.01s\n'; exit 0 ;;
        --junitxml=*) report=${a#--junitxml=} ;;
        *::*) ids="$ids ${a#*::}" ;;
    esac
done
{
    printf '<testsuites><testsuite>'
    for t in ${ids:-test_x TestK::test_x}; do
        case $t in
            TestK::*) printf '<testcase classname="tests.test_k.TestK" name="%s" time="0.1"/>' "${t#TestK::}" ;;
            *) printf '<testcase classname="tests.test_k" name="%s" time="0.1"/>' "$t" ;;
        esac
    done
    printf '</testsuite></testsuites>'
} >"$report"
EOF
chmod +x "$K/.venv/bin/python"
got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$K;frame;testdump;vim:1G;command:Run the test at the cursor;testdump" "$K/tests/test_k.py" 2>&1 >/dev/null | tail -1)
check "one name at a module's level and in a Test class: the cursor's alone" 'tests/test_k.py: +test_x .test_x | here: 1+ 6.'
got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$K;frame;testdump;vim:6G;command:Run the test at the cursor;testdump" "$K/tests/test_k.py" 2>&1 >/dev/null | tail -1)
check "and the cursor on the class's" 'tests/test_k.py: .test_x +test_x | here: 1. 6+'
got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$V;frame;builddump;command:Run the tests with coverage;testdump;coveragedump" "$V/tests/test_v.py" 2>&1 >/dev/null | tail -1)
check "and coverage.py there, its report in the run configuration's environment" 'here: ran 1 2 5, missed 6 | files: tests/test_v.py=75%'
printf 'import nosuch\n' >"$V/tests/test_bad.py"
got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$V;frame;testdump;command:Run the tests;testdump" "$V/tests/test_v.py" 2>&1 >/dev/null | grep '^blit: ' | tail -1)
check "a module pytest could not collect: said, and why" "blit: pytest could not collect tests/test_bad.py: ModuleNotFoundError: No module named 'nosuch'"
rm "$V/tests/test_bad.py"
rm "$V/.venv/bin/python"
got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$V;frame;testdump" "$V/tests/test_v.py" 2>&1 >/dev/null | tail -1)
case "$got" in "No tests found: "*"No module named pytest") got="said" ;; esac
[ "$pytest" = yes ] || check "with no pytest: why none were found" 'said'
# On another machine (tests/fake-ssh.sh's): found and run with the .venv's
# Python there, and a run with coverage that recorded none names it by its
# path in the project, as here.
R="$dir/there/r"
mkdir -p "$R/.venv/bin" "$R/tests"
printf 'def test_r():\n    pass\n' >"$R/tests/test_r.py"
cat >"$R/.venv/bin/python" <<'EOF'
#!/bin/sh
[ "$1" = -m ] || exit 1
module=$2
shift 2
if [ "$module" = coverage ]; then
    [ "$1" = json ] && exit 0  # (nothing recorded)
    shift 3  # run -m pytest
fi
for a in "$@"; do
    case $a in
        --collect-only) printf 'tests/test_r.py::test_r\n\n1 test collected in 0.01s\n'; exit 0 ;;
        --junitxml=*) printf '<testsuites><testsuite><testcase classname="tests.test_r" name="test_r" time="0.1"/></testsuite></testsuites>' >"${a#--junitxml=}" ;;
    esac
done
EOF
chmod +x "$R/.venv/bin/python"
got=$(BLIT_SSH="$(cd "$(dirname "$0")" && pwd)/fake-ssh.sh" BLIT_FAKE_HOME="$dir/there" "$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "remotewait;workdir:ssh://box$R;frame;testdump;command:Run the tests with coverage;testdump" "ssh://box$R/tests/test_r.py" 2>&1 >/dev/null | grep -v '^blit: ' | tail -1)
check "on another machine: the .venv's pytest there" 'tests/test_r.py: +test_r | here: 1+'
got=$(BLIT_SSH="$(cd "$(dirname "$0")" && pwd)/fake-ssh.sh" BLIT_FAKE_HOME="$dir/there" "$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "remotewait;workdir:ssh://box$R;frame;testdump;command:Run the tests with coverage;testdump" "ssh://box$R/tests/test_r.py" 2>&1 >/dev/null | grep '^blit: ' | tail -1)
check "and coverage.py not there: named by its path in the project" 'blit: No coverage was recorded (coverage.py installed for .venv/bin/python?)'
# Coverage, read with GCC's gcov from a build made with --coverage: the
# lines run and missed, each file's share (none of a function no test calls).
gcc=
for c in g++-16 g++-15 g++-14 g++-13 g++; do
    if command -v "$c" >/dev/null 2>&1 && "$c" --version 2>/dev/null | grep -q 'Free Software Foundation'; then gcc=$c; break; fi
done
if [ -n "$gcc" ]; then
    C="$dir/cov"
    mkdir -p "$C"
    cat >"$C/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.21)
project(cov CXX)
enable_testing()
add_executable(run run.cpp)
add_test(NAME runs COMMAND run)
EOF
    cat >"$C/run.cpp" <<'EOF'
int never() {
    return 3;
}

int main() {
    int sum = 0;
    for (int i = 0; i < 3; ++i) sum += i;
    return sum == 3 ? 0 : 1;
}
EOF
    cmake -S "$C" -B "$C/build" -DCMAKE_CXX_COMPILER="$gcc" -DCMAKE_CXX_FLAGS=--coverage >/dev/null 2>&1 && cmake --build "$C/build" >/dev/null 2>&1 || { echo "testing: cannot build with coverage"; exit 1; }
    got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$C;frame;command:Run the tests with coverage;testdump;coveragedump" "$C/run.cpp" 2>&1 >/dev/null | tail -1)
    check "a run with coverage: the lines run and missed in the file in front, and its share" 'here: ran 5 6 7 8, missed 1 2 | files: run.cpp=67%'
    got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$C;frame;command:Run the tests with coverage;testdump;command:Show or hide coverage;coveragedump" "$C/run.cpp" 2>&1 >/dev/null | tail -1)
    check "hidden" 'here: none | files: run.cpp=67%'
    got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$C;frame;command:Run the tests with coverage;testdump;key:End;text:x;coveragedump" "$C/run.cpp" 2>&1 >/dev/null | tail -1)
    check "not on a file changed since" 'here: none | files: run.cpp=67%'
fi
# Where the gcov on the PATH is LLVM's (Apple's), which has no JSON: said.
if gcov --version 2>/dev/null | head -1 | grep -q LLVM && command -v c++ >/dev/null 2>&1; then
    L="$dir/llvmcov"
    mkdir -p "$L"
    printf 'cmake_minimum_required(VERSION 3.21)\nproject(cov CXX)\nenable_testing()\nadd_executable(run run.cpp)\nadd_test(NAME runs COMMAND run)\n' >"$L/CMakeLists.txt"
    printf 'int main() { return 0; }\n' >"$L/run.cpp"
    cmake -S "$L" -B "$L/build" -DCMAKE_CXX_COMPILER=c++ -DCMAKE_CXX_FLAGS=--coverage >/dev/null 2>&1 && cmake --build "$L/build" >/dev/null 2>&1 || { echo "testing: cannot build with clang's coverage"; exit 1; }
    got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$L;frame;command:Run the tests with coverage;testdump" "$L/run.cpp" 2>&1 >/dev/null | grep '^blit: ' | tail -1 | sed 's/LLVM'"'"'s (.*), which/LLVM'"'"'s (VERSION), which/')
    check "LLVM's gcov: named as one that cannot be read" "blit: No coverage could be read: the gcov here is LLVM's (VERSION), which cannot write the JSON blit reads; GCC's gcov can"
fi
# A preset's layout: configured into build/debug, one level in.
Q="$dir/q"
mkdir -p "$Q"
cp "$P/CMakeLists.txt" "$P/check.cpp" "$Q/"
cmake -S "$Q" -B "$Q/build/debug" >/dev/null 2>&1 || { echo "testing: cannot configure build/debug"; exit 1; }
got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$Q;frame;testdump" "$Q/check.cpp" 2>&1 >/dev/null | tail -1)
check "a build folder one level in (build/debug)" 'debug: .adds .breaks'
mkdir -p "$dir/empty"
: >"$dir/empty/a.txt"
got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$dir/empty;frame;testdump" "$dir/empty/a.txt" 2>&1 >/dev/null | tail -1)
check "none: where they would have been looked for" "No tests found: CTest's are found in a build folder that has been configured, pytest's beside a test_*.py"

# A task of the Build tab's that runs a test program: its tests marked
# here as its output says how each went (tests/output: the real output of
# GoogleTest 1.17.0's and Catch2 3.8.1's programs and CTest 4.5's run, as
# tests/build.sh says), added under the task where no runner here has
# them, with their marks where they are defined; one run from here runs
# the task again.
O="$(cd "$(dirname "$0")" && pwd)/output"
G="$dir/gt"
mkdir -p "$G/tests"
cat >"$G/tests/gtest_math.cpp" <<'EOF'
#include <gtest/gtest.h>
#include "../math.h"

TEST(Math, Adds) {
    EXPECT_EQ(add(1, 2), 3);
}

TEST(Math, Breaks) {
    EXPECT_EQ(add(1, 2), 4);
    EXPECT_TRUE(add(2, 2) == 5) << "two and two";
}

TEST(Math, Later) {
    GTEST_SKIP() << "not yet";
}
EOF
printf 'gtest:\n\t@echo ran >>runs.log; cat "%s/gtest.txt"; exit 1\ncatch2:\n\t@cat "%s/catch2-durations.txt"; exit 1\n' "$O" "$O" >"$G/Makefile"
got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$G;frame;buildrun:make gtest;buildwait;testdump" "$G/tests/gtest_math.cpp" 2>&1 >/dev/null | grep -v '^blit: ' | tail -1)
check "GoogleTest's, from a task's output: under the task, and their marks" 'make gtest (GoogleTest): +Math.Adds xMath.Breaks -Math.Later | here: 4+ 8x 13.'
got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$G;frame;testdump;buildrun:make catch2;buildwait;testdump" "$G/tests/gtest_math.cpp" 2>&1 >/dev/null | grep -v '^blit: ' | tail -1)
check "Catch2's (each named, with --durations yes), the tab looked at first" 'make catch2 (Catch2): +adds numbers xbreaks on purpose -skipped for now'
rm -f "$G/runs.log"
"$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$G;frame;buildrun:make gtest;buildwait;testdump;command:Run the failed tests again;buildwait;builddump" "$G/tests/gtest_math.cpp" >/dev/null 2>&1
got=$(wc -l <"$G/runs.log" | tr -d ' ')
check "one of them run from here: the task again" '2'
printf 'ctest:\n\t@cat "%s/ctest.txt"; exit 1\n' "$O" >"$P/Makefile"
run "$P/check.cpp" "buildrun:make ctest;buildwait;testdump"
check "CTest's own lines: the folder's tests of those names marked (one it has not, added)" "build: +adds xbreaks$(py ' | tests/test_math.py: .test_adds .test_fails') | make ctest (CTest): -later"
run "$P/check.cpp" "testdump;buildrun:make ctest;buildwait;testdump"
check "and with the tab looked at first" "build: +adds xbreaks$(py ' | tests/test_math.py: .test_adds .test_fails') | make ctest (CTest): -later"
rm "$P/Makefile"

[ "$failed" = 0 ] && echo "testing: all passed"
exit "$failed"
