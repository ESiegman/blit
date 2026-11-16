#!/bin/sh
# Debugging, end to end: tests/debug/count.c built with -g, breakpoints,
# stepping, continuing, the program's output, an expression evaluated,
# stopping; here, and on "another machine" through tests/fake-ssh.sh.
# Skipped where there is no debugger (lldb-dap on a Mac, gdb 14 or later
# or lldb-dap elsewhere).
#
#   tests/debug.sh path/to/blit            run the cases
#   tests/debug.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/debug.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
if [ "$(uname -s)" = Darwin ]; then
    xcrun --find lldb-dap >/dev/null 2>&1 || [ -x /Applications/Xcode.app/Contents/Developer/usr/bin/lldb-dap ] || { echo "debug skipped: no lldb-dap"; exit 0; }
else
    command -v gdb >/dev/null 2>&1 || command -v lldb-dap >/dev/null 2>&1 || { echo "debug skipped: no debugger"; exit 0; }
fi
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
T="$dir/there"
mkdir -p "$dir/here" "$T"
for d in "$dir/here" "$T"; do
    cp "$here/debug/count.c" "$d/" && (cd "$d" && cc -g -O0 -o count count.c) || exit 1
done
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua" BLIT_RECENT_FILE="$dir/recent" BLIT_SSH="$here/fake-ssh.sh" BLIT_FAKE_HOME="$T"

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run SCRIPT [FILE]: the lines it printed, joined by " ;; ".
run() {
    got=$("$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "$1" "${2:-$dir/here/count.c}" 2>&1 >/dev/null | sed -e "s#$dir#@#g" |
        awk 'NR > 1 { printf " ;; " } { printf "%s", $0 }')
}
P="debug:program=$dir/here/count"

run "$P;break:8;key:F5;debugwait:stopped;debugdump;frame;statusdump"
check "a breakpoint: stopped there, the stack and the variables, and its line in front" \
    'stopped (breakpoint); stack: square@8 main@15; vars: x=1 y=1 ;; count.c | Ln 8, Col 1 Spaces: 4 LF C'
run "$P;break:8;key:F5;debugwait:stopped;key:F10;debugwait:stopped;debugdump"
got=$(printf '%s' "$got" | sed -e 's/ start@[0-9]*//')  # (macOS's loader's frame, whose line is its own)
check "F10 steps over (back out to the caller)" 'stopped (step); stack: main@15; vars: total=0 i=1'
run "$P;break:8;key:F5;debugwait:stopped;key:F5;debugwait:stopped;debugdump"
check "F5 goes on to the next stop; what the program printed is there" 'stopped (breakpoint); stack: square@8 main@15; vars: x=2 y=4; output: i=1 total=1'
run "$P;break:8;key:F5;debugwait:stopped;key:cmd+shift+y;text:x*10+y;key:Return;debugwait:stopped;debugdump"
check "an expression evaluated where it stopped" 'stopped (breakpoint); stack: square@8 main@15; vars: x=1 y=1; output: x*10+y = 11'
run "$P;break:8;key:F5;debugwait:stopped;key:F9;key:F5;debugwait:idle;debugdump"
case "$got" in "idle; output: "*"i=3 total=14"*"exited with 0"*) got="ran to its end" ;; esac
check "F9 takes the breakpoint away; F5 runs it to its end" "ran to its end"
run "$P;break:16;key:F5;debugwait:stopped;key:shift+F5;debugwait:idle;debugdump"
check "Shift+F5 stops it" 'idle'
run "debug:program=$dir/here/no-such;key:F5;debugwait:idle;debugdump"
case "$got" in "idle error: cannot launch it"*) got="it says why" ;; esac
check "a program that is not there" "it says why"

# On another machine: the debugger runs there, its source opens from there.
run "remotewait;debug:program=ssh://box$T/count;break:8;key:F5;debugwait:stopped;debugdump;frame;statusdump" "ssh://box$T/count.c"
check "on another machine" 'stopped (breakpoint); stack: square@8 main@15; vars: x=1 y=1 ;; box count.c | Ln 8, Col 1 Spaces: 4 LF C'

[ -n "$print" ] || [ $failed = 1 ] || echo "debug ok"
exit $failed
