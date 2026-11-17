#!/bin/sh
# The text's shape from its syntax tree: brackets coloured by how deep they
# are and the pair at the cursor marked (more to come: folding, the
# selection grown by the tree, the scopes pinned at the top and named
# under the tabs). Needs the C++ and Python grammars (blit --install-grammar).
#
#   tests/shape.sh path/to/blit            run the cases
#   tests/shape.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/shape.sh path/to/blit [--print]}
print=${2:-}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
base='blit.opt.lsp.enabled = false
blit.opt.git.enabled = false
blit.opt.editor.restore = false
blit.opt.theme.dark = "blit-dark"
blit.opt.theme.light = "blit-dark"'
printf '%s\nblit.opt.editor.modal = false\n' "$base" >"$dir/init.lua"
printf '%s\nblit.opt.editor.modal = true\n' "$base" >"$dir/vim.lua"
printf '%s\nblit.opt.editor.modal = false\nblit.opt.syntax.bracket_colors = false\n' "$base" >"$dir/plain.lua"
printf '%s\nblit.opt.editor.modal = false\nblit.opt.themes["blit-dark"].brackets = { "#ff0000" }\n' "$base" >"$dir/one.lua"
printf '%s\nblit.opt.editor.modal = false\nblit.opt.editor.breadcrumbs = false\n' "$base" >"$dir/nocrumbs.lua"
printf '%s\nblit.opt.editor.modal = false\nblit.opt.editor.sticky_scroll = false\n' "$base" >"$dir/nosticky.lua"
export BLIT_RECENT_FILE="$dir/recent"
init=
fresh() {
    printf 'int f(int a[3]) {\n    if (a[0]) { g("(x"); }\n    return (a[1]);\n}\n' >"$dir/b.cpp"
    printf 'xs = [1, (2, {3: [4]})]\ns = "([{"\n' >"$dir/b.py"
    printf 'some words here\nand more\n' >"$dir/plain.txt"
    printf 'top\n  inner one\n  inner two\n    deeper\nback\n' >"$dir/indent.txt"
    mkdir -p "$dir/proj/src"
    printf 'namespace geo {\nstruct Point {\n    int x;\n    int len() const {\n        return x;\n    }\n};\nint area(int w) {\n    return w;\n}\n}\n' >"$dir/proj/src/s.cpp"
    printf 'class Shape:\n    def area(self):\n        return 0\n\ndef make():\n    return Shape()\n' >"$dir/proj/s.py"
    i=0
    while [ $i -lt 60 ]; do  # (room to scroll)
        echo >>"$dir/proj/src/s.cpp"
        echo >>"$dir/proj/s.py"
        i=$((i + 1))
    done
}

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run SCRIPT [FILE]: the last line the script prints. with SETTINGS SCRIPT
# [FILE]: the same with other settings.
run() {
    fresh
    got=$(BLIT_INIT="${init:-$dir/init.lua}" "$blit" --offscreen 1200x800 --shot "$dir/shot.bmp" --script "frame;$1" "${2:-$dir/b.cpp}" 2>&1 >/dev/null | grep -v '^blit: ' | tail -1)
}
with() {
    init="$dir/$1.lua"
    shift
    run "$@"
    init=
}

# Brackets by depth, and the pair at the cursor.
run "shapedump"
check "each bracket as deep as the pairs it is inside; one in a string is not the tree's" 'brackets 1:6=0 1:12=1 1:14=1 1:15=0 1:17=0 2:8=1 2:10=2 2:12=2 2:13=1 2:15=1 2:18=2 2:20=- 2:23=2 2:26=1 3:12=1 3:14=2 3:16=2 3:17=1 4:1=0 | inks 3 | match none'
run "shapedump" "$dir/b.py"
check "in Python too" 'brackets 1:6=0 1:10=1 1:14=2 1:18=0 1:20=0 1:21=2 1:22=1 1:23=0 2:6=- 2:7=- 2:8=- | inks 3 | match none'
with plain "shapedump"
check "syntax.bracket_colors = false: one ink, punctuation's" 'brackets 1:6=0 1:12=1 1:14=1 1:15=0 1:17=0 2:8=1 2:10=2 2:12=2 2:13=1 2:15=1 2:18=2 2:20=- 2:23=2 2:26=1 3:12=1 3:14=2 3:16=2 3:17=1 4:1=0 | inks 1 | match none'
with one "shapedump"
check "a theme's own brackets, round and round" 'brackets 1:6=0 1:12=1 1:14=1 1:15=0 1:17=0 2:8=1 2:10=2 2:12=2 2:13=1 2:15=1 2:18=2 2:20=- 2:23=2 2:26=1 3:12=1 3:14=2 3:16=2 3:17=1 4:1=0 | inks 1 | match none'
run "key:End;shapedump"
got=${got#*| inks 3 }
check "the cursor just after a bracket: it and its pair" '| match 1:17 4:1'
run "key:Down;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;shapedump"
got=${got#*| inks 3 }
check "on one" '| match 2:8 2:13'
run "key:Down;key:Down;key:Down;shapedump"
got=${got#*| inks 3 }
check "a closing one at the start of a line" '| match 1:17 4:1'
run "key:Down;key:End;key:Left;key:Left;key:Left;key:Left;key:Left;key:Left;key:Left;shapedump"
got=${got#*| inks 3 }
check "in a string: none" '| match none'
run "key:Down;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:shift+Right;shapedump"
got=${got#*| inks 3 }
check "nor with a selection" '| match none'
with vim "text:j;text:f(;shapedump"
got=${got#*| inks 3 }
check "Vim: the one under the block" '| match 2:8 2:13'
run "key:Down;key:Home;text:x;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;key:Right;shapedump"
got=${got#*| inks 3 }
check "after an edit, where it is now" '| match 2:9 2:14'

# Expand the selection to the tree's node around it, then its parent; shrink back.
grow="keyof:Expand the selection"
shrink="keyof:Shrink the selection"
on_g="key:Down;key:End;key:Left;key:Left;key:Left;key:Left;key:Left;key:Left;key:Left;key:Left;key:Left;key:Left"
run "$on_g;$grow;seldump"
check "Expand: the name at the cursor" '[g]'
run "$on_g;$grow;$grow;seldump"
check "then what it is in" '[g("(x")]'
run "$on_g;$grow;$grow;$grow;seldump"
check "and what that is in" '[g("(x");]'
run "$on_g;$grow;$grow;$grow;$grow;seldump"
check "in brackets: what is between them first" '[ g("(x"); ]'
run "$on_g;$grow;$grow;$grow;$grow;$grow;seldump"
check "then with them" '[{ g("(x"); }]'
run "$on_g;$grow;$grow;$grow;$grow;$grow;$grow;$grow;$grow;$grow;$grow;$grow;$grow;seldump"
check "up to the whole file" '[int f(int a[3]) { /     if (a[0]) { g("(x"); } /     return (a[1]); / } / ]'
run "$on_g;$grow;$grow;$grow;$shrink;seldump"
check "Shrink goes back one" '[g("(x")]'
run "$on_g;$grow;$grow;$shrink;$shrink;$shrink;seldump"
check "and back to where it started" 'none'
run "$on_g;$grow;key:Right;$grow;seldump"
check "a selection made since starts again" '[(]'
with vim "text:j;text:fg;$grow;$grow;vimdump"
check "Vim: Visual mode" 'VISUAL | -- VISUAL -- | 2:23 |     if (a[0]) { g("(x"); }'
with vim "text:j;text:fg;$grow;$grow;seldump"
check "the same" '[g("(x")]'
run "$grow;seldump" "$dir/plain.txt"
check "no grammar: the word" '[some]'
run "$grow;$grow;$grow;seldump" "$dir/plain.txt"
check "then its line, then everything" '[some words here / and more / ]'

# The breadcrumbs under the tabs: the file's folders from the project's,
# its name, and the definitions the cursor is in; the outline without a
# language server, from the tree.
S="$dir/proj/src/s.cpp"
down4="key:Down;key:Down;key:Down;key:Down"
run "workdir:$dir/proj;frame;crumbsdump" "$S"
check "the folders, the file, the namespace the cursor is in" 'src > s.cpp > geo'
run "workdir:$dir/proj;frame;$down4;frame;crumbsdump" "$S"
check "a method, in its struct, in its namespace" 'src > s.cpp > geo > Point > len()'
run "workdir:$dir/proj;frame;$down4;$down4;frame;crumbsdump" "$S"
check "a function" 'src > s.cpp > geo > area()'
run "workdir:$dir/proj;frame;key:Down;key:Down;key:Down;key:Down;key:Down;frame;crumbsdump" "$dir/proj/s.py"
check "in Python" 's.py > make()'
with nocrumbs "workdir:$dir/proj;frame;crumbsdump" "$S"
check "editor.breadcrumbs = false" 'none'
run "workdir:$dir/proj;frame;$down4;frame;key:cmd+alt+O;frame;finddump" "$S"
check "the outline without a server: the tree's definitions, nested, the one the cursor is in chosen" 'pick [] 4 : geo  · namespace   Point  · struct >    len  · method   area  · function'
run "workdir:$dir/proj;frame;key:cmd+alt+O;frame;text:are;key:Return;vimdump" "$S"
check "Enter goes there" 'NORMAL |  | 8:5 | int area(int w) {'
run "workdir:$dir/proj;frame;key:Down;key:Down;key:cmd+alt+O;frame;finddump" "$dir/proj/s.py"
check "in Python" 'pick [] 3 : Shape  · class >  area  · method make  · function'

run "workdir:$dir/proj;frame;open:$dir/proj/s.py;frame;key:cmd+alt+P;frame;text:ar;frame;finddump" "$S"
check "Cmd+Option+P without a server: the tree's definitions in the files open" 'pick [ar] 3 : >area  · method  · s.py:2 area  · function  · src/s.cpp:8 geo  · namespace  · src/s.cpp:1'
run "workdir:$dir/proj;frame;open:$dir/proj/s.py;frame;key:cmd+alt+P;frame;text:ar;frame;key:Down;key:Return;frame;vimdump" "$S"
check "Enter goes there, in its tab" 'NORMAL |  | 8:5 | int area(int w) {'

# Sticky scroll: the lines that open the scopes the top of the view is in.
run "workdir:$dir/proj;frame;stickydump" "$S"
check "at the top: none" 'none'
run "workdir:$dir/proj;frame;line:5;frame;stickydump" "$S"
check "the namespace's and the struct's lines, the line under them in both" '1 2'
run "workdir:$dir/proj;frame;line:3;frame;stickydump" "$S"
check "and the method's, the line under them in it" '1 2 4'
run "workdir:$dir/proj;frame;line:8;frame;stickydump" "$S"
check "a function" '1 8'
run "workdir:$dir/proj;frame;line:5;frame;stickydump" "$dir/proj/s.py"
check "in Python" 'none'
run "workdir:$dir/proj;frame;line:2;frame;stickydump" "$dir/proj/s.py"
check "a method in its class" '1 2'
with nocrumbs "workdir:$dir/proj;frame;line:5;frame;stickydump" "$S"
check "(not with the breadcrumbs' setting)" '1 2'
with nosticky "workdir:$dir/proj;frame;line:5;frame;stickydump" "$S"
check "editor.sticky_scroll = false" 'none'

# Folding: the block at the cursor folds under its first line; the rows go
# past it; moving, searching or jumping into it opens it.
fold="keyof:Fold the block at the cursor"
unfold="keyof:Unfold the block at the cursor"
run "workdir:$dir/proj;frame;key:Down;key:Down;key:Down;$fold;frame;folddump" "$S"
check "on a block's first line: it folds" 'folds 4-5 | rows 1 2 3 4 6 7 8 9 10 11 12 13'
run "workdir:$dir/proj;frame;key:Down;key:Down;key:Down;$fold;key:Down;frame;vimdump" "$S"
check "Down goes past it" 'NORMAL |  | 6:1 |     }'
run "workdir:$dir/proj;frame;key:Down;key:Down;key:Down;$fold;key:Down;key:Up;frame;vimdump" "$S"
check "Up back to it" 'NORMAL |  | 4:1 |     int len() const {'
run "workdir:$dir/proj;frame;key:Down;key:Down;key:Down;key:Down;$fold;frame;vimdump" "$S"
check "inside one: the innermost around it, the cursor to its first line" 'NORMAL |  | 4:5 |     int len() const {'
run "workdir:$dir/proj;frame;key:Down;key:Down;key:Down;$fold;key:Up;$fold;frame;folddump" "$S"
check "a fold in a fold" 'folds 4-5 2-6 | rows 1 2 7 8 9 10 11 12 13 14 15 16'
run "workdir:$dir/proj;frame;key:Down;key:Down;key:Down;$fold;key:Up;$fold;$unfold;frame;folddump" "$S"
check "unfolded, the one inside stays as it was" 'folds 4-5 | rows 1 2 3 4 6 7 8 9 10 11 12 13'
run "workdir:$dir/proj;frame;command:Fold every block;frame;folddump" "$S"
check "every block" 'folds 1-10 2-6 4-5 8-9 | rows 1 11 12 13 14 15 16 17 18 19 20 21'
run "workdir:$dir/proj;frame;command:Fold every block;command:Unfold every block;frame;folddump" "$S"
check "and none" 'folds none | rows 1 2 3 4 5 6 7 8 9 10 11 12'
run "workdir:$dir/proj;frame;key:Down;key:Down;key:Down;$fold;key:cmd+P;text::5;key:Return;frame;folddump" "$S"
check "a jump into it opens it" 'folds none | rows 1 2 3 4 5 6 7 8 9 10 11 12'
run "workdir:$dir/proj;frame;key:Down;key:Down;key:Down;$fold;key:End;text: // here;frame;folddump" "$S"
check "typing on its first line keeps it" 'folds 4-5 | rows 1 2 3 4 6 7 8 9 10 11 12 13'
with vim "workdir:$dir/proj;frame;text:jjj;text:zc;text:j;frame;vimdump" "$S"
check "Vim: zc, and j goes past it" 'NORMAL |  | 6:1 |     }'
with vim "workdir:$dir/proj;frame;text:jjj;text:zc;text:zo;frame;folddump" "$S"
check "zo" 'folds none | rows 1 2 3 4 5 6 7 8 9 10 11 12'
with vim "workdir:$dir/proj;frame;text:jjj;text:za;text:za;text:za;frame;folddump" "$S"
check "za, and again, and again" 'folds 4-5 | rows 1 2 3 4 6 7 8 9 10 11 12 13'
with vim "workdir:$dir/proj;frame;text:zM;frame;folddump" "$S"
check "zM" 'folds 1-10 2-6 4-5 8-9 | rows 1 11 12 13 14 15 16 17 18 19 20 21'
with vim "workdir:$dir/proj;frame;text:zM;text:zR;frame;folddump" "$S"
check "zR" 'folds none | rows 1 2 3 4 5 6 7 8 9 10 11 12'
run "key:Down;$fold;frame;folddump" "$dir/indent.txt"
check "no grammar: by indentation" 'folds 1-4 | rows 1 5 6'

# The scroll bar's marks: where the cursor, the matches and the rest are.
run "workdir:$dir/proj;frame;rulerdump" "$S"
check "the cursor's place, at the top" '0:>'
run "workdir:$dir/proj;frame;key:cmd+F;text:return;frame;wait:100;frame;rulerdump" "$S"
check "Find in this file's matches (the cursor on the first)" '3:> 7:f'
run "workdir:$dir/proj;frame;key:cmd+P;text::70;key:Return;frame;rulerdump" "$S"
check "the cursor near the end" '60:>'
run "frame;rulerdump" "$dir/plain.txt"
check "a file shorter than the view: none" 'none'

[ "$failed" = 0 ] && echo "shape: all passed"
exit "$failed"
