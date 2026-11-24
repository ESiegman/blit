#!/bin/sh
# blit's agent on another machine outlives the connection: a terminal, a
# task, a cell keep running while the connection is down, and are taken back
# (with what they wrote meanwhile) when blit connects again, which it does by
# itself; and the agent ends once nothing holds it. tests/fake-ssh.sh stands
# in for ssh, as in tests/remote.sh: what ssh runs (blit-agent --attach) is
# killed to cut the connection, the agent itself (--serve) is left.
#
#   tests/agent.sh path/to/blit            run the cases
#   tests/agent.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/agent.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
dir=$(mktemp -d)
dir=$(cd "$dir" && pwd -P)
T="$dir/there"  # the home there
W="$T/work"
mkdir -p "$W" "$dir/run" && chmod 700 "$dir/run"
trap 'pkill -9 -f "$T/.cache/blit/[b]lit-agent" 2>/dev/null; rm -rf "$dir"' EXIT
printf 'hello\nworld\n' >"$W/notes.txt"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
# (BLIT_AGENT_RUN: the agent's socket in a folder of the test's own; and the
# agent waits 4 s idle, not 10 min: longer than connecting again takes here.)
export BLIT_SSH="$here/fake-ssh.sh" BLIT_FAKE_HOME="$T" BLIT_RECENT_FILE="$dir/recent" BLIT_FAKE_DOWN="$dir/down" BLIT_FAKE_LOG="$dir/tries" \
    BLIT_AGENT_RUN="$dir/run" BLIT_AGENT_IDLE=4 BLIT_TERM_SHELL="exec env PS1='\$ ' /bin/sh -i"
R="ssh://box$W"
# The end of the file: Cmd+Down on macOS, Ctrl+End elsewhere.
if [ "$(uname)" = Darwin ]; then docend="key:cmd+Down"; else docend="key:cmd+End"; fi
agent="$T/.cache/blit/[b]lit-agent"
# The connection cut, as a lost link cuts it: what ssh ran there killed (the agent goes on).
cut="lua:os.execute('pkill -9 -f \"$agent.*--attach\"');remotelost"
# And the host out of reach meanwhile (why: what ssh says).
down() { printf '%s' "lua:os.execute('echo \"$1\" > $dir/down')"; }
back="lua:os.remove('$dir/down')"
# (What is first typed into a terminal just opened in a scripted run does not
# reach it: a key that does nothing first.)
first="wait:300;text:x;key:Backspace;"
serving() { pgrep -f "$agent.*--serve" >/dev/null && echo serving || echo gone; }
# Waits (up to $1 s) for the agent to have ended.
ends_within() {
    i=0
    while [ $i -lt $(($1 * 10)) ] && pgrep -f "$agent.*--serve" >/dev/null; do sleep 0.1; i=$((i + 1)); done
    serving
}
tries() { wc -l <"$dir/tries" 2>/dev/null | tr -d ' '; }

failed=0
check() {
    got=$(printf '%s' "$got" | sed -e 's/⎇ //g' -e 's/⇄ //g' -e 's/ //g')
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
join() { awk 'NR > 1 { printf " ;; " } { printf "%s", $0 }'; }
# run SCRIPT [FILE]: what the run printed, its lines joined by " ;; ".
run() {
    rm -f "$dir/down" "$dir/tries"
    got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "$1" ${2:+"$2"} 2>&1 >/dev/null |
        sed -e "s#$T#~#g" -e "s#$dir#@#g" -e 's/ @[0-9,]* [0-9x]*\( ended\)/\1/' -e 's/ @[0-9,]* [0-9x]*$//' -e 's/^Ctrl+` back to the file|//' | join)
}
# (Each case ends with no agent left: blit quit, and nothing was kept.)
settle() { ends_within 5 >/dev/null; pkill -9 -f "$agent" 2>/dev/null; rm -f "$dir/k"*; }

# The agent outlives the connection: cut, and connected again by itself,
# it is the same agent, and a terminal there is the same shell, with what it
# wrote meanwhile.
run "remotewait;key:ctrl+\`;frame;termwait:\$;${first}lua:os.execute('pgrep -f \"$agent.*--serve\" > $dir/k1');text:echo \$\$ > $dir/k2;key:Return;text:sleep 1 && echo AFTER;key:Return;wait:200;$cut;remoteback;termwait:AFTER;text:echo \$\$ > $dir/k3;key:Return;wait:500;lua:os.execute('pgrep -f \"$agent.*--serve\" > $dir/k4');frame;statusdump" "$R/notes.txt"
got="$got | $( [ -s "$dir/k1" ] && cmp -s "$dir/k1" "$dir/k4" && echo same agent || echo another agent) | $( [ -s "$dir/k2" ] && cmp -s "$dir/k2" "$dir/k3" && echo same shell || echo another shell)"
check "cut and back by itself: the same agent, the same shell, what it wrote meanwhile" 'box Terminal: box | | same agent | same shell'
settle
# A program that ended while the connection was down: how it ended.
run "remotewait;key:ctrl+\`;frame;termwait:\$;${first}text:sleep 1 && exit 3;key:Return;wait:200;$cut;remoteback;termend;termdump" "$R/notes.txt"
check "a terminal whose shell ended while away: its exit code, once back" '$ sleep 1 && exit 3|exit|[exited with code 3] ended 3'
settle
# A task (a build) runs on while away: its output, read for problems once back.
mkdir -p "$W/mk" && printf 'all:\n\t@sleep 1\n\t@echo "x.c:2:5: error: boom" && false\n' >"$W/mk/Makefile" && printf 'int x;\nint y;\n' >"$W/mk/x.c"
run "remotewait;frame;buildrun;wait:300;$cut;remoteback;buildwait;problemsdump" "$R/mk"
check "a task that ran on while away: its problems once back" 'button: ▶ make · 1 error | make 1 · failed (exit 2) | file x.c | >E 2:5 boom'
settle

# A cell running when the connection went: the kernel goes on there, and
# what the cell wrote meanwhile comes once back. (Python there: plain python3.)
printf '{"cells": [{"cell_type": "code", "execution_count": null, "metadata": {}, "outputs": [], "source": ["import time\\n", "time.sleep(2)\\n", "print(\\"LATE\\")"]}], "metadata": {}, "nbformat": 4, "nbformat_minor": 5}\n' >"$W/k.ipynb"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n' >"$dir/modal.lua"
rm -f "$dir/down" "$dir/tries"
got=$(BLIT_INIT="$dir/modal.lua" "$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" \
    --script "open:$R/k.ipynb;remotewait;mdwait;vim:2G;key:shift+Return;nbwait:started;$cut;frame;statusdump;remoteback;nbwait;nbdump" 2>&1 >/dev/null | grep -E 'kernel|LATE' | sed -e 's/^.*\(kernel waiting for box\).*$/\1/' -e 's/^[0-9]*: *//' | join)
check "a cell running when the connection went: its output once back" 'kernel waiting for box ;; LATE'
settle

# Connecting again: at once, then waiting longer each time, and no more than
# four connections begun in 30 s (the first one counts: a firewall counts it).
run "remotewait;$(down refused);$cut;lua:os.execute('sleep 12');frame;statusdump;$back;remoteback;frame;statusdump" "$R/notes.txt"
got="$got | $(tries) begun"
check "down: made again by itself, but no more than four in 30 s" 'reconnecting to box… ~/work/notes.txt | Ln 1, Col 1 Tabs LF Plain text ;; box ~/work/notes.txt | Ln 1, Col 1 Tabs LF Plain text | 5 begun'
settle
# Refused (the key, a second factor): not tried again by itself; by hand it is.
run "remotewait;$(down 'tester@box: Permission denied (publickey).');$cut;lua:os.execute('sleep 4');frame;statusdump;lua:os.execute('wc -l < $dir/tries > $dir/k5');$back;command:Reconnect to the servers that dropped;remotewait;frame;statusdump" "$R/notes.txt"
got="$got | $(tr -d ' ' <"$dir/k5") begun, then $(tries)"
check "refused: not tried again by itself (by hand it is)" 'box: not connected (connect again) ~/work/notes.txt | Ln 1, Col 1 Tabs LF Plain text ;; box ~/work/notes.txt | Ln 1, Col 1 Tabs LF Plain text | 2 begun, then 3'
settle
# Its window to the front: one being made again by itself is tried now
# (by itself, the next try would be seconds away yet).
run "remotewait;$(down refused);$cut;lua:os.execute('sleep 3');$back;focus;lua:os.execute('sleep 1');remotewait;frame;statusdump" "$R/notes.txt"
check "the window to the front: tried at once" 'box ~/work/notes.txt | Ln 1, Col 1 Tabs LF Plain text'
settle
# A save just after the connection went waits for it to come back, and goes.
run "remotewait;frame;$docend;text:SAVED;$cut;clock;save;within:300;remotewait;lua:os.execute('sleep 1');remotewait;frame;statusdump" "$R/notes.txt"
got="$got | $(tr '\n' ' ' <"$W/notes.txt")"
check "a save just after a drop: saved once back" 'within 300 ms ;; box ~/work/notes.txt | Ln 3, Col 6 Tabs LF Plain text | hello world SAVED'
printf 'hello\nworld\n' >"$W/notes.txt"
settle

# When the agent ends. Nothing working and nobody attached: after the idle
# time (2 s here). blit killed (as a crash would), nothing working:
rm -f "$dir/down"
BLIT_INIT="$dir/init.lua" "$blit" --offscreen 800x600 --shot "$dir/shot.bmp" --script "remotewait;wait:60000" "$R/notes.txt" >/dev/null 2>&1 &
pid=$!
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do pgrep -f "$agent.*--serve" >/dev/null && break; sleep 0.2; done
kill -9 $pid; wait $pid 2>/dev/null
got="$(serving), then $(sleep 2; serving), then $(ends_within 6)"
check "blit gone, nothing working: the agent ends after the idle time" 'serving, then serving, then gone'
settle
# Something working (a program in front of a terminal's shell) holds it.
BLIT_INIT="$dir/init.lua" "$blit" --offscreen 800x600 --shot "$dir/shot.bmp" --script "remotewait;key:ctrl+\`;frame;termwait:\$;${first}text:sleep 5 && touch $dir/k6;key:Return;wait:60000" "$R/notes.txt" >/dev/null 2>&1 &
pid=$!
sleep 3
kill -9 $pid; wait $pid 2>/dev/null
got="$(sleep 1; serving), then $(ends_within 8) | $( [ -e "$dir/k6" ] && echo it ran to its end || echo it was ended)"
check "blit gone, something working: the agent waits for it, then ends" 'serving, then gone | it ran to its end'
settle
# Quitting with nothing working: the agent ends at once.
run "remotewait;key:ctrl+\`;frame;termwait:\$;${first}quit;asked" "$R/notes.txt"
got="$got | $(ends_within 1)"
check "quit, nothing working: nothing asked, and the agent ends at once" 'nothing asked | gone'
settle
# A serial port there (blit-agent --serial, its pump: not kept, and not
# work): quitting asks nothing, and the agent and the pump end at once.
run "remotewait;serial:ssh://box$dir/no-port;frame;wait:1000;lua:os.execute('pgrep -f \"$agent.*--serial\" > $dir/k8');quit;asked" "$R/notes.txt"
got="$got | $( [ -s "$dir/k8" ] && echo pumping || echo no pump) | $(ends_within 1) | $(pgrep -f "$agent.*--serial" >/dev/null && echo pump left || echo pump gone)"
check "quit with a serial tab there: nothing asked, the agent and its pump end at once" 'nothing asked | pumping | gone | pump gone'
settle
# Quitting with a job: asked; kept, it runs on, and the agent ends after it.
run "remotewait;key:ctrl+\`;frame;termwait:\$;${first}text:sleep 2 && touch $dir/k7;key:Return;wait:700;answer:keep;quit;asked" "$R/notes.txt"
got="$got | $(serving) | $(ends_within 8) | $( [ -e "$dir/k7" ] && echo it ran to its end || echo it was ended)"
check "quit with a job, kept: it runs to its end, then the agent ends" '1 job is running on box: keep it running, or end it? | serving | gone | it ran to its end'
settle
run "remotewait;key:ctrl+\`;frame;termwait:\$;${first}text:sleep 2 && touch $dir/k7;key:Return;wait:700;answer:end;quit;asked" "$R/notes.txt"
got="$got | $(ends_within 3) | $(sleep 2; [ -e "$dir/k7" ] && echo it ran to its end || echo it was ended)"  # (as soon as what it ended has gone)
check "and ended: it ends, and the agent at once" '1 job is running on box: keep it running, or end it? | gone | it was ended'
settle
# Stopped by hand: everything it runs ends, and it is not made again by itself.
run "remotewait;key:ctrl+\`;frame;termwait:\$;${first}command:Stop the agent on the server;asked;termend;lua:os.execute('sleep 2');frame;termdump;statusdump" "$R/notes.txt"
got="$got | $(serving) | $(tries) begun"
check "the agent stopped by hand: what it ran ends, and it is not made again" "blit: blit's agent on box was stopped ;; Stop blit's agent on box? Everything it runs there ends, for every blit. ;; \$|[connection to box lost] ended -1 ;; box: agent stopped (connect again) Terminal: box | | gone | 1 begun"
settle

# Where no socket can be made (a folder others can enter), the agent is the
# connection's own, as before: what it ran goes with the connection.
chmod 755 "$dir/run"
run "remotewait;key:ctrl+\`;frame;termwait:\$;${first}$cut;termend;termdump" "$R/notes.txt"
check "no lasting agent: a terminal goes with the connection" '$|[connection to box lost] ended -1'
# And an answer larger than the pipe to ssh takes at once still comes (what
# waits to go goes as there is room in its output, not its input's).
perl -e 'for (1..65536) { printf "%063d\n", $_ }' >"$W/large.txt"
run "remotewait;$docend;frame;statusdump" "$R/large.txt"
check "no lasting agent: a large file comes whole" 'box ~/work/large.txt | Ln 65537, Col 1 Tabs LF Plain text'
rm "$W/large.txt"
chmod 700 "$dir/run"
settle

[ -n "$print" ] || [ $failed = 1 ] || echo "agent ok"
exit $failed
