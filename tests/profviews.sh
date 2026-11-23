#!/bin/sh
# Profiles' views, on profiles written by hand (perf script's text, so that
# no profiler is needed and the numbers are known): the graph turned over
# (bottom-up), a function's callers and callees, two profiles compared,
# the timeline. And every allocation, through blit's own sampler (which
# needs no profiler either).
#
#   tests/profviews.sh path/to/blit            run the cases
#   tests/profviews.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/profviews.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
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

# The timeline: a stretch of it chosen narrows the graph and the list to
# what ran then (the samples at 0.2 s and 0.3 s: other's leaf_a, then
# worker's leaf_b), bottom-up too; Esc lets it go.
run "profopen:$dir/a.perf;frame;proftime:0.15,0.25;frame;profdump"
check "a stretch of time: what ran then" 'zoom all, stretch 0.15-0.25: 1 samples, heaviest leaf_a'
run "profopen:$dir/a.perf;frame;proftime:0.15,0.35;key:b;frame;profdump"
check "and bottom-up" 'zoom all, stretch 0.15-0.35: 2 samples, heaviest leaf_b, bottom-up: leaf_b=67% leaf_a=33%'
run "profopen:$dir/a.perf;frame;proftime:0.15,0.25;frame;key:Escape;frame;profdump"
check "Esc: all of it again" 'zoom all'

# Every allocation: tests/profile/allocs.c, whose make_big allocates most of
# the bytes and keeps half of them to the end, make_small many small blocks
# freed again. Two profiles: what was allocated, and what was still held.
cp "$here/profile/allocs.c" "$dir/" && (cd "$dir" && cc -g -O1 -fno-omit-frame-pointer -o allocs allocs.c) || exit 1
got=$("$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "prof:program=$dir/allocs;prof:kind=allocations;profrecord;profwait;frame;profdump;profpanel;profpick:profiles:2;frame;profdump" 2>&1 >/dev/null |
    grep -v '^\[' | awk 'NR > 1 { printf " ;; " } { printf "%s", $0 }')
check "every allocation: what was allocated, and what was still held at the end" '2 profiles; allocs (allocations) by blit, 1 thread, over 10%: make_big@allocs.c; tab: zoom all ;; 2 profiles; allocs (held) by blit, 1 thread, over 10%: make_big@allocs.c; tab: zoom all'
# No atos (addr2line elsewhere) to name the stacks with: said in each
# profile's heading, not left to the missing source lines.
case $(uname -s) in Darwin) namer=atos this="this Mac" lost="no source lines" ;; *) namer=addr2line this="this machine" lost="no function names or source lines" ;; esac
mkdir -p "$dir/bare"
ln -s /usr/bin/* /bin/* "$dir/bare/" 2>/dev/null
rm -f "$dir/bare/atos" "$dir/bare/addr2line"
got=$(PATH="$dir/bare" "$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "prof:program=$dir/allocs;prof:kind=allocations;profrecord;profwait;frame;profnote;profpanel;profpick:profiles:2;frame;profnote" 2>&1 >/dev/null |
    grep -v '^\[' | sed -e 's/; [0-9][0-9]* allocations, .*/; (allocations)/' -e 's/; [0-9.]* MB still held .*/; (held)/' | awk 'NR > 1 { printf " ;; " } { printf "%s", $0 }')
check "no $namer: said in both profiles" "$namer is not installed on $this, so $lost; (allocations) ;; $namer is not installed on $this, so $lost; (held)"

[ -n "$print" ] || [ $failed = 1 ] || echo "profviews ok"
exit $failed
