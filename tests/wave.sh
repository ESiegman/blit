#!/bin/sh
# Waveforms: the timeline model (src/timeline.hpp), through
# blit_timeline_test beside blit: lanes built a change at a time, checked
# against plain lists and against answers found by brute force.
#
#   tests/wave.sh path/to/blit
set -u
blit=${1:?usage: tests/wave.sh path/to/blit}
src=$(cd "$(dirname "$0")/.." && pwd)
bin="$(cd "$(dirname "$blit")" && pwd)/blit_timeline_test"
[ -x "$bin" ] || { echo "FAIL: no $bin"; exit 1; }
failed=0

"$bin" check "$src/tests/wave" 200 || failed=1

[ $failed = 0 ] && echo "wave ok"
exit $failed
