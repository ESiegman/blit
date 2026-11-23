#!/bin/sh
# Engines run with no terminal tab (engine.cpp): here and on "another
# machine" (tests/fake-ssh.sh standing in for ssh, blit's agent copied and
# run as it would be there), on pipes and on a pseudo-terminal: their
# lines (a line drawn over with \r, a JSON line), what they read, how they
# ended, a time limit and a cancel; and `which`, asked once per machine.
#
#   tests/engine.sh path/to/blit            run the cases
#   tests/engine.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/engine.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd -P)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
T="$dir/there"  # the home there
mkdir -p "$T/bin" "$dir/home/.ssh" "$dir/bin"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n' >"$dir/init.lua"
printf 'Host box\n    HostName example.invalid\n' >"$dir/home/.ssh/config"
export HOME="$dir/home" BLIT_RECENT_FILE="$dir/recent" BLIT_SSH="$here/fake-ssh.sh" BLIT_FAKE_HOME="$T"

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n%s\n\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n  expected:\n%s\n  got:\n%s\n' "$1" "$2" "$got"
        failed=1
    fi
}
want() { expected=$(cat); }
# run SCRIPT: what it printed, $dir as ~.
run() {
    got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 800x600 --shot "$dir/shot.bmp" --script "$1" 2>&1 >/dev/null | sed -e "s#$T#@#g" -e "s#$dir#~#g")
}

for where in here there; do
    [ $where = here ] && h= || h="host=box "

    run "enginerun:$h-- echo one && sleep 0.2 && echo two >&2 && printf 'no newline' && exit 3"
    want <<'EOF'
out: one
err: two
out: no newline
exit: 3
EOF
    check "$where, on pipes: its output and errors apart, its exit status" "$expected"

    run "enginerun:$h-- [ -t 1 ] && echo a terminal || echo no terminal"
    check "$where, on pipes: no terminal" "out: no terminal
exit: 0"

    run "enginerun:${h}pty -- [ -t 1 ] && echo a terminal && printf 'Erasing 10%%\\rErasing 50%%\\rErasing done\\n' && echo oops >&2"
    want <<'EOF'
out: a terminal
redrawn: Erasing 10%
redrawn: Erasing 50%
out: Erasing done
out: oops
exit: 0
EOF
    check "$where, on a terminal: the lines drawn over, its errors with its output" "$expected"

    run "enginerun:${h}json -- echo '{\"probe\": \"ST-LINK\", \"index\": 2}' && echo plain"
    want <<'EOF'
out json: probe=ST-LINK index=2
out: plain
exit: 0
EOF
    check "$where, JSON lines as values" "$expected"

    run "enginerun:${h}input=hello -- read x && echo got \$x"
    check "$where, what it reads" "out: got hello
exit: 0"

    run "enginerun:${h}timeout=1500 -- echo started && exec sleep 20"
    check "$where, past its time limit: ended" "out: started
exit: 143 (timed out) (ended within 5 s)"

    run "enginerun:${h}pty timeout=1500 -- echo started && exec sleep 20"
    check "$where, on a terminal, past its time limit: ended" "out: started
exit: 143 (timed out) (ended within 5 s)"

    run "enginerun:${h}cancel=400 -- echo started && exec sleep 20"
    check "$where, called off" "out: started
exit: 143 (cancelled) (ended within 5 s)"

    # A program that started another, which holds its pipes (a tool's own
    # helper, a shell's sleep): called off, both end, at once.
    run "enginerun:${h}cancel=400 -- echo started && sleep 23.75 & sleep 23.75 & wait"
    sleep 1
    got="$got | left running: $(pgrep -f 'sleep 23.75' | wc -l | tr -d ' ')"
    check "$where, called off, with what it started" "out: started
exit: 143 (cancelled) (ended within 5 s) | left running: 0"

    run "enginerun:$h-- exec nothere-at-all"
    got=$(printf '%s' "$got" | sed -e 's/^err: .*nothere-at-all.*/err: (not found)/')
    check "$where, a program that is not there" "err: (not found)
exit: 127"
done

# which: a program on the machine's PATH (there, the agent's), and one not
# on it; asked once, the second answer the kept one.
run "enginewhich:- sh;enginewhich:- nothere-at-all"
got=$(printf '%s' "$got" | sed -e 's#: /[a-z/]*/sh #: SH #')
want <<'EOF'
which sh: SH (asked 1)
which nothere-at-all: not there (asked 1)
EOF
check "which here, kept" "$expected"
run "enginewhich:box sh;enginewhich:box sh;enginewhich:box nothere-at-all"
got=$(printf '%s' "$got" | sed -e 's#: /[a-z/]*/sh #: SH #')
want <<'EOF'
which sh: SH (asked 1)
which sh: SH (asked 0)
which nothere-at-all: not there (asked 1)
EOF
check "which there, kept" "$expected"

# One not there, installed since (on the PATH a moment later): kept as not
# there for a few seconds, then asked about again, and found.
mkdir -p "$dir/later"
rm -f "$dir/later/blit-later-tool"
make="lua:os.execute([[printf '#!/bin/sh\\\\n' >$dir/later/blit-later-tool && chmod +x $dir/later/blit-later-tool]])"
PATH="$dir/later:$PATH" run "enginewhich:- blit-later-tool;$make;enginewhich:- blit-later-tool;wait:3200;enginewhich:- blit-later-tool"
got=$(printf '%s' "$got" | sed -e 's#: ~/later/blit-later-tool #: FOUND #')
want <<'EOF'
which blit-later-tool: not there (asked 1)
which blit-later-tool: not there (asked 0)
which blit-later-tool: FOUND (asked 1)
EOF
check "which here, one installed since: asked again a few seconds on" "$expected"

# A machine that cannot be reached: the run never starts, and says why.
run "enginerun:host=nowhere -- echo hi"
BLIT_FAKE_FAIL="Connection refused" run "enginerun:host=box -- echo hi"
got=$(printf '%s' "$got" | sed -e 's/(not started: .*Connection refused.*)/(not started: refused)/')
check "there, no connection" "exit: -1 (not started: refused)"

exit $failed
