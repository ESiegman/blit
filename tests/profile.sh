#!/bin/sh
# Profiling, end to end: a program recorded by the system's profiler
# (xctrace here on a Mac), its tab, the times in its code; and the same
# on "another machine" through tests/fake-ssh.sh (which runs it here).
# Skipped where there is no profiler to run.
#
#   tests/profile.sh path/to/blit            run the cases
#   tests/profile.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/profile.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
if [ "$(uname -s)" = Darwin ]; then
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
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua" BLIT_RECENT_FILE="$dir/recent" BLIT_SSH="$here/fake-ssh.sh" BLIT_FAKE_HOME="$T"
tool=xctrace
[ "$(uname -s)" = Darwin ] || tool=perf

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
    "1 profiles; spin by $tool, 1 thread, over 10%: inner@spin.c outer@spin.c; tab: zoom all ;; 1 profiles; spin by $tool, 1 thread, over 10%: inner@spin.c outer@spin.c; times on lines 8F 16F 24F ;; Ln 8, Col 1"
run "prof:program=$dir/here/spin;prof:args=40;prof:annotate=off;profrecord;profwait;frame;open:$dir/here/spin.c;frame;profdump"
check "inline performance labels turned off" "1 profiles; spin by $tool, 1 thread, over 10%: inner@spin.c outer@spin.c"
run "prof:program=$dir/here/spin;prof:args=20;profrecord;profwait;frame;open:$dir/here/spin.c;frame;dump:tabs;click:200,12;drag:300,100;drag:760,250;release;frame;dump:moved"
got=$(printf '%s' "$got" | sed -e 's/[0-9][0-9]:[0-9][0-9]:[0-9][0-9]/hh:mm:ss/g')
check "a profile's tab is a tab like a file's: dragged into a split of its own" \
    "tabs: >[untitled Profile: spin hh:mm:ss spin.c*] ;; moved: H([untitled spin.c*] | >[Profile: spin hh:mm:ss*])0.50"
run "prof:program=$dir/here/no-such-program;profrecord;profwait;frame;profdump"
case "$got" in *"failed: "*"0 profiles") got="failed, 0 profiles" ;; esac
check "a program that is not there: why, and no profile" "failed, 0 profiles"
run "prof:program=ssh://box$T/spin;prof:args=40;profrecord;profwait;frame;profdump;key:Return;remotewait;frame;frame;profdump"
check "recorded on another machine: its source opened from there, with the times" \
    "1 profiles; spin by $tool on box, 1 thread, over 10%: inner@spin.c outer@spin.c; tab: zoom all ;; 1 profiles; spin by $tool on box, 1 thread, over 10%: inner@spin.c outer@spin.c; times on lines 8F 16F 24F"

[ -n "$print" ] || [ $failed = 1 ] || echo "profile ok"
exit $failed
