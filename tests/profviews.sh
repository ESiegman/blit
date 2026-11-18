#!/bin/sh
# Profiles' views, on profiles written by hand (perf script's text, so that
# no profiler is needed and the numbers are known): the graph turned over
# (bottom-up), a function's callers and callees, two profiles compared,
# the timeline.
#
#   tests/profviews.sh path/to/blit            run the cases
#   tests/profviews.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/profviews.sh path/to/blit [--print]}
print=${2:-}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
blit=$(cd "$(dirname "$blit")" && pwd)/$(basename "$blit")
cd "$dir" || exit 1
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua" BLIT_RECENT_FILE="$dir/recent"

# A program: main calls mid (which calls leaf_a and leaf_b) and other
# (which calls leaf_a); a second thread runs worker, which calls leaf_b.
# Each sample's weight is its period: 100 in all.
sample() {  # thread, time, weight, frames (the innermost first)
    printf 'prog %s %s: %s\n' "$1" "$2" "$3"
    shift 3
    for f in "$@"; do printf '\t1 %s (/t/prog)\n' "$f"; done
    printf '\n'
}
{
    sample 100 1.000 10 leaf_a mid main
    sample 100 1.100 30 leaf_b mid main
    sample 100 1.200 20 leaf_a other main
    sample 101 1.300 40 leaf_b worker
} >"$dir/a.perf"
# Later: leaf_b down to 10, leaf_a up to 50, fresh new at 40, worker gone.
{
    sample 100 1.000 10 leaf_a mid main
    sample 100 1.100 10 leaf_b mid main
    sample 100 1.200 40 leaf_a other main
    sample 100 1.300 40 fresh main
} >"$dir/b.perf"

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run SCRIPT: what the profile's tab says of itself (profdump's "tab:" part).
run() {
    got=$("$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "$1" 2>&1 >/dev/null | sed -n 's/.*; tab: //p' | tail -1)
}

# Bottom-up: where the time was spent, each with all of its own time, and
# what called it above; again, top-down.
run "profopen:$dir/a.perf;frame;key:b;frame;profdump"
check "bottom-up: the functions time was spent in, at the bottom" 'zoom all, bottom-up: leaf_b=70% leaf_a=30%'
run "profopen:$dir/a.perf;frame;key:b;frame;key:b;frame;profdump"
check "and top-down again" 'zoom all'
# Callers and callees of the function chosen (the heaviest by its own time
# to start with), each with its weight there; one of them chosen in turn.
run "profopen:$dir/a.perf;frame;key:c;frame;profdump"
check "the heaviest function's callers and callees" 'zoom all, of leaf_b: callers worker=40% mid=30% | callees'
run "profopen:$dir/a.perf;frame;key:c;frame;key:Down;key:Return;frame;profdump"
check "a caller chosen: its own callers and callees" 'zoom all, of mid: callers main=40% | callees leaf_b=30% leaf_a=10%'

# Two compared: the later's graph, its frames coloured by how their share
# moved (1 grown much ... 5 shrunk much); each function's own share before
# and after, those that moved most first, one gone among them.
run "profopen:$dir/a.perf;profopen:$dir/b.perf;frame;command:Compare with a profile…;key:Return;frame;profdump"
check "two profiles compared" 'zoom all, compared with 00:00: leaf_b 70%→10% fresh 0.0%→40% leaf_a 30%→50% main 0.0%→0.0%, frames: main=1'

[ -n "$print" ] || [ $failed = 1 ] || echo "profviews ok"
exit $failed
