#!/bin/sh
# The Markdown preview: what it draws of tests/markdown/*.md (compared with
# the .txt beside each: rows as text), Mermaid diagrams in it, the preview
# following the file as it is typed in, and Cmd+Shift+V.
#
#   tests/markdown.sh path/to/blit            run the cases
#   tests/markdown.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/markdown.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd -P)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
export BLIT_RECENT_FILE="$dir/recent"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua"

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n%s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n' "$1"
        printf '%s\n' "$2" >"$dir/want"
        printf '%s\n' "$got" >"$dir/got"
        diff "$dir/want" "$dir/got" | head -20
        failed=1
    fi
}
# run SIZE FILE SCRIPT: what the preview shows, a row a line.
run() {
    got=$("$blit" --offscreen "$1" --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;$3;mddump" "$2" 2>&1 >/dev/null | grep -v '^\[')
}

for f in sample edge diagrams; do
    size=1600x1100
    [ $f = sample ] || [ $f = diagrams ] && size=3600x1100
    run $size "$here/markdown/$f.md" ""
    check "$f.md drawn" "$(cat "$here/markdown/$f.txt")"
done

# A flowchart too wide across for the view is drawn down instead.
run 1600x1100 "$here/markdown/diagrams.md" ""
got=$(printf '%s\n' "$got" | grep -c 'drawn top to bottom: too wide across')
check "a flowchart too wide across, drawn down" "1"

# It follows the file: typed at the top, drawn at once.
cp "$here/markdown/edge.md" "$dir/live.md"
run 1600x1100 "$dir/live.md" "key:ctrl+Home;text:# Typed;key:Return;key:Return;frame"
got=$(printf '%s\n' "$got" | head -1)
check "what is typed in the file is in the preview" "Typed"

# Cmd+Shift+V: the preview in a split to the right, the keyboard left with the file; again, the same preview.
got=$("$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;key:cmd+shift+v;frame;key:cmd+shift+v;frame;dump" "$here/markdown/edge.md" 2>&1 >/dev/null | grep -v '^\[' | tail -1)
check "Cmd+Shift+V: beside it, once" "H(>[edge.md*] | [Preview: edge.md*])0.50"

[ -n "$print" ] || [ $failed = 1 ] || echo "markdown ok"
exit $failed
