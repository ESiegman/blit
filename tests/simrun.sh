#!/bin/sh
# Simulation runs and their waves (src/simrun.cpp, src/appsim.cpp): a task
# that runs Verilator, Icarus or SymbiYosys is a simulation (its row says
# so); as a run of one ends, the dump it wrote opens in the wave tab beside
# the run's terminal, or is read again if it is open (the view kept); which
# dump: the one its output names (Icarus's vvp, SymbiYosys's counterexample
# before its cover trace), else one its command names (a word, or
# $dumpfile in a source it names), else the newest written during the run,
# and none written before it. "Open the last run's waves" (gW): that dump
# again, or with no run yet the project's newest.
#
# Each simulator is a stand-in (tests/fake-sim.sh) writing a sample of the
# real tool's output (tests/sim, each saying what made it) and copies of
# the dumps the real tool wrote with it (tests/wave).
#
#   tests/simrun.sh path/to/blit            run the cases
#   tests/simrun.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/simrun.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd -P)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
export BLIT_RECENT_FILE="$dir/recent" BLIT_CACHE_DIR="$dir/cache"
export HOME="$dir/home"
export FAKE_SIM="$dir/fake"
mkdir -p "$HOME" "$FAKE_SIM"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua"

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

# The stand-ins, outside the project, each run by its whole path.
B="$dir/bin"
mkdir -p "$B"
for t in verilator iverilog vvp sby Vtb runsim; do
    printf '#!/bin/sh\nexec sh "%s" %s "$@"\n' "$here/fake-sim.sh" "$t" >"$B/$t"
    chmod +x "$B/$t"
done
# What each stand-in does in the next run: sim NAME SAMPLE ["FROM TO"...].
sim() {
    name=$1 sample=$2
    shift 2
    rm -f "$FAKE_SIM/$name.out" "$FAKE_SIM/$name.files"
    [ -n "$sample" ] && printf '%s\n' "$here/sim/$sample.txt" >"$FAKE_SIM/$name.out"
    for f in "$@"; do printf '%s\n' "$f" >>"$FAKE_SIM/$name.files"; done
}
W="$here/wave"

# The project: the design its dumps were made from, and a task for each
# simulator, as tests/wave/make/make.sh runs them.
P="$dir/proj"
fresh() {
    rm -rf "$P"
    mkdir -p "$P/.blit"
    cp "$here/wave/make/counter.sv" "$P/"
    printf 'module tb;\n    initial begin\n        $dumpfile("waves.vcd");\n        $dumpvars(0, tb);\n    end\nendmodule\n' >"$P/tb.v"
    cat >"$P/.blit/tasks.json" <<EOF
{ "tasks": [
  { "label": "Icarus", "command": "$B/iverilog -g2012 '-DDUMPVARS=\"icarus.vcd\"' -DDUMPOFF -o tb.vvp counter.sv && $B/vvp -n tb.vvp" },
  { "label": "Icarus FST", "command": "$B/iverilog -g2012 '-DDUMPVARS=\"icarus.fst\"' -DDUMPOFF -o tb-fst.vvp counter.sv && $B/vvp -n tb-fst.vvp -fst" },
  { "label": "Verilator", "command": "$B/verilator --binary --trace -Wno-fatal '-DDUMPVARS=\"verilator.vcd\"' -DDUMPOFF --top-module tb counter.sv && ./obj_dir/Vtb" },
  { "label": "Formal", "command": "$B/sby -f counter.sby" },
  { "label": "Bench", "command": "$B/runsim tb.v", "kind": "simulation" },
  { "label": "Lint", "command": "$B/runsim counter.sv" }
] }
EOF
}
# run SCRIPT: the project worked in, counter.sv in front; what was printed, its lines joined by " ;; ".
run() {
    got=$("$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "workdir:$P;panel;frame;$1" "$P/counter.sv" 2>&1 >/dev/null | grep -v '^\[' | sed -e "s#$dir#~#g" | join)
}
# The first line of each wavedump.
heads() { got=$(printf '%s' "$got" | awk 'BEGIN { RS = " ;; " } /^[a-z_.-]*\.(vcd|fst): / { print }' | join); }
ended="buildwait;simwait;wavewait"

# Each is a simulation, said on its row; one that runs none of them is
# not, unless it says it is.
fresh
run "buildpanel;builddump:rows"
got=$(printf '%s' "$got" | sed -e 's/: [^|]*simulation[^|]*/: simulation/g' -e 's/: [^|]*runsim[^|]*/: not/g')
check "the kinds: from the commands, and as said" "Icarus: simulation| Icarus FST: simulation| Verilator: simulation| Formal: simulation| Bench: simulation| Lint: not"

# Icarus: vvp names its dump; the wave tab opens it beside the run's
# terminal (in the split with the file, the keyboard still there).
fresh
sim iverilog ""
sim vvp vvp "$W/icarus.vcd icarus.vcd"
run "buildrun:Icarus;$ended;wavedump;tabsdump"
got=$(printf '%s' "$got" | awk 'BEGIN { RS = " ;; " } /^(icarus|counter)/ { print }' | join)
check "Icarus: its dump opened, beside the terminal" "icarus.vcd: read, cursor 0 ps, view 0 ps to 2.772 us ;; counter.sv | icarus.vcd"

# Run again (the same design built without DUMPOFF): the tab open is read
# again, the rows, radix, marker and cursor kept, its values the new run's.
fresh
sim iverilog ""
sim vvp vvp "$W/icarus.vcd icarus.vcd"
keys="key:j;key:j;key:j;key:j;key:r;key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:n;key:m;key:a;key:n;key:n"
mkdir -p "$dir/next"
printf '%s\n' "$here/sim/vvp-rerun.txt" >"$dir/next/vvp.out"
printf '%s %s\n' "$W/icarus-rerun.vcd" icarus.vcd >"$dir/next/vvp.files"
again="lua:os.execute('cp $dir/next/vvp.out $dir/next/vvp.files $FAKE_SIM/')"
run "buildrun:Icarus;$ended;$keys;wavedump;$again;buildrun:Icarus;$ended;wavedump;wavedump:load;tabsdump"
all=$got
got=$(printf '%s' "$all" | awk 'BEGIN { RS = " ;; " } /^(icarus.vcd|>  tb.count|   tb.en)/ { print }' | join)
check "run again: read again, the view kept, the values new" "icarus.vcd: read, cursor 105 ns, view 0 ps to 2.772 us, markers a=72 ns(-33 ns) ;; >  tb.count[7:0] = 10 unsigned ;;    tb.en = 1 ;; icarus.vcd: read, cursor 105 ns, view 0 ps to 2.737 us, markers a=72 ns(-33 ns) ;; >  tb.count[7:0] = 9 unsigned ;;    tb.en = 0"
got=$(printf '%s' "$all" | awk 'BEGIN { RS = " ;; " } /^(load|counter)/ { print }' | sed -e 's/[0-9]* bytes/N bytes/' | join)
check "(the same tab, read again once)" "load: icarus.vcd on show, N bytes read, grown 0 times, read again 1 times, waiting for more ;; counter.sv | icarus.vcd"

# Its FST.
fresh
sim iverilog ""
sim vvp vvp-fst "$W/icarus.fst icarus.fst"
run "buildrun:Icarus FST;$ended;wavedump"
heads
check "Icarus's FST" "icarus.fst: read, cursor 0 ps, view 0 ps to 2.772 us"

# Verilator: its program says nothing of its dump; the command names it
# (in a define), and it is the one written during the run.
fresh
sim verilator verilator "$B/Vtb obj_dir/Vtb"
sim Vtb Vtb "$W/verilator.vcd verilator.vcd" "$W/yosys.vcd obj_dir/other.vcd 1"
run "buildrun:Verilator;$ended;wavedump"
heads
check "Verilator: the dump its command names, of the two written" "verilator.vcd: read, cursor 0 ps, view 0 ps to 2.772 us"

# SymbiYosys: the failed assertion's counterexample, though a cover trace
# was written too (after it).
fresh
cp "$here/wave/make/formal/counter.v" "$here/wave/make/formal/counter.sby" "$P/"
sim sby sby "$W/sby-bmc.vcd counter_bmc/engine_0/trace.vcd" "$W/sby-cover.vcd counter_cover/engine_0/trace0.vcd 1"
run "buildrun:Formal;$ended;wavedump;builddump"
got=$(printf '%s' "$got" | awk 'BEGIN { RS = " ;; " } /^(trace|Tasks)/ { print }' | sed -e 's/ | Tasks:.*(last/ (last/' | join)
check "SymbiYosys: the counterexample, not the cover trace (written after it)" "trace.vcd: read, cursor 0 ns, view 0 ns to 80 ns ;; Tasks: Icarus Icarus FST Verilator >Formal Bench Lint (last: 2)"

# A run of kind simulation whose output and command name no dump: the one
# its source's $dumpfile names, not a newer one; a dump written before the
# run is not taken for its own.
fresh
sim runsim "" "$W/icarus.vcd waves.vcd" "$W/yosys.vcd junk.vcd 1"
run "buildrun:Bench;$ended;wavedump"
heads
check "\$dumpfile in a source the command names" "waves.vcd: read, cursor 0 ps, view 0 ps to 2.772 us"
fresh
cp "$W/yosys.vcd" "$P/old.vcd"
touch -t 202601010000 "$P/old.vcd"
sim runsim ""
run "buildrun:Bench;$ended;wavedump;tabsdump"
check "a run that wrote no dump: nothing opened" "no wave tab ;; counter.sv"

# "Open the last run's waves": named in the command search with gW; with
# no run yet, the project's newest dump; after a run (its tab closed),
# that run's dump.
fresh
run "key:cmd+shift+p;text:open the last run's waves;findwait;finddump;key:Escape"
got=$(printf '%s' "$got" | sed -e 's/.* : >//' -e 's/) .*/)/')
check "the command search: with its keys" "Open the last run's waves (gW)"
cp "$W/yosys.vcd" "$P/a.vcd"
cp "$W/icarus.vcd" "$P/b.vcd"
touch -t 202601010000 "$P/a.vcd"
run "vim:gW;simwait;wavewait;wavedump"
heads
check "gW with no run: the newest dump in the project" "b.vcd: read, cursor 0 ps, view 0 ps to 2.772 us"
fresh
sim runsim "" "$W/icarus.vcd waves.vcd"
cp "$W/yosys.vcd" "$P/newer.vcd"
run "buildrun:Bench;$ended;command:Close the tab;frame;tabsdump;lua:os.execute('touch $P/newer.vcd');vim:gW;simwait;wavewait;wavedump;tabsdump"
got=$(printf '%s' "$got" | awk 'BEGIN { RS = " ;; " } /^(counter|waves)/ { print }' | join)
check "gW after a run: its dump again" "counter.sv ;; waves.vcd: read, cursor 0 ps, view 0 ps to 2.772 us ;; counter.sv | waves.vcd"
fresh
run "vim:gW;simwait;frame"
got=$(printf '%s' "$got" | grep -o 'No waves[^;]*' | sed -e "s#$P#P#")
check "gW with none in the project: said" "No waves (.vcd or .fst) in ~/proj"

[ $failed = 0 ] && echo "simrun ok"
exit $failed
