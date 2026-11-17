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
mkdir -p "$dir/empty"
: >"$dir/empty/a.txt"
got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "workdir:$dir/empty;frame;testdump" "$dir/empty/a.txt" 2>&1 >/dev/null | tail -1)
check "none: where they would have been looked for" "No tests found: CTest's are found in a build folder that has been configured, pytest's beside a test_*.py"

[ "$failed" = 0 ] && echo "testing: all passed"
exit "$failed"
