#!/bin/sh
# Sessions: a project folder opened again brings back what was open in it
# (the tabs and their order, the splits and their sizes, each file's cursor
# and view, the side panel's tab and width, the Build tab's task), through
# the real key and text events, the start screen, `blit DIR` and the box
# that opens folders. Kept in a scratch folder beside the file
# BLIT_RECENT_FILE names, never the real one; files on another machine are
# read through tests/fake-ssh.sh, as tests/remote.sh does.
#
#   tests/session.sh path/to/blit            run the cases
#   tests/session.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/session.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
W="$dir/work"
V="$dir/other"
T="$dir/there"  # another machine's home (tests/fake-ssh.sh)
mkdir -p "$W/src" "$V" "$T/far" "$dir/home/.ssh"
seq 1 300 | sed 's/^/line /' >"$W/long.txt"
printf 'notes\nsecond line\nthird\n' >"$W/notes.txt"
printf 'int main() {}\n' >"$W/src/main.cpp"
printf 'all:\n\techo all\ntest:\n\techo test\n' >"$W/Makefile"
printf 'other\n' >"$V/x.txt"
printf 'one\ntwo\nthree\n' >"$T/far/a.txt"
printf 'Host box\n    HostName example.invalid\n' >"$dir/home/.ssh/config"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\nblit.opt.editor.restore = false\n' >"$dir/off.lua"
export BLIT_RECENT_FILE="$dir/recent" BLIT_SSH="$here/fake-ssh.sh" BLIT_FAKE_HOME="$T"
export BLIT_TERM_SHELL="exec env PS1='\$ ' /bin/sh -i"  # (a shell that is the same everywhere, as tests/term.sh has)
S="$dir/sessions"
R="ssh://box$T/far"

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
    got=$(BLIT_INIT="${INIT:-$dir/init.lua}" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "$1" ${2:+"$2"} 2>&1 >/dev/null |
        sed -e "s#$dir#~#g" | join)
    INIT=
}
# kept FOLDER: its session as written, its lines joined.
kept() {
    f=$(grep -lx "folder	$1" "$S"/* 2>/dev/null | head -1)
    got=$([ -n "$f" ] && sed -e "s#$dir#~#g" "$f" | join)
}

# What is kept, and when.
run "open:$W/notes.txt;quit"
got=$(ls "$S" 2>/dev/null | wc -l | tr -d ' ')
check "a file opened, no folder opened to work in: nothing kept" '0'
run "folder:$W;open:$W/notes.txt;key:Down;key:End;open:$W/long.txt;key:cmd+Down;key:cmd+\\;open:$W/src/main.cpp;key:Right;key:Right;frame"
got=$(ls "$S" 2>/dev/null | wc -l | tr -d ' ')
check "not before it quits" '0'
run "folder:$W;open:$W/notes.txt;key:Down;key:End;open:$W/long.txt;key:cmd+Down;key:cmd+\\;open:$W/src/main.cpp;key:Right;key:Right;frame;buildrun:make test;buildwait;quit"
kept "$W"
check "on quit: the splits, the tabs in order, the cursors and views, the panel, the Build tab's task" \
    'blit session 1 ;; folder	~/work ;; tabs	3 ;; panel	0	Files	32 ;; build	Make	make test ;; H	0.5000 ;; pane	1	0 ;; file	1	11	0.00	0.00	~/work/notes.txt ;; file	300	0	281.10	0.00	~/work/long.txt ;; pane	0	1 ;; file	0	2	0.00	0.00	~/work/src/main.cpp'

# Brought back.
run "frame;dump;statusdump;builddump" "$W"
check "blit DIR: as it was left" \
    'H([notes.txt long.txt*]@282 | >[main.cpp*])0.50 ;; src/main.cpp | Ln 1, Col 3 Tabs LF C++ ;; Make: make make all >make test'
run "frame;key:cmd+alt+Left;key:cmd+1;frame;statusdump" "$W"
check "each file's cursor" 'notes.txt | Ln 2, Col 12 Tabs LF Plain text'
run "frame;startdump"
check "Recent says what will come back" \
    'start >n:New file p:New project… o:Open a file… f:Go to file s:Search in files d:Open a folder… r:Connect to a server… c:Settings (init.lua) q:Quit 1:~/work/(3) 2:~/work/src/main.cpp 3:~/work/long.txt 4:~/work/notes.txt'
run "frame;text:1;frame;dump;startdump"
check "and the start screen's Recent brings it back" 'H([notes.txt long.txt*]@282 | >[main.cpp*])0.50 ;; no start screen'
run "workdir:$dir;frame;text:d;text:work;key:Return;key:Return;frame;dump"
check "so does the box that opens folders (d)" 'H([notes.txt long.txt*]@282 | >[main.cpp*])0.50'
INIT="$dir/off.lua"
run "frame;dump" "$W"
check "editor.restore = false: nothing comes back" '>[untitled*]'
INIT="$dir/off.lua"
run "frame;startdump"
check "and Recent says nothing of it" \
    'start >n:New file p:New project… o:Open a file… f:Go to file s:Search in files d:Open a folder… r:Connect to a server… c:Settings (init.lua) q:Quit 1:~/work/ 2:~/work/src/main.cpp 3:~/work/long.txt 4:~/work/notes.txt'

# The panel's tab and width, as kept.
f=$(grep -lx "folder	$W" "$S"/*)
sed -e 's/^panel	.*/panel	1	Build	40/' "$f" >"$f.new" && mv "$f.new" "$f"
run "frame;sessiondump" "$W"
got=$(printf '%s' "$got" | sed -e 's/ ;; build.*//')
check "the side panel: shown, its tab, its width" 'blit session 1 ;; folder	~/work ;; tabs	3 ;; panel	1	Build	40'

# What is gone stays gone.
mv "$W/notes.txt" "$dir/notes.txt"
run "frame;dump" "$W"
check "a file that is gone is left out" 'H([long.txt*]@282 | >[main.cpp*])0.50'
mv "$W/src/main.cpp" "$dir/main.cpp"
run "frame;dump" "$W"
check "a split left with nothing goes" '>[long.txt*]@282'
mv "$dir/notes.txt" "$W/notes.txt"
mv "$dir/main.cpp" "$W/src/main.cpp"

# Another folder opened: the last one's session is kept then, and the window is this one's.
run "folder:$V;open:$V/x.txt;quit"
run "folder:$W;frame;key:cmd+alt+Left;key:Up;folder:$V;frame;dump"
check "another folder: its tabs in place of the last one's" '>[x.txt*]'
kept "$W"
got=$(printf '%s' "$got" | sed -e 's/.*\(file	[^;]*long.txt\).*/\1/')
check "and the last one's kept as it was then" 'file	299	0	281.10	0.00	~/work/long.txt'
run "folder:$W;frame;key:cmd+alt+Left;text:zz;folder:$V;frame;dump"
check "a file with changes not saved stays (in the split with the keyboard)" '>[x.txt* long.txt]'
run "folder:$V;key:ctrl+\`;frame;termwait:\$;key:space;text:echo kep''t;key:Return;termwait:kept;folder:$W;frame;dump;key:ctrl+\`;frame;termwait:kept;dump"
check "the terminals' split along the bottom stays as it was, its shell still running" \
    'H(V(>[notes.txt long.txt*]@282 / [Terminal*])0.70 | [main.cpp*])0.50 ;; H(V([notes.txt long.txt*]@282 / >[Terminal*])0.70 | [main.cpp*])0.50'

# Another machine's folder: its files come back when the connection is up.
run "folder:$R;open:$R/a.txt;remotewait;key:Down;key:Right;quit"
run "remotewait;frame;dump;statusdump" "$R"
check "another machine's: back when it is here, the cursor where it was" '>[a.txt*] ;; box a.txt | Ln 2, Col 2 Tabs LF Plain text'
rm "$T/far/a.txt"
run "remotewait;frame;dump" "$R"
check "and one gone there goes quietly" '>[untitled*]'

[ -n "$print" ] || [ $failed = 1 ] || echo "session ok"
exit $failed
