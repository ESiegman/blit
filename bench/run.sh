#!/bin/sh
# Runs the whole benchmark suite and writes bench/results/LABEL.jsonl.
#   bench/run.sh LABEL [--no-window]
# Offscreen runs draw into a texture with no window. Window runs open a real
# window for a few seconds each (startup, frame pacing, idle).
set -eu
cd "$(dirname "$0")/.."
label=${1:?usage: bench/run.sh LABEL [--no-window]}
window=1
[ "${2:-}" = "--no-window" ] && window=0
out=bench/results/$label.jsonl
data=bench/data
# Benchmarks exist only in a Debug build, and only mean something when that
# build is optimized like a release:
#   cmake -S . -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBLIT_OPTIMIZE_DEBUG=ON
#   cmake --build build-bench
build=${BLIT_BUILD:-build-bench}
blit=./$build/blit
bench=./$build/blit_bench
size=2560x1600
if [ ! -x $bench ]; then
    echo "no $bench: configure $build with -DCMAKE_BUILD_TYPE=Debug -DBLIT_OPTIMIZE_DEBUG=ON and build it" >&2
    exit 1
fi
if ! grep -q "BLIT_OPTIMIZE_DEBUG:BOOL=ON" $build/CMakeCache.txt; then
    echo "$build is not optimized (BLIT_OPTIMIZE_DEBUG is off): its numbers would mean nothing" >&2
    exit 1
fi

# Measure the defaults, whatever the personal settings file says, except
# that no language server is started: a real one working in the background
# would be measured along with everything else. The language server client
# has its own section below, against a test server that answers predictably.
# Vim's modes are off: the scenarios type, and typing must type. ("type_modal"
# below is the same typing through Vim's Insert mode.)
# The tab bar, line numbers and indent lines are off as well, so that the
# frame numbers stay those of the text view alone and compare with earlier
# rounds; "scroll_full" below is the same scroll with all three on.
mkdir -p $data
# (Settings are init.lua's: each of these is one, named by BLIT_INIT.)
printf 'blit.opt.lsp.enabled = false\nblit.opt.editor.modal = false\nblit.opt.editor.tab_bar = false\nblit.opt.editor.line_numbers = false\nblit.opt.editor.indent_guides = false\n' >$data/bench.lua
printf 'blit.opt.lsp.enabled = false\nblit.opt.editor.modal = false\n' >$data/full.lua
printf 'blit.opt.lsp.servers.fake = { command = { "%s/%s/blit_fakelsp", "--utf8" }, languages = { "cpp" } }\nblit.opt.editor.modal = false\n' "$PWD" "$build" >$data/lsp.lua
printf 'blit.opt.lsp.servers.fake = { command = { "%s/%s/blit_fakelsp", "--utf8", "--quiet" }, languages = { "cpp" } }\nblit.opt.editor.modal = false\n' "$PWD" "$build" >$data/lsp-quiet.lua
# Everything on, as it is used: a C++ file coloured by tree-sitter (when the
# cpp grammar is installed), the test language server attached and its
# diagnostics in, the tab bar, line numbers and indent lines, Vim's modes.
printf 'blit.opt.lsp.servers.fake = { command = { "%s/%s/blit_fakelsp", "--utf8" }, languages = { "cpp" } }\n' "$PWD" "$build" >$data/everything.lua
export BLIT_INIT=$PWD/$data/bench.lua

[ -f $data/log-1g.txt ] || $bench gen $data
: >"$out"

echo "== text core (headless)"
for f in code-1m code-100m log-1g longline-64m utf8-16m; do
    $bench run $data/$f.txt >>"$out"
done

# With BLIT_BENCH_FONT naming a font that has ligatures (for example
# "Victor Mono"), also measure the grid fill with ligatures on.
if [ -n "${BLIT_BENCH_FONT:-}" ]; then
    for f in code-100m log-1g; do
        BLIT_BENCH_LIGATURES=1 $bench run $data/$f.txt fill >>"$out"
    done
fi

# Syntax highlighting, on real C++ (blit's own source, repeated). Needs a C++
# tree-sitter grammar; without one these lines are simply absent.
cat src/*.cpp src/*.hpp >$data/real-200k.cpp
for i in 1 2 3 4 5 6 7 8 9 10; do cat $data/real-200k.cpp; done >$data/real-2m.cpp
for f in real-200k real-2m; do
    $bench run $data/$f.cpp syntax >>"$out"
done

echo "== language server client (against the test server)"
$bench run $data/real-200k.cpp lsp >>"$out"
# Typing with a server attached, and frames with every kind of popup open.
BLIT_INIT=$PWD/$data/lsp-quiet.lua $blit --offscreen $size --bench assist $data/real-200k.cpp >>"$out"
BLIT_INIT=$PWD/$data/lsp.lua $blit --offscreen $size --bench popups $data/real-200k.cpp 2>/dev/null >>"$out"

echo "== frames (offscreen $size)"
app() { $blit --offscreen $size "$@" >>"$out"; }
app --bench scroll $data/code-100m.txt
app --bench scroll $data/log-1g.txt
app --bench scroll $data/utf8-16m.txt
app --bench select $data/code-100m.txt
app --bench jump $data/log-1g.txt
app --bench hscroll $data/log-1g.txt
app --bench hscroll --frames 600 $data/longline-64m.txt
app --bench hscroll --frames 300 --col 60000000 $data/longline-64m.txt
app --bench type $data/code-100m.txt
app --bench type $data/log-1g.txt
# Typing as it arrives, a text event a frame through the real handlers:
# without Vim, in Vim's Insert mode, and keys that are Normal-mode commands.
printf 'blit.opt.lsp.enabled = false\nblit.opt.editor.modal = true\nblit.opt.editor.tab_bar = false\nblit.opt.editor.line_numbers = false\nblit.opt.editor.indent_guides = false\n' >$data/modal.lua
app --bench keys $data/code-100m.txt
BLIT_INIT=$PWD/$data/modal.lua $blit --offscreen $size --bench keys --script 'text:i' $data/code-100m.txt | sed 's/"bench":"keys"/"bench":"keys_insert"/' >>"$out"
BLIT_INIT=$PWD/$data/modal.lua BLIT_BENCH_KEYS='wwwwwwwwjbbbbbbbbj' $blit --offscreen $size --bench keys $data/code-100m.txt | sed 's/"bench":"keys"/"bench":"keys_normal"/' >>"$out"
# The same scroll with the tab bar, line numbers and indent lines on.
BLIT_INIT=$PWD/$data/full.lua $blit --offscreen $size --bench scroll $data/code-100m.txt | sed 's/"bench":"scroll"/"bench":"scroll_full"/' >>"$out"
# And everything on: colours, the language server, the looks, Vim's modes.
BLIT_INIT=$PWD/$data/everything.lua $blit --offscreen $size --bench scroll --script 'ready;diag' $data/real-200k.cpp | sed 's/"bench":"scroll"/"bench":"scroll_everything"/' >>"$out"
BLIT_INIT=$PWD/$data/everything.lua $blit --offscreen $size --bench keys --script 'ready;diag;text:Go' $data/real-200k.cpp | sed 's/"bench":"keys"/"bench":"keys_everything"/' >>"$out"
# And the same again with the window in four splits, three of them showing
# other files and standing still.
BLIT_INIT=$PWD/$data/full.lua $blit --offscreen $size --bench scroll \
    --script 'key:cmd+\;open:src/font.cpp;key:cmd+shift+\;open:src/render.cpp;key:cmd+alt+Left;key:cmd+shift+\;open:src/lsp.cpp;key:cmd+alt+Up' \
    $data/code-100m.txt | sed 's/"bench":"scroll"/"bench":"scroll_4_splits"/' >>"$out"
# GPU-bound throughput at this display's full resolution.
$blit --offscreen 4608x2592 --bench scroll $data/code-100m.txt | sed 's/"bench":"scroll"/"bench":"scroll_4k"/' >>"$out"

if [ $window = 1 ]; then
    echo "== window"
    for i in 1 2 3 4 5 6 7; do
        $bench launch $blit --bench startup $data/code-1m.txt >>"$out"
    done
    for i in 1 2 3; do
        $bench launch $blit --bench startup $data/log-1g.txt >>"$out"
    done
    # The same launch for an SDL program that draws nothing: the floor.
    for i in 1 2 3 4 5 6 7; do
        $bench launch ./$build/blit_null >>"$out"
    done
    $blit --bench keypress $data/code-100m.txt >>"$out"
    $blit --bench scroll --frames 2000 $data/code-100m.txt >>"$out"
    $blit --bench pace $data/code-100m.txt >>"$out"
    $blit --bench idle $data/code-100m.txt >>"$out"
fi
echo "wrote $out ($(wc -l <"$out") results)"
