#!/bin/sh
# The wave tab (src/appwave.cpp, waveview.cpp, wavetile.cpp): a .vcd opened
# shows its waves, not its text. On the real dumps in tests/wave (Icarus,
# Verilator, pyvcd; tests/wave/*.txt say what made each): the rows a dump
# starts with, values at the cursor in each radix (names given to values,
# fixed point, float), the keys (pan, zoom, fit, rows, next change,
# markers with deltas, a stretch chosen and zoomed to, a bus opened into its
# bits, groups, removing and undoing), the picker that adds a signal, the
# commands in Command search, the view kept in .blit/waves and brought back
# (rows whose signals the dump has not got left out), the tiles painted and
# their pixels where colour matters (x, z, a busy band, a high line), a dump
# over timeline.max_mb, one that cannot be read, dumps on another
# machine (tests/fake-ssh.sh stands in for ssh): one fetched whole, one too
# large to be yet, an ILA capture's CSV told from another .csv there; and
# ILA captures (Vivado's, of a Basys 3), as VCD and as CSV: their samples,
# radixes (ASCII and values given names too), triggers and windows; and
# times in a terminal (as Icarus and Verilator print them) Cmd+clicked into
# the cursor.
#
# As they change: a dump written again (a rerun) read
# again with the view kept, here and there; one still being written
# followed as it grows, live only while its tab is on show and the file
# changes, drawn at most 30 times a second, nothing drawn while idle; an
# FST still being written said, and read once closed.
#
#   tests/wavetab.sh path/to/blit            run the cases
#   tests/wavetab.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/wavetab.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd -P)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua" BLIT_RECENT_FILE="$dir/recent" BLIT_CACHE_DIR="$dir/cache"
mkdir -p "$dir/work"
cp "$here"/wave/*.vcd "$here"/wave/*.csv "$here"/wave/*.log "$dir/work/"
W="$dir/work"

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
join() { awk 'NR > 1 { printf " ;; " } { printf "%s", $0 }'; }
# run SIZE SCRIPT FILE: what the run printed, its lines joined by " ;; ".
# Each run starts with no view kept of any dump, unless KEEP is set.
run() {
    [ -n "${KEEP:-}" ] || rm -rf "$W/.blit"
    got=$("$blit" --offscreen "$1" --shot "$dir/shot.bmp" --script "$2" "$3" 2>&1 >/dev/null | grep -v '^\[' | sed -e "s#$dir#~#g" | join)
    all=$got
}
ZOOM=
fresh() { rm -rf "$W/.blit"; }
# What was printed, a line at a time again.
lines() { printf '%s' "$all" | awk 'BEGIN { RS = " ;; " } { print }'; }
# The first line of a wavedump (what it shows, the cursor and the stretch on show).
head1() { got=$(lines | head -1); }
# The row of a wavedump whose name begins NAME, as it is printed (> where it is the row chosen).
row() { got=$(lines | grep -E "^>? *$1" | head -1); }
# Lines N to M of a wavedump, its rows without their values.
names() { got=$(lines | sed -n "$1,$2p" | sed -e 's/ = .*//' | join); }

# Opened, it is waves: the rows a dump starts with (a group for each scope,
# aliases left out: u_cnt's clk, en, rst and wrap are tb's), each value at
# the cursor (the start: x and z as Icarus dumped them), the radix of each
# bus (an integer signed).
fresh
run 1600x900 "frame;wavewait;wavedump" "$W/icarus.vcd"
check "icarus.vcd opened: its waves" "icarus.vcd: read, cursor 0 ps, view 0 ps to 2.772 us ;; >tb (open) ;;    tb.done =  ;;    tb.wrap = x ;;    tb.drv[3:0] = z hex ;;    tb.count[7:0] = xx hex ;;    tb.clk = 0 ;;    tb.en = 0 ;;    tb.nib[3:0] = x hex ;;    tb.rst = 1 ;;    tb.wide[95:0] = xxxxxxxxxxxxxxxxxxxxxxxx hex ;;    tb.steps[31:0] = 0 signed ;;    tb.temp = 0 ;;  tb.u_cnt (open) ;;    tb.u_cnt.W = 00000008 hex ;;    tb.u_cnt.count[7:0] = xx hex"
# A dump of one scope: its signals, with no group.
run 1600x900 "frame;wavewait;wavedump" "$W/yosys.vcd"
check "yosys.vcd: one scope, no group" "yosys.vcd: read, cursor 0 ns, view 0 ns to 242 ns ;; >counter.wrap = x ;;  counter.rst = 1 ;;  counter.en = x ;;  counter.count[7:0] = xx hex ;;  counter.clk = x"
# Strings and an event (pyvcd: task and fork scopes, GTKWave's strings).
run 1600x900 "frame;wavewait;wavedump" "$W/pyvcd.vcd"
check "pyvcd.vcd: strings, an event, a real" "pyvcd.vcd: read, cursor 0 ns, view 0 ns to 1.03 us ;; >top (open) ;;    top.clk = 0 ;;  top.uart (open) ;;    top.uart.tx = 1 ;;    top.uart.state = IDLE ;;    top.uart.volts = 3.3 ;;    top.uart.sent =  ;;  top.uart.fifo (open) ;;    top.uart.fifo.data = zzzz hex ;;    top.uart.fifo.level = 0 signed"

# The cursor to the next change of the row chosen (n, N back), and the values there.
fresh
run 1600x900 "frame;wavewait;key:j;key:j;key:j;key:j;key:n;key:n;key:n;key:n;wavedump" "$W/icarus.vcd"
head1
check "n: the next changes of tb.count" "icarus.vcd: read, cursor 35 ns, view 0 ps to 2.772 us"
run 1600x900 "frame;wavewait;key:j;key:j;key:j;key:j;key:n;key:n;key:n;key:n;key:shift+N;wavedump" "$W/icarus.vcd"
head1
check "N: back one" "icarus.vcd: read, cursor 25 ns, view 0 ps to 2.772 us"
row "  tb.count"
check "tb.count at 25 ns" ">  tb.count[7:0] = 02 hex"
# A bit of a bus opened (Enter): n goes to where that bit changes.
run 1600x900 "frame;wavewait;key:j;key:j;key:j;key:j;key:Return;key:j;key:j;key:j;key:j;key:j;key:j;key:j;key:n;key:n;wavedump" "$W/icarus.vcd"
head1
check "a bus opened: n on its bit 1 (from x to 0 at 5 ns, to 1 at 25 ns)" "icarus.vcd: read, cursor 25 ns, view 0 ps to 2.772 us"
row "count\\[1\\]"
check "bit 1 there" ">    count[1] = 1"

# Each radix in turn (r), and names given to values, and fixed point.
fresh
radix="frame;wavewait;key:j;key:j;key:j;key:j;key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:n"
for r in 0 1 2 3 4 5; do
    keys=$(i=0; while [ $i -lt $r ]; do printf 'key:r;'; i=$((i + 1)); done)
    run 1600x900 "$radix;${keys}wavedump" "$W/icarus.vcd"
    row "tb.count"
    case $r in
        0) check "hex" ">  tb.count[7:0] = 13 hex" ;;
        1) check "unsigned" ">  tb.count[7:0] = 19 unsigned" ;;
        2) check "signed" ">  tb.count[7:0] = 19 signed" ;;
        3) check "binary" ">  tb.count[7:0] = 00010011 binary" ;;
        4) check "octal" ">  tb.count[7:0] = 023 octal" ;;
        5) check "ascii" ">  tb.count[7:0] = . ascii" ;;
    esac
done
# Past the radixes a signal has (no names, no Q given, 8 bits: no float), round to hex.
run 1600x900 "$radix;key:r;key:r;key:r;key:r;key:r;key:r;wavedump" "$W/icarus.vcd"
row "tb.count"
check "round again to hex" ">  tb.count[7:0] = 13 hex"
run 1600x900 "$radix;command:Waves: name the row's values…;text:0x13=LOW 20=HIGH;key:Return;wavedump;key:n;wavedump" "$W/icarus.vcd"
got=$(lines | grep 'tb.count\[' | join)
check "names given to values: a value named, and one not (in hex)" ">  tb.count[7:0] = LOW enum ;; >  tb.count[7:0] = HIGH enum"
run 1600x900 "$radix;command:Waves: the row in fixed point…;text:UQ4.4;key:Return;wavedump" "$W/icarus.vcd"
row "tb.count"
check "fixed point UQ4.4: 0x13" ">  tb.count[7:0] = 1.1875 fixed"
run 1600x900 "$radix;command:Waves: the row in fixed point…;text:Q4.3;key:Return;wavedump" "$W/icarus.vcd"
row "tb.count"
check "a Q that is not 8 bits: refused" ">  tb.count[7:0] = 13 hex"
# Signed fixed point and IEEE float on the 32-bit integer (-7 at 220 ns).
float="frame;wavewait;key:0;key:j;key:j;key:j;key:j;key:j;key:j;key:j;key:j;key:j;key:j;key:n;key:n;key:n;key:n"
run 1600x900 "$float;wavedump" "$W/icarus.vcd"
row "tb.steps"
check "steps: -7, signed" ">  tb.steps[31:0] = -7 signed"
run 1600x900 "$float;command:Waves: the row in fixed point…;text:Q30.2;key:Return;wavedump" "$W/icarus.vcd"
row "tb.steps"
check "steps in Q30.2" ">  tb.steps[31:0] = -1.75 fixed"
run 1600x900 "$float;key:r;key:r;key:r;key:r;wavedump" "$W/icarus.vcd"
row "tb.steps"
check "steps as a float (0xfffffff9: a NaN)" ">  tb.steps[31:0] = -nan float"

# The view: pan (h l, H L), zoom about the cursor (= -), fit (0).
fresh
run 1600x900 "frame;wavewait;key:=;wavedump;key:l;wavedump;key:shift+L;wavedump;key:h;wavedump;key:-;wavedump;key:0;wavedump" "$W/icarus.vcd"
got=$(lines | grep '^icarus.vcd' | sed -e 's/^icarus.vcd: read, cursor 0 ps, //' | join)
check "zoomed about the cursor, panned and fitted" "view 0 ps to 1.386 us ;; view 138.6 ns to 1.5246 us ;; view 1.5246 us to 2.9106 us ;; view 1.386 us to 2.772 us ;; view 693 ns to 3.465 us ;; view 0 ps to 2.772 us"

# Markers: m{a-z} at the cursor, '{a-z} back to one, each with its delta
# from the cursor; v chooses a stretch, z zooms to it; z with none zooms
# from the cursor to the last marker.
fresh
marks="frame;wavewait;key:j;key:j;key:j;key:j;key:n;key:n;key:m;key:a;key:n;key:n;key:n;key:m;key:b;key:n"
run 1600x900 "$marks;wavedump" "$W/icarus.vcd"
head1
check "two markers, and their deltas" "icarus.vcd: read, cursor 55 ns, view 0 ps to 2.772 us, markers a=15 ns(-40 ns) b=45 ns(-10 ns)"
run 1600x900 "$marks;key:';key:a;wavedump" "$W/icarus.vcd"
head1
check "'a: the cursor to marker a" "icarus.vcd: read, cursor 15 ns, view 0 ps to 2.772 us, markers a=15 ns(+0 ps) b=45 ns(+30 ns)"
run 1600x900 "$marks;key:z;wavedump" "$W/icarus.vcd"
head1
check "z: from the cursor to the last marker" "icarus.vcd: read, cursor 55 ns, view 44.5 ns to 55.5 ns, markers a=15 ns(-40 ns) b=45 ns(-10 ns)"
run 1600x900 "$marks;key:v;key:n;key:n;key:n;key:n;wavedump;key:z;wavedump" "$W/icarus.vcd"
got=$(lines | grep '^icarus.vcd' | join)
check "v: a stretch chosen, and z zooms to it" "icarus.vcd: read, cursor 105 ns, view 0 ps to 2.772 us, chosen from 55 ns, markers a=15 ns(-90 ns) b=45 ns(-60 ns) ;; icarus.vcd: read, cursor 105 ns, view 52.5 ns to 107.5 ns, markers a=15 ns(-90 ns) b=45 ns(-60 ns)"
run 1600x900 "$marks;key:';key:q;wavedump" "$W/icarus.vcd"
got=$(lines | grep -v '^blit: ' | head -1)
check "'q with no marker q: nothing moves" "icarus.vcd: read, cursor 55 ns, view 0 ps to 2.772 us, markers a=15 ns(-40 ns) b=45 ns(-10 ns)"

# Rows: moved (J K), removed (dd) and back (u), grouped (V, j, gG), a group shut (Enter).
fresh
run 1600x900 "frame;wavewait;key:j;key:shift+J;key:shift+J;wavedump" "$W/icarus.vcd"
names 3 6
check "J twice: tb.done two rows down" "   tb.wrap ;;    tb.drv[3:0] ;; >  tb.done ;;    tb.count[7:0]"
run 1600x900 "frame;wavewait;key:j;key:d;key:d;key:d;key:d;wavedump;key:u;wavedump" "$W/icarus.vcd"
got=$(lines | grep -c 'tb\.done\|tb\.wrap')
check "dd twice, u once: tb.done gone, tb.wrap back" "1"
run 1600x900 "frame;wavewait;key:j;key:j;key:shift+V;key:j;key:j;key:g;key:shift+G;wavedump" "$W/icarus.vcd"
names 11 15
check "V, j j, gG: three rows in a group of their own" ">Group 1 (open) ;;    tb.wrap ;;    tb.drv[3:0] ;;    tb.count[7:0] ;;  tb.u_cnt (open)"
run 1600x900 "frame;wavewait;key:Return;wavedump" "$W/icarus.vcd"
names 2 4
check "Enter on a group: shut" ">tb (shut) ;;  tb.u_cnt (open) ;;    tb.u_cnt.W"

# The picker (/): a signal added after the row chosen; one listed already is chosen.
fresh
run 1600x900 "frame;wavewait;key:j;key:d;key:d;key:/;text:tb.done;findwait;key:Return;wavedump" "$W/icarus.vcd"
names 2 4
check "/: tb.done added back" " tb (open) ;;    tb.wrap ;; >  tb.done"
run 1600x900 "frame;wavewait;key:/;text:u_cnt.W;findwait;key:Return;wavedump" "$W/icarus.vcd"
got=$(lines | grep '^>' | sed -e 's/ = .*//')
check "/: one listed already is chosen" ">  tb.u_cnt.W"

# The mouse (window points: the pixels halved): a click on the traces puts
# the cursor there and chooses the row; a drag along the ruler zooms to
# that stretch.
run 1600x900 "frame;wavewait;click:370,124;release;wavedump" "$W/icarus.vcd"
got=$(lines | grep '^icarus\|^>' | join)
check "a click on tb.count at 500 ns" "icarus.vcd: read, cursor 495 ns, view 0 ps to 2.772 us ;; >  tb.count[7:0] = 2f hex"
run 1600x900 "frame;wavewait;click:400,31;drag:450,31;drag:500,31;release;wavedump" "$W/icarus.vcd"
head1
check "a drag along the ruler" "icarus.vcd: read, cursor 1.21 us, view 632.5 ns to 1.2375 us"

# Without Vim's modes, the same keys.
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/plain.lua"
rm -rf "$W/.blit"
got=$(BLIT_INIT="$dir/plain.lua" "$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "frame;wavewait;key:j;key:j;key:j;key:j;key:n;key:n;key:=;wavedump" "$W/icarus.vcd" 2>&1 >/dev/null | grep '^icarus')
check "without modes: j n =" "icarus.vcd: read, cursor 15 ns, view 7.5 ns to 1.3935 us"

# Its keys are commands: Command search finds them, and runs them.
run 1600x900 "frame;wavewait;key:cmd+shift+p;text:waves pan right a screen;findwait;finddump" "$W/icarus.vcd"
got=$(printf '%s' "$got" | sed -e 's/.* : //' -e 's/) .*/)/')
check "Command search finds the keys" ">Waves: pan right a screen (Shift+L)"
run 1600x900 "frame;wavewait;command:Waves: zoom in about the cursor;command:Waves: zoom in about the cursor;wavedump" "$W/icarus.vcd"
head1
check "a command run from the search" "icarus.vcd: read, cursor 0 ps, view 0 ps to 693 ns"
# (Listed only while a wave tab is in front: elsewhere they are nobody's.)
echo text >"$dir/a.txt"
run 1600x900 "frame;key:cmd+shift+p;text:waves pan right a screen;findwait;finddump" "$dir/a.txt"
case $got in *Waves:*) got=listed ;; *) got="not listed" ;; esac
check "with no wave tab in front, not listed" "not listed"

# The view kept (.blit/waves/icarus.json, beside the dump: no project
# folder here) and brought back as it was.
fresh
run 1600x900 "$marks;key:k;key:r;key:Return;key:=;wavedump" "$W/icarus.vcd"
before=$(printf '%s' "$got" | tr '>' ' ')
[ -f "$W/.blit/waves/icarus.json" ] && got=kept || got="not kept"
check "the view kept in .blit/waves" "kept"
KEEP=1 run 1600x900 "frame;wavewait;wavedump" "$W/icarus.vcd"
got=$(printf '%s' "$got" | tr '>' ' ')
check "and brought back as it was (but for the row chosen)" "$before"
# Rows of a kept view whose signals the dump has not: left out, and said.
sed -e 's/"tb.wrap"/"tb.gone"/' "$W/.blit/waves/icarus.json" >"$dir/k" && mv "$dir/k" "$W/.blit/waves/icarus.json"
KEEP=1 run 1600x900 "frame;wavewait;wavedump" "$W/icarus.vcd"
head1
check "a kept row the dump has not got" "icarus.vcd: read, cursor 55 ns, view 27.5 ns to 1.4135 us, 1 kept rows dropped, markers a=15 ns(-40 ns) b=45 ns(-10 ns)"
# Not changed by hand: nothing kept.
fresh
run 1600x900 "frame;wavewait;key:j;key:k;wavedump" "$W/verilator.vcd"
[ -d "$W/.blit" ] && got=kept || got="nothing kept"
check "a dump only looked at: nothing written" "nothing kept"

# Tiles: painted on workers, none to come once waited for; at this width
# the clock is a busy band in places, zoomed out.
fresh
run 1200x800 "frame;wavewait;wavedump:tiles" "$W/icarus.vcd"
got=$(printf '%s' "$got" | sed -e 's/ painted,/,/' -e 's/^tiles: [0-9]*/tiles: N/' -e 's/last .*//')
check "tiles painted, none to come" "tiles: N, 1 sharp, 0 stretched, 0 to come, "
# Pixels: the x of tb.wide at the start (filled in the x ink, orange),
# the high line of tb.rst (the signal's ink, green), the z of tb.nib at the
# end (a dashed blue line), and tb.clk zoomed out: a busy band, filled
# across six pixels running, where zoomed in it is edges with room between.
pix() {  # pix ROW TIME DX DY [N]: the colours of N pixels running from there (zoomed in first by ZOOM, keys), " | " between
    rm -rf "$W/.blit"
    "$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "frame;wavewait;${ZOOM}wavewait;wavedump:at:$1,$2,$3,$4,${5:-1}" "$W/icarus.vcd" 2>&1 >/dev/null |
        grep '^at ' | sed -e 's/^at [-0-9,]* | //'
}
rgb_is() {  # rgb_is "R G B" WHAT: whether the colour is that
    set -- $1 "$2"
    [ $# = 4 ] || return 1
    case $4 in
        x) [ "$1" -gt $(($3 + 20)) ] && [ "$1" -ge "$2" ] && [ "$2" -gt "$3" ] ;;  # orange: red over green over blue
        line) [ "$2" -gt $(($1 + 20)) ] && [ "$2" -gt $(($3 + 20)) ] ;;  # green
        z) [ "$3" -gt $(($1 + 40)) ] ;;  # blue
        bg) [ "$1" -lt 50 ] && [ "$2" -lt 50 ] && [ "$3" -lt 50 ] ;;
    esac
}
# (Whether any of the colours given, " | " between, is that.)
any_is() {
    printf '%s\n' "$1" | awk 'BEGIN { RS = " [|] " } { print }' >"$dir/colours"
    while read -r c; do rgb_is "$c" "$2" && return 0; done <"$dir/colours"
    return 1
}
# (Six pixels running from DX, all drawn on: none the background.)
filled() {
    c=$(pix "$1" "$2" "$3" 0 6)
    case $c in *none*) return 1 ;; esac
    ! any_is "$c" bg
}
ZOOM="key:=;key:=;key:=;"
c=$(pix 9 16000 0 -8)
got=$(rgb_is "$c" x && echo x || echo "not x: $c")
check "x: tb.wide before 32 ns, filled in the x ink" "x"
c=$(pix 8 6000 0 -10)
got=$(rgb_is "$c" line && echo line || echo "not the line: $c")
check "a high line: tb.rst at the start" "line"
filled 5 200000 0 && got=filled || got="room between"
check "tb.clk zoomed in: edges, room between" "room between"
ZOOM=
any_is "$(pix 7 2500000 0 0 10)" z && got=yes || got=no
check "z: tb.nib at the end, a dashed line in the z ink" "yes"
filled 5 1000000 0 && got=filled || got="room between"
check "a busy band: tb.clk zoomed out" "filled"
got=$(rgb_is "$(pix 8 1500000 0 0)" bg && echo bg || echo "not bg")
check "beside it, tb.rst low: nothing in the middle of the row" "bg"

# A dump past timeline.max_mb: what it read shows, and the tab says why it stopped.
bin="$(cd "$(dirname "$blit")" && pwd)/blit_timeline_test"
if [ -x "$bin" ]; then
    "$bin" gen "$dir/big.vcd" 48
    printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.timeline.max_mb = 16\n' >"$dir/small.lua"
    got=$(BLIT_INIT="$dir/small.lua" "$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "frame;wavewait;wavedump" "$dir/big.vcd" 2>&1 >/dev/null | grep '^big.vcd' | sed -e 's/cursor.*stopped/stopped/' -e 's/read up to .*/read up to …/')
    check "a dump past timeline.max_mb" "big.vcd: read, stopped: the dump needs more memory than timeline.max_mb allows: read up to …"
fi
# An FST (Icarus's and Verilator's of the same runs as their VCDs): the
# same run, the same view, and its values at a time the same; except that
# Verilator's FST says steps is a signed integer, which its VCD cannot, so
# that it is shown signed.
for t in icarus verilator; do
    run 1600x900 "frame;wavewait;wavedump" "$here/wave/$t.vcd"
    v=$(printf '%s' "$got" | sed -e "s/$t.vcd/X/g")
    [ $t = verilator ] && v=$(printf '%s' "$v" | sed -e 's/tb.steps\[31:0\] = 00000000 hex/tb.steps[31:0] = 0 signed/')
    run 1600x900 "frame;wavewait;wavedump" "$here/wave/$t.fst"
    got=$(printf '%s' "$got" | sed -e "s/$t.fst/X/g")
    check "$t's FST: as its VCD" "$v"
done
# One that cannot be read.
run 1600x900 "frame;wavewait;wavedump" "$W/none.vcd"
check "a dump that is not there" "none.vcd: cannot show it: No such file or directory"

# Run again (the folder worked in, watched): a dump written again while
# it is open is read again, keeping the rows, the radix, the marker, the
# cursor and the whole run on show (the run's new end); the values are the
# new run's (Icarus's of the same design, built without DUMPOFF:
# tests/wave/icarus.txt). A row whose signal the new run has not got is
# left out, and said, as a kept view's is.
watched="workdir:$W;panel;frame;wavewait"
marked="key:j;key:j;key:j;key:j;key:r;key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:m;key:a;key:n;key:n"
fresh; cp "$W/icarus.vcd" "$W/run.vcd"
run 1600x900 "$watched;$marked;wavedump;lua:os.execute('cp $W/icarus-rerun.vcd $W/run.vcd');watchwait;wavewait;wavedump;wavedump:load" "$W/run.vcd"
got=$(lines | grep -E '^(run.vcd|>  tb.count|   tb.en =|load)' | sed -e 's/[0-9]* bytes/N bytes/' | join)
check "written again while open: read again, the view kept" "run.vcd: read, cursor 105 ns, view 0 ps to 2.772 us, markers a=72 ns(-33 ns) ;; >  tb.count[7:0] = 10 unsigned ;;    tb.en = 1 ;; run.vcd: read, cursor 105 ns, view 0 ps to 2.737 us, markers a=72 ns(-33 ns) ;; >  tb.count[7:0] = 9 unsigned ;;    tb.en = 0 ;; load: run.vcd on show, N bytes read, grown 0 times, read again 1 times, waiting for more"
fresh; cp "$W/icarus.vcd" "$W/run.vcd"
run 1600x900 "$watched;key:=;lua:os.execute('cp $W/icarus-top.vcd $W/run.vcd');watchwait;wavewait;wavedump" "$W/run.vcd"
got=$(lines | grep -E '^(run.vcd| *tb.u_cnt)' | join)
check "written again without two of its signals: they are left out, and said" "run.vcd: read, cursor 0 ps, view 0 ps to 1.386 us, 2 kept rows dropped ;;  tb.u_cnt (open)"

# A dump still being written (the first half of Icarus's, cut in a line):
# what is there is shown, and what is added is read as it comes, the
# whole run on show following its end. A view not at the end stays where
# it is.
size=$(wc -c <"$W/icarus.vcd" | tr -d ' ')
head -c $((size / 2)) "$W/icarus.vcd" >"$dir/half.vcd"
tail -c +$((size / 2 + 1)) "$W/icarus.vcd" >"$dir/rest.vcd"
fresh; cp "$dir/half.vcd" "$W/grow.vcd"
run 1600x900 "$watched;wavedump;lua:os.execute('cat $dir/rest.vcd >>$W/grow.vcd');watchwait;wavewait;wavedump;wavedump:load;wavedump:tiles;wavedump:at:5,2600000,0,0,6" "$W/grow.vcd"
got=$(lines | grep -E '^(grow.vcd|load)' | join)
check "followed as it grows: the end followed" "grow.vcd: read, cursor 0 ps, view 0 ps to 1.33 us ;; grow.vcd: read, following, cursor 0 ps, view 0 ps to 2.772 us ;; load: grow.vcd on show, $size bytes read, grown 1 times, read again 0 times, waiting for more"
got=$(lines | grep '^tiles' | sed -e 's/^tiles: [0-9]* painted, [0-9]* sharp/tiles: sharp/' -e 's/, last .*//')
check "(its tiles painted again, sharp)" "tiles: sharp, 0 stretched, 0 to come"
c=$(lines | grep '^at ' | sed -e 's/^at [-0-9,]* | //')
any_is "$c" bg && got="the end not drawn: $c" || got="drawn to the end"
check "(tb.clk drawn to the new end)" "drawn to the end"
fresh; cp "$dir/half.vcd" "$W/grow.vcd"
run 1600x900 "$watched;key:=;key:l;wavedump;lua:os.execute('cat $dir/rest.vcd >>$W/grow.vcd');watchwait;wavewait;wavedump" "$W/grow.vcd"
got=$(lines | grep -E '^grow.vcd' | join)
check "followed, a view not at the end: it stays" "grow.vcd: read, cursor 0 ps, view 66.5 ns to 731.5 ns ;; grow.vcd: read, following, cursor 0 ps, view 66.5 ns to 731.5 ns"

# Live, under the rule: while it is written (twenty pieces, 50 ms apart)
# the tab reads it and draws it again (with what the watched folder draws
# anyway) no more than 30 times a second; once it is not, nothing is drawn
# and nothing read. Nor is anything drawn before it is.
cat >"$dir/writer.sh" <<'EOF'
size=$(wc -c <"$1" | tr -d ' ')
piece=$(( (size + 19) / 20 ))
i=0
while [ $i -lt 20 ]; do
    dd if="$1" bs="$piece" skip=$i count=1 2>/dev/null >>"$2"
    i=$((i + 1))
    perl -e 'select(undef, undef, undef, 0.05)'
done
EOF
fresh; cp "$dir/half.vcd" "$W/grow.vcd"
run 1600x900 "$watched;frame;waveframes:300;waveframes:500;lua:os.execute('sh $dir/writer.sh $dir/rest.vcd $W/grow.vcd &');waveframes:2500;waveframes:1000;wavedump" "$W/grow.vcd"
got=$(lines | sed -n 2p)  # (the first: what starting up still had to draw)
check "nothing drawn before it is written to" "0 frames drawn, 0 bytes read"
set -- $(lines | sed -n 3p)
[ "$1" -le 75 ] && [ "$4" -gt 0 ] && got="read, at most 30 frames a second" || got="$1 frames, $4 bytes"
check "while it is written: read, at most 30 frames a second" "read, at most 30 frames a second"
got=$(lines | sed -n 4p)
check "written no more: nothing drawn, nothing read" "0 frames drawn, 0 bytes read"
got=$(lines | sed -n 5p)
check "(and all of it read)" "grow.vcd: read, following, cursor 0 ps, view 0 ps to 2.772 us"

# Opened while still empty (as Verilator 5.054 leaves a VCD it writes
# until its buffer fills): waited at, not said to be no dump, and read
# once something is written.
fresh; : >"$W/grow.vcd"
run 1600x900 "$watched;wavedump;lua:os.execute('cat $dir/half.vcd >>$W/grow.vcd');watchwait;wavewait;wavedump" "$W/grow.vcd"
got=$(lines | grep '^grow.vcd' | join)
check "empty when opened: waited at, then read" "grow.vcd: nothing in it yet ;; grow.vcd: read, following, cursor 0 ps, view 0 ps to 1.33 us"

# Behind another tab it is not read; brought to the front, it is.
echo text >"$W/notes.txt"
fresh; cp "$dir/half.vcd" "$W/grow.vcd"
run 1600x900 "$watched;open:$W/notes.txt;frame;lua:os.execute('cat $dir/rest.vcd >>$W/grow.vcd');watchwait;frame;wavewait;wavedump:load;open:$W/grow.vcd;frame;wavewait;wavedump:load" "$W/grow.vcd"
got=$(lines | sed -e 's/[0-9]* bytes/N bytes/' | join)
check "behind another tab: read once it is on show" "load: grow.vcd behind another tab, N bytes read, grown 0 times, read again 0 times, changed, waiting for more ;; load: grow.vcd on show, N bytes read, grown 1 times, read again 0 times, waiting for more"

# An FST its writer has not closed (Icarus's, caught while it ran: its
# header alone; Verilator's, a header of zeros): said, and read once it
# has been (as libfst can only then).
for t in icarus verilator; do
    fresh; cp "$here/wave/$t-writing.fst" "$W/sim.fst"
    run 1600x900 "$watched;wavedump;lua:os.execute('cp $here/wave/$t.fst $W/sim.fst');watchwait;wavewait;wavedump" "$W/sim.fst"
    got=$(lines | grep '^sim.fst' | join)
    check "$t: an FST still being written, then closed" "sim.fst: still being written ;; sim.fst: read, cursor 0 ps, view 0 ps to 2.772 us"
done

# On another machine: fetched whole, in parts (a 5 MB one too, as the
# reply may be no larger than 1 MB), and one larger than
# wave.remote_fetch_mb, which is not shown.
T="$dir/fake"
mkdir -p "$T/work"
cp "$here/wave/verilator.vcd" "$T/work/"
awk 'BEGIN { print "$timescale 1ns $end"; print "$scope module t $end"; print "$var wire 1 ! a $end"; print "$upscope $end"; print "$enddefinitions $end";
             for (i = 0; i < 500000; i++) { print "#" i * 10; print (i % 2) "!" } }' >"$T/work/large.vcd"
export BLIT_SSH="$here/fake-ssh.sh" BLIT_FAKE_HOME="$T"
run 1600x900 "remotewait;frame;wavewait;wavedump" "ssh://box$T/work/verilator.vcd"
got=$(printf '%s' "$got" | sed -e 's/ ;; .*//')
check "another machine's dump, fetched whole" "verilator.vcd: read, cursor 0 ps, view 0 ps to 2.772 us"
# Written again there: once blit looks again (as when a run there ends,
# or the window comes back to the front), its time stamp asked there, it
# is read again, the view kept.
cp "$here/wave/icarus.vcd" "$T/work/run.vcd"
touch -t 202601010000 "$T/work/run.vcd"
run 1600x900 "remotewait;frame;wavewait;key:m;key:a;reload;wavewait;wavedump;lua:os.execute('cp $here/wave/icarus-rerun.vcd $T/work/run.vcd');reload;wavewait;wavedump" "ssh://box$T/work/run.vcd"
got=$(lines | grep '^run.vcd' | join)
check "another machine's dump written again: read again" "run.vcd: read, cursor 0 ps, view 0 ps to 2.772 us, markers a=0 ps(+0 ps) ;; run.vcd: read, cursor 0 ps, view 0 ps to 2.737 us, markers a=0 ps(+0 ps)"
run 1600x900 "remotewait;frame;wavewait;wavedump" "ssh://box$T/work/large.vcd"
got=$(printf '%s' "$got" | sed -e 's/ ;; .*//')
check "another machine's dump over 4 MB, fetched in parts" "large.vcd: read, cursor 0 ns, view 0 ns to 4.99999 ms"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.wave.remote_fetch_mb = 1\n' >"$dir/fetch1.lua"
got=$(BLIT_INIT="$dir/fetch1.lua" "$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "remotewait;frame;wavewait;wavedump" "ssh://box$T/work/large.vcd" 2>&1 >/dev/null | grep '^large.vcd')
check "one over wave.remote_fetch_mb: not shown, and why" "large.vcd: cannot show it: waves larger than 1 MB on another machine are not shown (wave.remote_fetch_mb)"
# Another machine's FST, which libfst reads by its path: fetched in parts
# into a file in the cache folder here, read, and that file removed.
cp "$here/wave/verilator.fst" "$T/work/"
run 1600x900 "remotewait;frame;wavewait;wavedump" "ssh://box$T/work/verilator.fst"
got=$(printf '%s' "$got" | sed -e 's/ ;; .*//')
check "another machine's FST, fetched and read" "verilator.fst: read, cursor 0 ps, view 0 ps to 2.772 us"
got=$(ls "$dir/cache/waves" 2>/dev/null | wc -l | tr -d ' ')
check "(its copy here removed once read)" "0"
# Another machine's .csv: an ILA capture's (told by its first bytes,
# asked for alone as its tab opens) is fetched as any dump there is, and
# opens as its waves, in its radixes, with its triggers and windows; any
# other opens as text; a capture over wave.remote_fetch_mb is not shown.
cp "$here/wave/ila_windows.csv" "$here/wave/ila_radix.csv" "$T/work/"
printf 'a,b\n1,2\n' >"$T/work/table.csv"
awk 'NR <= 2 { print } END { for (i = 0; i < 60000; i++) printf "%d,%d,0,%02x,0,0,%d,0000\n", i, i, i % 256, i % 4096 }' "$here/wave/ila_one.csv" >"$T/work/big.csv"
run 1600x900 "remotewait;frame;wavewait;key:n;wavedump" "ssh://box$T/work/ila_windows.csv"
check "another machine's ILA capture: its waves, triggers, windows and radixes" "ila_windows.csv: read, cursor 33, view 0 to 1023, triggers at 32 288 544 800, windows from 256 512 768 ;; >dut.count[7:0] = 0 signed ;;  dut.wrap = 0 ;;  dut.phase[1:0] = 2 octal ;;  dut.sum[11:0] = 255 unsigned ;;  dut.sw_q[3:0] = 0000 binary"
# (Shown in two splits while it was coming: the other split's view goes, with the split.)
run 1600x900 "key:cmd+\\;open:ssh://box$T/work/ila_windows.csv;dump;remotewait;frame;wavewait;dump" "ssh://box$T/work/ila_windows.csv"
check "another machine's capture shown in two splits as it came: one wave tab" "H([ila_windows.csv*] | >[ila_windows.csv*])0.50 ;; >[ila_windows.csv*]"
run 1600x900 "remotewait;frame;wavewait;wavedump" "ssh://box$T/work/ila_radix.csv"
check "another machine's capture in ASCII and with names" "ila_radix.csv: read, cursor 100, view 0 to 1023, triggers at 100 ;; >dut.count[7:0] = 40 hex ;;  dut.ch[7:0] = z ascii ;;  dut.pair[7:0] = H ascii ;;  dut.odd[7:0] = z ascii ;;  dut.state[2:0] = 7"
run 1600x900 "remotewait;frame;wavedump;rowsdump:2" "ssh://box$T/work/table.csv"
check "another machine's CSV that is not a capture: text" "no wave tab ;; rows | a,b | 1,2"
run 1600x900 "remotewait;frame;wavewait;wavedump" "ssh://box$T/work/big.csv"
got=$(printf '%s' "$got" | sed -e 's/ ;; .*//')
check "another machine's capture over 1 MB, fetched in parts" "big.csv: read, cursor 0, view 0 to 59999"
got=$(BLIT_INIT="$dir/fetch1.lua" "$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "remotewait;frame;wavewait;wavedump" "ssh://box$T/work/big.csv" 2>&1 >/dev/null | grep '^big.csv')
check "a capture over wave.remote_fetch_mb: not shown, and why" "big.csv: cannot show it: waves larger than 1 MB on another machine are not shown (wave.remote_fetch_mb)"

# ILA captures (tests/wave/ila.txt): their times are samples; the cursor
# starts at the trigger; Vivado's own _TRIGGER, _WINDOW and _GAP are not
# rows (the triggers and windows are drawn instead); the CSV form's radix
# row is each probe's radix; the form with no radix row is not shown, and
# why. A CSV that is not a capture opens as text.
fresh
run 1600x900 "frame;wavewait;wavedump" "$W/ila_one.vcd"
check "an ILA capture's VCD: samples, the cursor at the trigger" "ila_one.vcd: read, cursor 100, view 0 to 1023, triggers at 100 ;; >dut.count[7:0] = 40 hex ;;  dut.wrap = 0 ;;  dut.phase[1:0] = 0 hex ;;  dut.sum[11:0] = 03f hex ;;  dut.sw_q[3:0] = 0 hex"
run 1600x900 "frame;wavewait;wavedump" "$W/ila_one.csv"
check "its CSV: the same, in the radixes it gives" "ila_one.csv: read, cursor 100, view 0 to 1023, triggers at 100 ;; >dut.count[7:0] = 40 hex ;;  dut.wrap = 0 ;;  dut.phase[1:0] = 0 hex ;;  dut.sum[11:0] = 63 unsigned ;;  dut.sw_q[3:0] = 0000 binary"
run 1600x900 "frame;wavewait;key:n;key:n;key:m;key:a;wavedump" "$W/ila_windows.csv"
check "four windows: a trigger each, n from the first, a marker" "ila_windows.csv: read, cursor 34, view 0 to 1023, markers a=34(+0), triggers at 32 288 544 800, windows from 256 512 768 ;; >dut.count[7:0] = 1 signed ;;  dut.wrap = 0 ;;  dut.phase[1:0] = 2 octal ;;  dut.sum[11:0] = 0 unsigned ;;  dut.sw_q[3:0] = 0000 binary"
run 1600x900 "frame;wavewait;wavedump" "$W/ila_one-legacy.csv"
check "the CSV form with no radix row: not shown, and why" "ila_one-legacy.csv: cannot show it: no radix row (-legacy_csv_file): write it with -csv_file"
printf 'a,b\n1,2\n' >"$W/table.csv"
run 1600x900 "frame;wavedump" "$W/table.csv"
check "a CSV that is not a capture: text" "no wave tab"
# Probes in ASCII and with names for their values (tests/wave/ila_radix.txt):
# ASCII's start in ASCII, a character each (a byte not printable x), a
# wider one as its lowest byte; r goes round from there; state's values
# as written, a name or a value with none (7, in hex).
fresh
run 1600x900 "frame;wavewait;wavedump;key:j;key:shift+N;wavedump;key:n;key:n;key:n;wavedump;key:j;key:j;key:j;key:n;wavedump" "$W/ila_radix.csv"
check "ASCII and names: at the trigger, a byte not printable, a comma, a name" "ila_radix.csv: read, cursor 100, view 0 to 1023, triggers at 100 ;; >dut.count[7:0] = 40 hex ;;  dut.ch[7:0] = z ascii ;;  dut.pair[7:0] = H ascii ;;  dut.odd[7:0] = z ascii ;;  dut.state[2:0] = 7 ;; ila_radix.csv: read, cursor 97, view 0 to 1023, triggers at 100 ;;  dut.count[7:0] = 3d hex ;; >dut.ch[7:0] = x ascii ;;  dut.pair[7:0] = x ascii ;;  dut.odd[7:0] = x ascii ;;  dut.state[2:0] = 7 ;; ila_radix.csv: read, cursor 102, view 0 to 1023, triggers at 100 ;;  dut.count[7:0] = 42 hex ;; >dut.ch[7:0] = i ascii ;;  dut.pair[7:0] = , ascii ;;  dut.odd[7:0] = i ascii ;;  dut.state[2:0] = IDLE ;; ila_radix.csv: read, cursor 117, view 0 to 1023, triggers at 100 ;;  dut.count[7:0] = 51 hex ;;  dut.ch[7:0] = H ascii ;;  dut.pair[7:0] = i ascii ;;  dut.odd[7:0] = H ascii ;; >dut.state[2:0] = LOAD"
r6="key:r;wavedump;key:r;wavedump;key:r;wavedump;key:r;wavedump;key:r;wavedump;key:r;wavedump"
fresh
run 1600x900 "frame;wavewait;key:j;$r6" "$W/ila_radix.csv"
got=$(lines | grep 'dut.ch' | join)
check "r on an ASCII probe: hex, unsigned, signed, binary, octal, ASCII" ">dut.ch[7:0] = 7a hex ;; >dut.ch[7:0] = 122 unsigned ;; >dut.ch[7:0] = 122 signed ;; >dut.ch[7:0] = 01111010 binary ;; >dut.ch[7:0] = 172 octal ;; >dut.ch[7:0] = z ascii"
fresh
run 1600x900 "frame;wavewait;wavedump;key:j;key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:n;wavedump" "$W/ila_radix2.csv"
check "a name among ASCII's characters, names in binary, octal, signed" "ila_radix2.csv: read, cursor 20, view 0 to 1023, triggers at 20 532, windows from 512 ;; >dut.count[7:0] = 100 octal ;;  dut.ch[7:0] = z ;;  dut.pair[15:0] = 31304 signed ;;  dut.odd[11:0] = 3962 unsigned ;;  dut.state[2:0] = 111 ;; ila_radix2.csv: read, cursor 28, view 0 to 1023, triggers at 20 532, windows from 512 ;;  dut.count[7:0] = 110 octal ;; >dut.ch[7:0] = AYE ;;  dut.pair[15:0] = 16674 signed ;;  dut.odd[11:0] = 1857 unsigned ;;  dut.state[2:0] = IDLE"
# The trigger's line, in the errors' ink (red), and the cursor's, moved
# ten samples on (n), in its own: four pixels about each.
fresh
tens="key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:n"
got=$("$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "frame;wavewait;key:0;$tens;wavewait;wavedump:at:8,100,-1,0,4;wavedump:at:8,110,-1,0,4" "$W/ila_one.vcd" 2>&1 >/dev/null | grep '^at ' | sed -e 's/^at [-0-9,]* | //' | join)
reds() { printf '%s\n' "$1" | awk 'BEGIN { RS = " [|] " } NF == 3 && $1 > $2 + 60 && $1 > $3 + 30 { n++ } END { print n + 0 }'; }
[ "$(reds "${got%% ;; *}")" -gt 0 ] && c=red || c="not red: ${got%% ;; *}"
[ "$(reds "${got#* ;; }")" = 0 ] && c="$c, the cursor not" || c="$c, the cursor red too: ${got#* ;; }"
got=$c
check "the trigger's line at sample 100: red" "red, the cursor not"

# Times in a terminal, as testbenches print them (tests/wave/times.txt:
# make/times.v under Icarus and Verilator, times.vcd Icarus's dump of the
# same run, in 1ps): Cmd+click on one sets the wave tab's cursor there, a
# bare number in the dump's unit, one with a unit converted. The terminal
# is a split below the wave tab (Ctrl+`), and stays there to be clicked
# again; a shell of the test's own prints the logs.
export BLIT_TERM_SHELL="exec env PS1='\$ ' /bin/sh -i"
below="frame;wavewait;key:ctrl+\`;frame;termwait:\$;key:space"
clicks() {  # clicks LOG TEXT...: the cursor after each Cmd+click, " ;; " between
    log=$1
    shift
    s="$below;text:cat $W/$log;key:Return;termwait:finish"
    for c in "$@"; do s="$s;termclick:$c;frame;wavedump"; done
    rm -rf "$W/.blit"
    got=$("$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "$s" "$W/times.vcd" 2>&1 >/dev/null | grep -v '^\[' |
        sed -n -e 's/^times.vcd: read, cursor \(.*\), view .*/\1/p' -e 's/^\(blit: .*\)/\1/p' -e 's/^\(termclick: .*\)/\1/p' | join)
}
clicks times-icarus.log "@ 50000" "62500]" "73000 count" "93.00 ns" "94.00 ns" "0.124us" "0.129us" "128500" "131500 (1ps)"
check "Icarus's: @ %0t, [%0t], %t, \$timeformat's, Time:, \$error's Time and \$finish's" "50 ns ;; 62.5 ns ;; 73 ns ;; 93 ns ;; 94 ns ;; 124 ns ;; 129 ns ;; 128.5 ns ;; 131.5 ns"
clicks times-verilator.log "@ 50000" "62500]" "73000 count" "93.00 ns" "0.1240us" "0.1290us]" "132ns"
check "Verilator's: the same, and its \$finish past the dump's end, said" "50 ns ;; 62.5 ns ;; 73 ns ;; 93 ns ;; 124 ns ;; 129 ns ;; blit: 132 ns is not in times.vcd (0 ps to 131.5 ns) ;; 129 ns"
# Not times: a count after "count =", a version, a rate, the word Time itself.
clicks times-verilator.log "9" "5.054" "4.249 ms/s" "Time:" "Verilog \$stop"
check "not times: nothing moves" "0 ps ;; 0 ps ;; 0 ps ;; 0 ps ;; 0 ps"
# A path:line in the same lines is still a path (it opens the file).
rm -rf "$W/.blit"
got=$("$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "$below;text:cat $W/times-icarus.log;key:Return;termwait:finish;termclick:times.v:26;frame;dump" "$W/times.vcd" 2>&1 >/dev/null | grep -v '^\[' | tail -1)
case $got in *"No file"*times.v*|*"[times.v"*) got="a path" ;; esac
check "times.v:26 beside them: a path, not a time" "a path"
# The wave tab behind the terminal in the same split: brought to the front.
rm -rf "$W/.blit"
got=$("$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "frame;wavewait;term;frame;termwait:\$;text:cat $W/times-icarus.log;key:Return;termwait:finish;dump;termclick:94.00 ns;frame;dump;wavedump" "$W/times.vcd" 2>&1 >/dev/null |
    grep -v '^\[' | grep -e '^>\[' -e '^times.vcd' | sed -e 's/, view .*//' | join)
check "a wave tab behind: shown" ">[times.vcd Terminal*] ;; >[times.vcd* Terminal] ;; times.vcd: read, cursor 94 ns"
# With no wave tab open, a time is no target: the click is a click.
got=$("$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "term;frame;termwait:\$;text:cat $W/times-icarus.log;key:Return;termwait:finish;termclick:@ 50000;frame;dump" 2>&1 >/dev/null | grep -v '^\[' | tail -1)
check "no wave tab: the click is a click" ">[untitled Terminal*]"
# An ILA capture counts samples: a bare number is one, a time with a unit is none.
rm -rf "$W/.blit"
got=$("$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "$below;text:echo @ 300 and 5 ns;key:Return;termwait:and 5;termclick:@ 300;frame;wavedump;termclick:5 ns;frame;wavedump" "$W/ila_one.vcd" 2>&1 >/dev/null |
    grep '^ila_one.vcd' | sed -e 's/, view .*//' | join)
check "an ILA capture: @ 300 is sample 300, 5 ns nothing" "ila_one.vcd: read, cursor 300 ;; ila_one.vcd: read, cursor 300"

[ $failed = 0 ] && echo "wavetab ok"
exit $failed
