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
# over timeline.max_mb, one that cannot be read, and dumps on another
# machine (tests/fake-ssh.sh stands in for ssh): one fetched whole, one too
# large to be yet.
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
export BLIT_INIT="$dir/init.lua" BLIT_RECENT_FILE="$dir/recent"
mkdir -p "$dir/work"
cp "$here"/wave/*.vcd "$dir/work/"
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
# One that cannot be read.
run 1600x900 "frame;wavewait;wavedump" "$W/none.vcd"
check "a dump that is not there" "none.vcd: cannot show it: No such file or directory"

# On another machine: fetched whole (no larger than a reply may be), and
# one larger, which is not shown yet.
T="$dir/fake"
mkdir -p "$T/work"
cp "$here/wave/verilator.vcd" "$T/work/"
awk 'BEGIN { print "$timescale 1ns $end"; print "$scope module t $end"; print "$var wire 1 ! a $end"; print "$upscope $end"; print "$enddefinitions $end";
             for (i = 0; i < 500000; i++) { print "#" i * 10; print (i % 2) "!" } }' >"$T/work/large.vcd"
export BLIT_SSH="$here/fake-ssh.sh" BLIT_FAKE_HOME="$T"
run 1600x900 "remotewait;frame;wavewait;wavedump" "ssh://box$T/work/verilator.vcd"
got=$(printf '%s' "$got" | sed -e 's/ ;; .*//')
check "another machine's dump, fetched whole" "verilator.vcd: read, cursor 0 ps, view 0 ps to 2.772 us"
run 1600x900 "remotewait;frame;wavewait;wavedump" "ssh://box$T/work/large.vcd"
check "another machine's dump over 4 MB" "large.vcd: cannot show it: waves larger than 4 MB on another machine are not shown yet"

[ $failed = 0 ] && echo "wavetab ok"
exit $failed
