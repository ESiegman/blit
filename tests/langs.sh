#!/bin/sh
# Languages, through the editor: which language a file is (its ending, its
# whole name or a pattern of names, a #! line), what init.lua can say of
# one (a `parser` alias, `comment`, `filenames`, a display `name`, and a
# server's `language_id`), and the languages built in beside C++ and
# Python: their names in the status bar, what gc makes a comment with,
# their servers, and their colours. The colours are looked at only where
# the grammars are installed in the data folder blit uses (with the
# environment this runs in); the rest runs with a scratch one.
#
#   tests/langs.sh path/to/blit            run the cases
#   tests/langs.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/langs.sh path/to/blit [--print]}
print=${2:-}
fake="$(cd "$(dirname "$blit")" && pwd)/blit_fakelsp"
[ -x "$fake" ] || { echo "langs.sh: no $fake"; exit 1; }
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
W="$dir/work"
mkdir -p "$W" "$dir/data" "$dir/bin"
off='blit.opt.git.enabled = false
blit.opt.editor.restore = false'
printf '%s\nblit.opt.lsp.enabled = false\nblit.opt.editor.modal = false\n' "$off" >"$dir/init.lua"
export BLIT_RECENT_FILE="$dir/recent"
real_data=${XDG_DATA_HOME:-}

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run SCRIPT FILE: the last line the run printed, in a scratch data folder
# (no grammars). INIT, if set, is the init.lua for this run only.
INIT=
run() {
    got=$(XDG_DATA_HOME="$dir/data" BLIT_INIT="${INIT:-$dir/init.lua}" "$blit" --offscreen 1600x700 --shot "$dir/shot.bmp" --script "workdir:$W;frame;$1" "$2" 2>&1 >/dev/null |
        sed -e "s#$dir#~#g" -e 's/^blit: //' | tail -1)
    INIT=
}
# language FILE: the language blit takes the file to be ("" for plain text).
language() { run "lua:print('[' .. (blit.info(0).language or '') .. ']')" "$1"; }

# ---------------------------------------------------------------- what init.lua can say

printf 'SECTIONS\n{\n  .text : { *(.text*) } > FLASH\n}\n' >"$W/link.ld"
printf 'int x;\n' >"$W/main.c"
printf 'config FOO\n\tbool "foo"\n' >"$W/Kconfig"
printf 'CONFIG_FOO=y\n' >"$W/board_defconfig"
printf 'CONFIG_FOO=y\n' >"$W/defconfig.txt"
printf '%s\nblit.opt.lsp.enabled = false\nblit.opt.editor.modal = false\n%s\n' "$off" \
    'blit.opt.syntax.grammars.ldc = { parser = "c", extensions = { "ld" }, name = "Linker script (C)" }
blit.opt.syntax.grammars.kc = { filenames = { "Kconfig", "*_defconfig" }, comment = "#" }
blit.opt.syntax.grammars.blk = { parser = "c", extensions = { "blk" }, comment = "/* %s */" }' >"$dir/own.lua"

INIT="$dir/own.lua" language "$W/Kconfig"
check "filenames: a whole name" '[kc]'
INIT="$dir/own.lua" language "$W/board_defconfig"
check "filenames: a pattern of names" '[kc]'
INIT="$dir/own.lua" language "$W/defconfig.txt"
check "a name the pattern does not fit is not that language" '[]'
INIT="$dir/own.lua" run "statusdump" "$W/link.ld"
check "an alias's display name in the status bar" 'link.ld | Ln 1, Col 1 Spaces: 2 LF Linker script (C) (no colours)'
INIT="$dir/own.lua" run "statusclick:C;text:linker script (c);key:Return;frame;statusdump" "$W/main.c"
check "and in the list of languages, to choose for a file" 'main.c | Ln 1, Col 1 Tabs LF Linker script (C) (no colours)'
INIT="$dir/own.lua" run "statusdump" "$W/Kconfig"
check "a language with no name of its own: its key, with a capital" 'Kconfig | Ln 1, Col 1 Tabs LF Kc (no colours)'

# gc: the language's comment from init.lua, both ends of it, or what the
# language whose grammar an alias uses has.
printf '%s\nblit.opt.lsp.enabled = false\nblit.opt.editor.modal = true\n%s\n' "$off" "$(grep grammars "$dir/own.lua")" >"$dir/vim.lua"
printf 'one\n' >"$W/thing.blk"
INIT="$dir/vim.lua" run "text:g;text:c;text:c;vimdump" "$W/Kconfig"
check "gc with the comment init.lua gives" 'NORMAL |  | 1:1 | # config FOO'
INIT="$dir/vim.lua" run "text:g;text:c;text:c;vimdump" "$W/thing.blk"
check "both ends of it" 'NORMAL |  | 1:1 | /* one */'
INIT="$dir/vim.lua" run "text:g;text:c;text:c;vimdump" "$W/link.ld"
check "an alias with none: its grammar's language's" 'NORMAL |  | 1:1 | // SECTIONS'

# An alias has no server of the language whose grammar it uses: a server
# for C takes main.c and not link.ld.
log="$dir/opened"
printf '%s\n%s\n' "$off" "blit.opt.lsp.servers.fakec = { command = { \"$fake\", \"--opened\", \"$log\", \"--name\", \"c\" }, languages = { \"c\" }, roots = { \".root\" } }
blit.opt.lsp.servers.fakecu = { command = { \"$fake\", \"--opened\", \"$log\", \"--name\", \"cu\" }, languages = { \"cu\", \"cuh\" }, roots = { \".root\" }, language_id = { cu = \"cuda-cpp\" } }
blit.opt.lsp.servers.fakeany = { command = { \"$fake\", \"--opened\", \"$log\", \"--name\", \"any\" }, languages = { \"anyl\" }, roots = { \".root\" }, language_id = \"plain-text\" }
blit.opt.syntax.grammars.ldc = { parser = \"c\", extensions = { \"ld\" } }
blit.opt.syntax.grammars.cu = { extensions = { \"cu\" } }
blit.opt.syntax.grammars.cuh = { extensions = { \"cuh\" } }
blit.opt.syntax.grammars.anyl = { extensions = { \"any\" } }" >"$dir/servers.lua"
: >"$W/.root"
printf '__global__ void k() {}\n' >"$W/k.cu"
printf '#pragma once\n' >"$W/k.cuh"
printf 'words\n' >"$W/x.any"
got=$(XDG_DATA_HOME="$dir/data" BLIT_INIT="$dir/servers.lua" "$blit" --grammars 2>&1 | grep '^  ldc ' | tr -s ' ')
check "an alias's grammar is the one it names" ' ldc the c grammar'
INIT="$dir/servers.lua" run "lspsettle;open:$W/link.ld;lspsettle;lspdump" "$W/main.c"
check "an alias gets no server of its grammar's language" 'fakec work 1 | main.c+ link.ld- | front not followed 0'
rm -f "$log"
INIT="$dir/servers.lua" run "lspsettle;open:$W/k.cuh;lspsettle;open:$W/x.any;lspsettle;open:$W/main.c;lspsettle;lspdump" "$W/k.cu"
got=$(sort "$log" | tr '\n' ' ')
check "language_id: by language, the protocol's name; blit's for the others; or one for every language" 'c main.c cuda-cpp k.cu cuh k.cuh plain-text x.any '

# ---------------------------------------------------------------- Objective-C
printf 'int main() {}\n' >"$W/a.m"
printf 'int main() {}\n' >"$W/a.mm"
language "$W/a.m"
check ".m is C to blit (and clangd)" '[c]'
language "$W/a.mm"
check ".mm is C++" '[cpp]'

# ---------------------------------------------------------------- built in
# Hardware, shaders, CUDA and firmware's files: a small file of each, with
# a comment, words the language keeps and a number.
B="$W/built"
mkdir -p "$B"
printf '// counter\nmodule top(input logic clk, output logic [7:0] q);\n  always_ff @(posedge clk) q <= q + 8'"'"'d1;\nendmodule\n' >"$B/top.sv"
printf '// adder\nmodule add(input a, b, output y);\n  assign y = a ^ b;\nendmodule\n' >"$B/add.v"
printf '# build\nset top counter\nproc run {n} {\n  puts "step $n"\n  return [expr {$n + 1}]\n}\n' >"$B/build.tcl"
printf '# clock\ncreate_clock -period 10.000 [get_ports clk]\n' >"$B/pins.xdc"
printf '# clock\ncreate_clock -name clk -period 5 [get_ports clk]\n' >"$B/timing.sdc"
printf '#version 450\n// a colour\nlayout(location = 0) out vec4 color;\nvoid main() {\n  color = vec4(1.0, 0.5, 0.0, 1.0);\n}\n' >"$B/fill.frag"
printf '// a colour\nfloat4 main(float4 p : SV_Position) : SV_Target {\n  return float4(1.0, 0.5, 0.0, 1.0);\n}\n' >"$B/fill.hlsl"
printf '// a colour\n@fragment\nfn main() -> @location(0) vec4<f32> {\n  return vec4<f32>(1.0, 0.5, 0.0, 1.0);\n}\n' >"$B/fill.wgsl"
printf '// a colour\nstruct Params { float scale; };\nfloat4 fill(Params p) {\n  return float4(p.scale, 0.5, 0.0, 1.0);\n}\n' >"$B/fill.slang"
printf '// a colour\n#include <metal_stdlib>\nusing namespace metal;\nfragment float4 fill() {\n  return float4(1.0, 0.5, 0.0, 1.0);\n}\n' >"$B/fill.metal"
printf '// one more each\n__global__ void add(int* x) {\n  x[threadIdx.x] += 1;\n}\n' >"$B/add.cu"
printf '/* reset */\n.global _start\n_start:\n  li a0, 42\n  j _start\n' >"$B/start.S"
printf '/* memory */\nMEMORY\n{\n  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 64K\n}\nSECTIONS\n{\n  .text : { *(.text*) } > FLASH\n}\n' >"$B/link.ld"
printf '// board\n/dts-v1/;\n/ {\n  model = "test";\n  leds {\n    compatible = "gpio-leds";\n  };\n};\n' >"$B/board.dts"
printf '# options\nconfig BLINK_GPIO\n\tint "Blink GPIO"\n\tdefault 5\n' >"$B/Kconfig"
printf 'CONFIG_BLINK_GPIO=5\n' >"$B/sdkconfig"

# Which language each file is, by its ending or its name.
all=
for f in top.sv x.svh add.v x.vh build.tcl x.tk pins.xdc timing.sdc x.glsl x.vert fill.frag x.comp x.geom x.tesc x.tese x.mesh x.task x.rgen \
    fill.hlsl x.hlsli x.fx fill.wgsl fill.slang fill.metal add.cu x.cuh start.S x.s x.asm link.ld x.lds board.dts x.dtsi x.overlay \
    Kconfig Kconfig.projbuild sdkconfig sdkconfig.defaults defconfig nrf52_defconfig; do
    [ -e "$B/$f" ] || printf 'x\n' >"$B/$f"
    language "$B/$f"
    all="$all $f$got"
done
got=$all
check "each ending and name" " top.sv[systemverilog] x.svh[systemverilog] add.v[verilog] x.vh[verilog] build.tcl[tcl] x.tk[tcl] pins.xdc[tcl] timing.sdc[tcl] x.glsl[glsl] x.vert[glsl] fill.frag[glsl] x.comp[glsl] x.geom[glsl] x.tesc[glsl] x.tese[glsl] x.mesh[glsl] x.task[glsl] x.rgen[glsl] fill.hlsl[hlsl] x.hlsli[hlsl] x.fx[hlsl] fill.wgsl[wgsl] fill.slang[slang] fill.metal[metal] add.cu[cuda] x.cuh[cuda] start.S[asm] x.s[asm] x.asm[asm] link.ld[linkerscript] x.lds[linkerscript] board.dts[devicetree] x.dtsi[devicetree] x.overlay[devicetree] Kconfig[kconfig] Kconfig.projbuild[kconfig] sdkconfig[kconfig] sdkconfig.defaults[kconfig] defconfig[kconfig] nrf52_defconfig[kconfig]"
printf '#!/usr/bin/env tclsh\nputs hi\n' >"$B/script"
language "$B/script"
check "a #! line for tclsh: Tcl" '[tcl]'
INIT="$dir/own.lua" language "$W/link.ld"
check "init.lua's endings still win over the built-in ones" '[ldc]'

# Their names in the status bar (no grammar is installed in the scratch data folder).
all=
for f in top.sv add.v build.tcl fill.frag fill.hlsl fill.wgsl fill.slang fill.metal add.cu start.S link.ld board.dts Kconfig; do
    run "statusdump" "$B/$f"
    all="$all;${got##* LF }"
done
got=$all
check "each one's name in the status bar" ";SystemVerilog (no colours);Verilog (no colours);Tcl (no colours);GLSL (no colours);HLSL (no colours);WGSL (no colours);Slang (no colours);Metal (no colours);CUDA (no colours);Assembly (no colours);Linker script (no colours);Devicetree (no colours);Kconfig (no colours)"

# What gc makes a comment with, in each.
printf '%s\nblit.opt.lsp.enabled = false\nblit.opt.editor.modal = true\n' "$off" >"$dir/modal.lua"
all=
for f in top.sv add.v build.tcl pins.xdc fill.frag fill.hlsl fill.wgsl fill.slang fill.metal add.cu start.S link.ld board.dts Kconfig sdkconfig; do
    INIT="$dir/modal.lua" run "text:j;text:g;text:c;text:c;vimdump" "$B/$f"
    all="$all;${got##* | }"
done
got=$all
check "gc in each" ";// module top(input logic clk, output logic [7:0] q);;// module add(input a, b, output y);;# set top counter;# create_clock -period 10.000 [get_ports clk];a colour;// float4 main(float4 p : SV_Position) : SV_Target {;// @fragment;// struct Params { float scale; };;// #include <metal_stdlib>;// __global__ void add(int* x) {;/* .global _start */;/* MEMORY */;// /dts-v1/;;# config BLINK_GPIO;# CONFIG_BLINK_GPIO=5"

# The servers: an alias has none of the language whose grammar it uses
# (Verilog's own, here none; Metal none of C++'s) ...
printf '%s\n%s\n' "$off" "blit.opt.lsp.servers.fakesv = { command = { \"$fake\", \"--name\", \"sv\" }, languages = { \"systemverilog\" }, roots = { \".root\" } }
blit.opt.lsp.servers.fakecpp = { command = { \"$fake\", \"--name\", \"cpp\" }, languages = { \"cpp\" }, roots = { \".root\" } }
blit.opt.lsp.servers.verible = { enabled = false }" >"$dir/alias.lua"
: >"$B/.root"
printf 'int main() {}\n' >"$B/main.cpp"
INIT="$dir/alias.lua" run "lspsettle;open:$B/add.v;open:$B/main.cpp;open:$B/fill.metal;lspsettle;lspdump" "$B/top.sv"
check "Verilog and Metal: no server of SystemVerilog's or C++'s" 'fakecpp built 1; fakesv built 1 | top.sv+ add.v- main.cpp+ fill.metal- | front not followed 0'
# ... and the presets, each the first of its language's on PATH (stand-ins here).
printf '%s\n' "$off" >"$dir/on.lua"
for p in verible-verilog-ls glsl_analyzer wgsl-analyzer shader-language-server slangd dts-lsp clangd; do
    printf '#!/bin/sh\nexit 0\n' >"$dir/bin/$p"
    chmod +x "$dir/bin/$p"
done
all=
# (A home of its own: blit looks for servers in ~/.local/bin, ~/.cargo/bin
# and the like too, where this machine's own may be.)
mkdir -p "$dir/home"
for f in top.sv add.v fill.frag fill.wgsl fill.hlsl fill.slang board.dts add.cu fill.metal build.tcl Kconfig; do
    got=$(HOME="$dir/home" PATH="$dir/bin:/usr/bin:/bin" XDG_DATA_HOME="$dir/data" BLIT_INIT="$dir/on.lua" "$blit" --info "$B/$f" 2>/dev/null | sed -n 's/^lsp  *//p' | sed 's/  (.*//')
    all="$all;$f $got"
done
got=$all
check "the server presets" ";top.sv verible;add.v verible;fill.frag glsl_analyzer;fill.wgsl wgsl-analyzer;fill.hlsl shader-language-server;fill.slang slangd;board.dts dts-lsp;add.cu clangd;fill.metal no language server installed for metal;build.tcl no language server installed for tcl;Kconfig no language server installed for kconfig"
rm "$dir/bin/glsl_analyzer"
got=$(HOME="$dir/home" PATH="$dir/bin:/usr/bin:/bin" XDG_DATA_HOME="$dir/data" BLIT_INIT="$dir/on.lua" "$blit" --info "$B/fill.frag" 2>/dev/null | sed -n 's/^lsp  *//p' | sed 's/  (.*//')
check "GLSL without glsl_analyzer: the server for three" 'shader-language-server'
# clangd tells CUDA's files by the name VS Code gives the language.
rm -f "$log"
printf '%s\n%s\n' "$off" "blit.opt.lsp.servers.clangd = { command = { \"$fake\", \"--opened\", \"$log\", \"--name\", \"c\" }, roots = { \".root\" } }" >"$dir/clangd.lua"
INIT="$dir/clangd.lua" run "lspsettle;open:$B/main.cpp;lspsettle;lspdump" "$B/add.cu"
got=$(sort "$log" | tr '\n' ' ')
check "clangd: CUDA as cuda-cpp" 'cpp main.cpp cuda-cpp add.cu '

# Colours, where the grammars are installed: every kind of token the
# small file has, from the whole query compiled (not slices of it).
installed=$(XDG_DATA_HOME="$real_data" "$blit" --grammars 2>/dev/null)
colours() {  # colours FILE GRAMMAR KINDS...: whether each kind is coloured
    f=$1 g=$2
    shift 2
    if ! printf '%s\n' "$installed" | grep -Eq "^ *$g +installed"; then
        echo "colours of $f skipped: the $g grammar is not installed"
        return
    fi
    out=$(env ${real_data:+XDG_DATA_HOME="$real_data"} BLIT_SYNTAX_COMPILE=whole BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x600 --shot "$dir/shot.bmp" --script "frame;tokensdump" "$B/$f" 2>&1 >/dev/null | tail -1)
    got=${out%% |*}
    for k in "$@"; do
        printf '%s\n' "$out" | tr ' ' '\n' | grep -Eq "^$k(\\.|\$)" || got="$got, no $k"
    done
    check "colours of $f" "$g"
}
# (Metal is C++'s grammar under its own name; Verilog SystemVerilog's.)
colours top.sv systemverilog comment keyword number
colours add.v systemverilog comment keyword
colours build.tcl tcl comment keyword function
colours timing.sdc tcl comment function
colours fill.frag glsl comment keyword type number
colours fill.hlsl hlsl comment type number
colours fill.wgsl wgsl comment keyword type number
colours fill.slang slang comment keyword type number
colours fill.metal cpp comment keyword type number
colours add.cu cuda comment keyword type
colours start.S asm comment label number
colours link.ld linkerscript comment keyword number
colours board.dts devicetree comment keyword
colours Kconfig kconfig comment keyword string
colours sdkconfig kconfig constant

if [ "$failed" = 0 ] && [ -z "$print" ]; then echo "langs.sh: all passed"; fi
exit "$failed"
