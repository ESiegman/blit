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
printf 'def test_one():\n    pass\n\n\ndef test_two():\n    pass\n' >"$V/tests/test_v.py"
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
for a in "$@"; do
    case $a in
        --collect-only) printf 'tests/test_v.py::test_one\ntests/test_v.py::test_two\n\n2 tests collected in 0.01s\n'; exit 0 ;;
        --junitxml=*) report=${a#--junitxml=} ;;
    esac
done
printf '<testsuites><testsuite><testcase classname="tests.test_v" name="test_one" time="0.1"/><testcase classname="tests.test_v" name="test_two" time="0.1"><failure message="no"/></testcase></testsuite></testsuites>' >"$report"
EOF
chmod +x "$V/.venv/bin/python"
got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$V;frame;builddump;command:Run the tests;testdump" "$V/tests/test_v.py" 2>&1 >/dev/null | tail -1)
check "pytest in the project's .venv: found and run" 'tests/test_v.py: +test_one xtest_two | here: 1+ 5x'
got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$V;frame;builddump;command:Run the tests with coverage;testdump;coveragedump" "$V/tests/test_v.py" 2>&1 >/dev/null | tail -1)
check "and coverage.py there, its report in the run configuration's environment" 'here: ran 1 2 5, missed 6 | files: tests/test_v.py=75%'
rm "$V/.venv/bin/python"
got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$V;frame;testdump" "$V/tests/test_v.py" 2>&1 >/dev/null | tail -1)
case "$got" in "No tests found: "*"No module named pytest") got="said" ;; esac
[ "$pytest" = yes ] || check "with no pytest: why none were found" 'said'
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
mkdir -p "$dir/empty"
: >"$dir/empty/a.txt"
got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$dir/empty;frame;testdump" "$dir/empty/a.txt" 2>&1 >/dev/null | tail -1)
check "none: where they would have been looked for" "No tests found: CTest's are found in a build folder that has been configured, pytest's beside a test_*.py"

[ "$failed" = 0 ] && echo "testing: all passed"
exit "$failed"
