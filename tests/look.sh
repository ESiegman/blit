#!/bin/sh
# The looks: rounded boxes, shadows, what lights up under the mouse, and the
# motion. Frames are drawn into a texture by scripted runs and compared
# pixel for pixel:
#   - the same script gives the same frame twice (nothing depends on the clock);
#   - the [look] settings change the frame, and switching them off changes it back
#     to one that no longer depends on them;
#   - a frame drawn with things in motion, once they have arrived, is the
#     frame drawn without motion;
#   - the mouse over a tab or a row of the side panel changes the frame, and
#     leaving changes it back.
#
#   tests/look.sh path/to/blit
set -u
blit=${1:?usage: tests/look.sh path/to/blit}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
mkdir "$dir/work" "$dir/work/sub"
i=0
while [ $i -lt 120 ]; do
    echo "line $i of the file, long enough to be scrolled sideways a little way and back again"
    i=$((i + 1))
done >"$dir/work/a.txt"
cp "$dir/work/a.txt" "$dir/work/b.txt"
cp "$dir/work/a.txt" "$dir/work/sub/c.txt"
base='blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.panel.open = true\n'
printf "$base" >"$dir/default.lua"
printf "$base"'blit.opt.look.rounding = 0\nblit.opt.look.shadows = false\n' >"$dir/square.lua"
printf "$base"'blit.opt.look.rounding = 0\nblit.opt.look.shadows = false\nblit.opt.look.animations = false\nblit.opt.look.cursor_glide_ms = 0\n' >"$dir/still.lua"
printf "$base"'blit.opt.look.rounding = 9\n' >"$dir/rounder.lua"

failed=0
# shot NAME CONFIG SCRIPT: draws the frame the script ends on into NAME.bmp.
shot() {
    BLIT_INIT="$dir/$2.lua" "$blit" --offscreen 1300x700 --shot "$dir/$1.bmp" --script "workdir:$dir/work;panel;$3;frame" "$dir/work/a.txt" >/dev/null 2>&1
    [ -s "$dir/$1.bmp" ] || { echo "FAIL: no frame for $1"; failed=1; }
}
same() { cmp -s "$dir/$1.bmp" "$dir/$2.bmp" || { echo "FAIL: $3"; failed=1; }; }
differ() { ! cmp -s "$dir/$1.bmp" "$dir/$2.bmp" || { echo "FAIL: $3"; failed=1; }; }

# A scene with most of it in: two tabs, a selection, the view scrolled both ways, a menu open.
scene="open:$dir/work/b.txt;vim:8jwvjje;line:5;col:3;rclick:60,120"
shot one default "$scene"
shot two default "$scene"
same one two "the same script gave two different frames"
shot square square "$scene"
differ one square "rounding = 0 and shadows = false changed nothing"
shot still still "$scene"
same square still "with nothing moving, animations and cursor_glide_ms changed a scripted frame"
shot rounder rounder "$scene"
differ one rounder "a larger rounding changed nothing"

# Motion: the cursor glides and the menu fades in; once they are there the frame is the one drawn without.
shot moved default "motion;$scene;frame;step:1000;step:1000"
same one moved "a frame drawn after the motion had finished differs from one drawn without motion"
shot moving default "motion;frame;$scene;frame;step:8"
differ one moving "a frame drawn 8 ms into the motion is already the finished one"

# What the mouse is over lights up, and goes out again.
plain="open:$dir/work/b.txt;frame"
shot rest default "$plain"
shot tab default "$plain;mouse:300,12"
differ rest tab "the mouse over a tab changed nothing"
shot row default "$plain;mouse:60,45"
differ rest row "the mouse over a row of the side panel changed nothing"
differ tab row "the mouse over a tab and over a row gave the same frame"
shot left default "$plain;mouse:300,12;mouse:60,45;mouse:700,400"
same rest left "the frame did not go back to what it was when the mouse left the tab and the row"

# Line numbers: Vim's relative ones by default, every line its own with them off.
printf "$base"'blit.opt.editor.relative_numbers = false\n' >"$dir/absolute.lua"
shot rel_far default "vim:5j"
shot rel_near default "vim:6j;vim:k"
same rel_far rel_near "relative numbers depend on how the cursor got to its line"
shot abs_far absolute "vim:5j"
differ rel_far abs_far "relative_numbers = false changed nothing"
shot abs_top absolute "vim:gg"
shot rel_top default "vim:gg"
differ abs_top rel_top "relative numbers look the same as absolute ones (the cursor's line's number stands at the left)"

[ $failed = 1 ] || echo "look ok"
exit $failed
