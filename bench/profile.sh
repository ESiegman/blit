#!/bin/sh
# Where does the time go? Runs one in-app benchmark scenario for long enough
# to be sampled by macOS's `sample` (it comes with the Xcode command line
# tools) and prints the main thread's time by function.
#
#   bench/profile.sh SCENARIO FILE [blit args...]
#   bench/profile.sh scroll bench/data/code-100m.txt
#   BLIT_INIT=my.lua bench/profile.sh popups bench/data/real-200k.cpp
#
# The full report, with every thread's call tree, is left in the file named
# on the last line. Uses the benchmark build (see run.sh).
set -eu
cd "$(dirname "$0")/.."
scenario=${1:?usage: bench/profile.sh SCENARIO FILE [blit args...]}
file=${2:?usage: bench/profile.sh SCENARIO FILE [blit args...]}
shift 2
build=${BLIT_BUILD:-build-bench}
# (With no BLIT_INIT: the defaults, without a language server.)
[ -n "${BLIT_INIT:-}" ] || { printf 'blit.opt.lsp.enabled = false\nblit.opt.editor.modal = false\n' >bench/data/full.lua; export BLIT_INIT=$PWD/bench/data/full.lua; }
report=$(mktemp -t blit-sample).txt
./$build/blit --offscreen 2560x1600 --bench "$scenario" --frames 400000 "$@" "$file" >/dev/null 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null || true' EXIT
sleep 1
sample $pid 3 1 -file "$report" >/dev/null 2>&1
kill $pid 2>/dev/null || true
python3 bench/sample_top.py "$report"
echo "full report: $report"
