#!/bin/sh
# The Markdown preview: what it draws of tests/markdown/*.md (compared with
# the .txt beside each: rows as text), Mermaid diagrams in it, code blocks
# in colour, the preview following the file as it is typed in,
# Cmd+Shift+V, and pictures on the web (fetched only when the setting says).
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

for f in sample edge diagrams code; do
    size=1600x1100
    [ $f = sample ] || [ $f = diagrams ] && size=3600x1100
    run $size "$here/markdown/$f.md" ""
    check "$f.md drawn" "$(cat "$here/markdown/$f.txt")"
done

# Code blocks in colour: each run of a row in the colour it is drawn in,
# with the theme's keywords made red. A language with a grammar is coloured
# by it, by its name or another (c++, py, sh); one with none stays as it was,
# a run in one colour. (Only where the grammars are installed.)
if "$blit" --grammars 2>/dev/null | grep -Eq '^ *cpp +installed' && "$blit" --grammars 2>/dev/null | grep -Eq '^ *python +installed' &&
    "$blit" --grammars 2>/dev/null | grep -Eq '^ *bash +installed'; then
    { cat "$dir/init.lua"; printf 'blit.opt.theme.dark = "blit-dark"\nblit.opt.theme.light = "blit-dark"\nblit.opt.themes["blit-dark"] = { syntax = { keyword = "#ff0000" } }\n'; } >"$dir/colours.lua"
    inks=$(BLIT_INIT="$dir/colours.lua" "$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mdinks" "$here/markdown/code.md" 2>&1 >/dev/null)
    got=$(printf '%s\n' "$inks" | grep -c 'main.*\[#ff0000\]return'),$(printf '%s\n' "$inks" | grep -c '\[#ff0000\]def'),$(printf '%s\n' "$inks" | grep -c '\[#ff0000\]for.*\[#ff0000\]done')
    got=$got,$(printf '%s\n' "$inks" | grep 'int plain' | grep -o '\[#' | wc -l | tr -d ' '),$(printf '%s\n' "$inks" | grep 'no language' | grep -o '\[#' | wc -l | tr -d ' ')
    check "code blocks coloured by their language's grammar, in the theme's colours" "1,1,1,1,1"
else
    echo "code colours skipped: the cpp, python and bash grammars are not all installed"
fi

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

# At most so many seconds: timeout(1) where there is one (GitHub's macOS 26
# machines have none), perl's alarm where there is not.
limit() {
    if command -v timeout >/dev/null 2>&1; then timeout "$@"; else perl -e 'alarm shift; exec @ARGV' "$@"; fi
}

# Odd text: wide characters in a code block a few columns wide (it once
# drew rows forever), quotes nested thousands deep, a comment with blank
# lines in it, a pie's title on its first line, and a rule at the top that
# is not front matter.
printf '```\n\346\274\242\345\255\227\346\274\242\345\255\227\n```\n' >"$dir/wide.md"
got=$(limit 20 "$blit" --offscreen 420x600 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mddump" "$dir/wide.md" 2>&1 >/dev/null | grep -c '漢')
[ "$got" -ge 1 ] && got=drawn
check "wide characters in a very narrow preview" "drawn"
awk 'BEGIN { s = ""; for (i = 0; i < 5000; i++) s = s ">"; print s " deep" }' >"$dir/deep.md"
got=$(limit 20 "$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mddump" "$dir/deep.md" 2>&1 >/dev/null | grep -c '>')
[ "$got" -ge 1 ] && got=drawn
check "quotes nested 5,000 deep: drawn (the deepest as text), no crash" "drawn"
printf '<!--\nhidden\n\nstill hidden\n-->\nshown\n\n```mermaid\npie title Pets\n  "Dogs" : 3\n  "Cats" : 1\n```\n' >"$dir/odd.md"
run 1600x1100 "$dir/odd.md" ""
got=$(printf '%s\n' "$got" | grep -c 'hidden'),$(printf '%s\n' "$got" | grep -c 'shown'),$(printf '%s\n' "$got" | grep -c 'Pets')
check "a comment over blank lines, gone; a pie's title on its first line" "0,1,1"
printf -- '---\nA rule above this line\n\nsome text\n\n---\n\nmore\n' >"$dir/rule.md"
run 1600x1100 "$dir/rule.md" ""
got=$(printf '%s\n' "$got" | grep -c 'A rule above this line')
check "a rule at the top is not front matter" "1"

# Pictures on the web (markdown.web_pictures): their alt text, as a link,
# while the setting is off (as it starts); fetched when it is on (here from
# a server on this machine, never the network) and kept in blit's cache,
# and drawn from there once the server has gone.
if command -v python3 >/dev/null 2>&1; then
    mkdir -p "$dir/site" && cp "$(dirname "$here")/tests/images/quad.png" "$dir/site/"
    python3 -u -m http.server 0 --bind 127.0.0.1 --directory "$dir/site" >"$dir/server.log" 2>&1 &
    server=$!
    trap 'kill $server 2>/dev/null; rm -rf "$dir"' EXIT
    port= i=0
    while [ -z "$port" ] && [ $i -lt 100 ]; do
        port=$(sed -n 's/.*port \([0-9][0-9]*\).*/\1/p' "$dir/server.log" | head -1)
        [ -n "$port" ] || sleep 0.1
        i=$((i + 1))
    done
    printf '# Web\n\n![from the web](http://127.0.0.1:%s/quad.png)\n' "$port" >"$dir/web.md"
    { cat "$dir/init.lua"; printf 'blit.opt.markdown.web_pictures = true\n'; } >"$dir/web.lua"
    export BLIT_CACHE_DIR="$dir/cache"
    web() { run 1600x1100 "$dir/web.md" "imagewait;frame;mdwait"; got=$(printf '%s\n' "$got" | grep -v '^$' | tail -1 | sed -e "s/:$port\//:PORT\//"); }
    web
    check "a picture on the web, the setting off: its alt text" "▣ from the web"
    BLIT_INIT="$dir/web.lua"
    web
    check "the setting on: fetched, and drawn" "picture: http://127.0.0.1:PORT/quad.png 8x4"
    kill $server 2>/dev/null
    wait $server 2>/dev/null
    web
    got="$got, $(ls "$dir/cache/pictures" | wc -l | tr -d ' ') kept"
    check "the server gone: drawn from blit's cache" "picture: http://127.0.0.1:PORT/quad.png 8x4, 1 kept"
    BLIT_INIT="$dir/init.lua"
    unset BLIT_CACHE_DIR
else
    echo "web pictures skipped: no python3 to serve them"
fi

[ -n "$print" ] || [ $failed = 1 ] || echo "markdown ok"
exit $failed
