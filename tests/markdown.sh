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

for f in sample edge diagrams code alerts notes headings tables math html defs lists; do
    size=1600x1100
    [ $f = sample ] || [ $f = diagrams ] && size=3600x1100
    run $size "$here/markdown/$f.md" ""
    got=$(printf '%s\n' "$got" | pictures)
    check "$f.md drawn" "$(cat "$here/markdown/$f.txt")"
done

# inks FILE: each run of the preview's rows in its colour, in blit-dark
# whatever the system's appearance is.
{ cat "$dir/init.lua"; printf 'blit.opt.theme.dark = "blit-dark"\nblit.opt.theme.light = "blit-dark"\n'; } >"$dir/dark.lua"
inks() {
    got=$(BLIT_INIT="$dir/dark.lua" "$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mdwait;mdinks" "$1" 2>&1 >/dev/null)
}

# Alerts: each kind's bar and title in a colour of its own (the theme's
# blue, green, purple, yellow and its errors'), what it says in the text's
# colour (a quote's is quieter), one inside another in its own.
inks "$here/markdown/alerts.md"
got=$(printf '%s\n' "$got" | grep -E '\](Note|Tip|Important|Warning|Caution|Risks\.|A plain quote.*)$' | sed -e 's/ *$//' | tr '\n' ' ')
check "alerts: a bar and a title in each kind's colour" "[#61afef]▎ [#7f848e]A plain quote stays a quote. [#61afef]▌ [#61afef]Note [#98c379]▌ [#98c379]Tip [#c678dd]▌ [#c678dd]Important [#e5c07b]▌ [#e5c07b]Warning [#e06c75]▌ [#e06c75]Caution [#e06c75]▌ [#dbdedb]Risks. [#e06c75]▌ [#61afef]▌ [#61afef]Note [#61afef]• [#98c379]▌ [#98c379]Tip "

# Headings: every level its own. The built-in themes name one colour for
# all headings: the first level has it, the levels under it other colours
# of the theme's (the fourth to sixth one between them), bold, the fifth
# slanted too and the sixth only slanted; a heavy line in the first
# level's colour under it, a quiet light one under the second. In the
# light theme too. A level the theme names a colour for has that; a
# colour a level above has is not taken again.
# looks FILE [LUA]: each run with its colour and style (mdlooks), with LUA after the settings.
looks() {
    { cat "$dir/dark.lua"; printf '%s\n' "${2:-}"; } >"$dir/looks.lua"
    got=$(BLIT_INIT="$dir/looks.lua" "$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mdwait;mdlooks" "$1" 2>&1 >/dev/null)
}
levels() { printf '%s\n' "$got" | grep -E '\](One|Two|Three|Four|Five|Six)$' | tr '\n' ' '; }
looks "$here/markdown/headings.md"
all=$got
got=$(levels)
check "headings: each level its own colour or style" "[#e06c75 b]One [#61afef b]Two [#e5c07b b]Three [#c678dd b]Four [#c678dd bi]Five [#c678dd i]Six "
# (The colour and the line's first character: a pattern, not cut -c, which counts bytes or characters by the locale.)
got=$(printf '%s\n' "$all" | sed -n -E 's/^(\[#[0-9a-f]*\](━|─)).*/\1/p' | sort | uniq -c | tr -s ' ' | tr '\n' ',')
check "headings: a heavy line under the first level in its colour, a light quiet one under the second" " 4 [#7f848e]─, 3 [#e06c75]━,"
got=$(printf '%s\n' "$all" | grep -c '^\[#c678dd b\]Four with \[#c678dd bi\]emphasis\[#c678dd b\] and \[#98c379 b t1\]code$')
check "headings: emphasis and code in one" "1"
looks "$here/markdown/headings.md" 'blit.opt.theme.dark = "blit-light"; blit.opt.theme.light = "blit-light"'
got=$(levels)
check "headings: in the light theme" "[#ca3c2c b]One [#325cc0 b]Two [#a87b00 b]Three [#a626a4 b]Four [#a626a4 bi]Five [#a626a4 i]Six "
looks "$here/markdown/headings.md" 'blit.opt.themes["blit-dark"] = { syntax = { ["markup.heading"] = "#61afef", ["markup.heading.3"] = "#00ff00" } }'
got=$(levels)
check "headings: a level's own colour from the theme; one a level above has, not again" "[#61afef b]One [#e5c07b b]Two [#00ff00 b]Three [#c678dd b]Four [#c678dd bi]Five [#c678dd i]Six "

# Tables: their lines in the quiet muted colour (not an accent's), the
# header's tint one band from the first line to the last (the lines
# between its cells tinted too, the outer ones not), body rows plain.
looks "$here/markdown/tables.md"
all=$got
got=$(printf '%s\n' "$all" | grep -c '^\[#7f848e\]│\[#dbdedb t2\] \[#dbdedb b t2\]Name\[#dbdedb t2\]      \[#7f848e t2\]│\[#dbdedb t2\] \[#dbdedb b t2\]Size\[#dbdedb t2\] \[#7f848e t2\]│\[#dbdedb t2\]  \[#dbdedb b t2\]Kind\[#dbdedb t2\]  \[#7f848e\]│$')
got=$got,$(printf '%s\n' "$all" | grep -E '^\[#[0-9a-f]*\](┌|├|└)' | grep -vc '^\[#7f848e\]'),$(printf '%s\n' "$all" | grep '│' | grep -c 't2.*main.cpp')
check "tables: quiet lines, the header's tint one band" "1,0,0"

# Math: inline in the text's colour, slanted; display math slanted on a
# row of its own, in the middle (in a quote, in the quote's colour).
looks "$here/markdown/math.md"
all=$got
got=$(printf '%s\n' "$all" | grep -c '^\[#dbdedb\]Euler.s identity \[#dbdedb i\]e^(iπ) + 1 = 0\[#dbdedb\] links five$')
got=$got,$(printf '%s\n' "$all" | grep -c '^\[#dbdedb\]  *\[#dbdedb i\]∑₁^∞ 1/k² = π²/6$'),$(printf '%s\n' "$all" | grep -c '▎ \[#dbdedb\]  *\[#7f848e i\]E = mc²$')
check "math: slanted, display math in the middle" "1,1,1"

# Inline HTML: <kbd> a key on the code's tone, <mark> and ==text== on a
# highlighter's, <u> underlined, <s> struck and quiet, <sup> raised, <a
# href> a link; <details> open, its summary after a marker, what it holds
# indented; <h1 align="center"> a heading in the middle.
looks "$here/markdown/html.md"
all=$got
got=$(printf '%s\n' "$all" | grep -c '^\[#dbdedb\]Press \[#dbdedb t1\][^[]*Cmd[^[]*\[#dbdedb\]+\[#dbdedb t1\][^[]*Shift')
got=$got,$(printf '%s\n' "$all" | grep -c '\[#dbdedb\] and \[#dbdedb t5\]highlighted too\[#dbdedb\], but a == b is not\.$')
got=$got,$(printf '%s\n' "$all" | grep -c '\[#dbdedb u\]Underlined\[#dbdedb\], \[#dbdedb u\]inserted\[#dbdedb\], \[#7f848e s\]struck')
got=$got,$(printf '%s\n' "$all" | grep -c '\[#61afef >https://example.com/docs\]link')
got=$got,$(printf '%s\n' "$all" | grep -c '^\[#61afef\]▾ \[#dbdedb b\]More about it$'),$(printf '%s\n' "$all" | grep -c '^\[#dbdedb\]  \[#dbdedb\]What it holds')
got=$got,$(printf '%s\n' "$all" | grep -c '^\[#dbdedb\]  *\[#e06c75 b\]blit$')
check "inline HTML: keys, highlights, underlines, links, details, a heading in the middle" "1,1,1,1,1,1,1"

# Definition lists: the term bold, its definitions indented under it,
# numbered quietly when there are more than one.
looks "$here/markdown/defs.md"
all=$got
got=$(printf '%s\n' "$all" | grep -c '^\[#dbdedb b\]Apple$'),$(printf '%s\n' "$all" | grep -c '^\[#7f848e\] 1\. \[#dbdedb\]A fruit'),$(printf '%s\n' "$all" | grep -c '^\[#7f848e\] 2\. \[#dbdedb\]A company')
got=$got,$(printf '%s\n' "$all" | grep -c '^\[#dbdedb b\]Orange$'),$(printf '%s\n' "$all" | grep -c '^\[#dbdedb\]    \[#dbdedb\]A citrus fruit\.$')
check "definition lists: terms bold, definitions under them" "1,1,1,1,1"

# Task lists: an open task's square quiet, a done one's crossed square in
# the accent and its text quiet; in the light theme too.
looks "$here/markdown/lists.md"
got=$(printf '%s\n' "$got" | sed -n 3,4p | tr '\n' ,)
check "task lists: their squares in both themes (dark)" "[#7f848e]□ [#dbdedb]to do,[#61afef]⊠ [#7f848e]done,"
looks "$here/markdown/lists.md" 'blit.opt.theme.dark = "blit-light"; blit.opt.theme.light = "blit-light"'
got=$(printf '%s\n' "$got" | sed -n 3,4p | tr '\n' ,)
check "task lists: their squares in both themes (light)" "[#75777e]□ [#1a1a1c]to do,[#325cc0]⊠ [#75777e]done,"

# Footnotes: a mark is a link to its text at the end, the row that text
# starts on is named for it, and the mark after the text is a link back;
# one nothing refers to is not drawn (as on GitHub). A click on a mark puts its text
# on show (it was not), a click on the mark after the text puts the row
# that first referred to it at the top.
looks=$(BLIT_INIT="$dir/dark.lua" "$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mdwait;mdlooks" "$here/markdown/notes.md" 2>&1 >/dev/null)
got=$(printf '%s\n' "$looks" | grep -c 'footnote\[#61afef >#fn-1\]¹\[#dbdedb\]\. A'),$(printf '%s\n' "$looks" | grep -c '^#fn-1 \[#61afef\]1\. \[#dbdedb\]My reference'),$(printf '%s\n' "$looks" | grep -c '\[#61afef >#fnref-1\]↩$')
got=$got,$(printf '%s\n' "$looks" | grep -c 'Never referred to'),$(printf '%s\n' "$looks" | grep -c 'fn-5')
check "footnotes: marks that go to their text, text that goes back" "1,1,1,0,0"
{
    printf 'First[^a] words.\n\n'
    i=0
    while [ $i -lt 80 ]; do
        [ $i = 40 ] && printf 'Second[^b] words.\n\n'
        printf 'Line %s.\n\n' $i
        i=$((i + 1))
    done
    printf '[^b]: The second.\n\n[^a]: The first.\n'
} >"$dir/notes.md"
got=$("$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mdwait;frame;mdtop;mdclick:#fnref-2;mdclick:#fn-1;frame;mdtop;mdclick:#fnref-2;frame;mdtop;mdclick:#fn-2;frame;mdtop;mdclick:#fnref-1;frame;mdtop" "$dir/notes.md" 2>&1 >/dev/null | grep -v '^\[' | sed -e 's/^top 1[0-9][0-9]: Line.*/down at its text/' | tr '\n' '|')
check "a click on a footnote's mark shows its text; on the mark after the text, the row that referred to it" "top 0: First¹ words.|mdclick: no such link on show|down at its text|top 82: Second² words.|down at its text|top 0: First¹ words.|"

# What is wrong in a diagram's layout (diagramdump): an edge not drawn,
# two nodes over each other, a label over a node, how many at a relation's
# end (a label named A:word) over another label or with a line through it,
# an edge that does not end on its nodes or that passes through another
# node (or a subgraph neither end is in, or under a subgraph's title), a
# subgraph partly over another; a line each, nothing when all is well.
layout_problems() {
    awk '
    function word(i) { return bk[i] == "label" && index(bn[i], ":") && !index(bn[i], "->") }
    function flush(   i, j, e, k, q, p, xy, a, f) {
        if (want != "" && ne != want) print d ": " want " edges, " ne " drawn"
        want = ""
        for (i = 1; i <= nb; i++) for (j = i + 1; j <= nb; j++) {
            if (bk[i] == "label" && bk[j] == "label" && !word(i) && !word(j)) continue
            if (bk[i] == "cluster" || bk[j] == "cluster") {
                if ((bk[i] == "cluster" && bk[j] == "cluster") && !apart(i, j) && !within(i, j) && !within(j, i)) print d ": subgraphs " bn[i] " and " bn[j] " overlap"
                continue
            }
            if (!apart(i, j)) print d ": " bk[i] " " bn[i] " and " bk[j] " " bn[j] " overlap"
        }
        for (e = 1; e <= ne; e++) {
            k = split(ep[e], p, " ")
            for (i = 1; i <= nb; i++) {
                if (!word(i)) continue
                for (q = 1; q < k; q++) {
                    split(p[q], a, ","); split(p[q + 1], xy, ",")
                    for (f = 0; f <= 1; f += 0.125)
                        if (a[1] + (xy[1] - a[1]) * f > bx[i] + 1 && a[1] + (xy[1] - a[1]) * f < bx[i] + bw[i] - 1 && a[2] + (xy[2] - a[2]) * f > by[i] + 1 && a[2] + (xy[2] - a[2]) * f < by[i] + bh[i] - 1) {
                            print d ": edge " ef[e] " to " et[e] " passes through " bn[i]
                            q = k
                            break
                        }
                }
            }
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
    # (A box flush with a side of its subgraph, as the periods of a timeline
    # are with their section, is in it: the dump rounds to a tenth.)
    function holds(o, name,   j) {
        for (j = 1; j <= nb; j++)
            if (bk[j] != "label" && bk[j] != "title" && bn[j] == name)
                return bx[j] >= bx[o] - 0.2 && by[j] >= by[o] - 0.2 && bx[j] + bw[j] <= bx[o] + bw[o] + 0.2 && by[j] + bh[j] <= by[o] + bh[o] + 0.2
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

# Flowcharts, state diagrams, sequence diagrams and pies are pictures, one
# for each; their layouts are clean, wide and narrow (in a sequence
# diagram, each participant's lifeline is a "cluster" its messages end on);
# a mistake shows the code under what it is.
for f in diagrams layout sequence; do
    for size in 3600x1100 1200x1100; do
        dump $size "$here/markdown/$f.md" ""
        layout=$got
        got=$(printf '%s\n' "$layout" | layout_problems)
        check "$f.md at $size: laid out with nothing over anything" ""
        laid=$(printf '%s\n' "$layout" | grep -c '^diagram [0-9]* [0-9]*x[0-9]*$')
        run $size "$here/markdown/$f.md" ""
        got="$laid $(printf '%s\n' "$got" | grep -c '^picture: diagram:')"
        case $f in diagrams) want="5 5" ;; layout) want="10 10" ;; *) want="6 6" ;; esac
        check "$f.md at $size: a picture for each diagram laid out" "$want"
    done
done

# Class diagrams and entity relationship diagrams too: clean, wide and
# narrow (how many at a relation's end, as labels, clear of every box);
# each relation from its first class (or entity) to its second; a
# picture for each.
for size in 3600x1100 1200x1100; do
    dump $size "$here/markdown/classes.md" ""
    layout=$got
    got=$(printf '%s\n' "$layout" | layout_problems)
    check "classes.md at $size: laid out with nothing over anything" ""
    got=$(printf '%s\n' "$layout" | awk '/^diagram / { d = $2 } /^edge / { n[d]++ } /^label [^ ]*:/ && !/^label [^ ]*->/ { c[d]++ } END { for (i = 1; i <= 7; i++) printf "%d:%d,%d ", i, n[i], c[i] }')
    check "classes.md at $size: relations and their ends' words" "1:3,0 2:8,0 3:7,6 4:4,0 5:4,0 6:4,0 7:15,14 "
    got=$(printf '%s\n' "$layout" | grep -E '^edge (Customer Ticket|CUSTOMER ORDER|p a|Square Drawable) ' | cut -d' ' -f1-3 | tr '\n' ,)
    check "classes.md at $size: relations from the first named to the second" "edge Customer Ticket,edge Square Drawable,edge CUSTOMER ORDER,edge p a,"
    laid=$(printf '%s\n' "$layout" | grep -c '^diagram [0-9]* [0-9]*x[0-9]*$')
    run $size "$here/markdown/classes.md" ""
    got="$laid $(printf '%s\n' "$got" | grep -c '^picture: diagram:')"
    check "classes.md at $size: a picture for each diagram laid out" "7 7"
done
# The same pixels on every system: the diagrams' code calls none of the
# maths library's functions whose last bit may differ from one system's to
# another's (hypot, sin, pow...; sqrt is exact everywhere, and the
# rasterizer has a sine and cosine of its own).
got=$(grep -nE 'std::(hypot|sin|cos|tan|asin|acos|atan2?|pow|exp|log|log2|log10|cbrt|erf)\(|[^_a-z](hypotf?|sinf|cosf|atan2f?|powf|expf|logf)\(' \
    "$here/../src/diagram.cpp" "$here/../src/flowchart.cpp" "$here/../src/raster.cpp" \
    "$here/../src/classdiagram.cpp" "$here/../src/gantt.cpp" "$here/../src/mindmap.cpp" \
    "$here/../src/sequence.cpp" "$here/../src/pie.cpp" | grep -v raster_sincos)
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
# Frames that draw more than those before (the preview's pictures coming
# in, a diagram typed in made again) are drawn, and the shot taken: the GPU
# reads only memory that is there. (Vulkan: a frame's uniforms, made larger,
# came back with the old buffer's handle, so its descriptor set was not
# written again and pointed at the freed memory, and the AMD card faulted
# and lost the device, Nov 2026.) Where Vulkan's validation layers are
# installed, the same under them, which find nothing to say.
grown="frame;mdpreview;frame;mdwait;key:Down;key:Down;key:Down;key:Down;key:Down;key:Down;key:Down;key:End;text: & a1;frame;mdwait;text: & a2;frame;mdwait;text: & a3;frame;mdwait"
cp "$here/markdown/layout.md" "$dir/grown.md"
rm -f "$dir/grown.bmp"
"$blit" --offscreen 3600x1100 --shot "$dir/grown.bmp" --script "$grown" "$dir/grown.md" >/dev/null 2>"$dir/err"
got="$? $([ -s "$dir/grown.bmp" ] && echo shot) $(grep -ci 'gpuvm\|context is lost\|device lost\|blit: vulkan' "$dir/err")"
check "frames drawing more and more pictures: all drawn, the GPU never lost" "0 shot 0"
if [ "$(uname -s)" != Darwin ] && ls /usr/share/vulkan/explicit_layer.d/*khronos_validation* /etc/vulkan/explicit_layer.d/*khronos_validation* >/dev/null 2>&1; then
    cp "$here/markdown/layout.md" "$dir/grown.md"
    "$blit" --debug-gpu --offscreen 3600x1100 --shot "$dir/grown.bmp" --script "$grown" "$dir/grown.md" >/dev/null 2>"$dir/err"
    got="$? $(grep -c '^blit: vulkan' "$dir/err")"
    check "the same under Vulkan's validation layers: nothing said" "0 0"
fi

# A flowchart too wide across for the view is drawn down instead: wide, its
# nodes in a row; narrow, one under another.
for size in 3600x1100 1600x1100; do
    dump $size "$here/markdown/diagrams.md" ""
    got=$(printf '%s\n' "$got" | awk '/^diagram 2/ { exit } /^node / { printf "%.0f\n", $(NF - 2) + $NF / 2 }' | sort -u | wc -l | tr -d ' ')
    [ $size = 3600x1100 ] && want=1 || want=5
    check "a flowchart LR at $size: its nodes in $want rows" "$want"
done

# Gantt charts, mindmaps and timelines are pictures too, laid out cleanly
# wide and narrow: no bars, nodes, labels or sections over each other, a
# mindmap's branches and a timeline's joins ending on their nodes and
# passing through no other; a mistake (a task after one there is not)
# shows the code under its line.
for size in 3600x1100 1200x1100; do
    dump $size "$here/markdown/plans.md" ""
    layout=$got
    got=$(printf '%s\n' "$layout" | layout_problems)
    check "plans.md at $size: laid out with nothing over anything" ""
    got=$(printf '%s\n' "$layout" | grep -c '^diagram [0-9]* [0-9]*x[0-9]*$'),$(printf '%s\n' "$layout" | grep -c '^diagram 7 error: line 4: no task nowhere to come after$')
    got=$got,$(printf '%s\n' "$layout" | grep -c '^edge '),$(printf '%s\n' "$layout" | grep -c '^node ')
    check "plans.md at $size: six laid out (with their branches and joins), one mistake" "6,1,31,63"
    run $size "$here/markdown/plans.md" ""
    got="$(printf '%s\n' "$got" | grep -c '^picture: diagram:'),$(printf '%s\n' "$got" | grep -c 'mermaid: line 4: no task nowhere')"
    check "plans.md at $size: a picture for each, the mistake's code under it" "6,1"
done

# A Gantt chart that marks today is laid out again when the day changes
# (and not before: its marker is at the day's midnight), at that day; one
# that does not is kept. (16081 is 11 January 2014.)
printf '```mermaid\ngantt\n  dateFormat YYYY-MM-DD\n  todayMarker stroke:#0f0\n  A :a, 2014-01-01, 30d\n  B :b, 2014-01-11, 2d\n```\n\n```mermaid\ngantt\n  A :c, 2014-01-01, 30d\n```\n' >"$dir/today.md"
dump 3600x1100 "$dir/today.md" "mdwait;mdtoday:16080;key:ctrl+Home;text:x;key:Return;key:Left;frame;mdwait;text:y;frame;mdwait;mdtoday:16081;text:z;frame;mdwait"
got=$(printf '%s\n' "$got" | awk '/^laid out/ { n = $3 } /^node b / { b = $3 } /^today / { t = $3 } END { print n, (b != "" && b == t) }')
check "a chart marking today: laid out again the next day, its marker at that day" "4 1"

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

# Fenced diffs and patches: added lines in the Git views' colour for
# them, removed ones in the errors', a hunk's header in the accent, a
# file's header and git's notes quiet, the rest as code is.
inks "$here/markdown/code.md"
got=$(printf '%s\n' "$got" | sed -n '/diff --git/,/hello, world/p' | sed -e 's/^\[#dbdedb\] //' | cut -c1-12 | tr '\n' ',')
check "diffs: added, removed, hunks and headers in their colours" "[#7f848e]dif,[#7f848e]ind,[#7f848e]---,[#7f848e]+++,[#61afef]@@ ,[#dbdedb]   ,[#e06c75]-  ,[#98c379]+  ,[#98c379]+  ,[#dbdedb]   ,[#7f848e]\\ N,,,[#7f848e]   ,[#61afef]@@ ,[#e06c75]-he,[#98c379]+he,"

# It follows the file: typed at the top, drawn at once.
cp "$here/markdown/edge.md" "$dir/live.md"
run 1600x1100 "$dir/live.md" "key:ctrl+Home;text:# Typed;key:Return;key:Return;frame"
got=$(printf '%s\n' "$got" | head -1)
check "what is typed in the file is in the preview" "Typed"

# Cmd+Shift+V: the preview in a split to the right, the keyboard left with the file; again, the same preview.
got=$("$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;keyof:Preview beside the file;frame;keyof:Preview beside the file;frame;dump" "$here/markdown/edge.md" 2>&1 >/dev/null | grep -v '^\[' | tail -1)
check "Cmd+Shift+V: beside it, once" "H(>[edge.md*] | [Preview: edge.md*])0.50"
# On a file that is not Markdown (a tool's report), it refuses on the
# bottom line rather than drawing the report as Markdown.
printf 'Copyright 1986-2025 Xilinx, Inc. All Rights Reserved.\n# Utilization\n| Site Type | Used |\n' >"$dir/synth.rpt"
got=$("$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;keyof:Preview beside the file;frame;dump" "$dir/synth.rpt" 2>&1 >/dev/null | grep -v '^\[' | tr '\n' ' ')
check "Cmd+Shift+V on a file that is not Markdown: refused" "blit: No preview: synth.rpt is not Markdown, a notebook or an SVG >[synth.rpt*] "

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
got=$(limit 20 "$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mdwait;mddump" "$dir/loops.md" 2>&1 >/dev/null | grep -c '^picture: diagram:')
check "a sequence diagram's loops nested 2,000 deep: drawn, in time" "1"
awk 'BEGIN { for (i = 0; i < 20000; i++) printf "*a **b `c` [d](e) _f_ "; print "\n"
    for (i = 0; i < 100000; i++) printf "["; print "\n"
    for (i = 0; i < 100000; i++) printf "~~a "; print "\n"
    for (i = 0; i < 100000; i++) printf "<"; print ">\n"
    for (i = 0; i < 50000; i++) printf "["; for (i = 0; i < 50000; i++) printf "]"; print "\n"
    for (i = 0; i < 50000; i++) printf "[a]("; print "\n"; print "the end" }' >"$dir/marks.md"
got=$(limit 10 "$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mddump" "$dir/marks.md" 2>&1 >/dev/null | grep -c '^the end')
check "lines of half a megabyte full of marks that close nothing: drawn, in time (each mark once looked to the end of the line)" "1"
# What the preview has learnt to draw since, by the hundred thousand too:
# footnotes' marks (of one that is there, and that close nothing), and
# footnotes that each refer to the next.
awk 'BEGIN { for (i = 0; i < 100000; i++) printf "[^a] "; print "\n"
    for (i = 0; i < 100000; i++) printf "[^"; print "\n"
    for (i = 0; i < 20000; i++) print "[^n" i "]: to [^n" i + 1 "] and [^a]"
    print "\n[^a]: the end" }' >"$dir/marks.md"
got=$(limit 10 "$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mddump" "$dir/marks.md" 2>&1 >/dev/null | grep -c '1\. the end ↩$')
check "footnotes' marks by the hundred thousand, and footnotes that refer to each other: drawn, in time" "1"
# Inline HTML by the hundred thousand: tags that close nothing (each
# looked to the end of the line for its closing tag), comments that never
# end, == that close nothing, and tags nested that deep.
awk 'BEGIN { for (i = 0; i < 100000; i++) printf "<kbd>a "; print "\n"
    for (i = 0; i < 30000; i++) printf "<sup>a <b>b <a href=x>c "; print "\n"
    printf "a "; for (i = 0; i < 100000; i++) printf "<!-- a "; print "\n"
    for (i = 0; i < 100000; i++) printf "==a "; print "\n"
    for (i = 0; i < 50000; i++) printf "<b>"; printf "x"; for (i = 0; i < 50000; i++) printf "</b>"; print "\n"
    for (i = 0; i < 20000; i++) print "<details><summary>s</summary>"; print "\n"; print "the end" }' >"$dir/marks.md"
got=$(limit 10 "$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mddump" "$dir/marks.md" 2>&1 >/dev/null | grep -c '^ *the end')
check "inline HTML's tags by the hundred thousand, comments that never end, nested that deep: drawn, in time" "1"
# Definitions by the hundred thousand, under one term and each under its own.
awk 'BEGIN { print "term"; for (i = 0; i < 100000; i++) print ": d" i; print ""
    for (i = 0; i < 50000; i++) { print "t" i; print ": d" }; print ""; print "the end" }' >"$dir/marks.md"
got=$(limit 10 "$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mddump" "$dir/marks.md" 2>&1 >/dev/null | grep -c '^the end')
check "definitions by the hundred thousand: drawn, in time" "1"
# Math by the hundred thousand: $ that close nothing (each looked to the
# end of the line for its close), $$ and \( the same, $` with no `$, math
# that opens a hundred thousand braces and closes none, and math nested
# a hundred thousand deep.
awk 'BEGIN { for (i = 0; i < 100000; i++) printf "$a "; print "\n"
    for (i = 0; i < 100000; i++) printf "$$a "; print "\n"
    for (i = 0; i < 100000; i++) printf "\\(a "; print "\n"
    for (i = 0; i < 100000; i++) printf "$`a "; print "\n"
    printf "$"; for (i = 0; i < 100000; i++) printf "x^{"; print "$\n"
    printf "$"; for (i = 0; i < 100000; i++) printf "\\sqrt{"; printf "x"; for (i = 0; i < 100000; i++) printf "}"; print "$\n"
    print "$$"; for (i = 0; i < 50000; i++) print "\\frac{a}{b} \\\\"; print "$$\n"; print "the end" }' >"$dir/marks.md"
got=$(limit 10 "$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mddump" "$dir/marks.md" 2>&1 >/dev/null | grep -c '^the end')
check "math's marks by the hundred thousand, braces that never close and nested that deep: drawn, in time" "1"
awk 'BEGIN { print "```mermaid"; print "flowchart TD"; for (i = 0; i < 800; i++) print "subgraph s" i; print "x --> y"; for (i = 0; i < 800; i++) print "end"; print "```" }' >"$dir/nested.md"
got=$(limit 20 "$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mdwait;mddump" "$dir/nested.md" 2>&1 >/dev/null | grep -c 'subgraphs nested too deeply')
check "subgraphs nested 800 deep: said to be too deep (laid out, they ran a Mac's preview thread out of stack)" "1"
awk 'BEGIN { print "```mermaid"; print "graph TD"; for (i = 0; i < 1399; i++) print "a" i " --> a" i + 1; for (i = 0; i < 1500; i++) print "a0 --> a" 1399 - i % 50; print "```" }' >"$dir/long.md"
got=$(limit 20 "$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mdwait;mddump" "$dir/long.md" 2>&1 >/dev/null | grep -c 'too large to draw')
check "edges across thousands of ranks: too large to draw, said at once (they took seconds and gigabytes)" "1"
# Subgraphs, namespaces and composite states nested hundreds deep (each
# level laid out by recursion, on the worker: it ran out of stack): a
# mistake, said with its line.
awk 'BEGIN { print "```mermaid"; print "flowchart TD"; for (i = 0; i < 500; i++) print "subgraph s" i; print "a --> b"; for (i = 0; i < 500; i++) print "end"; print "```"
             print "```mermaid"; print "classDiagram"; for (i = 0; i < 500; i++) print "namespace n" i " {"; print "class A"; for (i = 0; i < 500; i++) print "}"; print "```"
             print "```mermaid"; print "stateDiagram-v2"; for (i = 0; i < 300; i++) { print "state s" i " {"; print "state s" i + 1 " {"; print "}"; print "}" } print "```" }' >"$dir/nested.md"
got=$(limit 20 "$blit" --offscreen 1600x1100 --shot "$dir/shot.bmp" --script "frame;mdpreview;frame;mdwait;mddump" "$dir/nested.md" 2>&1 >/dev/null | grep 'nested too deeply' | tr '\n' ,)
check "subgraphs, namespaces and states nested hundreds deep: a mistake, no crash" "mermaid: line 34: subgraphs nested too deeply,mermaid: line 34: namespaces nested too deeply,mermaid: line 127: states nested too deeply,"
# A diagram's mistake in a narrow preview: wrapped, all of it shown (it ran
# off the right edge).
printf '```mermaid\nerDiagram\n    CUSTOMER ||--x{ ORDER : places\n```\n' >"$dir/mistake.md"
run 900x600 "$dir/mistake.md" "mdwait"
got=$(printf '%s\n' "$got" | sed -n '/^mermaid: line 2/,/x{"$/p')
got="$(printf '%s\n' "$got" | wc -l | tr -d ' ') rows: $(printf '%s\n' "$got" | tr '\n' ' ')"
case $got in 1\ *) ;; *) got=${got#* } ;; esac
check "a diagram's mistake, wrapped" "rows: mermaid: line 2: a relation like ||--o{ is wanted at \"||--x{\" "
printf '<!--\nhidden\n\nstill hidden\n-->\nshown\n\n```mermaid\npie title Pets\n  "Dogs" : 3\n  "Cats" : 1\n```\n' >"$dir/odd.md"
run 1600x1100 "$dir/odd.md" "mdwait"
seen=$(printf '%s\n' "$got" | grep -c 'hidden'),$(printf '%s\n' "$got" | grep -c 'shown')
dump 1600x1100 "$dir/odd.md" ""
got=$seen,$(printf '%s\n' "$got" | grep -c '^node title ')
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
