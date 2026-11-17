#!/bin/sh
# Editing comforts: lines moved and copied, and brackets and quotes closed
# as they are typed. Needs the C++ grammar for the cases in a comment.
#
#   tests/edit.sh path/to/blit            run the cases
#   tests/edit.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/edit.sh path/to/blit [--print]}
print=${2:-}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
base='blit.opt.lsp.enabled = false
blit.opt.git.enabled = false
blit.opt.editor.restore = false'
printf '%s\nblit.opt.editor.modal = false\n' "$base" >"$dir/init.lua"
printf '%s\nblit.opt.editor.modal = true\n' "$base" >"$dir/vim.lua"
printf '%s\nblit.opt.editor.modal = false\nblit.opt.editor.close_pairs = false\n' "$base" >"$dir/off.lua"
export BLIT_RECENT_FILE="$dir/recent"
init=
fresh() {
    printf 'one\ntwo\nthree\nfour' >"$dir/l.txt"
    printf 'int f() {\n    g(x);\n}\n' >"$dir/p.cpp"
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
# run SCRIPT [FILE]: the text after it, its lines joined by " / ", and the
# cursor (vimdump's line:column), joined by " @ ".
run() {
    fresh
    out=$(BLIT_INIT="${init:-$dir/init.lua}" "$blit" --offscreen 1000x600 --shot "$dir/shot.bmp" --script "frame;$1;frame;lsptext;vimdump" "${2:-$dir/l.txt}" 2>&1 >/dev/null | grep -v '^blit: ' | tail -2)
    got="$(printf '%s\n' "$out" | head -1) @ $(printf '%s\n' "$out" | tail -1 | cut -d'|' -f3 | tr -d ' ')"
}
with() {
    init="$dir/$1.lua"
    shift
    run "$@"
    init=
}
up="keyof:Move the lines up"
down="keyof:Move the lines down"

# Lines moved and copied.
run "$down"
check "Option+Down: the line goes down past the next" 'two / one / three / four @ 2:1'
run "key:Down;$up"
check "Option+Up: up" 'two / one / three / four @ 1:1'
run "key:Down;key:Down;$down"
check "onto the last line, which has no end of its own" 'one / two / four / three @ 4:1'
run "key:Down;key:Down;key:Down;$down"
check "the last line: nowhere to go" 'one / two / three / four @ 4:1'
run "key:Down;key:shift+Down;key:shift+Down;$down"
check "the lines chosen (one the selection ends at the start of is not)" 'one / four / two / three @ 4:6'
run "key:Down;$down;key:cmd+Z"
check "one undo step" 'one / two / three / four @ 3:6'
run "key:Down;command:Copy the lines down"
check "copied below: the cursor goes with the copy" 'one / two / two / three / four @ 3:1'
run "key:Down;command:Copy the lines up"
check "above: it stays with the lines it was on" 'one / two / two / three / four @ 2:1'
with vim "text:j;text:V;text:j;$up"
check "Vim: Visual mode's lines" 'two / three / one / four @ 2:5'

# Brackets and quotes closed as they are typed.
F="$dir/p.cpp"
run "key:Down;key:End;key:Return;text:h;text:(" "$F"
check "an opening bracket puts its closer after the cursor" 'int f() { /     g(x); / h() / } /  @ 3:3'
run "key:Down;key:End;key:Return;text:h;text:(;text:a;text:)" "$F"
check "the closer typed steps over it" 'int f() { /     g(x); / h(a) / } /  @ 3:5'
run "key:Down;key:End;key:Return;text:h;text:(;key:Backspace" "$F"
check "Backspace between them takes both" 'int f() { /     g(x); / h / } /  @ 3:2'
run "key:Down;key:End;key:Return;text:s = \";text:x;text:\"" "$F"
check "quotes the same way" 'int f() { /     g(x); / s = "x" / } /  @ 3:8'
run "key:Down;key:End;key:Return;text:{;key:Return;text:y" "$F"
check "Enter between braces: an indented line between them" 'int f() { /     g(x); / { /     y / } / } /  @ 4:6'
run "key:Down;key:End;key:Left;key:Left;key:Left;key:shift+Right;text:[" "$F"
check "with a selection: round it" 'int f() { /     g([x]); / } /  @ 2:9'
run "key:Down;key:End;key:Left;key:Left;key:Left;text:(" "$F"
check "not before a word" 'int f() { /     g((x); / } /  @ 2:8'
run "key:Down;key:End;key:Return;text:// it's (fine" "$F"
check "nor a quote after a letter, nor in a comment" 'int f() { /     g(x); / // it'"'"'s (fine / } /  @ 3:14'
run "key:Down;key:End;key:Left;key:Left;text:)" "$F"
check "a closer it did not put in is typed" 'int f() { /     g(x)); / } /  @ 2:9'
with off "key:Down;key:End;key:Return;text:h;text:(" "$F"
check "editor.close_pairs = false" 'int f() { /     g(x); / h( / } /  @ 3:3'
with vim "text:jA;text: q(;text:1;text:);key:Escape" "$F"
check "Vim's Insert mode" 'int f() { /     g(x); q(1) / } /  @ 2:14'

[ "$failed" = 0 ] && echo "edit: all passed"
exit "$failed"
