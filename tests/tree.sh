#!/bin/sh
# The file tree and the disk: what another program changes there shown
# without asking, rows chosen with Cmd+click and Shift+click, several
# trashed at once, and rows dragged into a folder, through scripted mouse
# and key events. Each case starts from the same folder, made again.
#
#   tests/tree.sh path/to/blit            run the cases
#   tests/tree.sh path/to/blit --print    show what each case gives
#
# The tree is printed as its rows, each with where its middle is ("*"
# chosen, ">" selected). The mouse positions are for a 16 x 31 pixel cell
# (the default font at 13 pt on macOS); with another the test is skipped.
set -u
blit=${1:?usage: tests/tree.sh path/to/blit [--print]}
print=${2:-}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
printf 'blit.opt.lsp.enabled = false\nblit.opt.editor.modal = false\nblit.opt.git.enabled = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua"
export BLIT_TRASH_DIR="$dir/bin"
T="$dir/work"
fresh() {
    rm -rf "$T" "$BLIT_TRASH_DIR"
    mkdir -p "$T/sub" "$BLIT_TRASH_DIR"
    for f in x y z; do echo "$f" >"$T/$f.txt"; done
    echo q >"$T/sub/q.txt"
}
fresh
failed=0
# check NAME SCRIPT EXPECTED [DISK]: the last line the script's "treedump"
# prints; then, if given, the files on disk afterwards (find, sorted).
check() {
    fresh
    got=$("$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "key:cmd+B;panel;$2;treedump" "$T/x.txt" 2>&1 >/dev/null | grep '^tree:\|watchwait' | tail -1)
    disk=$(cd "$T" && find . -type f | sort | tr '\n' ' ')
    if [ -n "$print" ]; then
        printf '%s\n    %s\n    %s\n' "$1" "$got" "$disk"
        return
    fi
    if [ "$got" != "$3" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$3" "$got"
        failed=1
    fi
    if [ -n "${4:-}" ] && [ "$disk" != "$4" ]; then
        printf 'FAIL: %s (on disk)\n    expected %s\n    got      %s\n' "$1" "$4" "$disk"
        failed=1
    fi
}
# names NAME SCRIPT EXPECTED: as check, the rows' names only (with any
# font: these click nothing).
names() {
    fresh
    got=$("$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "key:cmd+B;panel;$2;treedump" "$T/x.txt" 2>&1 >/dev/null | grep '^tree:\|watchwait' | tail -1 | sed 's/@[0-9]*//g')
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$3" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$3" "$got"
        failed=1
    fi
}
names "a file another program makes: shown" "lua:os.execute('echo n > $T/new.txt');watchwait" \
    "tree: sub/ new.txt x.txt y.txt z.txt"
names "a file another program removes: gone" "lua:os.execute('rm $T/y.txt');watchwait" \
    "tree: sub/ x.txt z.txt"
# (Where the folder's row is, with this font, to open it with a click.)
fresh
suby=$("$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "key:cmd+B;panel;treedump" "$T/x.txt" 2>&1 >/dev/null | grep '^tree:' | sed 's/.* sub\/@\([0-9]*\).*/\1/')
names "a file made in an open folder: shown in it" "click:60,$suby;release;panel;lua:os.execute('echo r > $T/sub/r.txt');watchwait" \
    "tree: >sub/ sub/q.txt sub/r.txt x.txt y.txt z.txt"
got=$(fresh; "$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "key:cmd+B;panel;lua:os.execute('echo changed > $T/x.txt');watchwait;docdump:0" "$T/x.txt" 2>&1 >/dev/null | tail -1)
if [ -z "$print" ] && [ "$got" != "> |changed" ]; then
    printf 'FAIL: the open file another program writes: read again\n    got %s\n' "$got"
    failed=1
fi
if ! "$blit" --offscreen 1600x900 --info "$T/x.txt" 2>/dev/null | grep -q "cell 16 x 31 px"; then
    echo "skipped: the cell size here is not the one the mouse positions were written for"
    exit 77
fi
check "as it starts" "frame" \
    "tree: sub/@42 x.txt@57 y.txt@73 z.txt@88"
check "Cmd+click: one more chosen" "click:60,57;release;click:60,88,1,cmd;release" \
    "tree: sub/@42 *x.txt@57 y.txt@73 >*z.txt@88"
check "Cmd+click on one chosen: not any more" "click:60,57;release;click:60,88,1,cmd;release;click:60,57,1,cmd;release" \
    "tree: sub/@42 x.txt@57 y.txt@73 >z.txt@88"
check "Shift+click: the rows from the selected one" "click:60,57;release;click:60,88,1,shift;release" \
    "tree: sub/@42 *x.txt@57 *y.txt@73 >*z.txt@88"
check "a plain click: one again" "click:60,57;release;click:60,88,1,shift;release;click:60,42;release" \
    "tree: >sub/@42 x.txt@57 y.txt@73 z.txt@88"
check "two chosen, trashed together" "click:60,57;release;click:60,88,1,cmd;release;key:cmd+Backspace;panel" \
    "tree: sub/@42 y.txt@57" "./sub/q.txt ./y.txt "
check "a file dragged onto a folder: moved into it" "click:60,73;drag:70,60;drag:60,42;release;panel" \
    "tree: sub/@42 x.txt@57 z.txt@73" "./sub/q.txt ./sub/y.txt ./x.txt ./z.txt "
check "the chosen ones dragged onto a folder: all moved" "click:60,57;release;click:60,88,1,cmd;release;click:60,88;drag:70,70;drag:60,42;release;panel" \
    "tree: sub/@42 y.txt@57" "./sub/q.txt ./sub/x.txt ./sub/z.txt ./y.txt "
check "a file from an open folder dropped below the rows: moved to the top" "click:60,42;release;panel;click:60,57;drag:70,100;drag:60,300;release;panel" \
    "tree: sub/@42 >q.txt@57 x.txt@73 y.txt@88 z.txt@104" "./q.txt ./x.txt ./y.txt ./z.txt "
check "a file dropped in its own folder: nothing" "click:60,57;drag:70,70;drag:60,88;release;panel" \
    "tree: sub/@42 >x.txt@57 y.txt@73 z.txt@88" "./sub/q.txt ./x.txt ./y.txt ./z.txt "
exit $failed
