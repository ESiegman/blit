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
# A diagram's picture as the one row "picture: diagram" (its name and its
# size come from the font and the colours), the rows it covers as one.
pictures() {
    awk '/^picture: diagram:/ { if (skip) print ""; print "picture: diagram"; skip = 1; next }
         skip && /^$/ { next }
         { if (skip) print ""; skip = 0; print }'
}

for f in sample edge diagrams code; do
    size=1600x1100
    [ $f = sample ] || [ $f = diagrams ] && size=3600x1100
    run $size "$here/markdown/$f.md" ""
    got=$(printf '%s\n' "$got" | pictures)
    check "$f.md drawn" "$(cat "$here/markdown/$f.txt")"
done

# What is wrong in a diagram's layout (diagramdump): an edge not drawn,
# two nodes over each other, a label over a node, an edge that does not end on its nodes or
# that passes through another node (or a subgraph neither end is in, or
# under a subgraph's title), a subgraph partly over another; a line each,
# nothing when all is well.
layout_problems() {
    awk '
    function flush(   i, j, e, k, q, p, xy) {
        if (want != "" && ne != want) print d ": " want " edges, " ne " drawn"
        want = ""
        for (i = 1; i <= nb; i++) for (j = i + 1; j <= nb; j++) {
            if (bk[i] == "label" && bk[j] == "label") continue
            if (bk[i] == "cluster" || bk[j] == "cluster") {
                if ((bk[i] == "cluster" && bk[j] == "cluster") && !apart(i, j) && !within(i, j) && !within(j, i)) print d ": subgraphs " bn[i] " and " bn[j] " overlap"
                continue
            }
            if (!apart(i, j)) print d ": " bk[i] " " bn[i] " and " bk[j] " " bn[j] " overlap"
        }
        for (e = 1; e <= ne; e++) {
            k = split(ep[e], p, " ")
            if (!on(ef[e], p[1]) || !on(et[e], p[k])) print d ": edge " ef[e] " to " et[e] " does not end on its nodes"
            if (ef[e] == et[e]) continue
            for (q = 2; q < k; q++) {
                split(p[q], xy, ",")
                for (i = 1; i <= nb; i++)
                    if (bk[i] == "node" && bn[i] != ef[e] && bn[i] != et[e] && xy[1] > bx[i] + 2 && xy[1] < bx[i] + bw[i] - 2 && xy[2] > by[i] + 2 && xy[2] < by[i] + bh[i] - 2) {
                        print d ": edge " ef[e] " to " et[e] " passes through " bn[i]
                        q = k
                        break
                    }
            }
            # (nor under the title of a subgraph, which hid it)
            for (i = 1; i <= nb; i++) {
                if (bk[i] != "title") continue
                for (q = 2; q < k; q++) {
                    split(p[q], xy, ",")
                    if (xy[1] > bx[i] + 1 && xy[1] < bx[i] + bw[i] - 1 && xy[2] > by[i] + 1 && xy[2] < by[i] + bh[i] - 1) {
                        print d ": edge " ef[e] " to " et[e] " passes under the title of " bn[i]
                        break
                    }
                }
            }
            # (and through no subgraph it has nothing to do with)
            for (i = 1; i <= nb; i++) {
                if (bk[i] != "cluster" || bn[i] == ef[e] || bn[i] == et[e] || holds(i, ef[e]) || holds(i, et[e])) continue
                for (q = 2; q < k; q++) {
                    split(p[q], xy, ",")
                    if (xy[1] > bx[i] + 2 && xy[1] < bx[i] + bw[i] - 2 && xy[2] > by[i] + 2 && xy[2] < by[i] + bh[i] - 2) {
                        print d ": edge " ef[e] " to " et[e] " passes through subgraph " bn[i]
                        break
                    }
                }
            }
        }
        nb = ne = 0
    }
    function apart(i, j) { return bx[i] + bw[i] <= bx[j] || bx[j] + bw[j] <= bx[i] || by[i] + bh[i] <= by[j] || by[j] + bh[j] <= by[i] }
    function holds(o, name,   j) {
        for (j = 1; j <= nb; j++) if (bk[j] != "label" && bk[j] != "title" && bn[j] == name) return within(o, j)
        return 0
    }
    function within(o, i) { return bx[i] >= bx[o] && by[i] >= by[o] && bx[i] + bw[i] <= bx[o] + bw[o] && by[i] + bh[i] <= by[o] + bh[o] }
    function on(name, pt,   xy, i) {
        split(pt, xy, ",")
        for (i = 1; i <= nb; i++)
            if (bk[i] != "label" && bk[i] != "title" && bn[i] == name) return xy[1] >= bx[i] - 1 && xy[1] <= bx[i] + bw[i] + 1 && xy[2] >= by[i] - 1 && xy[2] <= by[i] + bh[i] + 1
        return 0
    }
    /^diagram / { flush(); d = $1 " " $2; next }
    /^edges / { want = $2; next }
    /^(node|cluster|label|title) / { nb++; bk[nb] = $1; bn[nb] = $2; bx[nb] = $(NF - 3); by[nb] = $(NF - 2); bw[nb] = $(NF - 1); bh[nb] = $NF; next }
    /^edge / { ne++; ef[ne] = $2; et[ne] = $3; ep[ne] = ""; for (i = 4; i <= NF; i++) ep[ne] = ep[ne] " " $i; next }
    END { flush() }'
}
# dump SIZE FILE SCRIPT: the preview's diagrams as laid out.
dump() {
    got=$("$blit" --offscreen "$1" --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;$3;diagramdump" "$2" 2>&1 >/dev/null | grep -v '^\[')
}

# Flowcharts and state diagrams are pictures, one for each; their layouts
# are clean, wide and narrow; a mistake shows the code under what it is.
for f in diagrams layout; do
    for size in 3600x1100 1200x1100; do
        dump $size "$here/markdown/$f.md" ""
        layout=$got
        got=$(printf '%s\n' "$layout" | layout_problems)
        check "$f.md at $size: laid out with nothing over anything" ""
        laid=$(printf '%s\n' "$layout" | grep -c '^diagram [0-9]* [0-9]*x[0-9]*$')
        run $size "$here/markdown/$f.md" ""
        got="$laid $(printf '%s\n' "$got" | grep -c '^picture: diagram:')"
        [ $f = diagrams ] && want="2 2" || want="10 10"
        check "$f.md at $size: a picture for each diagram laid out" "$want"
    done
done

# The same pixels on every system: the diagrams' code calls none of the
# maths library's functions whose last bit may differ from one system's to
# another's (hypot, sin, pow...; sqrt is exact everywhere, and the
# rasterizer has a sine and cosine of its own).
got=$(grep -nE 'std::(hypot|sin|cos|tan|asin|acos|atan2?|pow|exp|log|log2|log10|cbrt|erf)\(|[^_a-z](hypotf?|sinf|cosf|atan2f?|powf|expf|logf)\(' \
    "$here/../src/diagram.cpp" "$here/../src/flowchart.cpp" "$here/../src/raster.cpp" | grep -v raster_sincos)
check "diagrams: no maths whose results differ between systems" ""

# Typing elsewhere in the file lays no diagram out again (the worker keeps
# them); typing in one lays out only that one.
cp "$here/markdown/layout.md" "$dir/typed.md"
dump 3600x1100 "$dir/typed.md" "mdwait;key:ctrl+Home;text:# Typed;key:Return;frame;mdwait;text:more;frame;mdwait"
got=$(printf '%s\n' "$got" | head -1)
check "typing elsewhere: each diagram laid out once" "laid out 10"
dump 3600x1100 "$dir/typed.md" "mdwait;key:Down;key:Down;key:Down;key:Down;key:Down;key:Down;key:Down;key:End;text: & a1;frame;mdwait"
got=$(printf '%s\n' "$got" | head -1)
check "typing in the first diagram: that one laid out again" "laid out 11"
# Each version of a diagram typed in is a picture of its own: those no
# preview shows any more are let go of at once (they held megabytes on the
# GPU each, for a minute).
dump 3600x1100 "$dir/typed.md" "mdwait;key:Down;key:Down;key:Down;key:Down;key:Down;key:Down;key:Down;key:End;text: & a1;frame;mdwait;text: & a2;frame;mdwait;text: & a3;frame;mdwait"
got=$(printf '%s\n' "$got" | grep '^kept ')
check "typing in a diagram: the pictures of its old versions let go of" "kept 10"

# A flowchart too wide across for the view is drawn down instead: wide, its
# nodes in a row; narrow, one under another.
for size in 3600x1100 1600x1100; do
    dump $size "$here/markdown/diagrams.md" ""
    got=$(printf '%s\n' "$got" | awk '/^diagram 2/ { exit } /^node / { printf "%.0f\n", $(NF - 2) + $NF / 2 }' | sort -u | wc -l | tr -d ' ')
    [ $size = 3600x1100 ] && want=1 || want=5
    check "a flowchart LR at $size: its nodes in $want rows" "$want"
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

# It follows the file: typed at the top, drawn at once.
cp "$here/markdown/edge.md" "$dir/live.md"
run 1600x1100 "$dir/live.md" "key:ctrl+Home;text:# Typed;key:Return;key:Return;frame"
got=$(printf '%s\n' "$got" | head -1)
check "what is typed in the file is in the preview" "Typed"

# Cmd+Shift+V: the preview in a split to the right, the keyboard left with the file; again, the same preview.
got=$("$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;keyof:Preview beside the file;frame;keyof:Preview beside the file;frame;dump" "$here/markdown/edge.md" 2>&1 >/dev/null | grep -v '^\[' | tail -1)
check "Cmd+Shift+V: beside it, once" "H(>[edge.md*] | [Preview: edge.md*])0.50"

# At most so many seconds: timeout(1) where there is one (GitHub's macOS 26
# machines have none), perl's alarm where there is not. (Killed if it is
# still there 5 seconds after being told to stop: busy drawing, it would not.)
limit() {
    if command -v timeout >/dev/null 2>&1; then timeout -k 5 "$@"; else perl -e 'alarm shift; exec @ARGV' "$@"; fi
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
awk 'BEGIN { print "```mermaid"; print "sequenceDiagram"; for (i = 0; i < 2000; i++) print "loop x" i; print "A->>B: hi"; for (i = 0; i < 2000; i++) print "end"; print "```" }' >"$dir/loops.md"
got=$(limit 20 "$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mddump" "$dir/loops.md" 2>&1 >/dev/null | grep -c 'loop x1999')
check "a sequence diagram's loops nested 2,000 deep: drawn, in time (each row once copied the whole picture)" "1"
awk 'BEGIN { for (i = 0; i < 20000; i++) printf "*a **b `c` [d](e) _f_ "; print "\n"
    for (i = 0; i < 100000; i++) printf "["; print "\n"
    for (i = 0; i < 100000; i++) printf "~~a "; print "\n"
    for (i = 0; i < 100000; i++) printf "<"; print ">\n"
    for (i = 0; i < 50000; i++) printf "["; for (i = 0; i < 50000; i++) printf "]"; print "\n"
    for (i = 0; i < 50000; i++) printf "[a]("; print "\n"; print "the end" }' >"$dir/marks.md"
got=$(limit 10 "$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mddump" "$dir/marks.md" 2>&1 >/dev/null | grep -c '^the end')
check "lines of half a megabyte full of marks that close nothing: drawn, in time (each mark once looked to the end of the line)" "1"
awk 'BEGIN { print "```mermaid"; print "flowchart TD"; for (i = 0; i < 800; i++) print "subgraph s" i; print "x --> y"; for (i = 0; i < 800; i++) print "end"; print "```" }' >"$dir/nested.md"
got=$(limit 20 "$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mdwait;mddump" "$dir/nested.md" 2>&1 >/dev/null | grep -c 'subgraphs nested too deeply')
check "subgraphs nested 800 deep: said to be too deep (laid out, they ran a Mac's preview thread out of stack)" "1"
awk 'BEGIN { print "```mermaid"; print "graph TD"; for (i = 0; i < 1399; i++) print "a" i " --> a" i + 1; for (i = 0; i < 1500; i++) print "a0 --> a" 1399 - i % 50; print "```" }' >"$dir/long.md"
got=$(limit 20 "$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mdwait;mddump" "$dir/long.md" 2>&1 >/dev/null | grep -c 'too large to draw')
check "edges across thousands of ranks: too large to draw, said at once (they took seconds and gigabytes)" "1"
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
if "$blit" --offscreen 800x600 --info 2>/dev/null | grep -q '^pictures    none'; then
    echo "web pictures skipped: blit reads no pictures on this system yet"
elif command -v python3 >/dev/null 2>&1; then
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
