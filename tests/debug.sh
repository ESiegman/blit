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
    # (The status bar's icon beside another machine is the font's, where it has
    # one; and it says "no colours" where the C grammar is not installed.)
    got=$(printf '%s' "$got" | sed -e 's/⇄ //g' -e 's/ (no colours)//g' | sort_vars)
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
got=$(printf '%s' "$got" | sed -e 's/ start\(@[0-9]*\)\{0,1\}$//' -e 's/ start\(@[0-9]*\)\{0,1\};/;/')  # (macOS's loader's frame, whose line is its own)
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

# Breakpoints that wait for something: Shift+F9 asks for a condition, a
# hit count and a message to log (each left empty: none).
run "$P;breakif:8;text:x == 2;key:Return;key:Return;key:Return;key:F5;debugwait:stopped;debugdump"
check "a condition: it stops only where it holds" 'stopped (breakpoint); stack: square@8 main@15; vars: x=2 y=4; output: i=1 total=1'
run "$P;breakif:8;key:Return;text:3;key:Return;key:Return;debugbreaks;key:F5;debugwait:stopped;debugdump"
check "a hit count: it stops the third time there" 'count.c:8 hit 3 ;; stopped (breakpoint); stack: square@8 main@15; vars: x=3 y=9; output: i=1 total=1 | i=2 total=5'
run "$P;break:8;break:16;debugpanel;debugpick:breaks:2;debugpanel;key:shift+F9;text:i == 2;key:Return;key:Return;key:Return;debugpick:breaks:1;debugpanel;key:Delete;debugbreaks;key:F5;debugwait:stopped;debugdump"
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
got=$(printf '%s' "$got" | sed -e 's/main@22 [^;]*;/main@22;/' -e 's/t=0x[0-9a-f]* {[^}]*}/t=0x/' -e 's/t=0x[0-9a-f]*/t=0x/' -e 's/t=[1-9][0-9]*/t=0x/')  # (what called main: the system's; where the thread is, in hex (lldb: and what it points at summed up) or, from gdb, a number)
check "the threads, and the second one's stack chosen" \
    'stopped (breakpoint); threads: 2, #1 shown; stack: main@22; vars: t=0x ;; stopped (breakpoint); threads: 2, #2 shown; stack: wait_here@9 worker@13; vars:'

# A click in the gutter's first cell (where the dot goes) sets one, and takes it away; it selects nothing.
run "frame;breakclick:8;breakclick:15;frame;debugbreaks;breakclick:8;debugbreaks;seldump"
check "a click beside a line's number" 'count.c:8; count.c:15 ;; count.c:15 ;; none'

# Breakpoints move with their lines as the file is edited (as marks do):
# a line put above takes them down, lines taken away above take them up,
# one whose line goes stays where the line was; undone, they go back.
# (break:1 twice: the cursor to the first line, and no breakpoint there.)
run "break:8;break:15;break:1;break:1;key:Return;debugbreaks;key:Down;key:Down;key:shift+Down;key:shift+Down;key:Backspace;debugbreaks;key:cmd+z;key:cmd+z;debugbreaks"
check "breakpoints follow their lines" 'count.c:9; count.c:16 ;; count.c:7; count.c:14 ;; count.c:8; count.c:15'
run "lua:blit.opt.editor.modal = true;break:8;break:15;vim:ggO<Esc>;debugbreaks;vim:9Gdd;debugbreaks;vim:dk;debugbreaks"
check "with Vim's keys too" 'count.c:9; count.c:16 ;; count.c:9; count.c:15 ;; count.c:8; count.c:13'

# What the debugger says of them: one on a line with no code it puts on the
# next line that has some (it is on that line from now, the move said);
# one in a library not loaded yet it has not verified until the library is
# loaded (said in a breakpoint event).
run "$P;break:10;key:F5;debugwait:stopped;debugbreaks;debugdump"
got=$(printf '%s' "$got" | sed -e 's/; vars: .*//' -e 's/ start\(@[0-9]*\)\{0,1\}$//')
case "$got" in  # (lldb puts it on line 13; gdb, the next line it has code for, whichever that is)
"count.c:"*" moved from 10 ;; stopped (breakpoint); stack: main@"*)
    to=${got#count.c:}; to=${to%% *}
    [ "$got" = "count.c:$to moved from 10 ;; stopped (breakpoint); stack: main@$to" ] && [ "$to" -gt 10 ] && got="moved on, and stopped there" ;;
esac
check "a breakpoint on a line with no code: moved to one with some, and stopped there" "moved on, and stopped there"
cp "$here/debug/loads.c" "$here/debug/twice.c" "$dir/here/" && (cd "$dir/here" && cc -g -O0 -shared -fPIC -o loads.so twice.c && cc -g -O0 -o loads loads.c $([ "$(uname -s)" = Darwin ] || echo -ldl)) || exit 1
run "debug:program=$dir/here/loads;break:4;open:$dir/here/loads.c;break:14;key:F5;debugwait:stopped;debugbreaks;key:F5;debugwait:stopped;debugbreaks;debugdump" "$dir/here/twice.c"
got=$(printf '%s' "$got" | sed -e 's/unverified ([^)]*)/unverified/' -e 's/; vars: .*//' -e 's/ start\(@[0-9]*\)\{0,1\}$//')  # (gdb says why: it is "pending")
check "a breakpoint in a library not loaded yet: not verified until it is" \
    'loads.c:14; twice.c:4 unverified ;; loads.c:14; twice.c:4 ;; stopped (breakpoint); stack: twice@4 main@16'

# On another machine: the debugger runs there, its source opens from there.
run "remotewait;debug:program=ssh://box$T/count;break:8;key:F5;debugwait:stopped;debugdump;frame;statusdump" "ssh://box$T/count.c"
check "on another machine" 'stopped (breakpoint); stack: square@8 main@15; vars: x=1 y=1 ;; box count.c | Ln 8, Col 1 Spaces: 4 LF C'
run "remotewait;debug:program=ssh://box$T/count;breakif:8;key:Return;text:2;key:Return;key:Return;debugpanel;debugpick:add;text:y - x;key:Return;key:F5;debugwait:stopped;debugdump" "ssh://box$T/count.c"
check "a hit count and a watch, there" 'stopped (breakpoint); stack: square@8 main@15; vars: x=2 y=4; watch: y - x = 2; output: i=1 total=1'
# A program there that writes files: once the session is over, the tree
# lists the one it made and the open one it wrote is read again (nothing
# watches the disk there).
mkdir -p "$T/w" "$T/wbin" && printf 'one\n' >"$T/w/notes.txt"
printf '#include <stdio.h>\nint main(void) {\n    FILE* f = fopen(OUT "/made.txt", "w");\n    fclose(f);\n    f = fopen(OUT "/notes.txt", "a");\n    fputs("two\\n", f);\n    fclose(f);\n    return 0;\n}\n' >"$T/wbin/writes.c"
(cd "$T/wbin" && cc -g -O0 -DOUT="\"$T/w\"" -o writes writes.c) || exit 1
run "remotewait;key:cmd+shift+E;treewait;frame;treedump;debug:program=ssh://box$T/wbin/writes;key:F5;debugwait:idle;ranwait;remotewait;treewait;frame;treedump;lsptext;statusdump" "ssh://box$T/w/notes.txt"
check "a session there that ends: the tree and the open file there looked at again" \
    'tree: notes.txt@42 ;; tree: made.txt@42 notes.txt@57 ;; one / two /  ;; box notes.txt | Ln 1, Col 1 Tabs LF Plain text'

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

# Logpoints (a message said where it would stop, its {x} filled in) and
# breakpoints by a function's name.
down7="key:Down;key:Down;key:Down;key:Down;key:Down;key:Down;key:Down"
run "$P;$down7;key:shift+F9;frame;key:Return;frame;key:Return;frame;text:x is {x};key:Return;debugbreaks;key:F5;debugwait:idle;debugdump"
got=$(printf '%s' "$got" | sed -e 's/;; idle; output: .*\(x is 3 | i=3 total=14\) .*/;; \1/')
check "a logpoint: its message with the value, and no stop" 'count.c:8 log x is {x} ;; x is 3 | i=3 total=14'
run "$P;command:Break at a function…;frame;text:square;key:Return;debugbreaks;key:F5;debugwait:stopped;debugdump"
got=$(printf '%s' "$got" | sed -e 's/; vars: .*//')
check "a breakpoint by a function's name: it stops where the function starts" 'square() ;; stopped (breakpoint); stack: square@7 main@15'

# A variable set where it stopped, a variable's writes watched, running to the
# cursor (the breakpoints as they were after), and jumping to it where the
# debugger can.
run "$P;break:7;key:F5;debugwait:stopped;debugpanel;debugpick:vars:2;key:F2;key:ctrl+U;text:5;key:Return;debugwait:stopped;key:F10;debugwait:stopped;debugdump"
check "a variable set: the program goes on with its new value" 'stopped (step); stack: square@8 main@15; vars: x=5 y=25'
run "$P;break:15;key:F5;debugwait:stopped;debugpanel;debugpick:vars:2;key:w;debugwait:answers;debugbreaks;key:F5;debugwait:stopped;debugdump"
got=$(printf '%s' "$got" | sed -e 's/stopped ([^)]*)/stopped/' -e 's/ start\(@[0-9]*\)\{0,1\};/;/' -e 's/; output: .*//')
case "$got" in
*"cannot watch"*) check "a variable's writes: this debugger cannot watch them, and it says so" "blit: This debugger cannot watch a variable's writes ;; count.c:15 ;; stopped; stack: main@15; vars: i=2 total=1" ;;
*) check "a variable's writes watched: it stops where it is written" 'count.c:15; writes to total ;; stopped; stack: main@16; vars: total=1 i=1' ;;
esac
run "$P;break:13;key:F5;debugwait:stopped;key:cmd+P;text::16;key:Return;key:ctrl+F10;debugwait:stopped;debugdump;debugbreaks"
got=$(printf '%s' "$got" | sed -e 's/stopped ([^)]*)/stopped/' -e 's/ start\(@[0-9]*\)\{0,1\};/;/' -e 's/; output: [^;]*;;/ ;;/')
check "run to the cursor: it stops there, and the breakpoints are as they were" 'stopped; stack: main@16; vars: total=1 i=1 ;; count.c:13'
run "$P;break:13;key:F5;debugwait:stopped;key:cmd+P;text::18;key:Return;command:Jump to the cursor's line;debugwait:stopped;debugdump"
got=$(printf '%s' "$got" | sed -e 's/stopped ([^)]*)/stopped/' -e 's/ start\(@[0-9]*\)\{0,1\};/;/' -e 's/; vars: .*//')
case "$got" in
*"cannot jump"*) check "a jump: this debugger cannot, and it says so" 'blit: This debugger cannot jump ;; stopped; stack: main@13' ;;
*) check "a jump to the cursor's line: it is there, the lines between not run" 'stopped; stack: main@18' ;;
esac

# The disassembly (the instructions are the machine's own: what is checked is
# the heading of the line they are of, and the addresses): the cursor on the
# instruction it stopped at, a step there an instruction's, and a breakpoint
# at an instruction (the line's own taken away) where it stops the next time.
heading_of() { printf '%s' "$1" | awk 'BEGIN { RS = " ;; " } /^>/ { print h; exit } /^ *count\.c:/ { h = $0 }' | sed 's/^ *//'; }
run "$P;break:7;key:F5;debugwait:stopped;command:Show the disassembly;debugwait:answers;docdump:8;key:F10;debugwait:stopped;debugwait:answers;docdump:0;debugdump"
first=$(printf '%s' "$got" | awk 'BEGIN { RS = " ;; " } /^>/ { print; exit }' | sed -n 's/^> |* *\(0x[0-9a-f]*\) .*/\1/p')
second=$(printf '%s' "$got" | awk 'BEGIN { RS = " ;; " } /^>/ { n++ } n == 2 && /^>/ { print; exit }' | sed -n 's/^> |* *\(0x[0-9a-f]*\) .*/\1/p')
heading=$(heading_of "$got")
stop=$(printf '%s' "$got" | sed -e 's/.* ;; stopped/stopped/' -e 's/; vars: .*//')
got="$heading ;; $([ -n "$first" ] && [ -n "$second" ] && [ $((second)) -gt $((first)) ] && echo "a step on" || echo "first $first, then $second") ;; $stop"
check "the disassembly where it stopped, and a step of one instruction" 'count.c:7  int y = x * x; ;; a step on ;; stopped (step); stack: square@7 main@15'
run "$P;break:7;key:F5;debugwait:stopped;command:Show the disassembly;debugwait:answers;key:F9;debugpanel;debugpick:breaks:1;debugpanel;key:Delete;debugbreaks;key:F5;debugwait:stopped;debugdump"
got=$(printf '%s' "$got" | sed -e 's/at 0x[0-9a-f]*/at ADDRESS/' -e 's/stopped ([^)]*)/stopped/' -e 's/ y=[-0-9]*//')
check "a breakpoint at an instruction" 'at ADDRESS ;; stopped; stack: square@7 main@15; vars: x=2; output: i=1 total=1'

# Memory: where an expression points, the cursor on its byte; bytes written
# there (the program goes on with them); and a variable's own (M in the Debug
# tab), read again at the next stop with what changed.
run "$P;break:16;key:F5;debugwait:stopped;command:Show memory at…;text:&total;key:Return;debugwait:answers;docdump:0;command:Write to memory at the cursor…;text:2a 00;key:Return;debugwait:answers;key:F5;debugwait:stopped;debugdump"
# (Where total is: its four bytes, after the cursor.)
total_bytes() { printf '%s' "$1" | awk 'BEGIN { RS = " ;; " } /^>/ { print }' | sed -n 's/.*|\([0-9a-f?]*\) \([0-9a-f?]*\) \([0-9a-f?]*\) \([0-9a-f?]*\).*/\1 \2 \3 \4/p' | tr '\n' ';'; }
got="$(total_bytes "$got") ;; $(printf '%s' "$got" | sed -e 's/.* ;; stopped/stopped/' -e 's/ start\(@[0-9]*\)\{0,1\};/;/')"
check "memory where an expression points: written, and the program goes on with it" '01 00 00 00; ;; stopped (breakpoint); stack: main@16; vars: i=2 total=46; output: i=1 total=42'
run "$P;break:15;key:F5;debugwait:stopped;debugpanel;debugpick:vars:total;key:m;debugwait:answers;docdump:0;key:F10;debugwait:stopped;debugwait:answers;key:ctrl+Tab;docdump:0;command:Previous page of memory;command:Next page of memory;debugwait:answers;docdump:0"
tops=$(printf '%s' "$got" | awk 'BEGIN { RS = " ;; " } /^>/ { print }' | sed -n 's/^> |*\(0x[0-9a-f]*\) .*/\1/p' | sort -u | wc -l | tr -d ' ')
got="$(total_bytes "$got") ;; $tops"
check "a variable's memory, read again at the next stop (and a page back and on, to where it was)" '00 00 00 00;01 00 00 00; ;; 1'

# The values of the variables each line names, after it, from the start of the
# function it stopped in to where it stopped (not those in the string); and
# with debug.inline_values off, where it stopped alone.
run "$P;break:16;key:F5;debugwait:stopped;key:F5;debugwait:stopped;frame;debuginline;lua:blit.opt.debug.inline_values = false;frame;debuginline"
check "the values in the code" '13: total = 5 | 14: i = 2 | 15: total = 5, i = 2 | 16: ◀ stopped: breakpoint   i = 2, total = 5 ;; 16: ◀ stopped: breakpoint'

# Two sessions at once (the breakpoints are both's): each stopped, one run
# on, the other chosen in the Debug tab as it was left; one stopped, the
# other takes its place. And what is typed to evaluate completed with Tab.
run "$P;break:8;key:F5;debugwait:stopped;command:Start another debugging session;debugwait:stopped;debugdump;key:F5;debugwait:stopped;debugdump;debugpanel;debugpick:sessions:2;debugwait:stopped;debugdump;key:shift+F5;debugwait:stopped;debugdump"
got=$(printf '%s' "$got" | sed -e 's/; output: [^;]* ;;/ ;;/g' -e 's/; output: [^;]*$//' -e 's/ start\(@[0-9]*\)\{0,1\};/;/g')
check "two sessions at once" 'sessions: *count stopped, count stopped; stopped (breakpoint); stack: square@8 main@15; vars: x=1 y=1 ;; sessions: *count stopped, count stopped; stopped (breakpoint); stack: square@8 main@15; vars: x=2 y=4 ;; sessions: *count stopped, count stopped; stopped (breakpoint); stack: square@8 main@15; vars: x=1 y=1 ;; stopped (breakpoint); stack: square@8 main@15; vars: x=2 y=4'
run "$P;break:16;key:F5;debugwait:stopped;key:cmd+shift+y;text:tot;key:Tab;debugwait:answers;finddump;key:Return;debugwait:stopped;debugdump"
got=$(printf '%s' "$got" | sed -e 's/ start\(@[0-9]*\)\{0,1\};/;/g')
case "$got" in
'input [tot]'*)  # (gdb has no completions: what is typed is evaluated as it stands, and gdb says why it cannot be)
    got=$(printf '%s' "$got" | sed -e 's/output: tot = (.*)$/output: tot = (why not)/')
    check "evaluate: this debugger completes nothing (as typed, evaluated)" 'input [tot] 0 : ;; stopped (breakpoint); stack: main@16; vars: total=1 i=1; output: tot = (why not)' ;;
*) check "evaluate: what is typed completed with Tab, then evaluated" 'input [total] 0 : ;; stopped (breakpoint); stack: main@16; vars: total=1 i=1; output: total = 1' ;;
esac

# The debug console (Cmd+Option+Y) and exceptions, with lldb-dap (gdb's
# answers differ: its command line takes its own commands, and its
# exception filters are its own; left to check where gdb is).
if [ "$(uname -s)" = Darwin ]; then
    # An expression, one of lldb's commands, a monitor line (to the gdb
    # server, as lldb's `process plugin packet monitor`: macOS's debugserver
    # has no monitor commands, and says E69), what the program printed, and
    # a command completed with Tab.
    run "$P;break:8;key:F5;debugwait:stopped;key:cmd+alt+y;text:x*10+y;key:Return;debugwait:answers;text:frame variable;key:Return;debugwait:answers;text:monitor help;key:Return;debugwait:answers;key:F5;debugwait:stopped;text:fr;key:Tab;debugwait:answers;docdump:12"
    check "the debug console: an expression, a command, a monitor line, the program's output, Tab" \
        'Debug console ;;   › x*10+y ;;   (int) $0 = 11 ;;   › frame variable ;;   (int) x = 1 ;;   (int) y = 1 ;;   › monitor help ;;     packet: qRcmd,68656c70 ;;   response: E69 ;;   i=1 total=1 ;; > › frame|'
    # lldb-dap's exception filters in the Debug tab: with none on, an
    # uncaught C++ exception stops where it ends the program (the abort);
    # with C++ Throw on, where it is thrown, which says so after its line.
    cp "$here/debug/throws.cpp" "$dir/here/" && (cd "$dir/here" && c++ -g -O0 -o throws throws.cpp) || exit 1
    run "debug:program=$dir/here/throws;key:F5;debugwait:stopped;debugdump;debugpanel;debugpick:exceptions:2;key:shift+F5;debugwait:idle;debugbreaks;key:F5;debugwait:stopped;debugdump;frame;debuginline" "$dir/here/throws.cpp"
    got=$(printf '%s' "$got" | sed -e 's/exception = 0x[0-9a-f]*/exception = 0x/' -e 's/; stack: [^;]*pthread_kill[^;]*;/; stack: abort;/' -e 's/; output: [^;]*;;/ ;;/')
    check "exceptions: uncaught, stopped at the abort; C++ Throw chosen, at the throw" \
        'stopped (exception); exception: signal SIGABRT | (void *) exception = 0x; stack: abort; vars: n=3 ;; exception cpp_throw ;; stopped (exception); exception: C++ Throw; stack: __cxa_throw check(int)@5; vars: n=3 ;; 4: n = 3 | 5: ◀ exception: C++ Throw   n = 3'
fi

# The standard library's containers, as the debugger's printers show them:
# their sizes, and a map's entries (lldb: each pair summed up from its
# members; gdb: its printer's keys and values put together, [key] = value).
cp "$here/debug/containers.cpp" "$dir/here/" && (cd "$dir/here" && c++ -g -O0 -std=c++20 -o containers containers.cpp) || exit 1
run "debug:program=$dir/here/containers;break:10;key:F5;debugwait:stopped;debugpanel;debugpick:vars:m;debugwait:stopped;debugdump" "$dir/here/containers.cpp"
got=$(printf '%s' "$got" | sed -e 's/; stack: [^;]*;/;/' -e 's/std::vector of length 3, capacity 3/size=3/' -e 's/std::map with 2 elements/size=2/' -e 's/, second/,second/g')
case "$got" in
*'["a"]'*) check "containers: sizes, and a map's entries as [key] = value (gdb)" 'stopped (breakpoint); vars: m=size=2 ["a"]=1 ["b"]=2 s="hello" v=size=3' ;;
*) check "containers: sizes, and a map's entries summed up (lldb)" 'stopped (breakpoint); vars: m=size=2 [0]={first:"a",second:1} [1]={first:"b",second:2} s="hello" v=size=3' ;;
esac

# A HIP program, where ROCm is (its hipcc and rocgdb, and an AMD GPU): rocgdb
# chosen for it, a breakpoint in the kernel stopping a GPU wave, the waves
# named in the thread list, the kernel's frame and variables.
hipcc=$(command -v hipcc 2>/dev/null || { [ -x /opt/rocm/bin/hipcc ] && echo /opt/rocm/bin/hipcc; })
if [ "$(uname -s)" != Darwin ] && [ -n "$hipcc" ] && { command -v rocgdb >/dev/null 2>&1 || [ -x /opt/rocm/bin/rocgdb ]; } && [ -e /dev/kfd ]; then
    cp "$here/debug/vadd.hip" "$dir/here/" && (cd "$dir/here" && "$hipcc" -g -O0 -o vadd vadd.hip 2>/dev/null) || exit 1
    run "debug:program=$dir/here/vadd;break:9;key:F5;debugwait:stopped;debugdump" "$dir/here/vadd.hip"
    got=$(printf '%s' "$got" | sed -e 's/threads: [0-9]*, #[0-9]* shown ([0-9]* GPU waves; shown: \(GPU wave ([0-9,]*)\)\/[0-9]*)/\1/' -e 's/; vars: .*//')
    check "HIP: rocgdb, a breakpoint in the kernel: a GPU wave stopped there" 'stopped (breakpoint); GPU wave (0,0,0); stack: vadd@9'
fi

# Attaching to a process that is running, and a core file.
cp "$here/debug/waits.c" "$here/debug/crash.c" "$dir/here/" && (cd "$dir/here" && cc -g -O0 -o waits waits.c && cc -g -O0 -o crash crash.c) || exit 1
# (Not where Linux lets a debugger attach only to what it started itself:
# Yama's ptrace_scope above 0, unless the tests are root.)
scope=$(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null || echo 0)
if [ "$scope" = 0 ] || [ "$(id -u)" = 0 ]; then
    "$dir/here/waits" &
    waiting=$!
    sleep 0.3
    run "break:13;debugattach:$waiting;debugwait:stopped;debugdump" "$dir/here/waits.c"
    kill "$waiting" 2>/dev/null
    wait "$waiting" 2>/dev/null
    got=$(printf '%s' "$got" | sed -e 's/rounds=[0-9]*/rounds=N/' -e 's/ start\(@[0-9]*\)\{0,1\};/;/')
    check "attached to a process: it stops at a breakpoint as one launched would" 'stopped (breakpoint); stack: main@13; vars: rounds=N'
    # And one on another machine, from a folder there: the breakpoint set
    # there by its path there, and the frame's source opened from there.
    cp "$here/debug/waits.c" "$T/" && (cd "$T" && cc -g -O0 -o waits waits.c) || exit 1
    "$T/waits" &
    waiting=$!
    sleep 0.3
    run "remotewait;break:13;debugattach:$waiting;debugwait:stopped;debugdump;frame;statusdump;tabsdump" "ssh://box$T/waits.c"
    kill "$waiting" 2>/dev/null
    wait "$waiting" 2>/dev/null
    got=$(printf '%s' "$got" | sed -e 's/rounds=[0-9]*/rounds=N/' -e 's/ start\(@[0-9]*\)\{0,1\};/;/')
    check "attached on another machine: the same" 'stopped (breakpoint); stack: main@13; vars: rounds=N ;; box waits.c | Ln 13, Col 1 Spaces: 4 LF C ;; waits.c'
    "$dir/here/waits" &
    waiting=$!
    sleep 0.3
    run "break:13;debugattach:$waiting;debugwait:stopped;key:shift+F5;debugwait:idle;debugdump" "$dir/here/waits.c"
    sleep 0.5
    case "$(ps -o stat= -p "$waiting" 2>/dev/null)" in
        "") got="$got ;; it was ended" ;;
        T*) got="$got ;; it was left stopped" ;;
        *) got="$got ;; it goes on" ;;
    esac
    kill "$waiting" 2>/dev/null
    wait "$waiting" 2>/dev/null
    check "Shift+F5 lets a process attached to go on" 'idle ;; it goes on'
fi
# (A core file as the debugger writes it: lldb's save-core, gdb's generate-core-file.)
core="$dir/here/crash.core"
# (Elsewhere than a Mac, gdb's core where there is a gdb: lldb-dap, which blit uses first where it is, reads it too, and lldb's own are minidumps gdb cannot read.)
if [ "$(uname -s)" = Darwin ]; then
    (cd "$dir/here" && lldb -b -o run -k "process save-core $core" -k quit ./crash >/dev/null 2>&1)
elif command -v gdb >/dev/null 2>&1; then
    (cd "$dir/here" && gdb -batch -ex run -ex "generate-core-file $core" ./crash >/dev/null 2>&1)
else
    (cd "$dir/here" && lldb -b -o run -k "process save-core $core" -k quit ./crash >/dev/null 2>&1)
fi
if [ -s "$core" ]; then
    run "debugcore:$dir/here/crash:$core;debugwait:stopped;debugdump" "$dir/here/crash.c"
    got=$(printf '%s' "$got" | sed -e 's/; stack: [^;]*;/;/' -e 's/^stopped ([^)]*)/stopped/')  # (the library frames above the program's: the system's own; gdb's word for why)
    check "a core file: where it stopped, the program's own frame shown with its variables" 'stopped; vars: depth=3 here=21'
fi

# Restart (Cmd+Shift+F5): the program again from its start, the session's
# breakpoints kept (lldb-dap restarts it in the same session, as it says
# it can once it has launched; gdb's session is ended and begun again).
run "frame;$P;break:8;key:F5;debugwait:stopped;key:F5;debugwait:stopped;debugdump;key:cmd+shift+F5;debugwait:stopped;debugdump;key:F5;debugwait:stopped;debugdump"
got=$(printf '%s' "$got" | sed -E -e 's/; output: [^;]*( ;;|$)/\1/g')
check "restart: from the start again, its breakpoints kept" \
    'stopped (breakpoint); stack: square@8 main@15; vars: x=2 y=4 ;; stopped (breakpoint); stack: square@8 main@15; vars: x=1 y=1 ;; stopped (breakpoint); stack: square@8 main@15; vars: x=2 y=4'

# A program that reads what is typed to it (C's stdin, here) is given a
# terminal tab in the terminals' split, its input and output that tab's
# (the keyboard stays where it was): what is typed there is the program's.
# (With lldb, on the tab's device by target.input-path and the like; gdb's
# inferior-tty.) Once the session is over the tab says it is done.
cp "$here/debug/asks.c" "$dir/here/" && (cd "$dir/here" && cc -g -O0 -o asks asks.c) || exit 1
run "frame;debug:program=$dir/here/asks;break:8;key:F5;debugwait:answers;command:Go to the split below;termwait:name?;text:blit;key:Return;command:Go to the split above;debugwait:stopped;debugdump;key:F5;debugwait:idle;debugdump;command:Go to the split below;termend;termdump" "$dir/here/asks.c"
got=$(printf '%s' "$got" | sed -E -e 's/ start(@[0-9]*)?;/;/' -e 's/name="blit\\n"[^;]*;/name=blit ;/' -e 's/output: Process [0-9]* exited with status = 0 \(0x00000000\) *\| /output: /' \
    -e 's/\[done in [0-9.]* s\]/[done]/' -e 's/ @[0-9]*,[0-9]* [0-9]*x[0-9]*//')
check "a program that reads its input: a terminal tab of its own, what is typed there its" \
    'stopped (breakpoint); stack: main@8; vars: name=blit ;; idle; output: [exited with code 0] ;; name? blit|hello, blit|[done] ended 0'

# Debug, the tests and Record build first where the folder has a build (the
# run configuration's, else the Build tab's Build: make's make, here), and
# go only if it went well; one that fails says so.
B="$dir/built"
mkdir -p "$B"
cp "$here/debug/count.c" "$B/"
printf 'count: FORCE\n\techo built >>log\n\tcc -g -O0 -o count count.c\nFORCE:\n' >"$B/Makefile"
run "workdir:$B;builddump;debug:program=$B/count;break:8;key:F5;debugwait:stopped;debugdump;builddump" "$B/count.c"
got="$got ;; $(cat "$B/log" 2>/dev/null)"
check "F5 builds first (make, here), then debugs what it built" \
    'Make: >make make count make FORCE ;; stopped (breakpoint); stack: square@8 main@15; vars: x=1 y=1 ;; Make: >make make count make FORCE (last: 0) ;; built'
printf 'count: FORCE\n\techo broken >>log\n\texit 2\nFORCE:\n' >"$B/Makefile"
run "workdir:$B;builddump;debug:program=$B/count;break:8;key:F5;debugwait:stopped;debugdump" "$B/count.c"
got="$got ;; $(tail -1 "$B/log")"
check "a build that fails: it says so, and nothing is debugged" 'Make: >make make count make FORCE ;; blit: make failed: Debug did not start ;; idle ;; broken'
# (A run configuration's build is the one, and Restart builds again first: twice, then.)
mkdir -p "$B/.blit" && rm -f "$B/log"
printf 'count: FORCE\n\techo built >>log\n\tcc -g -O0 -o count count.c\nother:\n\techo other >>log\nFORCE:\n' >"$B/Makefile"
printf '{ "configurations": [ { "name": "count", "program": "count", "build": "make count" } ] }\n' >"$B/.blit/run.json"
run "workdir:$B;runconfdump;break:8;key:F5;debugwait:stopped;key:cmd+shift+F5;debugwait:stopped;debugdump" "$B/count.c"
got=$(printf '%s' "$got" | sed -E -e 's/; output: [^;]*$//')
got="$got ;; $(cat "$B/log" | tr '\n' ' ')"
check "a run configuration's build first, and again before a restart" 'count | debug: count in built | profile: count in built ;; stopped (breakpoint); stack: square@8 main@15; vars: x=1 y=1 ;; built built '
rm -rf "$B/.blit"

# A stand-in debugger (tests/fake_dap.cpp) as the machine's, started by an
# xcrun and a gdb of its own first on the PATH: what the debuggers blit
# meets say, where this machine's would not say it.
fakedap="$(cd "$(dirname "$blit")" && pwd)/blit_fakedap"
if [ -x "$fakedap" ]; then
    F="$dir/fake"
    mkdir -p "$F/bin"
    printf '#!/bin/sh\ncase "$1" in --find) echo "%s" ;; lldb-dap) exec "%s" ;; *) exit 1 ;; esac\n' "$fakedap" "$fakedap" >"$F/bin/xcrun"
    printf '#!/bin/sh\ncase "$1" in --version) echo "GNU gdb (GDB) 15.1" ;; *) exec "%s" ;; esac\n' "$fakedap" >"$F/bin/gdb"
    chmod +x "$F/bin/xcrun" "$F/bin/gdb"
    printf 'print("a program")\n' >"$F/prog"
    # fake SCRIPT [FILE]: as run, with the stand-in; what it was asked is in $F/log.
    fake() {
        rm -f "$F/log"
        was=$PATH
        PATH="$F/bin:$PATH" BLIT_FAKE_DAP_LOG="$F/log"
        export BLIT_FAKE_DAP_LOG
        run "$@"
        PATH=$was
        unset BLIT_FAKE_DAP_LOG
    }

    # Stopping lets go of a process attached to (it goes on), and ends one launched.
    fake "debug:program=$F/prog;key:F5;debugwait:stopped;key:shift+F5;debugwait:idle"
    stops=$(grep '^disconnect' "$F/log")
    fake "debugattach:4242;debugwait:stopped;key:shift+F5;debugwait:idle"
    got="$stops ;; $(grep '^disconnect' "$F/log")"
    check "stopping: a program launched ends, a process attached to goes on" 'disconnect {"terminateDebuggee":true} ;; disconnect {"terminateDebuggee":false}'

    # A launch refused with no message: the reason is the error's own (lldb-dap's way).
    fake "debug:program=$F/no-such;key:F5;debugwait:idle;debugdump"
    check "a launch refused: why, from the error the debugger gave" "idle error: cannot launch it: '@/fake/no-such' does not exist"

    # What the program printed in colour: the escape sequences left out (one cut between two events).
    fake "debug:program=$F/prog;key:F5;debugwait:stopped;debugdump" "$F/prog"
    check "what the program printed, without its escape sequences" 'stopped (breakpoint); stack: main@1; vars: df=DataFrame total=1; output: red plain'

    # Completions whose start is counted from 0 (debugpy's), though blit asked for 1: after a dot, and a name alone.
    fake "debug:program=$F/prog;key:F5;debugwait:stopped;key:cmd+shift+y;text:df.desc;key:Tab;debugwait:answers;finddump;key:Escape;key:cmd+shift+y;text:tot;key:Tab;debugwait:answers;finddump;key:Return;debugwait:stopped;debugdump" "$F/prog"
    check "evaluate: completions counted from 0 go where they should" 'input [df.describe] 0 : ;; input [total] 0 : ;; stopped (breakpoint); stack: main@1; vars: df=DataFrame total=1; output: red plain | total = <total>'

    # Tab straight after a dot: nothing of the word typed yet (debugpy's length 0), still counted from 0.
    fake "debug:program=$F/prog;key:F5;debugwait:stopped;key:cmd+shift+y;text:df.;key:Tab;debugwait:answers;finddump" "$F/prog"
    check "evaluate: completions straight after a dot go after it" 'pick [] 4 : >df.describe df.dtypes df.shape df.total'

    # What the debugger says of the breakpoints: one on a blank line put
    # on the next (and kept there), one whose condition it refuses with
    # its message, one in a file that is not there "pending", one in a
    # library not loaded yet not verified until a breakpoint event says it
    # is (once the program goes on).
    printf 'one\n\nthree\n' >"$F/two"
    printf 'def twice(x):\n    return x + x\n' >"$F/lib.py"
    fake "debug:program=$F/two;break:2;breakif:1;text:nope > 1;key:Return;key:Return;key:Return;open:$F/lib.py;break:2;open:$F/gone.py;break:1;open:$F/two;key:F5;debugwait:stopped;debugwait:answers;debugbreaks;debugpanel;debugrows:Breakpoints:5;key:F5;debugwait:stopped;debugwait:answers;debugbreaks" "$F/two"
    check "breakpoints not verified (and why), and one moved" \
        'gone.py:1 unverified (pending); lib.py:2 unverified; two:1 unverified (No symbol "nope" in current context) if nope > 1; two:3 moved from 2 ;; Breakpoints 4 | ○ gone.py:1 not verified: pending | ○ lib.py:2 not verified | ◇ two:1 not verified: No symbol "nope" in current context, if nope > 1 | ● two:3 moved from line 2 ;; gone.py:1 unverified (pending); lib.py:2; two:1 unverified (No symbol "nope" in current context) if nope > 1; two:3 moved from 2'
    # One moved onto a line that has one: one breakpoint, given again.
    fake "debug:program=$F/two;break:2;break:3;key:F5;debugwait:stopped;debugwait:answers;debugbreaks" "$F/two"
    got="$got ;; $(grep '^setBreakpoints' "$F/log" | sed -e 's/.*"breakpoints"://' | awk 'NR > 1 { printf " ;; " } { printf "%s", $0 }')"
    check "one moved onto another: they are one" 'two:3 ;; [{"line":2},{"line":3}]} ;; [{"line":3}]}'
    # Breakpoint events: one made by the debugger (gdb's `break`), and taken away.
    fake "debug:program=$F/two;break:3;key:F5;debugwait:stopped;debugbreaks;key:cmd+shift+y;text:break 1;key:Return;debugwait:answers;debugbreaks;key:cmd+shift+y;text:delete 2;key:Return;debugwait:answers;debugbreaks" "$F/two"
    check "breakpoints the debugger makes and takes away" 'two:3 ;; two:1; two:3 ;; two:3'
    # Disabled: Space on one in the Debug tab, Alt and a click on its dot,
    # and the command for the cursor's line, each again enabled; one
    # disabled is drawn so and not given to the debugger.
    fake "debug:program=$F/two;break:1;break:3;debugpanel;debugpick:breaks:1;debugpanel;key:space;debugbreaks;debugrows:Breakpoints:3;key:F5;debugwait:stopped;debugwait:answers;breakclick:3,alt;debugwait:answers;debugbreaks;command:Enable or disable the breakpoint;debugwait:answers;debugbreaks" "$F/two"
    got="$got ;; $(grep '^setBreakpoints' "$F/log" | sed -e 's/.*"breakpoints"://' | awk 'NR > 1 { printf " ;; " } { printf "%s", $0 }')"
    check "a breakpoint disabled and enabled: Space, Alt-click, the command" \
        'two:1 off; two:3 ;; Breakpoints 2 | ● two:1 disabled | ● two:3 ;; two:1 off; two:3 off ;; two:1; two:3 off ;; [{"line":3}]} ;; []} ;; [{"line":1}]}'

    # rocgdb's GPU waves (two thousand, and three threads of the program's
    # own; stopped in a wave): the waves one row with their count, opened to
    # list them below the variables, the wave that stopped under it all the
    # same; a wave chosen from the list is under it too.
    BLIT_FAKE_DAP_WAVES=2000
    export BLIT_FAKE_DAP_WAVES
    fake "debug:program=$F/prog;key:F5;debugwait:stopped;debugwait:answers;debugdump;debugpanel;debugrows:Threads:9;debugpick:waves;debugrows:Threads:9;debugrows:Variables:7" "$F/prog"
    check "GPU waves: one row, the stopped one under it, the stack and variables above the list" \
        'stopped (breakpoint); threads: 2003, #1004 shown (2000 GPU waves; shown: GPU wave (250,0,0)/0); stack: vadd@1; vars: df=DataFrame total=1; output: red plain ;; Threads 2003 | vadd | vadd | vadd | ▸ GPU waves 2000 | ▸ GPU wave (250,0,0)/0 breakpoint | Call stack 1 | ▸ vadd prog:1 | Variables ;; Threads 2003 | vadd | vadd | vadd | ▾ GPU waves 2000 | ▸ GPU wave (250,0,0)/0 breakpoint | Call stack 1 | ▸ vadd prog:1 | Variables ;; Variables | ▾ Locals | df = DataFrame | total = 1 | GPU waves 2000 | GPU wave (0,0,0)/0 | GPU wave (0,0,0)/1'
    fake "debug:program=$F/prog;key:F5;debugwait:stopped;debugwait:answers;debugpanel;debugpick:waves;debugpick:threads:5;debugwait:stopped;key:Left;debugrows:Threads:8;debugdump" "$F/prog"
    check "a wave chosen from the list: its stack, and it is under the waves' row with the one that stopped" \
        'Threads 2003 | vadd | vadd | vadd | ▸ GPU waves 2000 | GPU wave (250,0,0)/0 breakpoint | ▸ GPU wave (0,0,0)/0 | Call stack 1 ;; stopped (breakpoint); threads: 2003, #4 shown (2000 GPU waves; shown: GPU wave (0,0,0)/0); stack: vadd@1; vars: df=DataFrame total=1; output: red plain'
    unset BLIT_FAKE_DAP_WAVES
    # The debug console: a line to the debugger's command line, its answer
    # under it (a value with members opened a level, as the Variables view
    # opens it), one refused as an error; the lines typed before with Up and
    # Down (and what was being typed kept); Cmd+F over its lines. Its view
    # follows the line typed on, from the first (its split just made). F5 from
    # there shows where it stopped in the split above, the keyboard staying.
    fake "debug:program=$F/prog;key:F5;debugwait:stopped;key:cmd+alt+y;text:df;key:Return;debugwait:answers;text:nope;key:Return;debugwait:answers;frame;rowsdump:1;docdump:8;text:to;key:Up;docdump:0;key:Up;docdump:0;key:Up;key:Down;key:Down;docdump:0;key:Down;docdump:0;key:F5;debugwait:stopped;tabsdump;key:cmd+f;text:shape;finddump" "$F/prog"
    check "the debug console: scrolled to the line typed on; a value opened a level, an error, the history, F5, Cmd+F" \
        "rows | # df ;; Debug console ;;   red plain ;;   › df ;;   ▾ DataFrame ;;     shape = (3, 2)  (tuple) ;;     ▸ columns = Index(['a', 'b'])  (Index) ;;   › nope ;;   NameError: name 'nope' is not defined ;; > › | ;; Debug console ;; > › nope| ;; Debug console ;; > › df| ;; Debug console ;; > › to| ;; Debug console ;; > › to| ;; Debug console ;; file [shape] 1 of 1 at 33-38"
    # With Vim's keys: Esc leaves the line, k goes up over the lines, Enter
    # opens the value there a level more (and again shuts it), i comes back.
    fake "lua:blit.opt.editor.modal = true;debug:program=$F/prog;key:F5;debugwait:stopped;key:cmd+alt+y;text:df;key:Return;debugwait:answers;key:Escape;vim:k;key:Return;debugwait:answers;docdump:3;key:Return;text:i;text:ok;docdump:2" "$F/prog"
    check "the debug console with Vim's keys: a member opened from the lines above, and back to the line" \
        "Debug console ;;   › df ;;   ▾ DataFrame ;;     shape = (3, 2)  (tuple) ;; > |  ▾ columns = Index(['a', 'b'])  (Index) ;;       0 = 'a'  (str) ;;       1 = 'b'  (str) ;;   ›  ;; Debug console ;;     shape = (3, 2)  (tuple) ;;     ▸ columns = Index(['a', 'b'])  (Index) ;; > › ok|"
    # Ctrl+` hides it with the terminals' split (it is the split's own, as
    # they are), and Cmd+Option+Y brings it back.
    fake "debug:program=$F/prog;key:F5;debugwait:stopped;key:cmd+alt+y;dump;key:ctrl+\`;dump;key:cmd+alt+y;dump" "$F/prog"
    check "the debug console hides with the terminals' split, and comes back" 'V([prog*] / >[Debug console*])0.70 ;; >[prog*] ;; V([prog*] / >[Debug console*])0.70'
    # A monitor line (a gdb server's command: OpenOCD's, a probe's) goes to
    # gdb as it is; to lldb-dap as its command for it (above, on a Mac).
    BLIT_FAKE_DAP_GDB=1
    export BLIT_FAKE_DAP_GDB
    fake "debug:program=$F/prog;key:F5;debugwait:stopped;key:cmd+alt+y;text:monitor reset halt;key:Return;debugwait:answers"
    unset BLIT_FAKE_DAP_GDB
    got=$(grep '^evaluate' "$F/log")
    check "a monitor line to gdb: as it is" 'evaluate {"expression":"monitor reset halt","frameId":1,"context":"repl"}'

    # Exceptions (the stand-in has debugpy's filters): uncaught on, as the
    # debugger has it; stopped at one, what it is, its stack trace and the
    # one it was raised while handling, in the Debug tab and after its line.
    printf 'raise ValueError("bad value")\n' >"$F/raises"
    fake "debug:program=$F/raises;key:F5;debugwait:stopped;debugbreaks;debugdump;frame;debuginline" "$F/raises"
    check "an exception stopped at: what the debugger says of it" \
        "exception uncaught ;; stopped (exception: ValueError); exception: ValueError: bad value |   File \"@/fake/raises\", line 1, in <module> |     raise ValueError(\"bad value\") | ValueError: bad value | caused by KeyError: 'k' |   File \"@/fake/raises\", line 1, in <module> |     d[\"k\"]; stack: main@1; vars: df=DataFrame total=1 ;; 1: ◀ exception: ValueError: bad value"
    # Uncaught off (Enter on it in the Debug tab): it is not stopped at, and
    # what the program wrote as errors is in the console.
    fake "debug:program=$F/raises;key:F5;debugwait:stopped;debugpanel;debugpick:exceptions:2;key:shift+F5;debugwait:idle;debugbreaks;key:F5;debugwait:idle;debugdump;key:cmd+alt+y;docdump:3" "$F/raises"
    check "an exception not stopped at" \
        'no breakpoints ;; idle; output: Traceback (most recent call last): | ValueError: bad value | [exited with code 1] ;; Debug console ;;   Traceback (most recent call last): ;;   ValueError: bad value ;;   [exited with code 1] ;; > › |'
    # A condition on one (Shift+F9 there), sent with the debugger's filter options.
    fake "debug:program=$F/prog;key:F5;debugwait:stopped;debugpanel;debugpick:exceptions:1;debugpanel;key:shift+F9;text:isinstance(e, KeyError);key:Return;debugwait:answers;debugbreaks"
    got="$got ;; $(grep '^setExceptionBreakpoints' "$F/log" | tail -1)"
    check "an exception's condition" \
        'exception raised if isinstance(e, KeyError); exception uncaught ;; setExceptionBreakpoints {"filters":["uncaught"],"filterOptions":[{"filterId":"raised","condition":"isinstance(e, KeyError)"}]}'
    # A debugger that says no defaults: none on; but for a .py program
    # (debugpy, the stand-in here as the project's Python starts it),
    # uncaught on and raised off.
    mkdir -p "$F/py/.venv/bin"
    printf '#!/bin/sh\nexec "%s"\n' "$fakedap" >"$F/py/.venv/bin/python"
    chmod +x "$F/py/.venv/bin/python"
    printf 'print(1)\n' >"$F/py/one.py"
    BLIT_FAKE_DAP_NODEFAULTS=1
    export BLIT_FAKE_DAP_NODEFAULTS
    fake "debug:program=$F/prog;key:F5;debugwait:stopped;debugbreaks"
    c=$got
    fake "workdir:$F/py;debug:program=$F/py/one.py;key:F5;debugwait:stopped;debugbreaks" "$F/py/one.py"
    unset BLIT_FAKE_DAP_NODEFAULTS
    got="$c ;; $got"
    check "no defaults: none on, but debugpy's uncaught" 'no breakpoints ;; exception uncaught'

    # Kept in the folder's session: the exceptions chosen, and the lines
    # typed in the console (Up brings them back after a restart).
    mkdir -p "$F/proj"
    fake "folder:$F/proj;debug:program=$F/prog;key:F5;debugwait:stopped;debugpanel;debugpick:exceptions:1;key:cmd+alt+y;text:df;key:Return;debugwait:answers;text:total;key:Return;debugwait:answers;quit"
    kept=$(grep -h -E '^(exception|console)	' "$dir"/sessions/* | tr '\t' ' ' | awk 'NR > 1 { printf " ;; " } { printf "%s", $0 }')
    fake "folder:$F/proj;debug:program=$F/prog;key:F5;debugwait:stopped;debugbreaks;key:cmd+alt+y;key:Up;docdump:0;key:Up;docdump:0"
    got="$kept ;; $got"
    check "kept: the exceptions chosen, the console's lines" \
        'exception raised 1  ;; console df ;; console total ;; exception raised; exception uncaught ;; Debug console ;; > › total| ;; Debug console ;; > › df|'
    # A run configuration's "debug": its arguments laid over those blit sends
    # (an object's members merged, all the way down: the rest take the place
    # of blit's), and its commands for the debugger once the session has
    # started, each one's answer said (or why there is none).
    D="$F/proj"
    mkdir -p "$D/.blit"
    cp "$F/prog" "$D/prog"
    conf() { printf '{ "configurations": [ { "name": "p", "program": "prog", "args": ["one"], "env": { "FAKE_ENV": "yes" }, "debug": %s } ] }\n' "$1" >"$D/.blit/run.json"; }
    launched() { sed -En "s#$dir#@#g; s/^(launch|attach) //p" "$F/log" | sed 's/"enableAutoVariableSummaries":true,//'; }
    conf '{ "arguments": { "stopOnEntry": true, "args": ["two"], "more": { "a": 1 } }, "after": ["settings set target.x 1", "fail here"] }'
    fake "workdir:$D;runconfdump;key:F5;debugwait:stopped;debugdump" "$D/prog"
    got="${got#* ;; } ;; $(launched)"
    check "a run configuration's debug: its arguments over blit's, its commands once it has started" \
        "stopped (breakpoint); stack: main@1; vars: df=DataFrame total=1; output: <settings set target.x 1> | fail here: can't evaluate expressions when the process is running. | red plain ;; "'{"program":"@/fake/proj/prog","args":["two"],"cwd":"@/fake/proj","env":["FAKE_ENV=yes"],"stopOnEntry":true,"more":{"a":1}}'
    # (Attaching, as to a gdb server: what it attaches to is its arguments'; Shift+F5 lets it go.)
    conf '{ "request": "attach", "arguments": { "target": "localhost:3333" } }'
    fake "workdir:$D;runconfdump;key:F5;debugwait:stopped;key:shift+F5;debugwait:idle;debugdump" "$D/prog"
    got="${got#* ;; } ;; $(launched) ;; $(grep '^disconnect' "$F/log")"
    check "a run configuration that attaches" 'idle ;; {"program":"@/fake/proj/prog","cwd":"@/fake/proj","target":"localhost:3333"} ;; disconnect {"terminateDebuggee":false}'
    # (A debugger of its own, a command line: run as written.)
    printf '#!/bin/sh\necho "mydap $*" >"%s/ran"\nexec "%s"\n' "$F" "$fakedap" >"$F/mydap"
    chmod +x "$F/mydap"
    conf "{ \"adapter\": \"$F/mydap --verbose\" }"
    fake "workdir:$D;runconfdump;key:F5;debugwait:stopped;debugdump" "$D/prog"
    got="${got#* ;; } ;; $(cat "$F/ran")"
    check "a run configuration's own debugger" 'stopped (breakpoint); stack: main@1; vars: df=DataFrame total=1; output: red plain ;; mydap --verbose'

    # The machine's debugger on Linux (the shell here told it is Linux by an
    # uname of its own; the stand-in as its gdb and its lldb-dap): lldb-dap
    # where there is one, else gdb 14 or later (gdb -i dap: its launch's
    # environment an object, its own arguments' names passed on); and
    # debug.adapter "gdb" asks for gdb, wherever lldb-dap is. (There is no gdb
    # on this Mac: "GNU gdb (GDB) 15.1" is the first line of gdb's --version
    # as GNU's builds print it, from its documentation; the real gdb's own
    # checks are on the Linux box's list.)
    # linux GDB-VERSION LLDB-DAP SCRIPT [FILE]: as fake, gdb that version (none: no gdb), lldb-dap there if yes; what ran last.
    linux() {
        L="$dir/linux"
        rm -rf "$L" && mkdir -p "$L/bin"
        printf '#!/bin/sh\nif [ "$1" = -s ]; then echo Linux; else echo "Linux box 6.12.1"; fi\n' >"$L/bin/uname"
        [ -n "$1" ] && printf '#!/bin/sh\ncase "$1" in --version) echo "GNU gdb (GDB) %s" ;; *) echo "gdb $*" >"%s/ran"; exec "%s" ;; esac\n' "$1" "$L" "$fakedap" >"$L/bin/gdb"
        [ "$2" = yes ] && printf '#!/bin/sh\necho "lldb-dap $*" >"%s/ran"\nexec "%s"\n' "$L" "$fakedap" >"$L/bin/lldb-dap"
        chmod +x "$L/bin/"*
        rm -f "$F/log"
        was=$PATH
        PATH="$L/bin:$PATH" BLIT_FAKE_DAP_LOG="$F/log"
        export BLIT_FAKE_DAP_LOG
        run "$3" "${4:-$D/prog}"
        PATH=$was
        unset BLIT_FAKE_DAP_LOG
        got="${got#* ;; } ;; $(cat "$L/ran" 2>/dev/null)"
    }
    conf '{ "arguments": { "env": { "MORE": "1" }, "stopAtBeginningOfMainSubprogram": true } }'
    linux 15.1 yes "workdir:$D;runconfdump;key:F5;debugwait:stopped;debugdump"
    got="$got ;; $(launched)"
    check "Linux: lldb-dap where there is one (the environment a list, the configuration's object in its place)" \
        'stopped (breakpoint); stack: main@1; vars: df=DataFrame total=1; output: red plain ;; lldb-dap  ;; {"program":"@/fake/proj/prog","args":["one"],"cwd":"@/fake/proj","env":{"MORE":"1"},"stopAtBeginningOfMainSubprogram":true}'
    linux 15.1 no "workdir:$D;runconfdump;key:F5;debugwait:stopped;debugdump"
    got="$got ;; $(launched)"
    check "Linux: else gdb -i dap (the environment an object, merged with the configuration's)" \
        'stopped (breakpoint); stack: main@1; vars: df=DataFrame total=1; output: red plain ;; gdb -q -i dap ;; {"program":"@/fake/proj/prog","args":["one"],"cwd":"@/fake/proj","env":{"FAKE_ENV":"yes","MORE":"1"},"stopAtBeginningOfMainSubprogram":true}'
    linux 13.2 no "workdir:$D;runconfdump;key:F5;debugwait:idle;debugdump"
    check "Linux: a gdb older than 14 is no debugger blit can use" 'idle error: no debugger: install lldb, or gdb 14 or newer ;; '
    conf '{ "adapter": "gdb" }'
    linux 15.1 yes "workdir:$D;runconfdump;key:F5;debugwait:stopped;debugdump"
    check "debug.adapter gdb: gdb, though lldb-dap is there" 'stopped (breakpoint); stack: main@1; vars: df=DataFrame total=1; output: red plain ;; gdb -q -i dap'
    linux 12.1 yes "workdir:$D;runconfdump;key:F5;debugwait:idle;debugdump"
    check "debug.adapter gdb, and it is too old" 'idle error: gdb 12 is too old to be a debugger blit can use: 14 or newer speaks its protocol ;; '

    # Restart where the debugger cannot (lldb-dap says it can only once it has
    # launched; the stand-in does not): the session ended and begun again,
    # launched twice; where it can, DAP's restart, given the launch's
    # arguments, in the same session.
    fake "debug:program=$F/prog;key:F5;debugwait:stopped;key:cmd+shift+F5;debugwait:stopped;debugdump"
    got="$got ;; $(grep -c '^launch' "$F/log") launches, $(grep -c '^disconnect' "$F/log") disconnect, $(grep -c '^restart' "$F/log") restarts"
    check "restart, ended and begun again" 'stopped (breakpoint); stack: main@1; vars: df=DataFrame total=1; output: red plain ;; 2 launches, 1 disconnect, 0 restarts'
    BLIT_FAKE_DAP_RESTART=1
    export BLIT_FAKE_DAP_RESTART
    fake "debug:program=$F/prog;key:F5;debugwait:stopped;key:cmd+shift+F5;debugwait:stopped;debugdump"
    unset BLIT_FAKE_DAP_RESTART
    got="$got ;; $(grep -c '^launch' "$F/log") launch ;; $(sed -n "s#$dir#@#g; s/\"enableAutoVariableSummaries\":true,//; s/^restart //p" "$F/log")"
    check "restart, as the debugger restarts it" \
        'stopped (breakpoint); stack: main@1; vars: df=DataFrame total=1; output: restarted ;; 1 launch ;; {"arguments":{"program":"@/fake/prog","args":[],"cwd":"@/fake"}}'

    # The program's input and output in a terminal tab: where the debugger
    # asks blit to run it in one (runInTerminal: debugpy, lldb-dap on another
    # machine, a debugger of a configuration's own), the tab runs what it
    # asks, where it asks, with its environment and the configuration's;
    # blit answers with its process. ("console": "terminal" asks for it; the
    # stand-in's request is lldb-dap's as LLVM 21 makes it, its command a
    # shell's here.)
    conf "{ \"adapter\": \"$F/mydap\", \"console\": \"terminal\" }"
    fake "frame;workdir:$D;runconfdump;key:F5;debugwait:stopped;command:Go to the split below;termwait:in the terminal;text:typed;key:Return;termwait:read: typed;termdump" "$D/prog"
    got="${got#* ;; } ;; $(launched | sed 's/.*"env":\[[^]]*\]//') ;; $(grep '^answer:' "$F/log" | sed 's/[0-9][0-9]*/N/g')"
    got=$(printf '%s' "$got" | sed -E 's/ @[0-9]+,[0-9]+ [0-9]+x[0-9]+//')
    check "runInTerminal: the program in a terminal tab, what is typed there its" \
        'in the terminal: yes in proj|typed|read: typed ;; ,"runInTerminal":true,"console":"integratedTerminal"} ;; answer:runInTerminal true {"processId":N,"shellProcessId":N}'
    # (On another machine: the tab is there, as the program is.)
    mkdir -p "$T/fk" && cp "$F/prog" "$T/fk/prog"
    printf '#!/bin/sh\nBLIT_FAKE_DAP_LOG="%s/log" exec "%s"\n' "$F" "$fakedap" >"$F/logdap"
    chmod +x "$F/logdap"
    fake "frame;remotewait;debug:program=ssh://box$T/fk/prog;debug:adapter=$F/logdap;debug:console=terminal;key:F5;debugwait:stopped;command:Go to the split below;termwait:in the terminal;termdump;tabsdump" "ssh://box$T/fk/prog"
    got=$(printf '%s' "$got" | sed -E 's/ @[0-9]+,[0-9]+ [0-9]+x[0-9]+//')
    got="$got ;; $(grep '^answer:' "$F/log")"
    check "runInTerminal on another machine: the tab there" 'in the terminal: yes in fk ;; fake (box) ;; answer:runInTerminal true {}'

    # Which programs read what is typed to them, and so get a terminal tab by
    # default (debug.console "debug" keeps them in the Debug tab): C's stdin
    # and scanf, C++'s std::cin, Python's input(); not count's printf.
    printf '#include <iostream>\n#include <string>\nint main() { std::string s; std::cin >> s; return 0; }\n' >"$F/cin.cpp"
    printf '#include <stdio.h>\nint main(void) { int n = 0; return scanf("%%d", &n); }\n' >"$F/scan.c"
    (cd "$F" && c++ -g -o cin cin.cpp && cc -g -o scan scan.c) || exit 1
    terminal_for() {
        fake "frame;debug:program=$1;${2:-}key:F5;debugwait:stopped" "$F/prog"
        if grep -q '"preRunCommands":\["settings set target.input-path /dev/' "$F/log"; then printf '%s: terminal' "${1##*/}"; else printf '%s: debug tab' "${1##*/}"; fi
    }
    got="$(terminal_for "$dir/here/asks") ;; $(terminal_for "$F/cin") ;; $(terminal_for "$F/scan") ;; $(terminal_for "$dir/here/count") ;; $(terminal_for "$dir/here/asks" "debug:console=debug;")"
    check "the programs that read their input get a terminal" 'asks: terminal ;; cin: terminal ;; scan: terminal ;; count: debug tab ;; asks: debug tab'
    # (A .py program: debugpy is asked for its terminal, console "integratedTerminal",
    # where it calls input(); the stand-in as the project's Python's debugpy.)
    Q="$F/pyq"
    mkdir -p "$Q/.venv/bin"
    printf '#!/bin/sh
exec "%s"\n' "$fakedap" >"$Q/.venv/bin/python"
    chmod +x "$Q/.venv/bin/python"
    printf 'name = input("name? ")\nprint("hello,", name)\n' >"$Q/asks.py"
    printf 'print("hello")\n' >"$Q/plain.py"
    consoles=
    for py in asks plain; do
        fake "frame;workdir:$Q;debug:program=$Q/$py.py;key:F5;debugwait:stopped" "$Q/$py.py"
        consoles="$consoles $py: $(sed -n 's/^launch .*"console":"\([a-zA-Z]*\)".*/\1/p' "$F/log")"
    done
    got=$consoles
    check "a .py program that calls input(): debugpy asked for a terminal" ' asks: integratedTerminal plain: internalConsole'
fi
# A .py program: debugpy run with the project's Python (its .venv's, not
# the PATH's). Where that has no debugpy of its own, blit fetches its own
# copy for it: here from a wheel of the tests' own ($BLIT_DEBUGPY_URL,
# whose debugpy's adapter is tests/fake_dap.cpp, saying first what started
# it), into a data folder of their own, so nothing is fetched from PyPI.
if command -v python3 >/dev/null 2>&1; then
    mkdir -p "$dir/wheel/debugpy/adapter"
    : >"$dir/wheel/debugpy/__init__.py"
    : >"$dir/wheel/debugpy/adapter/__init__.py"
    cat >"$dir/wheel/debugpy/adapter/__main__.py" <<'EOF'
import os, sys
import debugpy
with open(os.environ["BLIT_FAKE_DEBUGPY_LOG"], "a") as f:
    f.write("adapter " + sys.executable + " " + os.path.dirname(debugpy.__file__) + "\n")
os.execv(os.environ["BLIT_FAKE_DAP"], [os.environ["BLIT_FAKE_DAP"]])
EOF
    (cd "$dir/wheel" && python3 -m zipfile -c "$dir/debugpy.whl" debugpy) || exit 1
    export XDG_DATA_HOME="$dir/data" BLIT_DEBUGPY_URL="$dir/debugpy.whl" BLIT_FAKE_DAP="$fakedap" BLIT_FAKE_DEBUGPY_LOG="$dir/adapter.log"
    # A Python that cannot import debugpy, not even blit's copy: said so of it.
    Y="$dir/py"
    mkdir -p "$Y/.venv/bin" "$Y/scripts"
    printf 'print(1)\n' >"$Y/scripts/one.py"
    printf '#!/bin/sh\necho "$0: Error while finding module specification for '"'"'debugpy.adapter'"'"' (ModuleNotFoundError: No module named '"'"'debugpy'"'"')" >&2\nexit 1\n' >"$Y/.venv/bin/python"
    chmod +x "$Y/.venv/bin/python"
    run "workdir:$Y;debug:program=$Y/scripts/one.py;key:F5;debugwait:idle;debugdump" "$Y/scripts/one.py"
    check "a .py program: the project's Python, and debugpy missing there said so" 'blit: Getting debugpy… ;; idle error: debugpy is not installed for .venv/bin/python'
    # On another machine, the same: its .venv's Python there (not the PATH's
    # python3 there), chosen in the project's folder there.
    mkdir -p "$T/py/.venv/bin" "$T/py/scripts"
    cp "$Y/scripts/one.py" "$T/py/scripts/" && cp "$Y/.venv/bin/python" "$T/py/.venv/bin/python"
    run "remotewait;workdir:ssh://box$T/py;debug:program=ssh://box$T/py/scripts/one.py;key:F5;debugwait:idle;debugdump" "ssh://box$T/py/scripts/one.py"
    check "a .py program on another machine: the project's Python there" 'blit: Getting debugpy… ;; idle error: debugpy is not installed for .venv/bin/python'
    # The copy could not be fetched: why, said with it.
    XDG_DATA_HOME="$dir/data-none" BLIT_DEBUGPY_URL="$dir/no-such.whl"
    run "workdir:$Y;debug:program=$Y/scripts/one.py;key:F5;debugwait:idle;debugdump" "$Y/scripts/one.py"
    XDG_DATA_HOME="$dir/data" BLIT_DEBUGPY_URL="$dir/debugpy.whl"
    check "blit's copy could not be had: why" "blit: Getting debugpy… ;; idle error: debugpy is not installed for .venv/bin/python, and blit's copy could not be had: could not fetch file://@/no-such.whl: curl: (37) Couldn't open file @/no-such.whl"
fi
if command -v python3 >/dev/null 2>&1 && [ -x "$fakedap" ]; then
    # A venv's Python with no debugpy: blit's copy fetched (the wheel kept,
    # for another machine), and the adapter run from it with that Python;
    # the next time, the copy as it is.
    Q="$dir/q"
    python3 -m venv --without-pip "$Q/.venv" || exit 1
    printf 'print("a program")\n' >"$Q/prog.py"
    rm -f "$dir/adapter.log"
    XDG_DATA_HOME="$dir/data-q"
    run "workdir:$Q;debug:program=$Q/prog.py;key:F5;debugwait:stopped;debugdump;key:shift+F5;debugwait:idle" "$Q/prog.py"
    first=$got
    run "workdir:$Q;debug:program=$Q/prog.py;key:F5;debugwait:stopped;key:shift+F5;debugwait:idle;pydump" "$Q/prog.py"
    got="$first ;; $got ;; $(sed "s#$dir#@#g" "$dir/adapter.log" | awk 'NR > 1 { printf " ;; " } { printf "%s", $0 }') ;; $(cd "$dir/data-q/blit/debugpy" && ls | tr '\n' ' ')"
    got=$(printf '%s' "$got" | sed -e 's/python [0-9][0-9.a-z+]*/python V/')
    XDG_DATA_HOME="$dir/data"
    check "debugpy fetched for a Python without it: the adapter run from blit's copy, with that Python" \
        'blit: Getting debugpy… ;; stopped (breakpoint); stack: main@1; vars: df=DataFrame total=1; output: red plain ;; @/q = @/q (.venv) @/q/.venv/bin/python V | debugpy @/data-q/blit/debugpy/1.8.22 ;; adapter @/q/.venv/bin/python @/data-q/blit/debugpy/1.8.22/debugpy ;; adapter @/q/.venv/bin/python @/data-q/blit/debugpy/1.8.22/debugpy ;; 1.8.22 debugpy-1.8.22-py2.py3-none-any.whl '
    # A Python with debugpy of its own: that one, and nothing fetched.
    O="$dir/own"
    python3 -m venv --without-pip "$O/.venv" || exit 1
    cp -R "$dir/wheel/debugpy" "$(echo "$O"/.venv/lib/python*/site-packages)/" || exit 1
    printf 'print("a program")\n' >"$O/prog.py"
    rm -f "$dir/adapter.log"
    XDG_DATA_HOME="$dir/data-own"
    run "workdir:$O;debug:program=$O/prog.py;key:F5;debugwait:stopped;key:shift+F5;debugwait:idle;pydump" "$O/prog.py"
    XDG_DATA_HOME="$dir/data"
    got="$got ;; $(sed -e "s#$dir#@#g" -e 's#/lib/python[0-9.]*/#/lib/pythonV/#' "$dir/adapter.log") ;; $(ls "$dir/data-own" 2>/dev/null | wc -l | tr -d ' ')"
    got=$(printf '%s' "$got" | sed -e 's/python [0-9][0-9.a-z+]*/python V/')
    check "a Python with debugpy of its own: that one, nothing fetched" \
        '@/own = @/own (.venv) @/own/.venv/bin/python V debugpy ;; adapter @/own/.venv/bin/python @/own/.venv/lib/pythonV/site-packages/debugpy ;; 0'
    # On another machine: the copy there (the wheel fetched here copied
    # there and unpacked in blit's cache there, above), the adapter run from
    # it with the Python there.
    python3 -m venv --without-pip "$T/q/.venv" || exit 1
    cp "$Q/prog.py" "$T/q/"
    rm -f "$dir/adapter.log"
    run "remotewait;workdir:ssh://box$T/q;debug:program=ssh://box$T/q/prog.py;key:F5;debugwait:stopped;debugdump;key:shift+F5;debugwait:idle;pydump" "ssh://box$T/q/prog.py"
    got="$got ;; $(sed "s#$dir#@#g" "$dir/adapter.log")"
    got=$(printf '%s' "$got" | sed -e 's/python [0-9][0-9.a-z+]*/python V/')
    check "on another machine: blit's copy there, with the Python there" \
        'stopped (breakpoint); stack: main@1; vars: df=DataFrame total=1; output: red plain ;; ssh://box@/there/q = ssh://box@/there/q (.venv) @/there/q/.venv/bin/python V | debugpy@box @/there/.cache/blit/debugpy/1.8.22 ;; adapter @/there/q/.venv/bin/python @/there/.cache/blit/debugpy/1.8.22/debugpy'

    # Debug the pytest test at the cursor (the command, and the Tests tab's
    # d on a test): debugpy runs pytest on it, in the project's folder, with
    # the project's Python, stopping at the first failure; a parametrised
    # test at the cursor, all its cases. (Its pytest a stand-in that lists
    # the tests as pytest 9.2.0 does, the debugger's launch what is looked at.)
    Pt="$dir/pt"
    python3 -m venv --without-pip "$Pt/.venv" || exit 1
    site=$(echo "$Pt"/.venv/lib/python*/site-packages)
    mkdir -p "$site/pytest" "$Pt/tests"
    : >"$site/pytest/__init__.py"
    printf 'print("tests/test_t.py::test_one")\nprint("tests/test_t.py::test_p[1]")\nprint("tests/test_t.py::test_p[2]")\nprint()\nprint("3 tests collected in 0.00s")\n' >"$site/pytest/__main__.py"
    printf 'import pytest\n\n\ndef test_one():\n    pass\n\n\n@pytest.mark.parametrize("n", [1, 2])\ndef test_p(n):\n    assert n\n' >"$Pt/tests/test_t.py"
    launched() {  # (the launch the debugger was asked for)
        grep '^launch' "$dir/dap.log" | sed -e "s#$dir#@#g" -e 's/{"enableAutoVariableSummaries":true,/{/'
    }
    rm -f "$dir/dap.log"
    export BLIT_FAKE_DAP_LOG="$dir/dap.log"
    run "workdir:$Pt;testdump;vim:10G;command:Debug the test at the cursor;debugwait:stopped;debugdump;key:shift+F5;debugwait:idle" "$Pt/tests/test_t.py"
    got="$got ;; $(launched)"
    check "debug the pytest test at the cursor: a parametrised one's cases" \
        'tests/test_t.py: .test_one .test_p[1] .test_p[2] | here: 4. 9. 9. ;; stopped (breakpoint); stack: main@1; vars: df=DataFrame total=1; output: red plain ;; launch {"module":"pytest","args":["tests/test_t.py::test_p","-x","-q"],"cwd":"@/pt","python":"@/pt/.venv/bin/python","type":"python","request":"launch","console":"internalConsole","justMyCode":true}'
    rm -f "$dir/dap.log"
    run "workdir:$Pt;testdump;command:Show Tests;frame;key:Down;key:Down;key:d;debugwait:stopped;key:shift+F5;debugwait:idle" "$Pt/tests/test_t.py"
    got="$(launched)"
    check "and one case from the Tests tab" \
        'launch {"module":"pytest","args":["tests/test_t.py::test_p[2]","-x","-q"],"cwd":"@/pt","python":"@/pt/.venv/bin/python","type":"python","request":"launch","console":"internalConsole","justMyCode":true}'
    unset BLIT_FAKE_DAP_LOG
fi
# And live: real debugpy (installed by uv in a venv of the test's own, its
# files as blit's copy), real pytest, a breakpoint in the code a
# parametrised test calls, each case stopping there. (Skipped where uv
# cannot make the venvs from its cache, with no network.)
L="$dir/live"
mkdir -p "$L/tests" "$dir/dbg"
# (uv with its own data folder, where its Pythons are: not the tests'.)
uvown() { env -u XDG_DATA_HOME uv "$@"; }
if command -v uv >/dev/null 2>&1 && (cd "$L" && uvown venv -q .venv && uvown pip install --offline -q -p .venv/bin/python pytest) >/dev/null 2>&1 &&
    (cd "$dir/dbg" && uvown venv -q .venv && uvown pip install --offline -q -p .venv/bin/python debugpy) >/dev/null 2>&1; then
    (cd "$dir/dbg"/.venv/lib/python*/site-packages && python3 -m zipfile -c "$dir/live.whl" debugpy) || exit 1
    printf 'def double(x):\n    y = x * 2\n    return y\n' >"$L/mod.py"
    : >"$L/conftest.py"
    printf 'import pytest\n\nfrom mod import double\n\n\n@pytest.mark.parametrize("n", [1, 2])\ndef test_double(n):\n    assert double(n) == 2 * n\n' >"$L/tests/test_live.py"
    XDG_DATA_HOME="$dir/data-live" BLIT_DEBUGPY_URL="$dir/live.whl"
    run "workdir:$L;open:$L/mod.py;break:2;open:$L/tests/test_live.py;testdump;vim:8G;command:Debug the test at the cursor;debugwait:stopped;debugdump;key:F5;debugwait:stopped;debugdump;key:F5;debugwait:idle;debugdump" "$L/tests/test_live.py"
    XDG_DATA_HOME="$dir/data" BLIT_DEBUGPY_URL="$dir/debugpy.whl"
    got=$(printf '%s' "$got" | sed -e 's/ in [0-9.]*s / in Ns /')
    check "live: debugpy on pytest, each case stopping in the code it calls" \
        'tests/test_live.py: .test_double[1] .test_double[2] | here: 7. 7. ;; blit: Getting debugpy… ;; stopped (breakpoint); stack: double@2 test_double@8; vars: x=1 ;; stopped (breakpoint); stack: double@2 test_double@8; vars: x=2 ;; idle; output: ..                                                                       [100%] | 2 passed in Ns | [exited with code 0]'
else
    echo "debug: the live pytest case skipped (uv could not make its venvs offline)"
fi

# debugpy itself, where it can be had without the network (from uv's
# cache), in a venv made here: uncaught on, an exception stopped at (its
# traceback as Python writes it, the carets under the expression or not, by
# its version), and the console (Python's error, in a line).
D="$dir/dpy"
mkdir -p "$D"
if command -v uv >/dev/null 2>&1 && uv venv -q --offline "$D/.venv" >/dev/null 2>&1 && uv pip install -q --offline --python "$D/.venv/bin/python" debugpy >/dev/null 2>&1; then
    printf 'def f(d):\n    return d["k"]\n\ntotal = 1\nf({})\n' >"$D/raises.py"
    run "workdir:$D;debug:program=$D/raises.py;key:F5;debugwait:stopped;debugbreaks;debugdump;key:cmd+alt+y;text:total + 1;key:Return;debugwait:answers;text:nope;key:Return;debugwait:answers;docdump:4" "$D/raises.py"
    got=$(printf '%s' "$got" | sed -e 's/ |  *~*^^*//g' -e "s/\(is not defined\)\. Did you mean: 'None'?/\1/")
    check "debugpy: uncaught on, an exception stopped at, the console" \
        "exception uncaught ;; stopped (exception: KeyError); exception: KeyError: 'k' |   File \"@/dpy/raises.py\", line 2, in f |     return d[\"k\"] |   File \"@/dpy/raises.py\", line 5, in <module> |     f({}) | KeyError: 'k'; stack: f@2 <module>@5; vars: d={} special variables= ;; Debug console ;;   › total + 1 ;;   2 ;;   › nope ;;   NameError: name 'nope' is not defined ;; > › |"
else
    echo "debug: debugpy's own cases skipped (debugpy cannot be had here without the network)"
fi

[ -n "$print" ] || [ $failed = 1 ] || echo "debug ok"
exit $failed
