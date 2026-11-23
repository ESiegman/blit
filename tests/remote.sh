#!/bin/sh
# Working on another machine's files ("ssh://host/path"), with
# tests/fake-ssh.sh standing in for ssh: it runs what blit asks of the host
# here, in a home of its own, so blit's agent is copied and started as it
# would be there. No real host is ever asked anything.
#
#   tests/remote.sh path/to/blit            run the cases
#   tests/remote.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/remote.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
T="$dir/there"  # the home there (so that its files show as ~/...)
W="$T/work"
mkdir -p "$W/src" "$dir/home/.ssh"
printf 'hello\nworld\n' >"$W/notes.txt"
printf 'int main() {}\n' >"$W/src/main.cpp"
printf 'Host box\n    HostName example.invalid\nHost *.wild\n    User x\nHost other second\n' >"$dir/home/.ssh/config"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
printf 'blit.opt.lsp.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/git.lua"
export BLIT_SSH="$here/fake-ssh.sh" BLIT_FAKE_HOME="$T" BLIT_RECENT_FILE="$dir/recent" BLIT_FAKE_DOWN="$dir/down"
R="ssh://box$W"
# The connection dropped, as a lost link would: blit's agent there killed,
# the host then out of reach (blit would connect again by itself at once),
# and that seen. (Each run starts with it in reach again.)
drop="lua:os.execute('echo refused > $dir/down');lua:os.execute('pkill -9 -f $T/.cache/blit/[b]lit-agent');remotelost"
back="lua:os.remove('$dir/down')"
# The end of the file: Cmd+Down on macOS, Ctrl+End elsewhere.
if [ "$(uname)" = Darwin ]; then docend="key:cmd+Down"; else docend="key:cmd+End"; fi

failed=0
check() {
    # (The status bar's icons beside a branch and another machine are the
    # font's, where it has them: the words are what is compared.)
    got=$(printf '%s' "$got" | sed -e 's/⎇ //g' -e 's/⇄ //g' -e 's/ //g')
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
join() { awk 'NR > 1 { printf " ;; " } { printf "%s", $0 }'; }
# run SCRIPT [FILE]: what the run printed, its lines joined by " ;; ". INIT,
# if set, is the init.lua for this run only. (Not "VAR=x run": sh keeps
# such an assignment after a function.)
INIT=
run() {
    rm -f "$dir/down"
    got=$(BLIT_INIT="${INIT:-$dir/init.lua}" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "$1" ${2:+"$2"} 2>&1 >/dev/null |
        sed -e "s#$T#~#g" -e "s#$dir#@#g" | join)
    INIT=
}

# A file there: its tab at once, the text when it comes.
run "frame;statusdump" "$R/notes.txt"
got=$(printf '%s' "$got" | sed -e 's#^\(connecting to box… \).*/\(notes.txt loading…\).*#\1\2#')
check "opened: its tab at once, the text on its way" 'connecting to box… notes.txt loading…'
run "remotewait;frame;statusdump;dump" "$R/notes.txt"
check "and then here" 'box ~/work/notes.txt | Ln 1, Col 1 Tabs LF Plain text ;; >[notes.txt*]'
run "remotewait;text:X;save;frame;statusdump;remotewait;frame;statusdump" "$R/notes.txt"
got="$got | $(tr '\n' ' ' <"$W/notes.txt")"
check "saved there (on a thread: the tab says so meanwhile)" 'box ~/work/notes.txt saving… | Ln 1, Col 2 Tabs LF Plain text ;; box ~/work/notes.txt | Ln 1, Col 2 Tabs LF Plain text | Xhello world '
run "remotewait;text:new;save;remotewait;frame;statusdump" "$R/fresh.txt"
got="$got | $(cat "$W/fresh.txt")"
check "one that is not there: made when saved" 'box ~/work/fresh.txt | Ln 1, Col 4 Tabs LF Plain text | new'
run "remotewait;text:lost;save;remotewait;frame;statusdump" "$R/nowhere/lost.txt"
check "a save that fails: the bottom line says why, and it is still changed" 'blit: cannot write ~/work/nowhere/lost.txt.blit-tmp: No such file or directory ;; box ~/work/nowhere/lost.txt ● | Ln 1, Col 5 Tabs LF Plain text'
saveas="command:Save as…;key:ctrl+U;text:"
run "remotewait;frame;${saveas}$R/nowhere/x.txt;remotewait;findwait;key:Return;remotewait;frame;statusdump;dump" "$R/notes.txt"
check "a Save as that fails: the tab keeps its name (and is still saved)" 'blit: cannot write ~/work/nowhere/x.txt.blit-tmp: No such file or directory ;; box ~/work/notes.txt | Ln 1, Col 1 Tabs LF Plain text ;; >[notes.txt*]'
run "frame;${saveas}$W/nowhere/x.txt;findwait;key:Return;frame;statusdump;dump" "$W/notes.txt"
check "and one on this machine" 'blit: cannot write: ~/work/nowhere/x.txt.blit-tmp ;; notes.txt | Ln 1, Col 1 Tabs LF Plain text ;; >[notes.txt*]'
run "remotewait;frame;${saveas}$R/copy.txt;remotewait;findwait;key:Return;remotewait;frame;statusdump;dump" "$R/notes.txt"
got="$got | $(tr '\n' ' ' <"$W/copy.txt")"
check "and one that is written: the tab has the new name" 'box ~/work/copy.txt | Ln 1, Col 1 Tabs LF Plain text ;; >[copy.txt*] | Xhello world '
rm "$W/copy.txt"

# A file changed there under changes here: the tab keeps them, a notice
# says so, and the first save asks (Esc: not saved; or compared first, or
# saved over it). And one changed there with nothing to say so yet (the
# window not come back to): the save finds it, and asks too.
mkdir -p "$T/.moved"
there="lua:local f = io.open('$T/.moved/m.txt', 'w') f:write('alpha\\nTHERE\\n') f:close()"
fresh_m() { printf 'alpha\nbeta\n' >"$T/.moved/m.txt"; }
fresh_m
run "remotewait;frame;$docend;text:MINE;$there;reload;remotewait;frame;save;remotewait;finddump;key:Escape;frame;statusdump" "ssh://box$T/.moved/m.txt"
got="$got | $(tr '\n' ' ' <"$T/.moved/m.txt")"
check "changed there under changes here: said, and saving asks" 'blit: m.txt changed on disk: its changes here are kept, and saving asks first (Compare with disk shows the two) ;; pick [] (m.txt changed on disk since it was read here) 2 : >Save over it Compare with disk ;; box ~/.moved/m.txt ● | Ln 3, Col 5 Tabs LF Plain text | alpha THERE '
fresh_m
run "remotewait;frame;$docend;text:MINE;$there;reload;remotewait;frame;save;key:Down;key:Return;remotewait;frame;dump;lsptext" "ssh://box$T/.moved/m.txt"
check "and compared with what is there" 'blit: m.txt changed on disk: its changes here are kept, and saving asks first (Compare with disk shows the two) ;; >[m.txt m.txt (against disk, box)*] ;; @@ -1 +1 @@ /  alpha / -THERE / +beta / +MINE'
fresh_m
run "remotewait;frame;$docend;text:MINE;$there;save;remotewait;finddump;key:Return;remotewait;frame;statusdump" "ssh://box$T/.moved/m.txt"
got="$got | $(tr '\n' ' ' <"$T/.moved/m.txt")"
check "changed there unseen: the save asks, and saves over it" 'pick [] (m.txt changed on disk since it was read here) 2 : >Save over it Compare with disk ;; box ~/.moved/m.txt | Ln 3, Col 5 Tabs LF Plain text | alpha beta MINE'
fresh_m
run "remotewait;frame;$docend;text:MINE;save;remotewait;text:X;save;remotewait;frame;statusdump" "ssh://box$T/.moved/m.txt"
got="$got | $(tr '\n' ' ' <"$T/.moved/m.txt")"
check "saved twice, nothing changed there: no question" 'box ~/.moved/m.txt | Ln 3, Col 6 Tabs LF Plain text | alpha beta MINEX'
fresh_m
run "remotewait;frame;$docend;text:MINE;$there;save;find:names;text:m.;remotewait;frame;finddump;key:Escape;finddump" "ssh://box$T/.moved/m.txt"
check "the save's question with another box open: it waits for that one to close" 'names [m.] 1 of 1 : >m.txt ;; pick [] (m.txt changed on disk since it was read here) 2 : >Save over it Compare with disk'
fresh_m
there=$(printf '%s' "$there" | sed -e "s#$T/.moved#$dir#")
printf 'alpha\nbeta\n' >"$dir/m.txt"
run "frame;$docend;text:MINE;$there;reload;frame;save;finddump;key:Return;frame;statusdump" "$dir/m.txt"
got="$got | $(tr '\n' ' ' <"$dir/m.txt")"
check "and the same on this machine" 'blit: m.txt changed on disk: its changes here are kept, and saving asks first (Compare with disk shows the two) ;; pick [] (m.txt changed on disk since it was read here) 2 : >Save over it Compare with disk ;; m.txt | Ln 3, Col 5 Tabs LF Plain text | alpha beta MINE'
# Save all with two of them: each is asked about in turn.
printf 'a\n' >"$dir/a.txt" && printf 'b\n' >"$dir/b.txt"
run "frame;text:X;open:$dir/b.txt;frame;text:Y;lua:local f = io.open('$dir/a.txt', 'w') f:write('A2\\n') f:close();lua:local f = io.open('$dir/b.txt', 'w') f:write('B2\\n') f:close();reload;frame;command:Save all;frame;finddump;key:Return;frame;finddump;key:Return;frame;statusdump" "$dir/a.txt"
got="$got | $(cat "$dir/a.txt" "$dir/b.txt" | tr '\n' ' ')"
check "Save all with two changed on disk: asked about each" 'blit: a.txt changed on disk: its changes here are kept, and saving asks first (Compare with disk shows the two) ;; blit: b.txt changed on disk: its changes here are kept, and saving asks first (Compare with disk shows the two) ;; pick [] (a.txt changed on disk since it was read here) 2 : >Save over it Compare with disk ;; pick [] (b.txt changed on disk since it was read here) 2 : >Save over it Compare with disk ;; b.txt | Ln 1, Col 2 Tabs LF Plain text | Xa Yb '
rm "$dir/a.txt" "$dir/b.txt"
# Closing its tab with Save, or quitting with Save: the save's question is
# part of that, which goes on once it is answered "Save over it" (and is
# called off by "Compare with disk", or Esc, the tab kept with its changes).
printf 'alpha\nbeta\n' >"$dir/m.txt"
run "frame;$docend;text:MINE;$there;reload;frame;answer:save;key:cmd+W;finddump;key:Return;frame;finddump;dump" "$dir/m.txt"
got="$got | $(tr '\n' ' ' <"$dir/m.txt")"
check "closed with Save, then Save over it: saved, and closed" 'blit: m.txt changed on disk: its changes here are kept, and saving asks first (Compare with disk shows the two) ;; pick [] (m.txt changed on disk since it was read here) 2 : >Save over it Compare with disk ;; closed ;; >[untitled*] | alpha beta MINE'
# Vim's :wq is the same: it closes once saved over what is on disk.
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n' >"$dir/wq.lua"
printf 'alpha\nbeta\n' >"$dir/m.txt"
INIT="$dir/wq.lua" run "frame;vim:GoMINE<Esc>;$there;reload;frame;vim::wq<CR>;finddump;key:Return;frame;finddump;dump" "$dir/m.txt"
got="$got | $(tr '\n' ' ' <"$dir/m.txt")"
check ":wq, then Save over it: saved, and closed" 'blit: m.txt changed on disk: its changes here are kept, and saving asks first (Compare with disk shows the two) ;; pick [] (m.txt changed on disk since it was read here) 2 : >Save over it Compare with disk ;; closed ;; >[untitled*] | alpha beta MINE '
printf 'alpha\nbeta\n' >"$dir/m.txt"
run "frame;$docend;text:MINE;$there;reload;frame;answer:save;key:cmd+W;key:Down;key:Return;frame;finddump;dump" "$dir/m.txt"
got="$got | $(tr '\n' ' ' <"$dir/m.txt")"
check "and with Compare with disk: it stays, beside what is on disk" 'blit: m.txt changed on disk: its changes here are kept, and saving asks first (Compare with disk shows the two) ;; closed ;; >[m.txt m.txt (against disk)*] | alpha THERE '
printf 'alpha\nbeta\n' >"$dir/m.txt"
run "frame;$docend;text:MINE;$there;reload;frame;answer:save;key:cmd+W;key:Escape;frame;finddump;dump;statusdump;save;key:Return;frame;dump" "$dir/m.txt"
got="$got | $(tr '\n' ' ' <"$dir/m.txt")"
check "or Esc: it stays (and saving it later does not close it)" 'blit: m.txt changed on disk: its changes here are kept, and saving asks first (Compare with disk shows the two) ;; closed ;; >[m.txt*] ;; m.txt ● | Ln 3, Col 5 Tabs LF Plain text ;; >[m.txt*] | alpha beta MINE'
printf 'a\n' >"$dir/a.txt" && printf 'b\n' >"$dir/b.txt"
run "frame;text:X;open:$dir/b.txt;frame;text:Y;lua:local f = io.open('$dir/a.txt', 'w') f:write('A2\\n') f:close();lua:local f = io.open('$dir/b.txt', 'w') f:write('B2\\n') f:close();reload;frame;answer:save;quit;finddump;key:Return;frame;finddump;alive;key:Return;frame;finddump;alive" "$dir/a.txt"
got="$got | $(cat "$dir/a.txt" "$dir/b.txt" | tr '\n' ' ')"
check "quitting with two of them: asked about each, then out" 'blit: a.txt changed on disk: its changes here are kept, and saving asks first (Compare with disk shows the two) ;; blit: b.txt changed on disk: its changes here are kept, and saving asks first (Compare with disk shows the two) ;; pick [] (a.txt changed on disk since it was read here) 2 : >Save over it Compare with disk ;; pick [] (b.txt changed on disk since it was read here) 2 : >Save over it Compare with disk ;; running ;; closed ;; quitting | Xa Yb '
printf 'a\n' >"$dir/a.txt" && printf 'b\n' >"$dir/b.txt"
run "frame;text:X;open:$dir/b.txt;frame;text:Y;lua:local f = io.open('$dir/a.txt', 'w') f:write('A2\\n') f:close();lua:local f = io.open('$dir/b.txt', 'w') f:write('B2\\n') f:close();reload;frame;answer:save;quit;key:Escape;frame;finddump;key:Return;frame;finddump;alive" "$dir/a.txt"
got="$got | $(cat "$dir/a.txt" "$dir/b.txt" | tr '\n' ' ')"
check "and Esc to the first: still asked about the second, but not out" 'blit: a.txt changed on disk: its changes here are kept, and saving asks first (Compare with disk shows the two) ;; blit: b.txt changed on disk: its changes here are kept, and saving asks first (Compare with disk shows the two) ;; pick [] (b.txt changed on disk since it was read here) 2 : >Save over it Compare with disk ;; closed ;; running | A2 Yb '
rm "$dir/a.txt" "$dir/b.txt" "$dir/m.txt"
# And there: the file changed there seen (the window come back to), or
# found by the save itself while the close or the quit waits for it.
fresh_m
there=$(printf '%s' "$there" | sed -e "s#$dir#$T/.moved#")
run "remotewait;frame;$docend;text:MINE;$there;reload;remotewait;frame;answer:save;key:cmd+W;finddump;key:Return;remotewait;frame;finddump;dump" "ssh://box$T/.moved/m.txt"
got="$got | $(tr '\n' ' ' <"$T/.moved/m.txt")"
check "there: closed with Save, then Save over it" 'blit: m.txt changed on disk: its changes here are kept, and saving asks first (Compare with disk shows the two) ;; pick [] (m.txt changed on disk since it was read here) 2 : >Save over it Compare with disk ;; closed ;; >[untitled*] | alpha beta MINE'
fresh_m
run "remotewait;frame;$docend;text:MINE;$there;answer:save;key:cmd+W;remotewait;finddump;key:Return;remotewait;frame;finddump;dump" "ssh://box$T/.moved/m.txt"
got="$got | $(tr '\n' ' ' <"$T/.moved/m.txt")"
check "there, found by the save the close waits for: asked, then closed" 'pick [] (m.txt changed on disk since it was read here) 2 : >Save over it Compare with disk ;; closed ;; >[untitled*] | alpha beta MINE'
fresh_m
run "remotewait;frame;$docend;text:MINE;$there;answer:save;key:cmd+W;remotewait;key:Escape;remotewait;frame;finddump;dump;statusdump" "ssh://box$T/.moved/m.txt"
got="$got | $(tr '\n' ' ' <"$T/.moved/m.txt")"
check "and Esc: it stays" 'closed ;; >[m.txt*] ;; box ~/.moved/m.txt ● | Ln 3, Col 5 Tabs LF Plain text | alpha THERE '
printf 'a\n' >"$T/.moved/a.txt" && printf 'b\n' >"$T/.moved/b.txt"
run "remotewait;frame;text:X;open:ssh://box$T/.moved/b.txt;remotewait;frame;text:Y;lua:local f = io.open('$T/.moved/a.txt', 'w') f:write('A2\\n') f:close();lua:local f = io.open('$T/.moved/b.txt', 'w') f:write('B2\\n') f:close();answer:save;quit;remotewait;finddump;key:Return;frame;finddump;alive;key:Return;remotewait;frame;alive" "ssh://box$T/.moved/a.txt"
got="$got | $(cat "$T/.moved/a.txt" "$T/.moved/b.txt" | tr '\n' ' ')"
check "there, quitting with two the saves find changed: asked about each, then out" 'pick [] (a.txt changed on disk since it was read here) 2 : >Save over it Compare with disk ;; pick [] (b.txt changed on disk since it was read here) 2 : >Save over it Compare with disk ;; running ;; quitting | Xa Yb '
rm "$T/.moved/a.txt" "$T/.moved/b.txt"

# A slow host (each answer held half a second): saving goes back to the keys at
# once, and the file is saved as it was then when the write has been made
# (typing meanwhile is kept, and is not saved). Closing its tab, or
# quitting, waits for it, in a box that says so; Esc there calls that off.
mkdir -p "$T/.slow" && printf 'hello\nworld\n' >"$T/.slow/slow.txt"
L="ssh://box$T/.slow"
# slow SCRIPT [FILE]: as run, on a host that holds each answer SLOW seconds (half a second).
slow() {
    rm -f "$dir/down"
    got=$(BLIT_FAKE_SLOW=${SLOW:-0.5} BLIT_INIT="${INIT:-$dir/init.lua}" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "$1" ${2:+"$2"} 2>&1 >/dev/null |
        sed -e "s#$T#~#g" -e "s#$dir#@#g" | join)
    INIT= SLOW=
}
slow "remotewait;text:S;clock;save;within:300;frame;statusdump;text:T;frame;statusdump;remotewait;frame;statusdump" "$L/slow.txt"
got="$got | $(tr '\n' ' ' <"$T/.slow/slow.txt")"
check "a save on a slow host: at once, and saved when written" 'within 300 ms ;; box ~/.slow/slow.txt saving… | Ln 1, Col 2 Tabs LF Plain text ;; box ~/.slow/slow.txt saving… | Ln 1, Col 3 Tabs LF Plain text ;; box ~/.slow/slow.txt ● | Ln 1, Col 3 Tabs LF Plain text | Shello world '
printf 'hello\nworld\n' >"$T/.slow/slow.txt"
slow "remotewait;text:C;save;clock;key:cmd+W;within:300;finddump;dump;remotewait;finddump;dump" "$L/slow.txt"
got="$got | $(tr '\n' ' ' <"$T/.slow/slow.txt")"
check "closing it waits for the save" 'within 300 ms ;; pick [] 1 : >Close without waiting ;; >[slow.txt*] ;; closed ;; >[untitled*] | Chello world '
slow "remotewait;text:E;save;key:cmd+W;key:Escape;finddump;remotewait;dump" "$L/slow.txt"
check "Esc: it stays" 'closed ;; >[slow.txt*]'
slow "remotewait;text:N;save;key:cmd+W;key:Return;dump;remotewait" "$L/slow.txt"
got="$got | $(tr '\n' ' ' <"$T/.slow/slow.txt")"
check "closed without waiting: the save goes on" '>[untitled*] | NEChello world '
slow "remotewait;text:Q;save;quit;finddump;alive;remotewait;finddump;alive" "$L/slow.txt"
got="$got | $(tr '\n' ' ' <"$T/.slow/slow.txt")"
check "quitting waits for it too" 'pick [] 1 : >Quit without waiting ;; running ;; closed ;; quitting | QNEChello world '
fresh_m
slow "remotewait;frame;$docend;text:ONE;save;wait:300;text:TWO;save;remotewait;frame;finddump;statusdump" "ssh://box$T/.moved/m.txt"
got="$got | $(tr '\n' ' ' <"$T/.moved/m.txt")"
check "saved again before the first is written: no question (it is blit's own write there)" 'closed ;; box ~/.moved/m.txt | Ln 3, Col 7 Tabs LF Plain text | alpha beta ONETWO'
run "remotewait;frame;lua:print(blit.workdir())" "$R"
check "a folder: the working directory" "blit: ssh://box~/work"
got=$(BLIT_FAKE_FAIL=refused BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "remotewait;frame;statusdump;remotedump" "$R/notes.txt" 2>&1 >/dev/null | join)
check "a host that cannot be reached: why, and the bar says so" 'blit: cannot connect to box: ssh: connect to host: refused ;; box: not connected (connect again) untitled | Ln 1, Col 1 Tabs LF Plain text ;; box: failed: cannot connect to box: ssh: connect to host: refused; '
# And once it can be (connected again from the bar, or its command): what was opened is opened.
echo refused >"$dir/down"
got=$(BLIT_FAKE_DOWN="$dir/down" BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" \
    --script "remotewait;frame;statusdump;lua:os.remove('$dir/down');command:Reconnect to the servers that dropped;remotewait;frame;statusdump;dump" "$R/notes.txt" 2>&1 >/dev/null | sed -e "s#$T#~#g" | join)
check "and connected again: opened then" 'blit: cannot connect to box: ssh: connect to host: refused ;; box: not connected (connect again) untitled | Ln 1, Col 1 Tabs LF Plain text ;; box ~/work/notes.txt | Ln 1, Col 1 Tabs LF Plain text ;; >[notes.txt*]'
# Unless something else was opened meanwhile: the bar no longer names box, and it is not opened then.
mkdir -p "$dir/here" && printf 'here\n' >"$dir/here/local.txt"
echo refused >"$dir/down"
got=$(BLIT_FAKE_DOWN="$dir/down" BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" \
    --script "remotewait;frame;open:$dir/here/local.txt;frame;statusdump;lua:os.remove('$dir/down');command:Reconnect to the servers that dropped;remotewait;frame;dump" "$R/notes.txt" 2>&1 >/dev/null | sed -e "s#$T#~#g" | join)
check "a file here opened meanwhile: box not named, and not opened when it can be" 'blit: cannot connect to box: ssh: connect to host: refused ;; local.txt | Ln 1, Col 1 Tabs LF Plain text ;; >[local.txt*]'
echo refused >"$dir/down"
got=$(BLIT_FAKE_DOWN="$dir/down" BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" \
    --script "remotewait;frame;folder:$dir/here;frame;statusdump;lua:os.remove('$dir/down');command:Reconnect to the servers that dropped;remotewait;frame;dump;lua:print(blit.workdir())" "$R" 2>&1 >/dev/null | sed -e "s#$T#~#g" -e "s#$dir#@#g" | join)
check "and a folder here" 'blit: cannot connect to box: ssh: connect to host: refused ;; untitled | Ln 1, Col 1 Tabs LF Plain text ;; >[untitled*] ;; blit: @/here'
rm -rf "$dir/here"

# No key for the host: ssh's questions come to blit's box, hidden as they
# are typed. (The password is the test's own, made up here.)
pw=not-a-real-password-42
got=$(BLIT_FAKE_PASSWORD=$pw BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "remotewait;finddump;text:$pw;finddump;key:Return;remotewait;frame;statusdump" "$R/notes.txt" 2>&1 >/dev/null | sed -e "s#$T#~#g" | join)
check "a password asked for, and answered" "ask box \"tester@box's password:\" [] ;; ask box \"tester@box's password:\" [**********************] ;; box ~/work/notes.txt | Ln 1, Col 1 Tabs LF Plain text"
got=$(BLIT_FAKE_PASSWORD=$pw BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "remotewait;text:wrong;key:Return;remotewait;frame;finddump" "$R/notes.txt" 2>&1 >/dev/null | join)
check "a wrong one: ssh says so" 'blit: cannot connect to box: tester@box: Permission denied, please try again. ;; closed'
got=$(BLIT_FAKE_PASSWORD=$pw BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "remotewait;key:Escape;remotewait;frame;finddump" "$R/notes.txt" 2>&1 >/dev/null | join)
check "Esc: not connecting" 'blit: cannot connect to box: tester@box: Permission denied (publickey,password). ;; closed'
got=$(BLIT_FAKE_PASSWORD=$pw BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "find:folder;text:ssh://box$W/;remotewait;finddump;text:$pw;key:Return;remotewait;findwait;finddump" 2>&1 >/dev/null | sed -e "s#$T#~#g" -e 's#\[[^]]*ssh://#[ssh://#g' | join)
check "asked while the box that opens folders is open: that box comes back" "ask box \"tester@box's password:\" [] ;; folder [ssh://box~/work/] 3 : >./ ../ src/"

# Folders, the finder and the box that opens files.
run "remotewait;lua:local t = blit.fs.list('$R') table.sort(t, function(a, b) return a.name < b.name end) for _, e in ipairs(t) do print(e.name, e.type) end;lua:print(blit.fs.exists('$R/src'), blit.fs.exists('$R/gone'))" "$R"
check "blit.fs, there" 'blit: fresh.txt	file ;; blit: notes.txt	file ;; blit: src	dir ;; blit: dir	nil'
run "remotewait;find:names;text:.;findwait;finddump" "$R"
check "files found by name there (the one opened lately first)" 'names [.] 3 of 3 : >notes.txt fresh.txt src/main.cpp'
run "remotewait;find:text;text:main;findwait;finddump;key:Return;remotewait;frame;statusdump" "$R"
check "text searched for there, and a line found opened" 'text [main] 1 of 1 : >src/main.cpp:1:5 ;; box src/main.cpp | Ln 1, Col 5 Tabs LF C++'
run "remotewait;find:text;text:main;findwait;key:Escape;$drop;find:text;text:main;findwait;finddump" "$R"
check "the connection lost: a search there says so, not that no line has it" 'text [main] 0 of 0 : (The connection to box was lost)'
run "remotewait;$drop;find:names;text:ma;findwait;finddump;key:cmd+shift+F;findwait;finddump" "$R"
check "and a folder there that could not be listed: why, in both" 'names [ma] 0 of 0 : (The connection to box was lost) ;; text [ma] 0 of 0 : (The connection to box was lost)'
run "remotewait;$drop;key:cmd+K;text:reconn;findwait;finddump;$back;key:Return;remotewait;key:cmd+K;text:main;findwait;finddump" "$R"
check "and Search everywhere: why, the commands still there; connected again, the files" 'everywhere [reconn] 1 : (The connection to box was lost) Commands: >Reconnect to the servers that dropped ;; everywhere [main] 1 : Files: >src/main.cpp'
run "remotewait;$drop;key:cmd+K;text:main;findwait;finddump;$back;command:Reconnect to the servers that dropped;remotewait;findwait;finddump" "$R"
check "connected again while it is open: the files come" 'everywhere [main] 0 : (The connection to box was lost) ;; everywhere [main] 1 : Files: >src/main.cpp'
# A host that does not answer when it is connected to again (the link down:
# ssh waits): Enter just after typing does not wait for the files there.
hang="lua:os.execute('echo hang > $dir/down')"
got=$(BLIT_FAKE_DOWN="$dir/down" BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" \
    --script "remotewait;$drop;$hang;wait:3200;key:cmd+K;text:main;clock;key:Return;within:300;lua:os.remove('$dir/down');remotewait;findwait;finddump" "$R" 2>&1 >/dev/null | join)
rm -f "$dir/down"
check "and Enter in it with no answer from there: no waiting" 'within 300 ms ;; everywhere [main] 1 : Files: >src/main.cpp'
for box in cmd+P cmd+shift+F; do
    got=$(BLIT_FAKE_DOWN="$dir/down" BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" \
        --script "remotewait;$drop;$hang;wait:3200;key:$box;text:main;clock;key:Return;within:300;lua:os.remove('$dir/down')" "$R" 2>&1 >/dev/null | join)
    rm -f "$dir/down"
    check "and Enter in Go to file or Search in files ($box) with no answer from there: no waiting" 'within 300 ms'
done
run "remotewait;find:open;remotewait;finddump;text:src/;remotewait;finddump" "$R"
check "the box that opens files, in a folder there" 'open [ssh://box~/work/] 4 : >../ src/ fresh.txt notes.txt ;; open [ssh://box~/work/src/] 2 : >../ main.cpp'
slow "remotewait;find:open;remotewait;clock;text:src/;within:300;finddump;text:m;remotewait;finddump" "$R"
check "a folder listed on a slow host: typing goes on, and its files come when they do" 'within 300 ms ;; open [ssh://box~/work/src/] 1 : >../ ;; open [ssh://box~/work/src/m] 1 : >main.cpp'
HOME="$dir/home" run "find:folder;text:ssh://;finddump"
got=$(printf '%s' "$got" | sed -e 's#\[.*ssh://#[ssh://#')
check "ssh:// typed: the hosts ~/.ssh/config names (not its patterns)" 'folder [ssh://] 5 : >./ ../ box/ other/ second/'
HOME="$dir/home" run "find:folder;text:ssh://tester@box;finddump;key:Return;remotewait;finddump;key:Return;lua:print(blit.workdir())"
got=$(printf '%s' "$got" | sed -e 's#\[[^]]*ssh://#[ssh://#g')
check "a host it does not name, typed: offered as it is, and Enter goes to its home folder" 'folder [ssh://tester@box] 1 : >tester@box/ ;; folder [ssh://tester@box/~/] 3 : >./ ../ work/ ;; blit: ssh://tester@box~'

# Git, run there.
mkdir -p "$dir/repo/src" && printf 'one\ntwo\nthree\n' >"$dir/repo/notes.txt"
(cd "$dir/repo" && HOME="$T" git init -q -b main && HOME="$T" git add -A && HOME="$T" git -c user.name=t -c user.email=t@example.invalid commit -qm first) || exit 1
printf 'one\nTWO\nthree\nfour\n' >"$dir/repo/notes.txt"
INIT="$dir/git.lua"
run "remotewait;panel;frame;gitwait;frame;open:ssh://box$dir/repo/notes.txt;remotewait;frame;gitwait;frame;gitwait;frame;gitdump" "ssh://box$dir/repo"
check "git's view of a repository there" 'main | changes M:notes.txt | branches *main | commits 1 "first" | lines 2~ 4+'
INIT="$dir/git.lua"
run "remotewait;panel;frame;gitwait;frame;git:diff:notes.txt;gitwait;dump" "ssh://box$dir/repo"
check "a diff there: its tab names the machine" '>[untitled notes.txt (changes, box)*]'
INIT="$dir/git.lua" SLOW=0.5 slow "remotewait;panel;frame;gitwait;frame;open:ssh://box$dir/repo/notes.txt;remotewait;frame;gitwait;frame;gitwait;command:Show or hide who changed each line (git blame);frame;gitbusy;clock;key:cmd+W;within:300;dump" "ssh://box$dir/repo"
check "a tab closed while a slow blame of it runs: at once (the blame is stopped)" 'within 300 ms ;; >[untitled*]'
# Another worktree there, opened from the Git tab: git there names it without
# the host, and the folder opened (and its file in front) is still that host's.
(cd "$dir/repo" && HOME="$T" git worktree add -q -b feature "$dir/repo-feature" 2>/dev/null) || exit 1
INIT="$dir/git.lua"
run "remotewait;panel;frame;gitwait;frame;open:ssh://box$dir/repo/notes.txt;remotewait;frame;gitwait;git:opentree:$dir/repo-feature;gitwait;remotewait;frame;statusdump;lua:print(blit.workdir())" "ssh://box$dir/repo"
check "another worktree there: opened there" 'box feature notes.txt | Ln 1, Col 1 Tabs LF Plain text ;; blit: ssh://box@/repo-feature'
# A new worktree there: git there is given the path there (beside the
# repository), and the tree made is then opened there as another is.
INIT="$dir/git.lua"
run "remotewait;panel;frame;gitwait;frame;open:ssh://box$dir/repo/notes.txt;remotewait;frame;gitwait;git:worktree:topic;gitwait;remotewait;frame;statusdump;lua:print(blit.workdir())" "ssh://box$dir/repo"
got="$got | $(cd "$dir/repo" && HOME="$T" git worktree list --porcelain | grep -c "^worktree $dir/repo-topic\$")"
check "a new worktree there: made there, and opened" 'box topic notes.txt | Ln 1, Col 1 Tabs LF Plain text ;; blit: ssh://box@/repo-topic | 1'
# One git there will not make (its folder is there already): the Git tab
# says why (not what git said it was doing first), and the folder stays.
INIT="$dir/git.lua"
run "remotewait;panel;frame;gitwait;frame;git:worktree:feature;gitwait;gitdump;lua:print(blit.workdir())" "ssh://box$dir/repo"
got=$(printf '%s' "$got" | sed -n 's/.* | note \(.*\) ;; \(blit: ssh:[^ ]*\)$/\1 ;; \2/p')
check "a new worktree there that git refuses: why, and the folder stays" "New worktree: fatal: '@/repo-feature' already exists ;; blit: ssh://box@/repo"

# A language server, run there (the test one, next to blit).
if [ -x "$(dirname "$blit")/blit_fakelsp" ]; then
    printf 'blit.opt.git.enabled = false\nblit.opt.lsp.servers.fake = { command = { "%s", "--utf8" }, languages = { "cpp" }, roots = { "CMakeLists.txt" } }\n' "$(cd "$(dirname "$blit")" && pwd)/blit_fakelsp" >"$dir/lsp.lua"
    mkdir -p "$dir/proj/src" && : >"$dir/proj/CMakeLists.txt"
    printf 'int a;\nint b; // an ERROR here\nint c;\nint d; // and a WARN\n' >"$dir/proj/src/e.cpp"
    INIT="$dir/lsp.lua"
    run "remotewait;frame;ready;diag;frame;text:];text:d;vimdump" "ssh://box$dir/proj/src/e.cpp"
    check "its diagnostics, in the file here" 'NORMAL |  | 2:14 | int b; // an ERROR here'
    # A symbol in a file there not open yet, past letters a server counting
    # UTF-16 counts one unit each: the cursor at its name once the file is here.
    printf 'blit.opt.git.enabled = false\nblit.opt.lsp.servers.fake = { command = { "%s" }, languages = { "cpp" }, roots = { "CMakeLists.txt" } }\n' "$(cd "$(dirname "$blit")" && pwd)/blit_fakelsp" >"$dir/lsp16.lua"
    printf '/*\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251\303\251*/ int wide(int n) {\n}\n' >"$dir/proj/w.cpp"
    INIT="$dir/lsp16.lua"
    run "remotewait;frame;ready;key:cmd+alt+P;frame;text:wide;frame;wait:300;frame;key:Return;remotewait;frame;vimdump" "ssh://box$dir/proj/src/e.cpp"
    check "a symbol in a file not open there yet: at its name" 'NORMAL |  | 1:30 | /*éééééééééé*/ int wide(int n) {'
    # And a reference found there, in a file not open yet (whose line could
    # not be read when it was found): at its name too, once the file is here.
    printf 'int wide;\n' >"$dir/proj/src/f.cpp"
    INIT="$dir/lsp16.lua"
    run "remotewait;frame;ready;key:Right;key:Right;key:Right;key:Right;key:shift+F12;wait:300;key:Down;key:Return;remotewait;frame;vimdump" "ssh://box$dir/proj/src/f.cpp"
    check "a reference in a file not open there yet: at its name" 'blit: Reference 2 of 2 ;; NORMAL |  | 1:30 | /*éééééééééé*/ int wide(int n) {'
    INIT="$dir/lsp16.lua"
    run "remotewait;frame;ready;key:Right;key:Right;key:Right;key:Right;key:shift+F12;wait:300;key:Down;key:Return;remotewait;frame;wait:300;key:F4;key:F4;frame;vimdump" "ssh://box$dir/proj/src/f.cpp"
    check "and again by F4, once it is open" 'blit: Reference 2 of 2 ;; blit: Reference 1 of 2 ;; blit: Reference 2 of 2 ;; NORMAL |  | 1:30 | /*éééééééééé*/ int wide(int n) {'
fi

# No server for the language there (three are named): said once, not once
# for each. And the connection lost while they are looked for there: that
# is said (once), not that none is installed.
printf 'int a;\n' >"$W/src/a.cpp"
none="blit.opt.git.enabled = false
blit.opt.editor.modal = false
blit.opt.lsp.servers.clangd = { enabled = false }"
for n in 1 2 3; do none="$none
blit.opt.lsp.servers.none$n = { command = { \"blit-no-such-server-$n\" }, languages = { \"cpp\" } }"; done
printf '%s\n' "$none" >"$dir/none.lua"
INIT="$dir/none.lua"
run "remotewait;frame;lspsettle;wait:300;frame;lspsettle" "$R/src/a.cpp"
check "no server there: said once, and in a toast which, and where" 'blit: language server: no language server for this file is installed on box ;; blit: No language server for C++: none of blit-no-such-server-1, blit-no-such-server-2 or blit-no-such-server-3 is installed on box (lsp.servers in Settings)'
INIT="$dir/none.lua" run "remotewait;frame;lspsettle;wait:300;frame;lspsettle;wait:300;frame;lspsettle" "ssh://tester@box$W/src/a.cpp"
check "and the same with a user named" 'blit: language server: no language server for this file is installed on tester@box ;; blit: No language server for C++: none of blit-no-such-server-1, blit-no-such-server-2 or blit-no-such-server-3 is installed on tester@box (lsp.servers in Settings)'
INIT="$dir/none.lua" slow "remotewait;frame;wait:300;$drop;lspsettle;wait:300;lspsettle" "$R/src/a.cpp"
check "the connection lost while one is looked for there: said, once" 'blit: language server: the connection to box was lost'
rm "$W/src/a.cpp"

# The tree, on a slow host: new, rename and the Trash are done on its
# thread (the keys go on at once), and it shows them when they are done; a
# file open in a tab goes with its new name, and what fails says why.
mkdir -p "$T/.tree/src" && printf 'a\n' >"$T/.tree/a.txt" && : >"$T/.tree/src/main.cpp"
up5="key:Backspace;key:Backspace;key:Backspace;key:Backspace;key:Backspace"
SLOW=0.3 slow "remotewait;treewait;open:ssh://box$T/.tree/a.txt;remotewait;key:cmd+shift+E;key:Down;key:cmd+N;text:made.txt;clock;key:Return;within:300;treewait;key:End;key:F2;$up5;text:src;key:Return;treewait;frame;statusdump;key:F2;$up5;text:b.txt;key:Return;treewait;frame;statusdump;key:Delete;treewait" "ssh://box$T/.tree"
got="$got | $(cd "$T/.tree" && ls * | tr '\n' ' ')| $(ls "$T/.local/share/Trash/files")"
check "the tree's changes there" 'within 300 ms ;; blit: "src" is already there ;; box a.txt | Ln 1, Col 1 Tabs LF Plain text ;; box b.txt | Ln 1, Col 1 Tabs LF Plain text | made.txt main.cpp | b.txt'

# The tree of a folder there, on a slow host: "Listing…" until its names
# come (not "This folder is empty"), and then what is in it.
mkdir -p "$T/.listed/some" "$T/.listed/none" && : >"$T/.listed/some/one.txt"
SLOW=0.3 slow "remotewait;key:cmd+shift+E;frame;treedump;treewait;frame;treedump" "ssh://box$T/.listed/some"
check "a folder there on its way: listing, then its files" 'tree: (Listing…) ;; tree: one.txt@42'
SLOW=0.3 slow "remotewait;key:cmd+shift+E;frame;treedump;treewait;frame;treedump" "ssh://box$T/.listed/none"
check "and an empty one" 'tree: (Listing…) ;; tree: (This folder is empty)'
# And one that cannot be listed for want of the connection: why, not that it is empty.
down="lua:os.execute('echo refused > $dir/down');lua:os.execute('pkill -9 -f $T/.cache/blit/[b]lit-agent');remotelost"
got=$(BLIT_FAKE_DOWN="$dir/down" BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" \
    --script "remotewait;key:cmd+shift+E;treewait;frame;treedump;$down;workdir:ssh://box$T/.listed/none;treewait;frame;treedump" "ssh://box$T/.listed/some" 2>&1 >/dev/null | join)
rm -f "$dir/down"
check "a folder there with no connection: why" 'tree: one.txt@42 ;; tree: (The connection to box was lost)'

# The New project tab, making a project there on a slow host: Create goes
# back to the keys at once (the form says it is creating), the project is
# made on its thread once what the host has has been looked at, and its
# main file opens; one that cannot be made says why in the form. What the
# host has is looked for on a thread too: a Location changed again, a name
# typed and Enter on the libraries wait for none of it (the search opens
# when it has come). (projectdump's fields are left out here.)
fields() { got=$(printf '%s' "$got" | sed -E 's/ \| [a-z_]+=[^|]* \| at [0-9]+//g'); }
SLOW=0.2 slow "newproject;project:where=ssh://box$T/proj;project:kind=C;project:build=Make;project:git=false;project:name=app;clock;projectcreate;within:300;projectdump;projectwait;projectdump;remotewait;frame;statusdump"
fields
got=$(printf '%s' "$got" | sed -e 's/ C (no colours)/ C/')  # (whether this machine has C's grammar is not the question here)
got="$got | $(cd "$T/proj/app" && find . -type f | LC_ALL=C sort | tr '\n' ' ')"
check "a project made there: at once, and made" 'within 300 ms ;; kind=C name=app where=ssh://box~/proj git=off | creating ;; kind=C name=app where=ssh://box~/proj git=off | made ssh://box~/proj/app ;; box src/main.c | Ln 1, Col 1 Spaces: 4 LF C | ./Makefile ./README.md ./compile_flags.txt ./include/app/app.h ./src/app.c ./src/main.c ./tests/test_app.c '
run "newproject;project:where=ssh://box$T/proj;project:name=app;projectcreate;projectwait;projectdump"
fields
check "and one that is there already: the form says so" 'kind=C++ name=app where=ssh://box~/proj git=on | error: ssh://box~/proj/app is there already.'
SLOW=0.3 slow "newproject;project:where=ssh://box$T/a;clock;project:where=ssh://box$T/proj;key:Down;text:n;text:b;key:Return;key:Down;key:Down;key:Down;key:Down;key:Down;key:Return;within:300;projectdump;projectwait;projectdump;finddump"
fields
got=$(printf '%s' "$got" | sed -e 's/ | error: vcpkg is not to be found.*/ | searched/' -e 's/^\(.*\) ;; pick \[.*/\1 | searched/')  # (its vcpkg there, or none)
check "what the host has, looked for meanwhile: the form goes on" 'within 300 ms ;; kind=C++ name=nb where=ssh://box~/proj git=on | looking ;; kind=C++ name=nb where=ssh://box~/proj git=on | searched'

# A program there that writes far faster than what reads it here: no more
# than a few MB of it are held here (the agent stops reading it meanwhile,
# or, an older agent, the connection is not read), and all of it comes.
run "remotewait;remotestream:67108864" "$R/notes.txt"
check "a stream nobody reads for a while: held there" 'streamed 67108864 bytes (exit 0), held a few MB at most'
got=$(BLIT_AGENT_LEVEL=1 BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "remotewait;remotestream:67108864" "$R/notes.txt" 2>&1 >/dev/null | join)
check "and from an agent that cannot pause it" 'streamed 67108864 bytes (exit 0), held a few MB at most'

# And one that takes none of what it is sent for a while: no more than a
# few MB of it are held for it (the agent tells blit to hold the rest, so
# that what writes to it waits), and all of it comes.
run "remotewait;remotefeed:67108864" "$R/notes.txt"
check "a program there that takes no input for a while: held back here" 'fed 67108864 bytes, it read 67108864 (exit 0), a few MB taken before it read'

# A big save there does not hold up the keys to a terminal there: the file
# goes in parts, and what is small goes between them. (Over a connection
# that carries 8 MB a second to the host: the save takes two seconds.)
cat >"$dir/narrow-ssh.sh" <<EOF
#!/bin/sh
# tests/fake-ssh.sh, with what blit sends coming to it at \$BLIT_NARROW bytes a second.
perl -e '\$| = 1; my \$rate = \$ENV{BLIT_NARROW}; while ((my \$n = sysread(STDIN, my \$b, 16384)) > 0) { syswrite(STDOUT, \$b) or exit 0; select(undef, undef, undef, \$n / \$rate) }' |
    exec "$here/fake-ssh.sh" "\$@"
EOF
chmod +x "$dir/narrow-ssh.sh"
mkdir -p "$W/big" && perl -e 'for (1..262144) { printf "%063d\n", $_ }' >"$W/big/big.txt"
keys="remotewait;text:X;key:ctrl+\`;frame;termwait:\$;key:space;key:ctrl+\`;frame;save;key:ctrl+\`;key:space;frame;wait:500;clock;text:echo h''i;key:Return;termwait:hi;within:1000;remotewait;frame;statusdump"
got=$(BLIT_SSH="$dir/narrow-ssh.sh" BLIT_NARROW=8000000 BLIT_TERM_SHELL="exec env PS1='\$ ' /bin/sh -i" BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" \
    --script "$keys" "$R/big/big.txt" 2>&1 >/dev/null | join)
got="$got | $(head -c 3 "$W/big/big.txt") $(wc -c <"$W/big/big.txt" | tr -d ' ')"
check "a big save there: keys to a terminal there go between its parts" 'within 1000 ms ;; box Terminal: box | | X00 16777217'
got=$(BLIT_AGENT_LEVEL=2 BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "remotewait;text:Y;save;remotewait;frame;statusdump" "$R/big/big.txt" 2>&1 >/dev/null |
    sed -e "s#$T#~#g" | join)
got="$got | $(head -c 3 "$W/big/big.txt") $(wc -c <"$W/big/big.txt" | tr -d ' ')"
check "and to an agent from before parts: whole" 'box ~/work/big/big.txt | Ln 1, Col 2 Tabs LF Plain text | YX0 16777218'

# A big file read from there comes the same way: in parts of about 1 MB
# (kReadAt), with what else the connection carries going between them, the
# status bar saying how much has come, and a click there stopping it. (Over
# a connection that brings 8 MB a second back from the host: the 20 MB file
# takes over two seconds.) No frame from the agent is larger than a part.
cat >"$dir/slow-ssh.sh" <<EOF
#!/bin/sh
# tests/fake-ssh.sh, with what comes back from the host coming at \$BLIT_NARROW bytes a second.
"$here/fake-ssh.sh" "\$@" | exec perl -e '\$| = 1; my \$rate = \$ENV{BLIT_NARROW}; while ((my \$n = sysread(STDIN, my \$b, 16384)) > 0) { syswrite(STDOUT, \$b) or exit 0; select(undef, undef, undef, \$n / \$rate) }'
EOF
chmod +x "$dir/slow-ssh.sh"
perl -e 'for (1..327680) { printf "%063d\n", $_ }' >"$W/big/dump.txt"
printf 'after\n' >"$W/big/after.txt"
slowly() {
    got=$(BLIT_SSH="$dir/slow-ssh.sh" BLIT_NARROW=8000000 BLIT_TERM_SHELL="exec env PS1='\$ ' /bin/sh -i" BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" \
        --script "$1" "$R/notes.txt" 2>&1 >/dev/null | sed -e "s#$T#~#g" -e 's/loading [0-9]* of/loading N of/' | join)
}
slowly "remotewait;key:ctrl+\`;frame;termwait:\$;key:space;key:ctrl+\`;frame;open:$R/big/dump.txt;fetchwait:2000000;frame;statusdump;key:ctrl+\`;key:space;frame;clock;text:echo h''i;key:Return;termwait:hi;within:2000;fetchdump;remotewait;key:ctrl+\`;$docend;frame;statusdump;remoteframes"
# (Within 2 s, with the fetch still going after (fetchdump): the whole 20 MB
# takes 2.5 s at this rate, and before parts the terminal waited for all of
# it, 2.9 s; GitHub's runner answered in 1.1 s, this Mac in 0.2.)
check "a big file read from there: in parts, said as it comes, a terminal there answering meanwhile" 'box ~/work/big/dump.txt loading N of 20 MB… | Ln 1, Col 1 Tabs LF Plain text ;; within 2000 ms ;; fetching dump.txt (20 MB) ;; box ~/work/big/dump.txt | Ln 327681, Col 1 Tabs LF Plain text ;; box: no frame over 1 MB'
slowly "remotewait;open:$R/big/dump.txt;fetchwait:2000000;frame;statusclick:loading;remotewait;fetchdump;open:$R/big/after.txt;remotewait;frame;dump;statusdump;remoteframes"
check "stopped from the status bar: its tab goes, and the connection goes on" 'blit: Stopped reading ssh://box~/work/big/dump.txt ;; nothing fetched ;; >[notes.txt after.txt*] ;; box ~/work/big/after.txt | Ln 1, Col 1 Tabs LF Plain text ;; box: no frame over 1 MB'
slowly "remotewait;open:$R/big/dump.txt;fetchwait:2000000;key:cmd+W;clock;remotewait;within:500;fetchdump;frame;dump"
check "and by closing its tab" 'within 500 ms ;; nothing fetched ;; >[notes.txt*]'
# From an agent before parts (level 4): whole, as before.
got=$(BLIT_AGENT_LEVEL=4 BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "remotewait;fetchdump;$docend;frame;statusdump" "$R/big/dump.txt" 2>&1 >/dev/null |
    sed -e "s#$T#~#g" | join)
check "and from an agent before parts: whole" 'nothing fetched ;; box ~/work/big/dump.txt | Ln 327681, Col 1 Tabs LF Plain text'
# Any large answer comes in parts: a folder of 20,000 names (1.3 MB of them).
mkdir -p "$W/many" && perl -e 'for (1..20000) { open(my $f, ">", sprintf("%s/%060d", $ARGV[0], $_)) or die }' "$W/many"
run "remotewait;lua:local f = io.open('$dir/count', 'w') f:write(#blit.fs.list('$R/many')) f:close();remoteframes" "$R/notes.txt"
got="$got | $(cat "$dir/count") names"
check "a large answer (a folder's names): in parts" 'box: no frame over 1 MB | 20000 names'
rm -rf "$W/many"

# gf and a click on a path there (Cmd+click in a terminal takes the same
# way): the file there, or, if it is not there, no new tab but a word.
printf 'src/main.cpp\nmissing.cpp\nmissing.cpp:3\n' >"$W/links.txt"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n' >"$dir/vim.lua"
INIT="$dir/vim.lua" run "remotewait;vim:gf;remotewait;frame;dump" "$R/links.txt"
check "gf on a name there opens the file there" '>[links.txt main.cpp*]'
INIT="$dir/vim.lua" run "remotewait;vim:jgf;remotewait;frame;dump" "$R/links.txt"
check "one that is not there is said so, and leaves no tab" 'blit: No file ssh://box~/work/missing.cpp ;; >[links.txt*]'
INIT="$dir/vim.lua" run "remotewait;vim:2jgf;remotewait;frame;dump" "$R/links.txt"
check "nor does it at a line (as a click on a path:line does)" 'blit: No file ssh://box~/work/missing.cpp ;; >[links.txt*]'
rm "$W/links.txt"
# The status bar names the machine of the tab in front: none for a file
# here, with the folder there; a terminal's own, with the folder on another.
printf 'here\n' >"$dir/local.txt"
run "remotewait;open:$dir/local.txt;frame;statusdump" "$R"
got=$(printf '%s' "$got" | sed -e 's#[^ ]*/local.txt.*#local.txt#')  # (the file's path, and what follows it, as wide as the window allows)
check "a file here, the folder there: no machine" 'local.txt'
got=$(BLIT_TERM_SHELL="exec env PS1='\$ ' /bin/sh -i" BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" \
    --script "remotewait;key:ctrl+\`;frame;termwait:\$;workdir:$R;remotewait;frame;statusdump" "ssh://other$W" 2>&1 >/dev/null | join)
check "a terminal on one machine, the folder on another: the terminal's" 'other Terminal: other |'

# The header for a source and the other way round, looked for there (with
# no language server to ask): beside it, elsewhere in the project (not in a
# build folder), or none; on a slow host, the keys go on meanwhile.
mkdir -p "$W/sw/include" "$W/sw/src/gpu" "$W/sw/build"
printf 'int a();\n' >"$W/sw/a.hpp" && printf 'int a() { return 1; }\n' >"$W/sw/a.cpp" && printf 'int z;\n' >"$W/sw/z.hpp"
printf 'int k();\n' >"$W/sw/include/kernels.hpp" && printf 'int k() { return 2; }\n' >"$W/sw/src/gpu/kernels.hip" && : >"$W/sw/build/kernels.cpp"
sw="command:Switch between header and source;remotewait;frame;statusdump"
nocolours() { got=$(printf '%s' "$got" | sed -e 's/ (no colours)//g'); }  # (whether this machine has C++'s grammar is not the question here)
run "remotewait;open:$R/sw/a.hpp;remotewait;$sw;open:$R/sw/include/kernels.hpp;remotewait;$sw;open:$R/sw/z.hpp;remotewait;$sw" "$R/sw"
nocolours
check "the other half, there" 'box a.cpp | Ln 1, Col 1 Tabs LF C++ ;; box src/gpu/kernels.hip | Ln 1, Col 1 Tabs LF C++ ;; blit: No header or source found for z.hpp ;; box z.hpp | Ln 1, Col 1 Tabs LF C++'
slow "remotewait;open:$R/sw/include/kernels.hpp;remotewait;clock;command:Switch between header and source;within:300;remotewait;frame;statusdump" "$R/sw"
nocolours
check "and on a slow host" 'within 300 ms ;; box src/gpu/kernels.hip | Ln 1, Col 1 Tabs LF C++'

# A project's run configurations there: the Run button runs the program
# there, by its path there (not ssh://...), in its folder, after its build.
mkdir -p "$W/run/bin" "$W/run/.blit"
printf '#!/bin/sh\necho "ran $1 in $(basename "$PWD")" >"$HOME/ran.txt"\n' >"$W/run/bin/hello" && chmod +x "$W/run/bin/hello"
printf 'all:\n\ttouch built\n' >"$W/run/Makefile"
printf '{ "configurations": [ { "name": "hello", "program": "bin/hello", "args": ["5"] } ] }\n' >"$W/run/.blit/run.json"
run "remotewait;frame;buildrun;buildwait;builddump" "$R/run"
got="$got | $(cat "$T/ran.txt" 2>/dev/null)"
check "a run configuration there: run there" 'Run: >hello | Make: make make all (last: 0) | ran 5 in run'
printf '{ "configurations": [ { "name": "hello", "program": "bin/hello", "args": ["6"], "cwd": "bin", "build": "make" } ] }\n' >"$W/run/.blit/run.json"
run "remotewait;frame;buildrun;buildwait;builddump" "$R/run"
got="$got | $(cat "$T/ran.txt" 2>/dev/null) | $(LC_ALL=C ls "$W/run" | tr '\n' ' ')"
check "and with its build first, in its own folder" 'Run: >hello | Make: make make all (last: 0) | ran 6 in bin | Makefile bin built '

# The Python console in a folder there with a virtual environment: its
# IPython, by its path there; chosen there, so that opening it waits for
# nothing on a slow host.
mkdir -p "$W/py/.venv/bin"
printf '#!/bin/sh\nprintf "In [1]: "\nread line\n' >"$W/py/.venv/bin/ipython" && chmod +x "$W/py/.venv/bin/ipython"
run "remotewait;command:Open the Python console;frame;termwait:In [1];termdump" "$R/py"
got=$(printf '%s' "$got" | sed -e 's/ @[0-9,]* [0-9x]*$//')
check "the Python console there: the folder's IPython" 'In [1]:'
slow "remotewait;clock;command:Open the Python console;within:300;termwait:In [1];termdump" "$R/py"
got=$(printf '%s' "$got" | sed -e 's/ @[0-9,]* [0-9x]*$//')
check "opened at once on a slow host" 'within 300 ms ;; In [1]:'

# pytest there (a stand-in, on the PATH the host is given: one test that
# passes, one that fails, and its report where it is told to write it).
mkdir -p "$dir/bin" "$W/py/tests"
cat >"$dir/bin/pytest" <<'EOF'
#!/bin/sh
for a in "$@"; do
    case $a in
        --collect-only) printf 'tests/test_a.py::test_one\ntests/test_a.py::test_two\n'; exit 0 ;;
        --junitxml=*) out=${a#--junitxml=} ;;
    esac
done
printf '<testsuites><testsuite><testcase classname="tests.test_a" name="test_one" time="0.01"/><testcase classname="tests.test_a" name="test_two" time="0.01"><failure message="1 is not 2">x</failure></testcase></testsuite></testsuites>\n' >"$out" || exit 1
exit 1
EOF
chmod +x "$dir/bin/pytest"
printf 'def test_one():\n    pass\n\n\ndef test_two():\n    assert 1 == 2\n' >"$W/py/tests/test_a.py"
PATH="$dir/bin:$PATH" slow "remotewait;testdump;clock;command:Run the tests;within:300;testdump" "$R/py"
check "the tests run on a slow host: at once, and how they went" 'tests/test_a.py: .test_one .test_two ;; within 300 ms ;; blit: 1 failed, 1 passed ;; tests/test_a.py: +test_one xtest_two'

# Their reports are written there and read back, not in this machine's temp
# folder (which the host cannot write in here: on a real host it is not
# there); and the marks beside the tests in a file there. With coverage too
# (coverage.py's stand-in: the run through pytest's, and what ran as JSON).
cat >"$dir/bin/coverage" <<'EOF'
#!/bin/sh
case $1 in
    run) shift 3; exec pytest "$@" ;;
    json) while [ $# -gt 0 ]; do [ "$1" = -o ] && out=$2; shift; done
          printf '{"files": {"tests/test_a.py": {"executed_lines": [1, 2, 5, 6], "missing_lines": [3]}}}\n' >"$out" ;;
esac
EOF
chmod +x "$dir/bin/coverage"
mkdir -p "$dir/ro" && chmod 555 "$dir/ro"
reports() { ls "$T/.cache/blit" | grep -c blit-tests; }
got=$(TMPDIR="$dir/ro" PATH="$dir/bin:$PATH" BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" \
    --script "workdir:$R/py;remotewait;testdump;command:Run the tests;testdump" "$R/py/tests/test_a.py" 2>&1 >/dev/null | join)
got="$got | $(reports)"
check "pytest's report read back from there, and the marks in the file there" \
    'tests/test_a.py: .test_one .test_two | here: 1. 5. ;; blit: 1 failed, 1 passed ;; tests/test_a.py: +test_one xtest_two | here: 1+ 5x | 0'
got=$(TMPDIR="$dir/ro" PATH="$dir/bin:$PATH" BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" \
    --script "workdir:$R/py;remotewait;command:Run the tests with coverage;testdump;coveragedump" "$R/py/tests/test_a.py" 2>&1 >/dev/null | join)
got="$got | $(reports)"
check "and coverage.py's" 'blit: Looking for the tests… ;; blit: Coverage: 80% of the lines in 1 files ;; tests/test_a.py: +test_one xtest_two | here: 1+ 5x ;; here: ran 1 2 5 6, missed 3 | files: tests/test_a.py=80% | 0'
# CTest's, in a build folder there (where cmake is, to configure it).
if command -v cmake >/dev/null 2>&1; then
    mkdir -p "$W/ct"
    printf 'cmake_minimum_required(VERSION 3.21)\nproject(ct NONE)\nenable_testing()\nadd_test(NAME passes COMMAND sh -c "exit 0")\nadd_test(NAME fails COMMAND sh -c "exit 1")\n' >"$W/ct/CMakeLists.txt"
    cmake -S "$W/ct" -B "$W/ct/build" >/dev/null 2>&1 || exit 1
    got=$(TMPDIR="$dir/ro" BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" \
        --script "remotewait;testdump;command:Run the tests;testdump" "$R/ct" 2>&1 >/dev/null | join)
    got="$got | $(reports)"
    check "CTest's tests there: found, run, and their report read back" 'build: .passes .fails ;; blit: 1 failed, 1 passed ;; build: +passes xfails | 0'
    # And with GCC's coverage (where there is a GCC): the build folder's
    # compiler read there first, gcov's report read back.
    gcc=
    for c in g++-16 g++-15 g++-14 g++-13 g++; do
        if command -v "$c" >/dev/null 2>&1 && "$c" --version 2>/dev/null | grep -q 'Free Software Foundation'; then gcc=$c; break; fi
    done
    if [ -n "$gcc" ]; then
        mkdir -p "$W/cov"
        printf 'cmake_minimum_required(VERSION 3.21)\nproject(cov CXX)\nenable_testing()\nadd_executable(run run.cpp)\nadd_test(NAME runs COMMAND run)\n' >"$W/cov/CMakeLists.txt"
        printf 'int never() {\n    return 3;\n}\n\nint main() {\n    int sum = 0;\n    for (int i = 0; i < 3; ++i) sum += i;\n    return sum == 3 ? 0 : 1;\n}\n' >"$W/cov/run.cpp"
        cmake -S "$W/cov" -B "$W/cov/build" -DCMAKE_CXX_COMPILER="$gcc" -DCMAKE_CXX_FLAGS=--coverage >/dev/null 2>&1 && cmake --build "$W/cov/build" >/dev/null 2>&1 || exit 1
        got=$(TMPDIR="$dir/ro" BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" \
            --script "workdir:$R/cov;remotewait;command:Run the tests with coverage;testdump;coveragedump" "$R/cov/run.cpp" 2>&1 >/dev/null | tail -1)
        got="$got | $(reports)"
        check "GCC's coverage there" 'here: ran 5 6 7 8, missed 1 2 | files: run.cpp=67% | 0'
        # Asked for again on a slow host while the compiler is still being
        # read there: the tests run once, and what ran is shown.
        SLOW=0.3 slow "workdir:$R/cov;remotewait;testdump;command:Run the tests with coverage;wait:100;command:Run the tests with coverage;testdump;coveragedump" "$R/cov/run.cpp"
        got="$got | $(reports)"
        check "and asked for twice on a slow host: run once" 'build: .runs ;; blit: Coverage: 67% of the lines in 1 files ;; build: +runs ;; here: ran 5 6 7 8, missed 1 2 | files: run.cpp=67% | 0'
    fi
fi
chmod 755 "$dir/ro"

# When blit goes, all its agent started there goes, and the agent too: a
# language server whose worker will not end on SIGTERM and holds the
# server's output open is ended with its process group, killed if it must.
if [ -x "$(dirname "$blit")/blit_fakelsp" ]; then
    mkdir -p "$dir/srv" && : >"$dir/srv/CMakeLists.txt" && printf 'int a;\n' >"$dir/srv/a.cpp"
    cat >"$dir/lingers.sh" <<EOF
#!/bin/sh
trap '' TERM
sleep 600 &
echo \$! >"$dir/lingers.pid"
exec "$(cd "$(dirname "$blit")" && pwd)/blit_fakelsp" "\$@"
EOF
    chmod +x "$dir/lingers.sh"
    printf 'blit.opt.git.enabled = false\nblit.opt.lsp.servers.fake = { command = { "%s" }, languages = { "cpp" }, roots = { "CMakeLists.txt" } }\n' "$dir/lingers.sh" >"$dir/lingers.lua"
    INIT="$dir/lingers.lua"
    run "remotewait;frame;ready" "ssh://box$dir/srv/a.cpp"
    worker=$(cat "$dir/lingers.pid" 2>/dev/null)
    alive() { if kill -0 "$worker" 2>/dev/null || pgrep -f "$T/.cache/blit/blit-agent" >/dev/null; then echo running; else echo ended; fi; }
    for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do [ "$(alive)" = ended ] && break; sleep 0.2; done
    got="${worker:+a worker} $(alive)"
    kill -9 "$worker" 2>/dev/null; pkill -9 -f "$T/.cache/blit/blit-agent"  # (where it failed: none left behind by the test)
    check "blit gone: the agent and a language server's stubborn worker end there" 'a worker ended'
    # A server that has exited while what it started still holds its
    # output: its pid is kept (not waited for) until that output closes, so
    # that what is sent to its process group cannot reach another program
    # given the pid meanwhile.
    printf '#!/bin/sh\necho $$ >"%s/leaves.pid"\nsleep 30 &\n' "$dir" >"$dir/leaves.sh" && chmod +x "$dir/leaves.sh"
    printf 'blit.opt.git.enabled = false\nblit.opt.lsp.servers.fake = { command = { "%s" }, languages = { "cpp" }, roots = { "CMakeLists.txt" } }\n' "$dir/leaves.sh" >"$dir/leaves.lua"
    INIT="$dir/leaves.lua"
    run "remotewait;frame;wait:1500;lua:print('held ' .. blit.system({ 'sh', '-c', 'ps -o stat= -p \$(cat $dir/leaves.pid)' }).stdout:sub(1, 1))" "ssh://box$dir/srv/a.cpp"
    leader=$(cat "$dir/leaves.pid" 2>/dev/null)
    for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do kill -0 "$leader" 2>/dev/null || break; sleep 0.2; done
    kill -0 "$leader" 2>/dev/null && got="$got, still there" || got="$got, then gone"
    check "a server that exited before its worker: its pid kept until the worker has gone" 'blit: held Z, then gone'
fi
# The machine there lost what blit ran (its agent killed: as a restart does):
# a terminal there says so once connected again, not that its program failed.
rm -f "$dir/down"
lost="lua:os.execute('pkill -9 -f $T/.cache/blit/[b]lit-agent')"
got=$(BLIT_TERM_SHELL="exec env PS1='\$ ' /bin/sh -i" BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" \
    --script "remotewait;key:ctrl+\`;frame;termwait:\$;$lost;termend;termdump" "$R/notes.txt" 2>&1 >/dev/null | sed -e 's/ @.*ended/ ended/' | join)
check "a terminal there whose machine lost it" 'Ctrl+` back to the file|$|[connection to box lost] ended -1'
got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" \
    --script "remotewait;$drop;key:ctrl+\`;frame;termend;termdump" "$R/notes.txt" 2>&1 >/dev/null | sed -e 's/ @.*ended/ ended/' | join)
check "and one opened just after: why, and no exit code" 'Ctrl+` back to the file|the connection to box was lost ended 1'
rm -f "$dir/down"
# A terminal on another machine than the folder's: the status bar names the terminal's.
got=$(BLIT_TERM_SHELL="exec env PS1='\$ ' /bin/sh -i" BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" \
    --script "remotewait;frame;statusdump;term:ssh://other$W;remotewait;termwait:\$;frame;statusdump" "$R" 2>&1 >/dev/null | join)
check "a terminal on another machine: the bar names its machine" 'box untitled | Ln 1, Col 1 Tabs LF Plain text ;; other Terminal: other |'
# A terminal on this machine from a folder there: a shell here, in the
# home folder (or the folder here opened last), and the bar names no
# machine for it (nor the folder's, when that could not be reached).
mkdir -p "$dir/lastlocal"
here_term() {  # here_term RECENT [ENV]: the command run from the folder there, with that list of folders opened lately
    printf "$1" >"$dir/recent-term"
    rm -f "$dir/pwd.txt"
    got=$(env ${2:-} HOME="$dir/home" BLIT_RECENT_FILE="$dir/recent-term" BLIT_TERM_SHELL="exec env PS1='\$ ' /bin/sh -i" BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" \
        --script "remotewait;frame;command:New terminal on this machine;termwait:\$;text:pwd >$dir/pwd.txt && echo w''rote;key:Return;termwait:wrote;frame;statusdump;dump" "$R" 2>&1 >/dev/null | join)
    got="$got | $(sed -e "s#$dir#@#g" "$dir/pwd.txt" 2>/dev/null)"
}
here_term ''
check "a terminal on this machine from a folder there: in the home folder" 'Terminal | ;; V([untitled*] / >[Terminal*])0.70 | @/home'
here_term "d\tssh://box$W/src\nd\t$dir/gone\nd\t$dir/lastlocal\nd\t$dir/home\n"
check "or in the folder here opened last" 'Terminal | ;; V([untitled*] / >[Terminal*])0.70 | @/lastlocal'
echo refused >"$dir/down"
here_term '' "BLIT_FAKE_DOWN=$dir/down"
got=$(printf '%s' "$got" | sed -e 's/^blit: cannot connect to box: ssh: connect to host: refused ;; //')
check "and with the folder's machine not reached: no machine named" 'Terminal | ;; V([untitled*] / >[Terminal*])0.70 | @/home'
rm -f "$dir/down"

# A build there: its output's tab names the machine, as a terminal's does.
mkdir -p "$T/.mk" && printf 'all:\n\t@echo built\n' >"$T/.mk/Makefile"
run "remotewait;frame;buildpanel;frame;buildrun;buildwait;frame;dump" "ssh://box$T/.mk"
check "a build there: its tab names the machine" 'V(>[untitled*] / [make (box)*])0.70'
run "remotewait;frame;buildpanel;frame;$drop;buildrun;buildwait;frame;key:cmd+alt+Down;termend;termdump" "ssh://box$T/.mk"
check "one started just after the connection is lost: why, and no exit code" 'the connection to box was lost @1,0 4x64 ended 1'

# Local history of a file there: kept here, at each save (and, at the
# first, the text as it was read from there), from the text here; listed,
# tried out and put back as a file here's, the put back saved there as any
# edit is. The same path on another host, or on this machine, has a history
# of its own (each in a folder named by a hash of its whole path).
export BLIT_HISTORY_DIR="$dir/data"
mkdir -p "$T/.hist" && printf 'one\ntwo\n' >"$T/.hist/h.txt"
H="ssh://box$T/.hist/h.txt"
run "remotewait;frame;text:X;save;remotewait;text:Y;save;remotewait;frame;historydump" "$H"
got="$got | $(tr '\n' ' ' <"$T/.hist/h.txt")"
check "a save there: kept in its local history (the text as read, and each save)" 'sss | XYone two '
run "remotewait;frame;command:Show the file's local history;frame;key:Down;frame;lsptext;key:Down;frame;key:Return;frame;lsptext;dump:tabs;save;remotewait;frame;statusdump" "$H"
got="$got | $(tr '\n' ' ' <"$T/.hist/h.txt")"
check "tried out, put back, and saved there" '@@ -1 +1 @@ / -Xone / +XYone /  two ;; one / two /  ;; tabs: >[h.txt*] ;; box ~/.hist/h.txt | Ln 1, Col 1 Tabs LF Plain text | one two '
run "remotewait;frame;historydump" "ssh://other$T/.hist/h.txt"
run2=$got
run "frame;historydump" "$T/.hist/h.txt"
got="$run2 | $got | $(cat "$dir"/data/history/*/path | sed -e "s#$T#~#g" | sort | tr '\n' ' ') | $(ls "$dir/data/history" | grep -cv '^[0-9a-f]\{16\}$')"
check "another host's same path, and this machine's: histories of their own" 'none | none | ssh://box~/.hist/h.txt  | 0'
run "remotewait;frame;text:O;save;remotewait;frame;historydump" "ssh://other$T/.hist/h.txt"
got="$got | $(cat "$dir"/data/history/*/path | sed -e "s#$T#~#g" | sort | tr '\n' ' ')"
check "saved from the other host: kept apart" 'ss | ssh://box~/.hist/h.txt ssh://other~/.hist/h.txt '
printf 'zed\n' >"$T/.hist/z.txt"
slow "remotewait;frame;text:Z;clock;save;within:300;remotewait;frame;historydump" "ssh://box$T/.hist/z.txt"
check "on a slow host, the first save (the text as read kept with it) asks nothing of the host" 'within 300 ms ;; ss'
unset BLIT_HISTORY_DIR
# A task that ends there: nothing watches the disk there, so what it wrote is
# looked for then (a moment later, once for all that end meanwhile): the tree
# lists the file it made, the open file it wrote is read again (or, changed
# here, kept with a word about it), and the Git tab counts the changes.
mkdir -p "$dir/ran" && printf 'one\n' >"$dir/ran/notes.txt" && printf 'all:\n\t@echo made >made.txt\n\t@echo two >>notes.txt\n' >"$dir/ran/Makefile"
(cd "$dir/ran" && HOME="$T" git init -q -b main && HOME="$T" git add -A && HOME="$T" git -c user.name=t -c user.email=t@example.invalid commit -qm first) || exit 1
INIT="$dir/git.lua"
run "remotewait;panel;frame;gitwait;open:ssh://box$dir/ran/notes.txt;remotewait;frame;treewait;treedump;gitdump;buildrun;buildwait;ranwait;remotewait;treewait;gitwait;frame;treedump;lsptext;gitdump" "ssh://box$dir/ran"
check "a task there that ends: the tree, the open file and git there looked at again" 'tree: Makefile@42 notes.txt@57 ;; main | branches *main | commits 1 "first" | lines ;; tree: made.txt@42 Makefile@57 notes.txt@73 ;; one / two /  ;; main | changes M:notes.txt ?:made.txt | branches *main | commits 1 "first" | lines 2+'
(cd "$dir/ran" && rm made.txt && git checkout -q notes.txt) || exit 1
INIT="$dir/git.lua"
run "remotewait;panel;frame;gitwait;open:ssh://box$dir/ran/notes.txt;remotewait;frame;text:X;buildrun;buildwait;ranwait;remotewait;frame;lsptext;statusdump" "ssh://box$dir/ran"
check "and the open file changed here: kept, and said" 'blit: notes.txt changed on disk: its changes here are kept, and saving asks first (Compare with disk shows the two) ;; Xone /  ;; box main notes.txt ● | Ln 1, Col 2 Tabs LF Plain text'
run "remotewait;panel;frame;buildrun;buildwait;buildrun;buildwait;ranwait;ranwait" "ssh://box$dir/ran"
check "two that end together: looked at once" 'ranwait: nothing ended there'
mkdir -p "$dir/ranhere" && cp "$dir/ran/Makefile" "$dir/ranhere/" && printf 'one\n' >"$dir/ranhere/notes.txt"
run "panel;frame;buildrun;buildwait;ranwait" "$dir/ranhere"
check "one here: left to the watcher" 'ranwait: nothing ended there'

# A Python file there: its project's Python found there (through the agent,
# on a worker: nothing here waits for it, on a slow host either), named in
# the status bar, and the Python console started there with it. (The
# Python, a stand-in answering what blit asks a Python, and saying what it
# is asked to run.)
mkdir -p "$W/pyr/.venv/bin"
printf '#!/bin/sh\nif [ "$1" = -c ]; then printf "3.99.1\\n%%s\\n0\\n" "$0"; exit 0; fi\necho "ran $0" >>"%s"\nexec python3 "$@"\n' "$dir/ran-there" >"$W/pyr/.venv/bin/python"
chmod +x "$W/pyr/.venv/bin/python"
printf 'x = 1\n' >"$W/pyr/a.py"
slow "remotewait;workdir:$R/pyr;clock;frame;key:Down;frame;within:300;pywait;frame;statusdump;pydump;command:Open the Python console;termwait:>>>" "$R/pyr/a.py"
got="$(printf '%s' "$got" | sed -e 's/ (no colours)//') ;; $(sed -e "s#$T#~#g" "$dir/ran-there")"
check "a Python file there: its project's Python there, found without waiting here; the console there with it" \
    'within 300 ms ;; box a.py | Python 3.99 (.venv) Ln 2, Col 1 Spaces: 4 LF Python ;; ssh://box~/work/pyr = ssh://box~/work/pyr (.venv) ~/work/pyr/.venv/bin/python 3.99.1 ;; ran ~/work/pyr/.venv/bin/python'
# The Pythons there to choose from (of them, the project's venv; the
# others, the PATH's and uv's there, are this machine's).
printf 'home = /usr/bin\n' >"$W/pyr/.venv/pyvenv.cfg"
run "remotewait;workdir:$R/pyr;frame;pywait;frame;statusclick:Python 3.99;pywait;finddump" "$R/pyr/a.py"
got=$(printf '%s' "$got" | sed -e 's/^blit: [^;]*;; //' -e 's/^pick [^:]* : //' | awk '{ n = split($0, w, " Python "); out = ""; for (i = 1; i <= n; i++) if (w[i] ~ /work\/pyr|Automatic/) out = out (i > 1 ? " Python " : "") w[i]; print out }')
check "and the Pythons there to choose from" '>Automatic Python 3.99.1 · .venv · ~/work/pyr/.venv/bin/python'
# A language server there for Python files told the project's Python there,
# once it is up (tests/fake_lsp.cpp asking for it as pyright does).
if [ -x "$(dirname "$blit")/blit_fakelsp" ]; then
    printf '[project]\nname = "pyr"\n' >"$W/pyr/pyproject.toml"
    printf 'blit.opt.git.enabled = false\nblit.opt.lsp.servers.fakepy = { command = { "%s", "--section", "python.pythonPath", "--configured", "%s" }, languages = { "python" }, roots = { "pyproject.toml" } }\n' \
        "$(cd "$(dirname "$blit")" && pwd)/blit_fakelsp" "$dir/configured" >"$dir/pylsp.lua"
    INIT="$dir/pylsp.lua"
    run "remotewait;frame;ready;pywait;wait:500" "$R/pyr/a.py"
    # (What it was told last: asked before the venv was found it is told
    # none first, else the venv's Python at once; which comes first is the
    # machine's speed.)
    got=$(sed -e "s#$T#~#g" "$dir/configured" | tail -1)
    check "a server there for Python files: told the project's Python there" 'python.pythonPath = "~/work/pyr/.venv/bin/python"'
fi

# The agent was copied there once, into the cache blit keeps in that home.
got=$(ls "$T/.cache/blit" | sed -e 's/-[0-9a-f]*$//')
check "the agent, copied once" "blit-agent-$(uname -s)-$(uname -m)"

# No ssh on this machine (nor docker, for a container): which, said, and
# on the status bar in place of "not connected".
case $(uname -s) in Darwin) this="this Mac" ;; *) this="this machine" ;; esac
bare="$dir/bare"
mkdir -p "$bare"
ln -s /usr/bin/* /bin/* "$bare/" 2>/dev/null
rm -f "$bare/ssh" "$bare/docker"
got=$(env -u BLIT_SSH -u BLIT_DOCKER PATH="$bare" BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "remotewait;frame;statusdump" "$R/notes.txt" 2>&1 >/dev/null | tail -2 | join)
check "no ssh here: said, and on the status bar" "blit: ssh is not installed on $this ;; box: ssh is not installed on $this untitled | Ln 1, Col 1 Tabs LF Plain text"
got=$(env -u BLIT_SSH -u BLIT_DOCKER PATH="$bare" BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "remotewait;frame;statusdump" "docker://box$W/notes.txt" 2>&1 >/dev/null | tail -2 | join)
check "no docker here, for a container: docker named (not ssh)" "blit: docker is not installed on $this ;; docker:box: docker is not installed on $this untitled | Ln 1, Col 1 Tabs LF Plain text"

[ -n "$print" ] || [ $failed = 1 ] || echo "remote ok"
exit $failed
