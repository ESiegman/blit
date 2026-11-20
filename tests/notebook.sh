#!/bin/sh
# A notebook's view: opened beside the script of its cells (from the
# command line too), its cells drawn as Jupyter shows them, each kind of
# output (text, stderr, a DataFrame's table, pictures, Markdown, JSON,
# LaTeX, an error's traceback in colour), a long one cut; following the
# script's cursor, a click taking the cursor to a cell; Cmd+Shift+V; a
# cell changed since it ran; outputs given as a kernel would, shown and
# saved; and broken notebooks. Cells run in the notebook's kernel (plain
# Python, and IPython where python3 has it): their outputs shown as they
# come and saved, an error, an interrupt, a restart, run all, a kernel
# that cannot start, and no process left behind.
#
#   tests/notebook.sh path/to/blit            run the cases
#   tests/notebook.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/notebook.sh path/to/blit [--print]}
print=${2:-}
command -v python3 >/dev/null 2>&1 || { echo "notebook skipped: no python3 to write the notebooks with"; exit 77; }
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua" BLIT_RECENT_FILE="$dir/recent" BLIT_CACHE_DIR="$dir/cache" BLIT_APPEARANCE=dark
case $(uname -s) in
    Darwin) close=cmd+W ;;
    *) close=ctrl+F4 ;;
esac

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
# view SCRIPT: the notebook's view as nbview gives it (rows as "line: text"), its trailing spaces gone.
view() {
    got=$("$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "open:$dir/n.ipynb;mdwait;imagewait;mdwait;$1;nbview" 2>&1 >/dev/null | sed -e 's/ *$//')
}

# A notebook with an output of each kind (a picture made here: a PNG's bytes).
cat >"$dir/make.py" <<'EOF'
import base64, json, struct, sys, zlib
def png(w, h):
    raw = b''.join(b'\x00' + bytes([200, 40, 40]) * w for _ in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
def src(s): return [l + '\n' for l in s.split('\n')[:-1]] + [s.split('\n')[-1]]
def code(n, s, outputs): return {"cell_type": "code", "execution_count": n, "metadata": {}, "outputs": outputs, "source": src(s)}
table = ('<div><style scoped>.dataframe { x: y; }</style><table border="1" class="dataframe"><thead><tr style="text-align: right;">'
         '<th></th><th>planet</th><th>moons</th></tr></thead><tbody><tr><th>0</th><td>Earth &amp; Moon</td><td>1</td></tr>'
         '<tr><th>1</th><td>Mars</td><td>2</td></tr></tbody></table><p>2 rows</p></div>')
tb = ["\u001b[0;31mZeroDivisionError\u001b[0m   Traceback (most recent call last)",
      "Cell \u001b[0;32mIn[6], line 1\u001b[0m\n\u001b[0;32m----> 1\u001b[0m \u001b[38;5;241m1\u001b[39m\u001b[38;5;241m/\u001b[39m\u001b[38;5;241m0\u001b[39m",
      "\u001b[0;31mZeroDivisionError\u001b[0m: division by zero"]
cells = [
    {"cell_type": "markdown", "metadata": {}, "source": src("# Orbits\n\nThe **planets**.")},
    code(1, "print('hello')", [{"name": "stdout", "output_type": "stream", "text": ["hel", "lo\n"]},
                               {"name": "stderr", "output_type": "stream", "text": "careful\n"}]),
    code(2, "df", [{"data": {"text/html": table, "text/plain": "   planet"}, "execution_count": 2, "metadata": {}, "output_type": "execute_result"}]),
    code(3, "plot()", [{"data": {"image/png": base64.b64encode(png(64, 32)).decode(), "text/plain": "<Figure size 64x32>"}, "metadata": {}, "output_type": "display_data"}]),
    code(4, "show()", [{"data": {"text/markdown": "**bold** words", "text/plain": "md"}, "metadata": {}, "output_type": "display_data"},
                       {"data": {"application/json": {"name": "Earth", "moons": [1]}, "text/plain": "js"}, "metadata": {}, "output_type": "display_data"},
                       {"data": {"text/latex": "$E = mc^2$", "text/plain": "latex"}, "metadata": {}, "output_type": "display_data"},
                       {"data": {"text/plain": "42"}, "execution_count": 4, "metadata": {}, "output_type": "execute_result"}]),
    code(5, "logo()", [{"data": {"image/svg+xml": '<svg xmlns="http://www.w3.org/2000/svg" width="80" height="40"><rect width="80" height="40" fill="#4c9be8"/></svg>',
                                 "text/plain": "<SVG logo>"}, "metadata": {}, "output_type": "display_data"}]),
    code(6, "1/0", [{"ename": "ZeroDivisionError", "evalue": "division by zero", "output_type": "error", "traceback": tb}]),
    code(7, "loop()", [{"name": "stdout", "output_type": "stream", "text": "".join("step %d\n" % i for i in range(100))}]),
    {"cell_type": "raw", "metadata": {}, "source": src("as it is")},
    code(None, "x = 1", []),
]
json.dump({"cells": cells, "metadata": {"language_info": {"name": "python"}}, "nbformat": 4, "nbformat_minor": 5}, open(sys.argv[1], "w"), indent=1, sort_keys=True)
EOF
python3 "$dir/make.py" "$dir/n.ipynb"

# Opened: the script, and beside it the view (the keyboard on the script).
got=$("$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "open:$dir/n.ipynb;mdwait;dump" 2>&1 >/dev/null | tail -1)
check "opened: the script, and the notebook beside it" 'H(>[n.ipynb*] | [Notebook: n.ipynb*])0.50'
# From the command line too: the script of its cells, not its JSON.
got=$("$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "mdwait;dump;lsptext;nbview" "$dir/n.ipynb" 2>&1 >/dev/null | sed -n '1,2p;4p' | cut -c1-50 | sed -e 's/ *$//')
check "from the command line: the script of its cells, and the notebook beside it" 'H(>[n.ipynb*] | [Notebook: n.ipynb*])0.50
# %% [markdown] / # # Orbits / # / # The **planets
2:         Orbits'

# What it draws (each row as the script's line it is of, and its text).
view ""
all=$got
got=$(printf '%s\n' "$all" | grep -E 'In \[|Out\[' | sed -e 's/^[0-9]*: *//' | tr '\n' '|')
check "code cells with In [n]: beside them, a result's Out[n]:" "In [1]:  print('hello')|In [2]:  df|Out[2]: ┌───┬──────────────┬───────┐|In [3]:  plot()|In [4]:  show()|Out[4]: 42|In [5]:  logo()|In [6]:  1/0|In [7]:  loop()|In [ ]:  x = 1|"
got=$(printf '%s\n' "$all" | grep -E '^(2|4|7|10): ' | grep -v ━ | grep -v 'In \[' | tr -s ' ' | grep -v ': *$' | tr '\n' '|')
check "Markdown drawn; a stream written in parts, one text; stderr; a DataFrame as a table" "2: Orbits|4: The planets.|7: hello|7: careful|10: Out[2]: ┌───┬──────────────┬───────┐|10: │ │ planet │ moons │|10: ├───┼──────────────┼───────┤|10: │ 0 │ Earth & Moon │ 1 │|10: │ 1 │ Mars │ 2 │|10: └───┴──────────────┴───────┘|10: 2 rows|"
got=$(printf '%s\n' "$all" | grep -E '^(13|16): ' | grep -v 'In \[' | sed -e 's/^[0-9]*: *//' | grep . | tr '\n' '|' | sed -e 's/nb-[0-9a-f]*\.png [0-9]*x[0-9]*/nb-HASH.png/')
check "a picture shown from blit's cache; Markdown, JSON indented, LaTeX's math as text" "picture: nb-HASH.png|bold words|{|\"moons\": [|1|],|\"name\": \"Earth\"|}|E = mc²|Out[4]: 42|"
got=$(ls "$dir/cache/pictures" | grep -c '^nb-[0-9a-f]*\.png$')
check "the picture written once, named by what is in it" "1"
# Pictures not shown for 30 days go from the cache (the next time one is
# kept); one shown has its time set again, one newer stays.
shown=$(ls "$dir/cache/pictures" | grep '^nb-[0-9a-f]*\.png$')
touch -t 202001010000 "$dir/cache/pictures/$shown"
printf x >"$dir/cache/pictures/nb-00000000000000aa.png" && touch -t 202001010000 "$dir/cache/pictures/nb-00000000000000aa.png"
printf x >"$dir/cache/pictures/nb-00000000000000bb.png"
"$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "open:$dir/n.ipynb;mdwait;imagewait" >/dev/null 2>&1
got="$(ls "$dir/cache/pictures" | grep -c '^nb-') $(find "$dir/cache/pictures" -name 'nb-*' -mtime +29 | wc -l | tr -d ' ')"
check "pictures not shown for 30 days gone, those shown kept" "$(($(ls "$dir/cache/pictures" | grep -c '^nb-[0-9a-f]*\.svg$') + 2)) 0"
[ -e "$dir/cache/pictures/$shown" ] && [ -e "$dir/cache/pictures/nb-00000000000000bb.png" ] || echo "FAIL: the picture shown, or the newer one, gone"
svg=$(printf '%s\n' "$all" | grep -E '^19: ' | grep -v 'In \[' | sed -e 's/^[0-9]*: *//' | grep . | sed -e 's/nb-[0-9a-f]*\.svg [0-9]*x[0-9]*/nb-HASH.svg/')
case $svg in
    "picture: nb-HASH.svg" | "<SVG logo>") got=ok ;;
    *) got=$svg ;;
esac
check "an SVG drawn where the system reads them, else its text" "ok"
got=$(printf '%s\n' "$all" | grep -E '^(22|25|28): ' | grep -v 'In \[' | sed -e 's/^[0-9]*: *//' | grep . | tr '\n' '|')
check "an error's traceback, a long output cut and how much more said, a raw cell" "ZeroDivisionError   Traceback (most recent call last)|Cell In[6], line 1|----> 1 1/0|ZeroDivisionError: division by zero|$(seq 0 59 | sed -e 's/^/step /' | tr '\n' '|')… 40 more lines|as it is|"
# The traceback's colours: the theme's (the error's name in its colour for errors, not the text's).
got=$("$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "open:$dir/n.ipynb;mdwait;mdinks" 2>&1 >/dev/null | sed -n 's/.*\[#\([0-9a-f]*\)\]ZeroDivisionError\[#\([0-9a-f]*\)\]: division.*/\1 \2/p')
[ -n "$got" ] && [ "${got% *}" != "${got#* }" ] && got=coloured
check "a traceback's colours turned into the theme's" "coloured"

# Following the script's cursor to its cell (from the blank row above it),
# and a click on a cell taking the cursor there, the keyboard back on the script.
got=$("$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "open:$dir/n.ipynb;mdwait;imagewait;vim:25G;mdwait;nbview" 2>&1 >/dev/null | head -1)
row=$(printf '%s\n' "$all" | tail -n +2 | grep -n "^24: *$" | head -1 | cut -d: -f1)
check "the view follows the cursor to its cell" "top $((row - 1)) @24"
mars=$(printf '%s\n' "$all" | tail -n +2 | grep -n "^10: .* Mars " | cut -d: -f1)
words=$(printf '%s\n' "$all" | tail -n +2 | grep -n "^4: .*The planets" | cut -d: -f1)
got=$("$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "open:$dir/n.ipynb;mdwait;imagewait;mdwait;nbclick:$((mars - 1));docdump:0;dump;nbclick:$((words - 1));docdump:0" 2>&1 >/dev/null | tr '\n' '#')
check "a click on an output: the cursor to its cell's first line; on Markdown, to its line" "#> |df#H(>[n.ipynb*] | [Notebook: n.ipynb*])0.50##> |# The **planets**.#"

# Cmd+Shift+V: the view closed, and opened again; closing the script closes it.
got=$("$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "open:$dir/n.ipynb;mdwait;keyof:Preview beside the file;dump;keyof:Preview beside the file;mdwait;dump;key:$close;dump" 2>&1 >/dev/null | tr '\n' '#')
check "Cmd+Shift+V closes the view and opens it again; closing the script closes it" ">[n.ipynb*]#H(>[n.ipynb*] | [Notebook: n.ipynb*])0.50#>[untitled*]#"

# A cell changed since it ran: said, its outputs quieter; the model's cells.
got=$("$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "open:$dir/n.ipynb;mdwait;vim:10GAx<Esc>;mdwait;nbview;nbcells" 2>&1 >/dev/null | sed -e 's/ *$//')
got=$(printf '%s\n' "$got" | grep -E '^10: ' | grep -E 'In|edited|Out'; printf '%s\n' "$got" | grep -E '^[0-9]+ (code|markdown|raw)' | head -4)
check "a changed cell: edited, under its label; the model's cells" "10: In [2]:  dfx
10:  edited
10: Out[2]: ┌───┬──────────────┬───────┐
1 markdown same
2 code [1] same stream stream
3 code [2] changed execute_result
4 code [3] same display_data"
got=$("$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "open:$dir/n.ipynb;mdwait;vim:10GAx<Esc>;mdwait;mdinks" 2>&1 >/dev/null | grep -E 'Mars' | grep -o '\[#[0-9a-f]*\]Mars')
plain=$("$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "open:$dir/n.ipynb;mdwait;mdinks" 2>&1 >/dev/null | grep -E 'Mars' | grep -o '\[#[0-9a-f]*\]Mars')
[ -n "$got" ] && [ "$got" != "$plain" ] && got=quieter
check "a changed cell's outputs quieter" "quieter"

# Outputs given as a kernel gives them: drawn with the count, the line
# after the cell, and saved.
cp "$dir/n.ipynb" "$dir/k.ipynb"
printf '[{"output_type": "stream", "name": "stdout", "text": "ran\\n"}, {"output_type": "execute_result", "execution_count": 8, "data": {"text/plain": "1"}, "metadata": {}}]' >"$dir/out.json"
got=$("$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "open:$dir/k.ipynb;mdwait;nbset:10,8,$dir/out.json;nbset:9,8,$dir/out.json;nbview;nbdump;save" 2>&1 >/dev/null | grep -E '^31: |nbset|^[0-9]+: .* \| ' | sed -e 's/ *$//')
check "a kernel's outputs: drawn, the count beside the cell, the line after it" 'nbset: no such code cell, or not outputs
31: In [8]:  x = 1
31:
31:         ran
31: Out[8]: 1
7: careful | 10: [table] | 13: [image/png] | 16: 42 | 19: [image/svg+xml] | 22: ZeroDivisionError: division by zero | 25: step 99 | 31: 1'
cp "$dir/n.ipynb" "$dir/k.ipynb"
got=$("$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "open:$dir/k.ipynb;mdwait;lua:print(blit.info().modified);nbset:10,8,$dir/out.json;lua:print(blit.info().modified);save;lua:print(blit.info().modified)" 2>&1 >/dev/null | tr '\n' ' ')
check "a kernel's outputs: the notebook changed (closing or quitting asks first), till saved" "blit: false blit: true blit: false "

# Broken notebooks: JSON that is not a notebook opens as text, without a
# view; outputs that are not what they should be are drawn as far as they go.
printf '{"cells": "no"}\n' >"$dir/bad.ipynb"
got=$("$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "open:$dir/bad.ipynb;mdwait;dump;nbview" 2>&1 >/dev/null | tr '\n' '#')
check "JSON that is not a notebook: as text, no view" ">[bad.ipynb*]#no notebook view#"
cat >"$dir/broken.ipynb" <<'EOF'
{"cells": [
 {"cell_type": "code", "execution_count": "seven", "metadata": {}, "outputs": {"not": "a list"}, "source": "a = 1"},
 {"cell_type": "code", "execution_count": 2, "metadata": {}, "outputs": [
   {"output_type": "display_data", "data": {"image/png": "!!!", "text/plain": "<Figure>"}, "metadata": {}},
   {"output_type": "display_data", "data": {"image/png": "iVBORw0KGgo="}, "metadata": {"image/png": {"width": -5}}},
   {"output_type": "execute_result", "data": {"text/plain": 42}, "metadata": {}},
   {"output_type": "error", "ename": "E", "evalue": "v", "traceback": "not a list"},
   {"output_type": "stream", "name": "stdout", "text": "\u001b[38;5;999modd\u001b[38;2;300m \u001b]8;;http://x\u001b\\link\u001b]8;;\u001b\\ \u001b["},
   {"output_type": "display_data", "data": {"application/vnd.plotly.v1+json": {}}, "metadata": {}},
   {"output_type": "execute_result", "execution_count": 2, "data": {"text/html": "<table><tr><td colspan=\"2\">wide</td></tr><tr><td rowspan=\"9\">tall</td><td>x"}, "metadata": {}},
   {"output_type": "nonsense"}, 42, "text"], "source": "b"},
 {"cell_type": "markdown", "metadata": {}, "source": 5},
 {"cell_type": "mystery", "source": ["c = 3"]}], "metadata": {"kernelspec": 1}, "nbformat": 4, "nbformat_minor": 5}
EOF
got=$("$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "open:$dir/broken.ipynb;mdwait;imagewait;mdwait;nbview;nbdump" 2>&1 >/dev/null | sed -e 's/^[0-9]*: *//' -e 's/ *$//' | grep . | tr '\n' '|')
check "broken outputs drawn as far as they go" 'top 0 @1|In [ ]:  a = 1|In [2]:  b|<Figure>|[image/png]|E: v|odd link|[application/vnd.plotly.v1+json]|Out[2]: ┌──────┬───┐|│ wide │   │|├──────┼───┤|│ tall │ x │|└──────┴───┘|In [ ]:  c = 3|odd link|'

# ---------------------------------------------------------------- running cells

# A notebook to run, in a folder whose .venv's Python is python3 without
# its site-packages: plain Python wherever the tests run (IPython's
# tracebacks and counts differ; it has cases of its own, below).
run=$dir/plain
mkdir -p "$run/.venv/bin"
printf '#!/bin/sh\nexec python3 -S "$@"\n' >"$run/.venv/bin/python" && chmod +x "$run/.venv/bin/python"
cat >"$dir/make-run.py" <<'EOF'
import json, sys
def src(s): return [l + '\n' for l in s.split('\n')[:-1]] + [s.split('\n')[-1]]
def code(s): return {"cell_type": "code", "execution_count": None, "metadata": {}, "outputs": [], "source": src(s)}
cells = [
    {"cell_type": "markdown", "metadata": {}, "source": src("# Running")},
    code("x = 21\nprint('half of', x * 2)\nx * 2"),
    code("import sys\nprint('careful', file=sys.stderr)\n1/0"),
    code("y = x + 1\ny"),
]
json.dump({"cells": cells, "metadata": {"language_info": {"name": "python"}}, "nbformat": 4, "nbformat_minor": 5}, open(sys.argv[1], "w"), indent=1, sort_keys=True)
EOF
python3 "$dir/make-run.py" "$run/r.ipynb"
# run SCRIPT: blit on r.ipynb (made again first), what it says on stderr.
run() {
    python3 "$dir/make-run.py" "$run/r.ipynb"
    got=$("$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "open:$run/r.ipynb;mdwait;$1" 2>&1 >/dev/null | sed -e 's/ *$//')
}
# outputs FILE: each cell of a saved notebook as "kind count: its outputs' types (a stream's text)".
outputs() {
    python3 -c '
import json, sys
for c in json.load(open(sys.argv[1]))["cells"]:
    if c["cell_type"] != "code": continue
    kinds = [o["output_type"] + ("(" + "".join(o["text"]).strip() + ")" if o["output_type"] == "stream" else "") for o in c["outputs"]]
    print((str(c["execution_count"]) + ": " + " ".join(kinds)).strip())' "$1"
}

# Shift+Return: the cell run in the notebook's kernel, the cursor to the
# next cell; its outputs in the view as they would be in Jupyter, its line
# after it; the notebook changed, and saved with them.
run "lua:print(blit.info().modified);vim:7G;key:shift+Return;docdump:0;nbwait;nbkernel;mdwait;nbview;nbdump;lua:print(blit.info().modified);save;lua:print(blit.info().modified)"
got=$(printf '%s\n' "$got" | grep -v ━ | grep .)
check "Shift+Return: run in the kernel, the cursor to the next cell, its outputs shown and its count; the notebook changed, and saved" "blit: false
> |import sys
python pid $(printf '%s\n' "$got" | sed -n 's/^python pid \([0-9]*\) idle$/\1/p') idle
top 0 @2
2:         Running
4:
4:
5: In [1]:  x = 21
6:          print('half of', x * 2)
7:          x * 2
7:
5:         half of 42
5: Out[1]: 42
9:
9:
10: In [ ]:  import sys
11:          print('careful', file=sys.stderr)
12:          1/0
12:
14:
14:
15: In [ ]:  y = x + 1
16:          y
16:
7: 42
blit: true
blit: false"
got=$(outputs "$run/r.ipynb")
check "saved: the cell's outputs and count, as Jupyter writes them" "1: stream(half of 42) execute_result
None:
None:"
got=$(python3 -c '
import json, sys
outs = json.load(open(sys.argv[1]))["cells"][1]["outputs"]
print(json.dumps(outs[0]["text"]), json.dumps(outs[1]["data"]["text/plain"]))' "$run/r.ipynb")
check "saved: a stream's text and text/plain as their lines, as Jupyter writes them" '["half of 42\n"] ["42"]'

# An error: its traceback (the cell's own lines, not the kernel's), and
# with it the cells after it, run with it, do not run (as in Jupyter).
run "command:Run all the cells;nbwait;nbcells;mdwait;nbview"
all=$got
got=$(printf '%s\n' "$all" | grep -E '^[0-9] (code|markdown)')
check "run all, an error: the cells after it not run" "1 markdown same
2 code [1] same stream execute_result
3 code [2] same stream error
4 code [ ] same"
got=$(printf '%s\n' "$all" | grep -E '^10: ' | grep -v 'In \[' | sed -e 's/^[0-9]*: *//' | grep . | tr '\n' '|')
check "an error's traceback, its stderr before it" 'careful|Traceback (most recent call last):|File "<cell 2>", line 3, in <module>|1/0|~^~|ZeroDivisionError: division by zero|'
# Run all with no error: each in turn, counted in order.
run "vim:12Gdd;command:Run all the cells;nbwait;nbcells;save"
check "run all: each cell in turn" "1 markdown same
2 code [1] same stream execute_result
3 code [2] same stream
4 code [3] same execute_result"
got=$(outputs "$run/r.ipynb")
check "run all, saved" "1: stream(half of 42) execute_result
2: stream(careful)
3: execute_result"
# Compared with disk once saved: the notebook as it would be saved, the same.
run "save;command:Compare with disk;frame;lsptext"
got=$(printf '%s\n' "$got" | tail -1)
check "a notebook compared with disk, as it is saved" "(the same as now)"

# While a cell runs: In [*], it and those waiting; what it has printed so
# far shown. An interrupt: KeyboardInterrupt in it, at once, and the cells
# waiting do not run.
start=$(date +%s)
run "vim:7GA<CR>import time<CR>print('before', flush=True)<CR>time.sleep(30)<Esc>;command:Run all the cells;nbwait:started;nbkernel;nbcells;mdwait;nbview;command:Interrupt the kernel;nbwait;nbkernel;nbcells;nbview"
took=$(($(date +%s) - start))
all=$got
got=$(printf '%s\n' "$all" | sed -n '1,5p' | sed -e 's/pid [0-9]*/pid P/')
check "running: In [*], it and the cells waiting" "python pid P running 2 waiting
1 markdown same
2 code [*] same stream
3 code [*] same
4 code [*] same"
got=$(printf '%s\n' "$all" | sed -n '6,$p' | grep -E '^(5: In|5: *before|5: *KeyboardInterrupt|python pid|[0-9] code)' | sed -e 's/pid [0-9]*/pid P/' -e 's/^5: *//' | tr '\n' '|')
check "interrupted: KeyboardInterrupt in the cell, the cells waiting not run" "In [*]:  x = 21|before|python pid P idle|2 code [1] same stream error|3 code [ ] same|4 code [ ] same|In [1]:  x = 21|before|KeyboardInterrupt|"
[ "$took" -lt 20 ] && took=soon
got=$took
check "interrupted at once, not after the sleep" "soon"

# Restart: what was defined is gone, and the counts start again.
run "vim:7G;key:shift+Return;nbwait;nbkernel;command:Restart the kernel;nbwait;nbkernel;vim:16G;key:shift+Return;nbwait;nbcells;mdwait;nbview"
pids=$(printf '%s\n' "$got" | sed -n 's/^python pid \([0-9]*\) idle$/\1/p' | tr '\n' ' ')
set -- $pids
[ $# = 2 ] && [ "$1" != "$2" ] && ! kill -0 "$1" 2>/dev/null && pids=new
all=$got
got=$pids
check "restarted: another process, the one before it gone" "new"
got=$(printf '%s\n' "$all" | grep -E '^[0-9] code|NameError' | sed -e 's/^[0-9]*: *//' | tr '\n' '|')
check "restarted: x not defined, counted from 1" "2 code [1] same stream execute_result|3 code [ ] same|4 code [1] same error|NameError: name 'x' is not defined|"

# A cell changed after it ran keeps its outputs, said to be of other code,
# and saving keeps them (as Jupyter does); run again, they are its own.
run "vim:7G;key:shift+Return;nbwait;vim:7GA + 1<Esc>;nbcells;mdwait;nbview;save;nbcells"
all=$got
got=$(printf '%s\n' "$all" | grep -m1 '^2 code')
check "a cell changed after it ran: its outputs kept, of other code" "2 code [1] changed stream execute_result"
got=$(printf '%s\n' "$all" | grep -E '^[0-9]+: +edited' | tr -s ' ')
check "a cell changed after it ran: said in the view" "6: edited print('half of', x * 2)"
got=$(printf '%s\n' "$all" | grep '^2 code' | tail -1)
check "a cell changed after it ran: still said once saved" "2 code [1] changed stream execute_result"
got=$(outputs "$run/r.ipynb" | head -1)
check "a cell changed after it ran: saved with its outputs" "1: stream(half of 42) execute_result"
run "vim:7GA + 1<Esc>;vim:7G;key:shift+Return;nbwait;nbcells;nbdump"
got=$(printf '%s\n' "$got" | grep -E '^2 code|^7: ')
check "run again: its own outputs" "2 code [1] same stream execute_result
7: 43"

# The status bar says what the kernel is (Python or IPython, its version,
# what it runs in) and whether it is running a cell; on the view's tab too.
run "frame;statusdump;vim:7GA<CR>import time<CR>time.sleep(30)<Esc>;vim:7G;key:shift+Return;nbwait:started;frame;statusdump;command:Interrupt the kernel;nbwait;frame;statusdump;key:cmd+alt+Right;frame;statusdump"
got=$(printf '%s\n' "$got" | grep 'r.ipynb' | sed -e 's/.*| //' -e 's/Python [0-9][0-9.a-z+]* ([^)]*)/Python V (P)/' -e 's/ Ln .*//' | tr '\n' '|')
check "the status bar: the kernel, and whether it is busy" "no kernel|Python V (P) · busy|Python V (P) · idle|Python V (P) · idle|"

# A result whose JSON has NaN and infinities (which JSON has no word for):
# shown, as null, and text that says NaN as it is.
python3 - "$run/nan.ipynb" <<'EOF'
import json, sys
code = "class J:\n    def _repr_json_(self):\n        return {'a': float('nan'), 'b': 'NaN here', 'c': [float('-inf'), 1.5]}\nJ()"
cells = [{"cell_type": "code", "execution_count": None, "metadata": {}, "outputs": [], "source": code}]
json.dump({"cells": cells, "metadata": {}, "nbformat": 4, "nbformat_minor": 5}, open(sys.argv[1], "w"))
EOF
got=$("$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "open:$run/nan.ipynb;mdwait;vim:2G;key:shift+Return;nbwait;nbcells;mdwait;nbview" 2>&1 >/dev/null |
    grep -E '^1 code|"[abc]"|null|1\.5' | grep -v "inf'" | sed -e 's/^[0-9]*: *//' -e 's/^Out\[1\]: *//' | tr -s ' ' | tr '\n' '|')
check "a result with NaN in its JSON: shown, as null" '1 code [1] same execute_result|"a": null,|"b": "NaN here",|"c": [|null,|1.5|'

# A cell's terminal (COLUMNS) is as wide as the view draws its outputs: a
# rule as wide as it fills a row, whatever the window's width.
python3 - "$run/cols.ipynb" <<'EOF'
import json, sys
cells = [{"cell_type": "code", "execution_count": None, "metadata": {}, "outputs": [], "source": "import shutil\nprint('-' * shutil.get_terminal_size().columns)"}]
json.dump({"cells": cells, "metadata": {}, "nbformat": 4, "nbformat_minor": 5}, open(sys.argv[1], "w"))
EOF
rule() {
    "$blit" --offscreen "$1" --shot "$dir/shot.bmp" --script "open:$run/cols.ipynb;mdwait;vim:2G;key:shift+Return;nbwait;mdwait;nbview" 2>&1 >/dev/null |
        grep -E '^[0-9]+: *-+ *$' | sed -e 's/^[0-9]*: *//' | awk '{ print length($0) }' | tr '\n' ' '
}
wide=$(rule 2400x1500) narrow=$(rule 1400x1000)
got=$([ "${wide% }" != "${narrow% }" ] && [ "${wide% * }" = "$wide" ] && [ "${narrow% * }" = "$narrow" ] && echo fits || echo "$wide/$narrow")
check "a cell's terminal as wide as the view's outputs" "fits"

# 20 MB printed over a few seconds: the main thread takes it in in time
# that grows as the output does (the model given it a few times a second,
# a long text cut to what the view shows), and all of it is saved.
python3 - "$run/big.ipynb" <<'EOF'
import json, sys
code = "import sys, time\nfor i in range(100):\n    sys.stdout.write(('x' * 99 + '\\n') * 2000)\n    time.sleep(0.02)\nprint('end')"
cells = [{"cell_type": "code", "execution_count": None, "metadata": {}, "outputs": [], "source": code}]
json.dump({"cells": cells, "metadata": {}, "nbformat": 4, "nbformat_minor": 5}, open(sys.argv[1], "w"))
EOF
got=$("$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "open:$run/big.ipynb;mdwait;vim:2G;clock;key:shift+Return;nbwait;nbspent:400;mdwait;nbview;save" 2>&1 >/dev/null |
    grep -E "kernels taken|more lines" | sed -e 's/^[0-9]*: *//' | tr '\n' '|')
got="$got $(python3 -c 'import json, sys; t = "".join(json.load(open(sys.argv[1]))["cells"][0]["outputs"][0]["text"]); print(len(t), t.count(chr(10)))' "$run/big.ipynb")"
check "20 MB printed: taken in at once, shown cut, saved whole" "kernels taken in within 400 ms|… 199971 more lines| 20000004 200001"

# No process left behind: the kernel (and what a cell started) ends with
# the notebook's tab, and with blit.
cat >"$dir/spawn.txt" <<'EOF'
import subprocess
p = subprocess.Popen(['sleep', '60'])
print('child', p.pid)
EOF
spawn=$(tr '\n' '\r' <"$dir/spawn.txt" | sed -e 's/\r/<CR>/g')
alive() {
    for p in "$@"; do kill -0 "$p" 2>/dev/null && printf 'alive %s ' "$p"; done
    printf 'gone'
}
run "vim:7GA<CR>$spawn<Esc>;vim:7G;key:shift+Return;nbwait;nbkernel;nbview;save;key:$close;dump;wait:300"
kernel=$(printf '%s\n' "$got" | sed -n 's/^python pid \([0-9]*\) idle$/\1/p')
child=$(printf '%s\n' "$got" | sed -n 's/^5: *child \([0-9]*\)$/\1/p')
[ -n "$kernel" ] && [ -n "$child" ] && got=$(alive "$kernel" "$child")
check "the notebook closed: its kernel and what it started, gone" "gone"
run "vim:7GA<CR>$spawn<Esc>;vim:7G;key:shift+Return;nbwait;nbkernel;nbview"
kernel=$(printf '%s\n' "$got" | sed -n 's/^python pid \([0-9]*\) idle$/\1/p')
child=$(printf '%s\n' "$got" | sed -n 's/^5: *child \([0-9]*\)$/\1/p')
sleep 0.3
[ -n "$kernel" ] && [ -n "$child" ] && got=$(alive "$kernel" "$child")
check "blit quit: its kernels and what they started, gone" "gone"

# A kernel that cannot run (the folder's Python is not one): said, and the
# cell is not left waiting.
mkdir -p "$dir/broken/.venv/bin" && cp "$run/r.ipynb" "$dir/broken/r.ipynb"
printf '#!/bin/sh\necho "not a Python" >&2\nexit 1\n' >"$dir/broken/.venv/bin/python" && chmod +x "$dir/broken/.venv/bin/python"
got=$("$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "open:$dir/broken/r.ipynb;mdwait;vim:7G;key:shift+Return;nbwait;frame;nbcells;nbkernel" 2>&1 >/dev/null | grep -E '^blit: |^2 code|kernel')
case $got in
    "blit: The kernel has stopped: not a Python"*"2 code [ ] same"*"no kernel") got=said ;;
esac
check "a kernel that cannot start: said, nothing left waiting" "said"

# A notebook on another machine (tests/fake-ssh.sh standing in for ssh,
# its home a folder here): read as one, its kernel run there (the helper
# written to blit's cache there), Ctrl+C sent there, saved as a notebook,
# and nothing left running there.
there="$dir/there"
mkdir -p "$there/work/.venv/bin" && cp "$run/.venv/bin/python" "$there/work/.venv/bin/python"
python3 "$dir/make-run.py" "$there/work/r.ipynb"
got=$(BLIT_SSH="$(cd "$(dirname "$0")" && pwd)/fake-ssh.sh" BLIT_FAKE_HOME="$there" "$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" \
    --script "open:ssh://box$there/work/r.ipynb;remotewait;mdwait;dump;vim:7G;key:shift+Return;nbwait;nbkernel;vim:11GA<CR>import time<CR>time.sleep(30)<Esc>;vim:10G;key:shift+Return;nbwait:started;command:Interrupt the kernel;nbwait;nbcells;mdwait;nbview;save;remotewait" 2>&1 >/dev/null | sed -e 's/ *$//')
kernel=$(printf '%s\n' "$got" | sed -n 's/^python pid \([0-9]*\) idle$/\1/p')
sleep 0.3
got=$(printf '%s\n' "$got" | grep -E '^H\(|^[0-9] code|KeyboardInterrupt$|^5: +half' | sed -e 's/^[0-9]*: *//' | tr '\n' '|')
check "on another machine: run there, interrupted there" "H(>[r.ipynb*] | [Notebook: r.ipynb*])0.50|2 code [1] same stream execute_result|3 code [2] same stream error|4 code [ ] same|half of 42|KeyboardInterrupt|"
got="$(ls "$there/.cache/blit" | grep -c '^kernel-[0-9a-f]*\.py$') $(outputs "$there/work/r.ipynb" | tr '\n' '|') $(alive $kernel)"
check "on another machine: the helper written there once, saved as a notebook, nothing left running" "1 1: stream(half of 42) execute_result|2: stream(careful) error|None:| gone"
# A slow host: the notebook comes after the keyboard has gone to another
# split, and its view opens beside it without taking the keyboard back.
python3 "$dir/make-run.py" "$there/work/r.ipynb"
got=$(BLIT_FAKE_SLOW=0.3 BLIT_SSH="$(cd "$(dirname "$0")" && pwd)/fake-ssh.sh" BLIT_FAKE_HOME="$there" "$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" \
    --script "open:ssh://box$there/work/r.ipynb;key:cmd+\\;key:Escape;frame;dump;remotewait;mdwait;frame;dump" 2>&1 >/dev/null | tr '\n' '|')
check "a slow host: the view opens, the keyboard stays where it went" "H([r.ipynb*] | >[untitled*])0.50|H(H([r.ipynb*] | [Notebook: r.ipynb*])0.50 | >[untitled*])0.50|"
# Two blits writing the helper there at once each write their own file
# first (here: another's half-written one is in the way).
helper=$(ls "$there/.cache/blit" | grep '^kernel-[0-9a-f]*\.py$')
rm "$there/.cache/blit/$helper" && mkdir "$there/.cache/blit/$helper.part"
got=$(BLIT_SSH="$(cd "$(dirname "$0")" && pwd)/fake-ssh.sh" BLIT_FAKE_HOME="$there" "$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" \
    --script "open:ssh://box$there/work/r.ipynb;remotewait;mdwait;vim:7G;key:shift+Return;nbwait;nbcells" 2>&1 >/dev/null | grep -E '^2 code|kernel')
check "the helper written there beside another's half-written one" "2 code [1] same stream execute_result"
rmdir "$there/.cache/blit/$helper.part"
# The connection lost under a kernel there (blit's agent there killed): said,
# and the status bar's kernel says so (in red) until a cell starts another.
got=$(BLIT_SSH="$(cd "$(dirname "$0")" && pwd)/fake-ssh.sh" BLIT_FAKE_HOME="$there" "$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" \
    --script "open:ssh://box$there/work/r.ipynb;remotewait;mdwait;vim:7G;key:shift+Return;nbwait;nbkernel;lua:os.execute('pkill -9 -f $there/.cache/blit/[b]lit-agent');remotelost;nbwait;frame;nbkernel;statusdump;command:Reconnect to the servers that dropped;remotewait;key:shift+Return;nbwait;nbkernel" 2>&1 >/dev/null |
    grep -E '^blit: The kernel|^lost|^python|kernel lost' | sed -e 's/pid [0-9]*/pid P/' -e 's/.*| \(kernel lost with box\).*/\1/' | tr '\n' '|')
check "the connection lost under a kernel there: said, and in the status bar" "python pid P idle|blit: The kernel has stopped: the connection to box was lost|lost with box|kernel lost with box|python pid P idle|"

# With IPython, where python3 has it: its display hooks, magics and tracebacks.
if python3 -c 'import IPython' 2>/dev/null; then
    mkdir -p "$dir/ipython" && python3 "$dir/make-run.py" "$dir/ipython/r.ipynb"
    got=$("$blit" --offscreen 2400x1500 --shot "$dir/shot.bmp" --script "open:$dir/ipython/r.ipynb;mdwait;vim:7GA<CR>from IPython.display import Markdown, display<CR>display(Markdown('**rich**'))<CR>%time 1<Esc>;command:Run all the cells;nbwait;nbkernel;nbcells;mdwait;mdinks" 2>&1 >/dev/null | sed -e 's/ *$//')
    all=$got
    got=$(printf '%s\n' "$all" | sed -e 's/\[#[0-9a-f]*\]//g' | grep -E '^ipython|^[0-9] code|rich|Wall time|ZeroDivisionError|Out\[' | grep -v 'display(' |
        sed -e 's/pid [0-9]*/pid P/' -e 's/^[0-9]*: *//' -e 's/Wall time: .*/Wall time/' -e 's/^ *//' | tr -s ' ' | tr '\n' '|')
    check "IPython: display(), a magic, its counts, a traceback" "ipython pid P idle|2 code [1] same stream display_data stream execute_result|3 code [2] same stream error|4 code [ ] same|rich|Wall time|Out[1]: 1|ZeroDivisionError Traceback (most recent call last)|ZeroDivisionError: division by zero|"
    got=$(printf '%s\n' "$all" | sed -n 's/.*\[#\([0-9a-f]*\)\]ZeroDivisionError\[#\([0-9a-f]*\)\] *Traceback.*/\1 \2/p')
    [ -n "$got" ] && [ "${got% *}" != "${got#* }" ] && got=coloured
    check "IPython: its traceback in colour" "coloured"
else
    echo "notebook: no IPython for python3 here, its cases skipped"
fi

[ -n "$print" ] || [ $failed = 1 ] || echo "notebook ok"
exit $failed
