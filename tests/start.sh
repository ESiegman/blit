#!/bin/sh
# The start screen, through the real key and text events: when it shows,
# what its keys do, and the list of files and folders opened lately (kept
# in a scratch file named by BLIT_RECENT_FILE, never the real one).
#
#   tests/start.sh path/to/blit            run the cases
#   tests/start.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/start.sh path/to/blit [--print]}
print=${2:-}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
W="$dir/work"
mkdir -p "$W/src"
printf 'int main() {}\n' >"$W/src/main.cpp"
printf 'notes\n' >"$W/notes.txt"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n' >"$dir/modal.lua"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.start_screen = false\n' >"$dir/off.lua"
export BLIT_RECENT_FILE="$dir/recent"

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run SCRIPT [FILE]: the last line the run printed. INIT, if set, is the
# init.lua for this run only. (Not "VAR=x run": sh keeps such an
# assignment after a function.)
INIT=
run() {
    got=$(BLIT_INIT="${INIT:-$dir/init.lua}" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$W;frame;$1" ${2:+"$2"} 2>&1 >/dev/null | sed -e "s#$dir#~#g" | tail -1)
    INIT=
}
actions='n:New file p:New project… o:Open a file… f:Go to file s:Search in files d:Open a folder… r:Connect to a server… c:Settings q:Quit'

rm -f "$dir/recent"
run "startdump"
check "started with no file: the start screen, New file chosen" "start >n:New file ${actions#n:New file }"
run "startdump" "$W/notes.txt"
check "started with a file: none" 'no start screen'
INIT="$dir/off.lua"
run "startdump"
check "editor.start_screen = false: none" 'no start screen'

# Its keys.
rm -f "$dir/recent"
run "text:n;text:hi;frame;statusdump"
check "n: an empty file, and what is typed goes into it" 'untitled ● | Ln 1, Col 3 Tabs LF Plain text'
run "key:Escape;frame;startdump"
check "Esc: gone" 'no start screen'
run "text:j;text:j;text:j;key:Down;key:Up;frame;startdump"
check "j and k, Down and Up choose" "start n:New file p:New project… o:Open a file… >f:Go to file s:Search in files d:Open a folder… r:Connect to a server… c:Settings q:Quit"
# The application's own chords reach it past the page: Command search
# (Ctrl+Shift+P where Ctrl is the shortcut key, though the page's Ctrl+P
# moves up its list) and Flash the board (Alt+Shift+B there).
rm -f "$dir/recent"
run "keyof:Command search;finddump"
case "$got" in 'commands [] '*) got='Command search' ;; esac
check "Command search's chord: its box, not a move on the list" 'Command search'
rm -f "$dir/recent"
run "keyof:Flash the board;wait:300"
check "Flash the board's chord: the command's" 'blit: No run configuration has a board'
run "text:j;text:j;text:j;key:Return;findwait;finddump"
check "Enter does what is chosen: the box that finds files" 'names [] 2 of 2 : >notes.txt src/main.cpp'
run "text:o;finddump"
check "o: a file opened by its path" "open [~/work/] 3 : >../ src/ notes.txt"
run "text:o;key:Escape;frame;startdump"
check "Esc shuts the box, and the start screen is still there" "start n:New file p:New project… >${actions#n:New file p:New project… }"
run "text:o;text:notes.txt;key:Return;frame;statusdump"
check "a file opened from the box: the start screen is gone" 'notes.txt | Ln 1, Col 1 Tabs LF Plain text'
cp "$dir/init.lua" "$dir/settings.lua"
INIT="$dir/settings.lua"
run "text:c;frame;dump"
check "c: the settings screen, in place of the empty tab" '>[Settings*]'
INIT="$dir/modal.lua"
run "text:n;text:ihello;key:Escape;frame;vimdump"
check "with Vim: n, then Vim's keys" 'NORMAL |  | 1:5 | hello'

# Opened lately: what is kept, what is shown, what the keys open.
rm -f "$dir/recent"
run "frame" "$W/notes.txt"
run "text:o;text:src/main.cpp;key:Return;frame"
got=$(sed -e "s#$dir#~#g" "$dir/recent" | tr '\n' ' ')
check "files opened are kept, the latest first" 'f	~/work/src/main.cpp f	~/work/notes.txt '
printf 'd\t%s\nf\t%s/gone.txt\n' "$W/src" "$W" >>"$dir/recent"
run "startdump"
check "shown with numbers; one that is gone is left out" "start >n:New file ${actions#n:New file } 1:~/work/src/main.cpp 2:~/work/notes.txt 3:~/work/src/"
run "text:2;frame;statusdump"
check "a number opens it" 'notes.txt | Ln 1, Col 1 Tabs LF Plain text'
run "text:3;frame;startdump"
check "a folder: it is the first of those opened lately now, and the start screen stays" "start n:New file ${actions#n:New file } >1:~/work/src/ 2:~/work/notes.txt 3:~/work/src/main.cpp"
run "text:1;frame;lua:print(blit.workdir())"
check "and it is the working directory" 'blit: ~/work/src'

# A box opened over it (Go to file) ends on the edge of one of its rows,
# so that no row shows half of itself under the box, at any size.
for size in 1000x700 1400x900 1600x1000; do
    got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen $size --shot "$dir/shot.bmp" --script "workdir:$W;frame;command:Go to file;findedge" 2>&1 >/dev/null | tail -1)
    check "a box over it ends on a row's edge ($size)" 'edge 0'
done

[ -n "$print" ] || [ $failed = 1 ] || echo "start ok"
exit $failed
