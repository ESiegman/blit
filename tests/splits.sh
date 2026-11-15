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
check "a split below, and the keyboard moved up, left and back" "$two;key:cmd+shift+\\;key:cmd+alt+Up;key:cmd+alt+Left;key:cmd+alt+Right" \
    "H([a.txt b.txt*] | V([c.txt*] / >[untitled*])0.50)0.50"
check "the divider dragged" "$two;click:400,200;drag:600,200;release" \
    "H([a.txt b.txt*] | >[c.txt*])0.75"
check "the wheel scrolls the split under the mouse, not the one with the keyboard" "$two;mouse:200,200;wheel:-5" \
    "H([a.txt b.txt*]@16 | >[c.txt*])0.50"
check "a ninth split is not made" 'key:cmd+\;key:cmd+\;key:cmd+\;key:cmd+\;key:cmd+\;key:cmd+\;key:cmd+\;key:cmd+\;key:cmd+\;key:cmd+W' \
    "H([a.txt*] | H([untitled*] | H([untitled*] | H([untitled*] | H([untitled*] | H([untitled*] | >[untitled*])0.50)0.50)0.50)0.50)0.50)0.50"
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
check "a file that is open already: its tab moves to the new split" "$panel;click:60,59;drag:300,300;drag:660,430;release" \
    "H([a.txt*] | V([c.txt*] / >[b.txt*])0.50)0.50"
check "tabs: new, next, close, and the last one closed leaves a new file" 'key:cmd+T;key:cmd+T;key:ctrl+Tab;key:cmd+W;key:cmd+W;key:cmd+W' \
    ">[untitled*]"

[ -n "$print" ] || [ $failed = 1 ] || echo "splits ok"
exit $failed
