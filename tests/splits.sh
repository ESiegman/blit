#!/bin/sh
# Tabs and splits, driven the way a person drives them: scripted mouse and
# key events through the real event handlers, drawing into a texture, with
# the arrangement that results printed and compared with what it should be.
#
#   tests/splits.sh path/to/blit            run the cases
#   tests/splits.sh path/to/blit --print    show what each case gives
#
# A split is printed as its tabs in brackets, the one in front starred, the
# split with the keyboard marked ">"; H(.. | ..) is two side by side and
# V(.. / ..) one above the other, each followed by the first one's share.
#
# The mouse positions below are for a 16 x 31 pixel cell (the default font
# at 13 pt on macOS). With another cell size the test is skipped (exit 77).
set -u
blit=${1:?usage: tests/splits.sh path/to/blit [--print]}
print=${2:-}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
printf 'blit.opt.lsp.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua"
export BLIT_TERM_SHELL="exec env PS1='\$ ' /bin/sh -i"
mkdir "$dir/work"
T="$dir/work"
for f in a b c d; do
    i=0
    while [ $i -lt 200 ]; do
        echo "line $i of $f"
        i=$((i + 1))
    done >"$T/$f.txt"
done

if ! "$blit" --offscreen 1600x900 --info "$T/a.txt" 2>/dev/null | grep -q "cell 16 x 31 px"; then
    echo "skipped: the cell size here is not the one the mouse positions were written for"
    exit 77
fi

failed=0
# check NAME SCRIPT EXPECTED: the last line the script's "dump" prints.
check() {
    got=$("$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "$2;dump" "$T/a.txt" 2>&1 >/dev/null | tail -1)
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$3" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$3" "$got"
        failed=1
    fi
}

# views NAME SCRIPT EXPECTED: the last line the script's "views" prints
# (each split's file in front, its document, the split's own cursor as
# line:column, and that line).
views() {
    got=$("$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "$2;views" "$T/a.txt" 2>&1 >/dev/null | grep '^views:' | tail -1)
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$3" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$3" "$got"
        failed=1
    fi
}

# Two splits: a.txt and b.txt on the left, c.txt on the right, which has the keyboard.
two="open:$T/b.txt;key:cmd+\\;open:$T/c.txt;frame"
# The same with the side panel open, showing a.txt to d.txt.
panel="key:cmd+B;panel;$two"

check "the starting point" "$two" \
    "H([a.txt b.txt*] | >[c.txt*])0.50"
check "a tab to the right edge of the other split: a new split there" "$two;click:136,12;drag:300,100;drag:760,250;release" \
    "H([a.txt*] | H([c.txt*] | >[b.txt*])0.50)0.50"
check "a tab into the middle of the other split: among its tabs" "$two;click:136,12;drag:300,100;drag:600,250;release" \
    "H([a.txt*] | >[c.txt b.txt*])0.50"
check "a tab onto the other split's bar, before its first tab" "$two;click:136,12;drag:300,100;drag:420,12;release" \
    "H([a.txt*] | >[b.txt* c.txt])0.50"
check "a tab to the bottom edge of its own split" "$two;click:136,12;drag:136,200;drag:200,430;release" \
    "H(V([a.txt*] / >[b.txt*])0.50 | [c.txt*])0.50"
check "a split's only tab to the far edge of the other: the split moves" "$two;click:440,12;drag:440,100;drag:20,250;release" \
    "H(>[c.txt*] | [a.txt b.txt*])0.50"
check "a split's only tab into the other's middle: the split goes" "$two;click:440,12;drag:440,100;drag:200,250;release" \
    ">[a.txt b.txt c.txt*]"
check "a tab let go over the middle of its own split: nothing" "$two;click:136,12;drag:136,200;drag:200,250;release" \
    "H(>[a.txt b.txt*] | [c.txt*])0.50"
check "a tab dragged out and Esc pressed: nothing" "$two;click:136,12;drag:300,100;drag:760,250;key:Escape;release" \
    "H(>[a.txt b.txt*] | [c.txt*])0.50"
check "a group, by the empty part of its bar, to the bottom of the other" "$two;click:350,12;drag:350,100;drag:600,430;release" \
    "V([c.txt*] / >[a.txt b.txt*])0.50"
check "a group into the middle of the other: the tabs join it" "$two;click:350,12;drag:350,100;drag:600,250;release" \
    ">[c.txt a.txt b.txt*]"
check "a tab moved along its own bar" "$two;click:40,12;drag:180,12;release" \
    "H(>[b.txt a.txt*] | [c.txt*])0.50"
check "closing a split's last tab closes the split" "$two;key:cmd+W" \
    ">[a.txt b.txt*]"
check "a split below, and the keyboard moved up, left and back" "$two;key:cmd+shift+\\;key:Escape;key:cmd+alt+Up;key:cmd+alt+Left;key:cmd+alt+Right" \
    "H([a.txt b.txt*] | V([c.txt*] / >[untitled*])0.50)0.50"
check "the divider dragged" "$two;click:400,200;drag:600,200;release" \
    "H([a.txt b.txt*] | >[c.txt*])0.75"
check "the wheel scrolls the split under the mouse, not the one with the keyboard" "$two;mouse:200,200;wheel:-5" \
    "H([a.txt b.txt*]@16 | >[c.txt*])0.50"
# Sixteen splits at most (each Cmd+\ opens Go to file for its split; the
# next one shuts that box and splits again).
many=$(i=0; while [ $i -lt 17 ]; do printf 'key:cmd+shift+\;'; i=$((i + 1)); done)
got=$("$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "${many}key:Escape;frame;dump" "$T/a.txt" 2>&1 >/dev/null | tail -1 | tr -cd '[' | wc -c | tr -d ' ')
if [ -n "$print" ]; then
    printf 'a seventeenth split is not made\n    %s splits\n' "$got"
elif [ "$got" != 16 ]; then
    printf 'FAIL: a seventeenth split is not made\n    expected 16 splits\n    got      %s\n' "$got"
    failed=1
fi
check "a file from the tree to the bottom edge of a split: a new split with it" "$panel;click:60,90;drag:300,300;drag:660,430;release" \
    "H([a.txt b.txt*] | V([c.txt*] / >[d.txt*])0.50)0.50"
check "a file from the tree into the middle of a split" "$panel;click:60,90;drag:300,250;drag:400,250;release" \
    "H(>[a.txt b.txt d.txt*] | [c.txt*])0.50"
check "a file from the tree onto a tab bar" "$panel;click:60,90;drag:300,100;drag:600,12;release" \
    "H([a.txt b.txt*] | >[c.txt d.txt*])0.50"
check "a click on a file in the tree opens it where the keyboard is" "$panel;click:60,90;release" \
    "H([a.txt b.txt*] | >[c.txt d.txt*])0.50"
check "a file dragged within the panel: nothing" "$panel;click:60,90;drag:80,110;release" \
    "H([a.txt b.txt*] | >[c.txt*])0.50"
check "a file open in another split, dropped on an edge: a new split shows it too" "$panel;click:60,59;drag:300,300;drag:660,430;release" \
    "H([a.txt b.txt*] | V([c.txt*] / >[b.txt*])0.50)0.50"
# Cmd+\ opens Go to file for the new split: what is chosen opens in it.
check "Cmd+\\: Go to file for the new split, and the file chosen opens there" "workdir:$T;$two;key:cmd+\\;text:d.txt;findwait;key:Return" \
    "H([a.txt b.txt*] | H([c.txt*] | >[d.txt*])0.50)0.50"
check "Esc leaves the new split with a new file" "$two;key:cmd+\\;key:Escape" \
    "H([a.txt b.txt*] | H([c.txt*] | >[untitled*])0.50)0.50"
check "a file open in another split: the new one shows it too" "workdir:$T;$two;key:cmd+\\;text:a.txt;findwait;key:Return" \
    "H([a.txt b.txt*] | H([c.txt*] | >[a.txt*])0.50)0.50"
check "Cmd+\\ again while the box is open: another split" "$two;key:cmd+\\;key:cmd+shift+\\;key:Escape" \
    "H([a.txt b.txt*] | H([c.txt*] | V([untitled*] / >[untitled*])0.50)0.50)0.50"
# Cmd+Shift+W closes a split: its tabs join the one beside it.
check "Cmd+Shift+W: the split goes, its tabs join the one beside it" "$two;key:cmd+shift+W" \
    ">[a.txt b.txt c.txt*]"
check "from the left split: its tabs after the right one's, the one in front still in front" "$two;key:cmd+alt+Left;key:cmd+1;key:cmd+shift+W" \
    ">[c.txt a.txt* b.txt]"
check "a new file with nothing in it goes with the split" "$two;key:cmd+\\;key:Escape;key:cmd+shift+W" \
    "H([a.txt b.txt*] | >[c.txt*])0.50"
check "one with something typed in it does not" "$two;key:cmd+\\;key:Escape;text:x;key:cmd+shift+W" \
    "H([a.txt b.txt*] | >[c.txt untitled*])0.50"
check "the last split: its tabs close (unsaved ones asked about), a new file left" "$two;key:cmd+shift+W;text:x;key:cmd+shift+W" \
    ">[untitled*]"
check "the terminals' split (Ctrl+\`): hidden, its programs still running" "$two;key:ctrl+\`;frame;key:cmd+shift+W" \
    "H([a.txt b.txt*] | >[c.txt*])0.50"

# One file in two splits: the same text in both, each with its own place.
self="workdir:$T;key:cmd+\\;text:a.txt;findwait;key:Return"
check "a file split beside itself: two views of it" "$self" \
    "H([a.txt*] | >[a.txt*])0.50"
views "what is typed in one shows in the other; each has its own cursor" "$self;text:XY;key:cmd+alt+Left;key:Down;key:Down" \
    "views: [a.txt#1 2:2 line 2 of a]< [a.txt#1 0:2 XYline 0 of a]"
views "a line put in above the other's cursor moves it along with its line" "workdir:$T;key:Down;key:Down;key:End;$self;key:Up;key:Up;key:Home;key:Return" \
    "views: [a.txt#1 3:11 line 2 of a] [a.txt#1 1:0 line 0 of a]<"
views "undone in one: the other keeps its place in the text" "$self;text:XY;key:cmd+alt+Left;key:Down;key:cmd+alt+Right;key:cmd+Z" \
    "views: [a.txt#1 1:2 line 1 of a] [a.txt#1 0:0 line 0 of a]<"
views "one view of a changed file closed: nothing asked, the other keeps the text" "$self;text:XY;key:cmd+W;asked" \
    "views: [a.txt#1 0:2 XYline 0 of a]<"
check "and its split goes with it" "$self;text:XY;key:cmd+W;asked" \
    ">[a.txt*]"
check "Cmd+Shift+W into a split that shows it already: one tab there" "$self;open:$T/b.txt;key:cmd+shift+W" \
    ">[a.txt b.txt*]"
check "the other split's other tabs stay when its view of the file is closed" "$self;open:$T/b.txt;key:cmd+alt+Left;key:cmd+W" \
    ">[a.txt b.txt*]"

check "tabs: new, next, close, and the last one closed leaves a new file" 'key:cmd+T;key:cmd+T;key:ctrl+Tab;key:cmd+W;key:cmd+W;key:cmd+W' \
    ">[untitled*]"
# (In a terminal Ctrl+W is the program's where Ctrl is the editor's key: Ctrl+F4 closes the tab there.)
close=cmd+W
[ "$(uname -s)" = Darwin ] || close=ctrl+F4
check "Cmd+W on that empty file, and on a last terminal tab: blit stays, an empty file left" "key:cmd+W;key:cmd+W;key:cmd+W;term;key:$close;key:$close" \
    ">[untitled*]"

[ -n "$print" ] || [ $failed = 1 ] || echo "splits ok"
exit $failed
