#!/bin/sh
# Stands in for a simulator in the tests (tests/simrun.sh), as
# tests/replay.sh stands in for a flashing tool: run as NAME (verilator,
# iverilog, vvp, sby, or the program Verilator built), it writes the tool's
# real output that the test chose for this run, then the files the tool
# wrote (copies of real ones), and ends as the tool did. What it does is
# in $FAKE_SIM, a file each, which the test writes before each run:
#   NAME.out     the sample written (tests/sim/*.txt, replay.sh's form)
#   NAME.files   "FROM TO [LATER]" a line each: FROM copied to TO (from
#                the folder it runs in), written LATER seconds after now
#                (as a slower tool would have; the tests' runs take none)
#
#   sh tests/fake-sim.sh NAME ARGS...
here=$(cd "$(dirname "$0")" && pwd)
name=$1
shift
code=0
if [ -f "$FAKE_SIM/$name.out" ]; then
    sh "$here/replay.sh" "$(cat "$FAKE_SIM/$name.out")"
    code=$?
fi
if [ -f "$FAKE_SIM/$name.files" ]; then
    while read -r from to later; do
        mkdir -p "$(dirname "$to")"
        cp "$from" "$to"
        [ -n "$later" ] && perl -e '$t = time + $ARGV[0]; utime($t, $t, $ARGV[1])' "$later" "$to"
    done <"$FAKE_SIM/$name.files"
fi
exit $code
