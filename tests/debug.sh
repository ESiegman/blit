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
cp "$here/debug/threads.c" "$dir/here/" && (cd "$dir/here" && cc -g -O0 -pthread -o threads threads.c) || exit 1
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua" BLIT_RECENT_FILE="$dir/recent" BLIT_SSH="$here/fake-ssh.sh" BLIT_FAKE_HOME="$T"

failed=0
# The variables of a stop in the order of their names: gdb lists a block's
# own before the function's, lldb the other way round.
sort_vars() {
    awk 'BEGIN { RS = " ;; "; ORS = "" }
         { n = split($0, parts, "; ")
           for (p = 1; p <= n; p++)
               if (parts[p] ~ /^vars: /) {
                   m = split(substr(parts[p], 7), v, " ")
                   for (i = 2; i <= m; i++) for (j = i; j > 1 && v[j - 1] > v[j]; j--) { t = v[j]; v[j] = v[j - 1]; v[j - 1] = t }
                   parts[p] = "vars:"; for (i = 1; i <= m; i++) parts[p] = parts[p] " " v[i]
               }
           out = parts[1]; for (p = 2; p <= n; p++) out = out "; " parts[p]
           printf "%s%s", (NR > 1 ? " ;; " : ""), out }'
}
check() {
    # (The status bar's icon beside another machine is the font's, where it has one.)
    got=$(printf '%s' "$got" | sed -e 's/⇄ //g' | sort_vars)
    set -- "$1" "$(printf '%s' "$2" | sort_vars)"
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
# (gdb stops at the function's closing brace on its way out, and from there
# in the caller once the line that called it is done; lldb in the caller, in
# the middle of that line.)
back='stopped (step); stack: main@15; vars: total=0 i=1'
case "$got" in *"stack: square@9 "*)
    run "$P;break:8;key:F5;debugwait:stopped;key:F10;debugwait:stopped;key:F10;debugwait:stopped;debugdump"
    back='stopped (step); stack: main@16; vars: total=1 i=1' ;;
esac
got=$(printf '%s' "$got" | sed -e 's/ start@[0-9]*//')  # (macOS's loader's frame, whose line is its own)
check "F10 steps over (back out to the caller)" "$back"
run "$P;break:8;key:F5;debugwait:stopped;key:F5;debugwait:stopped;debugdump"
check "F5 goes on to the next stop; what the program printed is there" 'stopped (breakpoint); stack: square@8 main@15; vars: x=2 y=4; output: i=1 total=1'
run "$P;break:8;key:F5;debugwait:stopped;key:cmd+shift+y;text:x*10+y;key:Return;debugwait:stopped;debugdump"
check "an expression evaluated where it stopped" 'stopped (breakpoint); stack: square@8 main@15; vars: x=1 y=1; output: x*10+y = 11'
run "$P;break:8;key:F5;debugwait:stopped;key:F9;key:F5;debugwait:idle;debugdump"
case "$got" in "idle; output: "*"i=3 total=14"*"exited with code 0"*) got="ran to its end" ;; esac
check "F9 takes the breakpoint away; F5 runs it to its end" "ran to its end"
run "$P;break:16;key:F5;debugwait:stopped;key:shift+F5;debugwait:idle;debugdump"
check "Shift+F5 stops it" 'idle'
run "debug:program=$dir/here/no-such;key:F5;debugwait:idle;debugdump"
case "$got" in "idle error: cannot launch it"*) got="it says why" ;; esac
check "a program that is not there" "it says why"

# Breakpoints that wait for something: Shift+F9 asks for a condition, then
# a hit count (each left empty: none).
run "$P;breakif:8;text:x == 2;key:Return;key:Return;key:F5;debugwait:stopped;debugdump"
check "a condition: it stops only where it holds" 'stopped (breakpoint); stack: square@8 main@15; vars: x=2 y=4; output: i=1 total=1'
run "$P;breakif:8;key:Return;text:3;key:Return;debugbreaks;key:F5;debugwait:stopped;debugdump"
check "a hit count: it stops the third time there" 'count.c:8 hit 3 ;; stopped (breakpoint); stack: square@8 main@15; vars: x=3 y=9; output: i=1 total=1 | i=2 total=5'
run "$P;break:8;break:16;debugpanel;debugpick:breaks:2;debugpanel;key:shift+F9;text:i == 2;key:Return;key:Return;debugpick:breaks:1;debugpanel;key:Delete;debugbreaks;key:F5;debugwait:stopped;debugdump"
got=$(printf '%s' "$got" | sed -e 's/main@16 [^;]*;/main@16;/')
check "and from the Debug tab, the one chosen there" 'count.c:16 if i == 2 ;; stopped (breakpoint); stack: main@16; vars: total=5 i=2; output: i=1 total=1'

# Expressions watched in the Debug tab, evaluated at each stop; changed, and taken away.
W="debugpanel;debugpick:add;text:x * 100;key:Return;debugpick:add;text:nope;key:Return"
run "$P;break:8;$W;key:F5;debugwait:stopped;key:F5;debugwait:stopped;debugdump"
got=$(printf '%s' "$got" | sed -e 's/nope ([^)]*)/nope (why)/')
check "expressions watched, at each stop (and why one cannot be)" 'stopped (breakpoint); stack: square@8 main@15; vars: x=2 y=4; watch: x * 100 = 200 | nope (why); output: i=1 total=1'
bs="key:Backspace;key:Backspace;key:Backspace;key:Backspace;key:Backspace;key:Backspace;key:Backspace"
run "$P;break:8;$W;key:F5;debugwait:stopped;debugpick:watch:1;$bs;text:y + 1;key:Return;debugpick:watch:2;key:Escape;debugpanel;key:Delete;debugwait:stopped;debugdump"
check "one changed, one taken away" 'stopped (breakpoint); stack: square@8 main@15; vars: x=1 y=1; watch: y + 1 = 2'

# Every thread: the stack of each, chosen from the list in the Debug tab.
run "debug:program=$dir/here/threads;break:22;key:F5;debugwait:stopped;debugdump;debugpanel;debugpick:threads:2;debugwait:stopped;debugdump" "$dir/here/threads.c"
got=$(printf '%s' "$got" | sed -e 's/main@22 [^;]*;/main@22;/' -e 's/t=0x[0-9a-f]*/t=0x/' -e 's/t=[1-9][0-9]*/t=0x/')  # (what called main: the system's; where the thread is, in hex or, from gdb, a number)
check "the threads, and the second one's stack chosen" \
    'stopped (breakpoint); threads: 2, #1 shown; stack: main@22; vars: t=0x ;; stopped (breakpoint); threads: 2, #2 shown; stack: wait_here@9 worker@13; vars:'

# Breakpoints move with their lines as the file is edited (as marks do):
# a line put above takes them down, lines taken away above take them up,
# one whose line goes stays where the line was; undone, they go back.
# (break:1 twice: the cursor to the first line, and no breakpoint there.)
run "break:8;break:15;break:1;break:1;key:Return;debugbreaks;key:Down;key:Down;key:shift+Down;key:shift+Down;key:Backspace;debugbreaks;key:cmd+z;key:cmd+z;debugbreaks"
check "breakpoints follow their lines" 'count.c:9; count.c:16 ;; count.c:7; count.c:14 ;; count.c:8; count.c:15'
run "lua:blit.opt.editor.modal = true;break:8;break:15;vim:ggO<Esc>;debugbreaks;vim:9Gdd;debugbreaks;vim:dk;debugbreaks"
check "with Vim's keys too" 'count.c:9; count.c:16 ;; count.c:9; count.c:15 ;; count.c:8; count.c:13'

# On another machine: the debugger runs there, its source opens from there.
run "remotewait;debug:program=ssh://box$T/count;break:8;key:F5;debugwait:stopped;debugdump;frame;statusdump" "ssh://box$T/count.c"
check "on another machine" 'stopped (breakpoint); stack: square@8 main@15; vars: x=1 y=1 ;; box count.c | Ln 8, Col 1 Spaces: 4 LF C'
run "remotewait;debug:program=ssh://box$T/count;breakif:8;key:Return;text:2;key:Return;debugpanel;debugpick:add;text:y - x;key:Return;key:F5;debugwait:stopped;debugdump" "ssh://box$T/count.c"
check "a hit count and a watch, there" 'stopped (breakpoint); stack: square@8 main@15; vars: x=2 y=4; watch: y - x = 2; output: i=1 total=1'

# On a slow one (each answer held a while): F5 goes back to the keys at once
# and the tab says it is starting (the debugger is started there on a thread
# of its own), and the session works once it is up. Stopping it, while it
# starts (Shift+F5, or F5 again) or once it has stopped, waits for nothing.
# slow SCRIPT [FILE]: as run, on a host that holds each answer SLOW seconds (0.2).
SLOW=
slow() {
    got=$(BLIT_FAKE_SLOW=${SLOW:-0.2} "$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "$1" "${2:-$dir/here/count.c}" 2>&1 >/dev/null |
        sed -e "s#$dir#@#g" | awk 'NR > 1 { printf " ;; " } { printf "%s", $0 }')
    SLOW=
}
slow "remotewait;debug:program=ssh://box$T/count;break:8;clock;key:F5;within:300;debugdump;debugwait:stopped;debugdump;clock;key:shift+F5;within:300;debugdump" "ssh://box$T/count.c"
check "on a slow host: F5 at once, and the session once it is up; Shift+F5 at once" \
    'within 300 ms ;; starting ;; stopped (breakpoint); stack: square@8 main@15; vars: x=1 y=1 ;; within 300 ms ;; idle'
slow "remotewait;debug:program=ssh://box$T/count;key:F5;clock;key:shift+F5;within:300;debugdump;key:F5;clock;key:F5;within:300;debugdump;key:F5;debugwait:idle;debugdump" "ssh://box$T/count.c"
case "$got" in *"[exited with code 0]") got="${got%%; output: *}; ran to its end" ;; esac
check "stopped while it starts, by Shift+F5 or F5 again: at once (and it starts again after)" \
    'within 300 ms ;; idle ;; within 300 ms ;; idle ;; idle; ran to its end'

[ -n "$print" ] || [ $failed = 1 ] || echo "debug ok"
exit $failed
