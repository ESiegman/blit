#!/bin/sh
# Profiling, end to end: a program recorded by the system's profiler
# (xctrace here on a Mac), its tab, the times in its code, its threads, the
# profile kept beside the project and opened again; and the same on
# "another machine" through tests/fake-ssh.sh (which runs it here).
# Skipped where there is no profiler to run, and on a Mac on CI.
#
#   tests/profile.sh path/to/blit            run the cases
#   tests/profile.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/profile.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
if [ "$(uname -s)" = Darwin ]; then
    # (Not on CI: xctrace on GitHub's Mac runners now and then never ends.
    # Reading its exports is tested from saved ones, tests/profile/*.xml.)
    [ -z "${CI:-}" ] || { echo "profile skipped: no live recordings on CI"; exit 0; }
    xcrun --find xctrace >/dev/null 2>&1 || [ -x /Applications/Xcode.app/Contents/Developer/usr/bin/xctrace ] || { echo "profile skipped: no xctrace"; exit 0; }
else
    command -v perf >/dev/null 2>&1 || { echo "profile skipped: no perf"; exit 0; }
fi
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
T="$dir/there"
mkdir -p "$dir/here" "$T"
for d in "$dir/here" "$T"; do
    cp "$here/profile/spin.c" "$d/" && (cd "$d" && cc -O1 -g -fno-omit-frame-pointer -o spin spin.c) || exit 1
done
cp "$here/profile/threads.c" "$dir/here/" && (cd "$dir/here" && cc -O1 -g -fno-omit-frame-pointer -pthread -o threads threads.c) || exit 1
cp "$here/profile/memwait.c" "$dir/here/" && (cd "$dir/here" && cc -O1 -g -fno-omit-frame-pointer -pthread -o memwait memwait.c) || exit 1
# (Profiles are kept beside the folder being worked in, which is blit's own
# folder when none is chosen: run there, so that nothing is left behind.)
blit=$(cd "$(dirname "$blit")" && pwd)/$(basename "$blit")
cd "$dir" || exit 1
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua" BLIT_RECENT_FILE="$dir/recent" BLIT_SSH="$here/fake-ssh.sh" BLIT_FAKE_HOME="$T"
tool=xctrace main="Main Thread"  # (the profiler, and what it calls the first thread)
[ "$(uname -s)" = Darwin ] || tool=perf main=threads

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run SCRIPT: the lines it printed, joined by " ;; ", with the hot lines'
# own notes (L) left out of the times on lines (where a loop's time lands
# is up to the compiler; the functions' lines are not).
run() {
    got=$("$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "$1" 2>&1 >/dev/null | sed -e "s#$dir#@#g" -e 's/ [0-9][0-9]*L//g' |
        awk 'NR > 1 { printf " ;; " } { printf "%s", $0 }')
}

run "prof:program=$dir/here/spin;prof:args=40;profpanel;profrecord;profwait;frame;profdump;key:Return;frame;frame;profdump;statusdump"
got=$(printf '%s' "$got" | sed -e 's#;; [^;]*spin.c | \(Ln [0-9]*, Col [0-9]*\).*#;; \1#')
check "recorded here: its tab, then the hottest function's source (at its definition) with the times in it" \
    "1 profiles; spin by $tool, 1 thread, over 10%: inner@spin.c outer@spin.c; tab: zoom all ;; 1 profiles; spin by $tool, 1 thread, over 10%: inner@spin.c outer@spin.c; times on lines 9F 17F 25F ;; Ln 9, Col 1"
run "prof:program=$dir/here/spin;prof:args=40;profrecord;profwait;frame;key:space;frame;profdump;key:Escape;frame;profdump"
got=$(printf '%s' "$got" | grep -o 'zoom [a-z]*' | tr '\n' ' ' | sed -e 's/ $//')
check "the flame graph from the keyboard: Space zooms into the chosen function, Esc back out a level" "zoom inner zoom outer"
run "prof:program=$dir/here/spin;prof:args=40;prof:annotate=off;profrecord;profwait;frame;open:$dir/here/spin.c;frame;profdump"
check "inline performance labels turned off" "1 profiles; spin by $tool, 1 thread, over 10%: inner@spin.c outer@spin.c"
run "prof:program=$dir/here/spin;prof:args=20;profrecord;profwait;frame;open:$dir/here/spin.c;frame;dump:tabs;click:200,12;drag:300,100;drag:760,250;release;frame;dump:moved"
got=$(printf '%s' "$got" | sed -e 's/[0-9][0-9]:[0-9][0-9]:[0-9][0-9]/hh:mm:ss/g')
check "a profile's tab is a tab like a file's: dragged into a split of its own" \
    "tabs: >[untitled Profile: spin hh:mm:ss spin.c*] ;; moved: H([untitled spin.c*] | >[Profile: spin hh:mm:ss*])0.50"
run "prof:program=$dir/here/no-such-program;profrecord;profwait;frame;profdump"
case "$got" in *"failed: "*"0 profiles") got="failed, 0 profiles" ;; esac
check "a program that is not there: why, and no profile" "failed, 0 profiles"
# Record builds first where the folder has a build (make's make, here), and
# records only if it went well; one that fails says so.
mkdir -p "$dir/made"
printf 'all:\n\techo built >>log\n\tcc -O1 -g -fno-omit-frame-pointer -o spin ../here/spin.c\n' >"$dir/made/Makefile"
run "workdir:$dir/made;prof:program=$dir/made/spin;prof:args=20;profrecord;profwait;frame;profdump"
got="$(printf '%s' "$got" | sed -e 's/ by .*//') ;; $(cat "$dir/made/log")"
check "Record builds first, then records what it built" "1 profiles; spin ;; built"
printf 'all:\n\techo broken >>log\n\texit 2\n' >"$dir/made/Makefile"
run "workdir:$dir/made;prof:program=$dir/made/spin;prof:args=20;profrecord;profwait;frame;profdump"
got="$got ;; $(tail -1 "$dir/made/log")"
check "a build that fails: it says so, and nothing is recorded" "blit: make failed: Record did not start ;; 0 profiles ;; broken"
run "prof:program=ssh://box$T/spin;prof:args=40;profrecord;profwait;frame;profdump;key:Return;remotewait;frame;frame;profdump"
check "recorded on another machine: its source opened from there, with the times" \
    "1 profiles; spin by $tool on box, 1 thread, over 10%: inner@spin.c outer@spin.c; tab: zoom all ;; 1 profiles; spin by $tool on box, 1 thread, over 10%: inner@spin.c outer@spin.c; times on lines 9F 17F 25F"

# Memory: the pages a program touches first (Instruments' Virtual Memory
# Trace; perf's page faults), each function by the bytes it first touched.
run "prof:program=$dir/here/memwait;prof:kind=memory;profrecord;profwait;frame;profdump"
check "a memory profile: where new memory is first touched" "1 profiles; memwait (memory) by $tool, 1 thread, over 10%: big@memwait.c small@memwait.c; tab: zoom all"
# Waiting: where threads wait (the System Call Trace; on Linux, perf's
# context switches, which need kernel.perf_event_paranoid at 1 or lower).
run "prof:program=$dir/here/memwait;prof:args=waiting;prof:kind=waiting;profrecord;profwait;frame;profdump"
case $got in
    *perf_event_paranoid*) echo "waiting profile skipped: perf_event_paranoid is above 1 here" ;;
    *) got=$(printf '%s' "$got" | sed -e 's/over 10%:.*;/over 10%: (system calls);/')
       check "a waiting profile: where the threads waited" "1 profiles; memwait (waiting) by $tool, 2 thread, over 10%: (system calls); tab: zoom all" ;;
esac

# A graph per thread: the Profile tab lists the threads of the profile shown
# (All threads, then the heaviest first), and the graph is of the one chosen.
W="workdir:$dir/here"
run "$W;prof:program=$dir/here/threads;prof:args=30;profpanel;profrecord;profwait;frame;profdump;profpick:threads:2;frame;profdump;profpick:threads:1;frame;profdump"
check "a graph per thread, the heaviest thread's first" \
    "1 profiles; threads by $tool, 2 thread, over 10%: main_work@threads.c worker_work@threads.c; tab: zoom all ;; 1 profiles; threads by $tool, 2 thread, over 10%: main_work@threads.c worker_work@threads.c; tab: zoom worker, heaviest worker_work ;; 1 profiles; threads by $tool, 2 thread, over 10%: main_work@threads.c worker_work@threads.c; tab: zoom all"

# Profiles kept: each one made is written into .blit/profiles in the folder
# being worked in; the Profile tab lists them there, and opens one again.
got=$(ls "$dir/here/.blit/profiles" | grep '^threads' | sed -e 's/-[0-9-]*\.blitprofile$/ (when).blitprofile/')
check "the profile kept beside the project" "threads (when).blitprofile"
run "$W;profpanel;frame;profwait;frame;profpick:profiles:1;profwait;frame;profdump;profpick:threads:3;frame;profdump"
check "and opened again from the Profile tab, its threads with it" \
    "1 profiles; threads by $tool, 2 thread, over 10%: main_work@threads.c worker_work@threads.c; tab: zoom all ;; 1 profiles; threads by $tool, 2 thread, over 10%: main_work@threads.c worker_work@threads.c; tab: zoom $main, heaviest main_work"

[ -n "$print" ] || [ $failed = 1 ] || echo "profile ok"
exit $failed
