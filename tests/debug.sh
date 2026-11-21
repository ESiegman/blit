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
# (Elsewhere than a Mac gdb is the one blit uses first, and lldb's are minidumps gdb cannot read.)
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
fi
# A .py program: debugpy run with the project's Python (its .venv's, not
# the PATH's), and when that has no debugpy, said so of that Python.
Y="$dir/py"
mkdir -p "$Y/.venv/bin" "$Y/scripts"
printf 'print(1)\n' >"$Y/scripts/one.py"
printf '#!/bin/sh\necho "$0: Error while finding module specification for '"'"'debugpy.adapter'"'"' (ModuleNotFoundError: No module named '"'"'debugpy'"'"')" >&2\nexit 1\n' >"$Y/.venv/bin/python"
chmod +x "$Y/.venv/bin/python"
run "workdir:$Y;debug:program=$Y/scripts/one.py;key:F5;debugwait:idle;debugdump" "$Y/scripts/one.py"
check "a .py program: the project's Python, and debugpy missing there said so" 'idle error: debugpy is not installed for .venv/bin/python'
# On another machine, the same: its .venv's Python there (not the PATH's
# python3 there), chosen in the project's folder there.
mkdir -p "$T/py/.venv/bin" "$T/py/scripts"
cp "$Y/scripts/one.py" "$T/py/scripts/" && cp "$Y/.venv/bin/python" "$T/py/.venv/bin/python"
run "remotewait;workdir:ssh://box$T/py;debug:program=ssh://box$T/py/scripts/one.py;key:F5;debugwait:idle;debugdump" "ssh://box$T/py/scripts/one.py"
check "a .py program on another machine: the project's Python there" 'idle error: debugpy is not installed for .venv/bin/python'

[ -n "$print" ] || [ $failed = 1 ] || echo "debug ok"
exit $failed
