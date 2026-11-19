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
names "an open folder removed and made again: watched again" \
    "click:60,$suby;release;panel;lua:os.execute('rm -rf $T/sub && mkdir $T/sub');watchwait;lua:os.execute('echo r > $T/sub/r.txt');watchwait" \
    "tree: >sub/ sub/r.txt x.txt y.txt z.txt"
names "an open folder moved away and another made in its place: that one watched" \
    "click:60,$suby;release;panel;lua:os.execute('mv $T/sub $T/gone && mkdir $T/sub');watchwait;lua:os.execute('echo r > $T/sub/r.txt');watchwait" \
    "tree: gone/ >sub/ sub/r.txt x.txt y.txt z.txt"
# (Linux: more changes than inotify's queue holds while nothing is taken
# from it, so that it overflows and what came after is lost.)
if [ -r /proc/sys/fs/inotify/max_queued_events ]; then
    many=$(cat /proc/sys/fs/inotify/max_queued_events)
    flood="cd $T/sub && seq $many | xargs touch && seq $many | xargs rm"
    names "inotify's queue overflowing: everything read again" \
        "click:60,$suby;release;panel;watchheld:$flood && echo n > $T/new.txt;watchwait" \
        "tree: >sub/ sub/q.txt new.txt x.txt y.txt z.txt"
    names "and an open folder moved away in what was lost: the one in its place watched" \
        "click:60,$suby;release;panel;watchheld:$flood && mv $T/sub $T/gone && mkdir $T/sub;watchwait;lua:os.execute('echo r > $T/sub/r.txt');watchwait" \
        "tree: gone/ >sub/ sub/r.txt x.txt y.txt z.txt"
fi
got=$(fresh; "$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "key:cmd+B;panel;lua:os.execute('echo changed > $T/x.txt');watchwait;docdump:0" "$T/x.txt" 2>&1 >/dev/null | tail -1)
if [ -z "$print" ] && [ "$got" != "> |changed" ]; then
    printf 'FAIL: the open file another program writes: read again\n    got %s\n' "$got"
    failed=1
fi
# (The new file put in its place, as git and most editors do: one written
# over where it is shows through the tab's mapping of it on Linux.)
got=$(fresh; "$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "key:cmd+B;panel;text:mine ;lua:os.execute('echo changed > $T/x.new && mv $T/x.new $T/x.txt');watchwait;docdump:0" "$T/x.txt" 2>&1 >/dev/null | grep -v '^$' | tail -2 | tr '\n' '#')
if [ -z "$print" ] && [ "$got" != "blit: x.txt changed on disk: its changes here are kept, and saving writes over it#> mine |x#" ]; then
    printf 'FAIL: the open file with changes another program writes: kept, and said\n    got %s\n' "$got"
    failed=1
fi
# Written over where it is: the tab's text stays as it was (it is copied at
# the first edit; before, the new text showed through on Linux).
got=$(fresh; "$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "key:cmd+B;panel;text:mine ;lua:os.execute('echo changed > $T/x.txt');watchwait;docdump:0" "$T/x.txt" 2>&1 >/dev/null | grep -v '^$' | tail -1)
if [ -z "$print" ] && [ "$got" != "> mine |x" ]; then
    printf 'FAIL: the open file with changes written over where it is: its text kept\n    got %s\n' "$got"
    failed=1
fi
if ! "$blit" --offscreen 1600x900 --info "$T/x.txt" 2>/dev/null | grep -q "cell 16 x 31 px"; then
    echo "skipped: the cell size here is not the one the mouse positions were written for"
    [ "$failed" = 0 ] || exit 1  # (the cases before this one ran)
    exit 77
fi
check "as it starts" "frame" \
    "tree: sub/@42 x.txt@57 y.txt@73 z.txt@88"
check "a file made in an open .github folder: shown (that is not git's own folder)" \
    "lua:os.execute('mkdir $T/.github');watchwait;click:60,42;release;panel;lua:os.execute('echo w > $T/.github/ci.yml');watchwait" \
    "tree: >.github/@42 .github/ci.yml@57 sub/@73 x.txt@88 y.txt@104 z.txt@119"
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
check "a file dragged onto a folder with a link of its name (to nothing) in it: not put over the link" \
    "lua:os.execute('ln -s nowhere $T/sub/y.txt');watchwait;click:60,73;drag:70,60;drag:60,42;release;panel" \
    "tree: sub/@42 x.txt@57 >y.txt@73 z.txt@88" "./sub/q.txt ./x.txt ./y.txt ./z.txt "
# (A folder, opened, and a file in it chosen with it, the file first.)
both="lua:os.execute('mkdir $T/dest');watchwait;click:60,57;release;panel;click:60,73;release;click:60,57,1,cmd;release"
check "a folder dragged with a file in it chosen too: the file stays in it" "$both;click:60,73;drag:70,60;drag:60,42;release;panel" \
    "tree: dest/@42 x.txt@57 y.txt@73 z.txt@88" "./dest/sub/q.txt ./x.txt ./y.txt ./z.txt "
check "and trashed together: the same" "$both;key:cmd+Backspace;panel" \
    "tree: dest/@42 x.txt@57 y.txt@73 z.txt@88" "./x.txt ./y.txt ./z.txt "
if [ -z "$print" ] && [ "$(cd "$BLIT_TRASH_DIR" && find . -type f | sort | tr '\n' ' ')" != "./sub/q.txt " ]; then
    printf 'FAIL: trashed together: in the Trash\n    got %s\n' "$(cd "$BLIT_TRASH_DIR" && find . -type f | tr '\n' ' ')"
    failed=1
fi
check "a file from an open folder dropped below the rows: moved to the top" "click:60,42;release;panel;click:60,57;drag:70,100;drag:60,300;release;panel" \
    "tree: sub/@42 >q.txt@57 x.txt@73 y.txt@88 z.txt@104" "./q.txt ./x.txt ./y.txt ./z.txt "
check "a file dropped in its own folder: nothing" "click:60,57;drag:70,70;drag:60,88;release;panel" \
    "tree: sub/@42 >x.txt@57 y.txt@73 z.txt@88" "./sub/q.txt ./x.txt ./y.txt ./z.txt "
exit $failed
