#!/bin/sh
# Waveforms: the timeline model and the VCD and FST readers
# (src/timeline.hpp, src/wave.hpp), through blit_timeline_test beside blit.
# Real dumps from Icarus, Verilator, Yosys and pyvcd (tests/wave) are read
# whole, in two pieces at every byte, a byte at a time and cut short, and
# checked against what their text says and against answers found by brute
# force; Icarus's and Verilator's FST files against the VCD of the same run,
# and cut short at every byte; FST files written by libfst's own
# writer against what was written; large dumps made up here (VCD, and FST
# through libfst's writer) are read on a thread under a memory budget they
# go over.
#
#   tests/wave.sh path/to/blit
set -u
blit=${1:?usage: tests/wave.sh path/to/blit}
src=$(cd "$(dirname "$0")/.." && pwd)
bin="$(cd "$(dirname "$blit")" && pwd)/blit_timeline_test"
[ -x "$bin" ] || { echo "FAIL: no $bin"; exit 1; }
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
failed=0

mkdir "$dir/scratch"
"$bin" check "$src/tests/wave" 200 "$dir/scratch" || failed=1

# Read as blit reads a file, on a thread: past timeline.max_mb it stops,
# says so, and keeps what it read.
"$bin" gen "$dir/big.vcd" 24 || { echo "FAIL: gen"; failed=1; }
got=$("$bin" load "$dir/big.vcd" 8)
case "$got" in
    *"error: the dump needs more memory than timeline.max_mb allows: read up to "[0-9]*" us"*) ;;
    *) printf 'FAIL: over the budget\n    %s\n' "$got"; failed=1 ;;
esac
got=$("$bin" load "$dir/big.vcd" 512)
case "$got" in
    *error*|*note*) printf 'FAIL: under the budget\n    %s\n' "$got"; failed=1 ;;
    "vars 813 lanes 813 changes "*) ;;
    *) printf 'FAIL: under the budget\n    %s\n' "$got"; failed=1 ;;
esac
"$bin" genfst "$dir/big.fst" 3 || { echo "FAIL: genfst"; failed=1; }
got=$("$bin" load "$dir/big.fst" 8)
case "$got" in
    *"error: the dump needs more memory than timeline.max_mb allows: read up to "[0-9]*" us"*) ;;
    *) printf 'FAIL: an FST over the budget\n    %s\n' "$got"; failed=1 ;;
esac
got=$("$bin" load "$dir/big.fst" 512)
case "$got" in
    *error*|*note*) printf 'FAIL: an FST under the budget\n    %s\n' "$got"; failed=1 ;;
    "vars 813 lanes 813 changes "*) ;;
    *) printf 'FAIL: an FST under the budget\n    %s\n' "$got"; failed=1 ;;
esac
got=$("$bin" load "$dir/none.vcd")
[ "$got" = "error: No such file or directory" ] || { printf 'FAIL: a file not there\n    %s\n' "$got"; failed=1; }

# The setting, as init.lua sets it and the settings screen shows it (one
# below 16 MB is wrong, and the default stays).
echo hello >"$dir/a.txt"
export BLIT_INIT="$dir/init.lua" BLIT_RECENT_FILE="$dir/recent"
for case in "512|timeline.max_mb 512 the memory a dump's waves may take: reading one stops there, and says how far it got" \
            "8|timeline.max_mb 2048 the memory a dump's waves may take: reading one stops there, and says how far it got"; do
    printf 'blit.opt.lsp.enabled = false\nblit.opt.timeline.max_mb = %s\n' "${case%%|*}" >"$BLIT_INIT"
    got=$("$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "settingsopen:timeline.max_mb;frame;settingsdump" "$dir/a.txt" 2>&1 >/dev/null |
        grep '^settings:' | tail -1 | tr -s ' ' | sed 's/^settings: [0-9]* items, line: //')
    [ "$got" = "${case#*|}" ] || { printf 'FAIL: timeline.max_mb = %s\n    expected %s\n    got      %s\n' "${case%%|*}" "${case#*|}" "$got"; failed=1; }
done

[ $failed = 0 ] && echo "wave ok"
exit $failed
