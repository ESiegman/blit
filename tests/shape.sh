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
export BLIT_RECENT_FILE="$dir/recent"
init=
fresh() {
    printf 'int f(int a[3]) {\n    if (a[0]) { g("(x"); }\n    return (a[1]);\n}\n' >"$dir/b.cpp"
    printf 'xs = [1, (2, {3: [4]})]\ns = "([{"\n' >"$dir/b.py"
    printf 'some words here\nand more\n' >"$dir/plain.txt"
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

[ "$failed" = 0 ] && echo "shape: all passed"
exit "$failed"
