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
#     leaving changes it back;
#   - a theme tried in Choose a theme changes it, and Esc changes it back.
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

# Choose a theme: the one the choice comes to is put on at once, and Esc
# puts back the frame there was.
shot trying default "$plain;command:Choose a theme;key:Down;key:Down;frame"
differ rest trying "the theme the choice came to was not put on"
shot tried default "$plain;command:Choose a theme;key:Down;key:Down;frame;key:Escape"
same rest tried "Esc did not put the theme there was back"

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

# The minimap: down the right of a split wide enough for it (not of a
# narrow one, nor with editor.minimap off); a click on it scrolls there.
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n' >"$dir/map.lua"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.minimap = false\n' >"$dir/nomap.lua"
i=0
while [ $i -lt 3000 ]; do echo "int line_$i = $i;"; i=$((i + 1)); done >"$dir/work/long.cpp"
mm() { BLIT_INIT="$dir/$1.lua" "$blit" --offscreen "$2" --shot "$dir/$1-mm.bmp" --script "workdir:$dir/work;frame;$3;minimapdump" "$dir/work/long.cpp" 2>&1 >/dev/null | tail -1; }
got=$(mm map 1600x700 "")
case $got in "top 0 tiles "[1-9]*) ;; *) echo "FAIL: a wide split's minimap: $got"; failed=1 ;; esac
cp "$dir/map-mm.bmp" "$dir/mapone.bmp"
got=$(mm map 700x700 "")
[ "$got" = "none" ] || { echo "FAIL: a narrow split has none: $got"; failed=1; }
got=$(mm nomap 1600x700 "")
cp "$dir/nomap-mm.bmp" "$dir/maptwo.bmp"
[ "$got" = "none" ] || { echo "FAIL: editor.minimap = false: $got"; failed=1; }
got=$(mm map 1600x700 "click:795,200;frame")
case $got in "top 0 "*) echo "FAIL: a click on the minimap scrolled nothing: $got"; failed=1 ;; esac
differ mapone maptwo "the minimap drew nothing"
# The view's box on it, with lines folded away or wrapped (the view scrolls
# by rows, the minimap is in lines): at the end of the file at its end, and
# a click puts the view over the line clicked on.
{
    echo "int f() {"
    i=0
    while [ $i -lt 2000 ]; do echo "    int a_$i = $i;"; i=$((i + 1)); done
    echo "}"
    i=0
    while [ $i -lt 1000 ]; do echo "int b_$i = $i;"; i=$((i + 1)); done
} >"$dir/work/fold.cpp"
i=0
while [ $i -lt 3000 ]; do
    if [ $i -lt 1000 ]; then echo "int long_$i = $i; // a comment long enough to go on into the rows under it when wrapped, further than any split is wide, and on past that"
    else echo "int line_$i = $i;"; fi
    i=$((i + 1))
done >"$dir/work/wrap.cpp"
mmbox() { BLIT_INIT="$dir/map.lua" "$blit" --offscreen 1600x700 --shot "$dir/box.bmp" --script "workdir:$dir/work;frame;$2;frame;minimapdump" "$dir/work/$1" 2>&1 >/dev/null | tail -1 | sed -e 's/.* box //'; }
got=$(mmbox fold.cpp "command:Fold every block;frame;vim:G")
case $got in 29[89][0-9]-300[0-9]) ;; *) echo "FAIL: the minimap's box at the end of a file with a fold: $got"; failed=1 ;; esac
got=$(mmbox wrap.cpp "command:Toggle soft wrap;frame;vim:G")
case $got in 29[89][0-9]-300[0-9]) ;; *) echo "FAIL: the minimap's box at the end of a wrapped file: $got"; failed=1 ;; esac
got=$(mmbox wrap.cpp "command:Toggle soft wrap;frame;vim:500G")
case $got in 49[0-9]-5[01][0-9]) ;; *) echo "FAIL: the minimap's box over line 500 of a wrapped file: $got"; failed=1 ;; esac
# Drawn again when what it is drawn with changes: a theme put on whose
# background is the one there was and whose text is another colour, and
# another tab width (with the indent lines of a file indented with tabs
# a tab apart). Each frame the same as one that started that way.
twins='blit.opt.themes.twin = { background = "#1c1d21", foreground = "#ff2020" }\nblit.opt.themes.plain = { background = "#1c1d21" }\n'
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n'"$twins"'blit.opt.theme = "plain"\n' >"$dir/plain.lua"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n'"$twins"'blit.opt.theme = "twin"\n' >"$dir/twin.lua"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.tab_width = 8\n' >"$dir/tab8.lua"
i=0
while [ $i -lt 300 ]; do printf 'int f_%d() {\n\treturn %d;\n}\n' $i $i; i=$((i + 1)); done >"$dir/work/tabs.cpp"
mmshot() { BLIT_INIT="$dir/$1.lua" "$blit" --offscreen 1600x700 --shot "$dir/$2.bmp" --script "workdir:$dir/work;frame;frame;$4;frame;frame" "$dir/work/$3" >/dev/null 2>&1; }
mmshot twin twin long.cpp ""
mmshot plain retheme long.cpp "lua:blit.opt.theme = 'twin'"
same twin retheme "the minimap kept the colours of the theme there was"
mmshot tab8 tab8 tabs.cpp ""
mmshot map retab tabs.cpp "lua:blit.opt.editor.tab_width = 8"
same tab8 retab "a tab width set while running is drawn otherwise than one set from the start (the minimap, the indent lines)"
# (Two clicks some way apart: the box lands as many lines apart as in a
# file with nothing wrapped.)
apart() { a=$(mmbox "$1" "$2;click:795,200") b=$(mmbox "$1" "$2;click:795,260"); echo $((${b%-*} - ${a%-*})); }
want=$(apart long.cpp "frame")
got=$(apart wrap.cpp "command:Toggle soft wrap;frame;vim:500G;frame")
[ "$got" -ge $((want - 1)) ] && [ "$got" -le $((want + 1)) ] || { echo "FAIL: clicks on the minimap of a wrapped file land $got lines apart, not $want"; failed=1; }

# Distraction-free and presentation modes: the chrome gone (and back), the
# text larger for showing.
fm=$(BLIT_INIT="$dir/map.lua" "$blit" --offscreen 1300x700 --shot "$dir/fm.bmp" --script "workdir:$dir/work;frame;focusdump;command:Toggle distraction-free mode;focusdump;command:Toggle presentation mode;focusdump;command:Toggle presentation mode;focusdump" "$dir/work/long.cpp" 2>&1 >/dev/null | tail -4 | sed -e 's/ tabs [1-9][0-9]*/ tabs N/' -e 's/ status [1-9][0-9]*/ status N/' -e 's/ gutter [1-9][0-9]*/ gutter N/' -e 's/ crumbs [1-9][0-9]*/ crumbs N/' -e 's/ tabs N status N gutter N panel [0-9]* crumbs N/ chrome/' | tr '\n' '#')
want="off | 13pt | chrome#distraction-free | 13pt | tabs 0 status 0 gutter 0 panel 0 crumbs 0#presentation | 21pt | tabs 0 status 0 gutter 0 panel 0 crumbs 0#off | 13pt | chrome#"
[ "$fm" = "$want" ] || { echo "FAIL: distraction-free and presentation modes: $fm"; failed=1; }

[ $failed = 1 ] || echo "look ok"
exit $failed
