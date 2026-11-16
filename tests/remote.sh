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
export BLIT_SSH="$here/fake-ssh.sh" BLIT_FAKE_HOME="$T" BLIT_RECENT_FILE="$dir/recent"
R="ssh://box$W"

failed=0
check() {
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
run "remotewait;text:X;save;frame;statusdump" "$R/notes.txt"
got="$got | $(tr '\n' ' ' <"$W/notes.txt")"
check "saved there" 'box ~/work/notes.txt | Ln 1, Col 2 Tabs LF Plain text | Xhello world '
run "remotewait;text:new;save;frame;statusdump" "$R/fresh.txt"
got="$got | $(cat "$W/fresh.txt")"
check "one that is not there: made when saved" 'box ~/work/fresh.txt | Ln 1, Col 4 Tabs LF Plain text | new'
run "remotewait;frame;lua:print(blit.workdir())" "$R"
check "a folder: the working directory" "blit: ssh://box~/work"
got=$(BLIT_FAKE_FAIL=refused BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "remotewait;frame;statusdump;remotedump" "$R/notes.txt" 2>&1 >/dev/null | join)
check "a host that cannot be reached: why, and the bar says so" 'blit: cannot connect to box: ssh: connect to host: refused ;; untitled | Ln 1, Col 1 Tabs LF Plain text ;; box: failed: cannot connect to box: ssh: connect to host: refused; '

# No key for the host: ssh's questions come to blit's box, hidden as they
# are typed. (The password is the test's own, made up here.)
pw=not-a-real-password-42
got=$(BLIT_FAKE_PASSWORD=$pw BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "remotewait;finddump;text:$pw;finddump;key:Return;remotewait;frame;statusdump" "$R/notes.txt" 2>&1 >/dev/null | sed -e "s#$T#~#g" | join)
check "a password asked for, and answered" "ask box \"tester@box's password:\" [] ;; ask box \"tester@box's password:\" [**********************] ;; box ~/work/notes.txt | Ln 1, Col 1 Tabs LF Plain text"
got=$(BLIT_FAKE_PASSWORD=$pw BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "remotewait;text:wrong;key:Return;remotewait;frame;finddump" "$R/notes.txt" 2>&1 >/dev/null | join)
check "a wrong one: ssh says so" 'blit: cannot connect to box: tester@box: Permission denied, please try again. ;; closed'
got=$(BLIT_FAKE_PASSWORD=$pw BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "remotewait;key:Escape;remotewait;frame;finddump" "$R/notes.txt" 2>&1 >/dev/null | join)
check "Esc: not connecting" 'blit: cannot connect to box: tester@box: Permission denied (publickey,password). ;; closed'
got=$(BLIT_FAKE_PASSWORD=$pw BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "find:folder;text:ssh://box$W/;remotewait;finddump;text:$pw;key:Return;remotewait;finddump" 2>&1 >/dev/null | sed -e "s#$T#~#g" -e 's#\[[^]]*ssh://#[ssh://#g' | join)
check "asked while the box that opens folders is open: that box comes back" "ask box \"tester@box's password:\" [] ;; folder [ssh://box~/work/] 3 : >./ ../ src/"

# Folders, the finder and the box that opens files.
run "remotewait;lua:local t = blit.fs.list('$R') table.sort(t, function(a, b) return a.name < b.name end) for _, e in ipairs(t) do print(e.name, e.type) end;lua:print(blit.fs.exists('$R/src'), blit.fs.exists('$R/gone'))" "$R"
check "blit.fs, there" 'blit: fresh.txt	file ;; blit: notes.txt	file ;; blit: src	dir ;; blit: dir	nil'
run "remotewait;find:names;findwait;finddump" "$R"
check "files found by name there" 'names [] 3 of 3 : >fresh.txt notes.txt src/main.cpp'
run "remotewait;find:text;text:main;findwait;finddump;key:Return;remotewait;frame;statusdump" "$R"
check "text searched for there, and a line found opened" 'text [main] 1 of 1 : >src/main.cpp:1:5 ;; box src/main.cpp | Ln 1, Col 5 Tabs LF C++'
run "remotewait;find:open;finddump;text:src/;finddump" "$R"
check "the box that opens files, in a folder there" 'open [ssh://box~/work/] 4 : >../ src/ fresh.txt notes.txt ;; open [ssh://box~/work/src/] 2 : >../ main.cpp'
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

# A language server, run there (the test one, next to blit).
if [ -x "$(dirname "$blit")/blit_fakelsp" ]; then
    printf 'blit.opt.git.enabled = false\nblit.opt.lsp.servers.fake = { command = { "%s", "--utf8" }, languages = { "cpp" }, roots = { "CMakeLists.txt" } }\n' "$(cd "$(dirname "$blit")" && pwd)/blit_fakelsp" >"$dir/lsp.lua"
    mkdir -p "$dir/proj/src" && : >"$dir/proj/CMakeLists.txt"
    printf 'int a;\nint b; // an ERROR here\nint c;\nint d; // and a WARN\n' >"$dir/proj/src/e.cpp"
    INIT="$dir/lsp.lua"
    run "remotewait;frame;ready;diag;frame;text:];text:d;vimdump" "ssh://box$dir/proj/src/e.cpp"
    check "its diagnostics, in the file here" 'NORMAL |  | 2:14 | int b; // an ERROR here'
fi

# The agent was copied there once, into the cache blit keeps in that home.
got=$(ls "$T/.cache/blit" | sed -e 's/-[0-9a-f]*$//')
check "the agent, copied once" "blit-agent-$(uname -s)-$(uname -m)"

[ -n "$print" ] || [ $failed = 1 ] || echo "remote ok"
exit $failed
