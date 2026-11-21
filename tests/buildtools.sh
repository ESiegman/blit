#!/bin/sh
# Problems read from the output of the tools a build runs besides the
# compilers: linkers and assemblers, HDL tools, shader compilers and
# Python's tools. Each case is a build that prints one tool's output (from
# tests/buildtools/), as make runs it, and the problems it gave: path, line,
# column, severity, the tool's own code, and the notes under it.
#
# The outputs are the tools' own, kept as they printed them, from these runs:
#   on macOS (arm64): Yosys 0.71+post, Icarus Verilog 13.0 (iverilog, vvp),
#     verible-verilog-lint v0.0-3946-g851d3ff4, Ruff 0.17.5, qsb 6.12.0 (Qt
#     shader tools), Apple ld-27106.1 under Apple Clang and under GCC 16
#     (collect2), and Metal's compiler run from source at run time
#     (MTLDevice newLibraryWithSource, whose file is "program_source");
#     ld-mac-dup.txt's object paths had the scratch folder they were made in
#     replaced by /work/ld.
#   on Linux (Arch, x86_64): GNU ld and as 2.47 under GCC 16.2.1,
#     glslangValidator 16.6.0, spirv-val (SPIRV-Tools v2026.4); and in docker
#     images there, Verilator 5.054 (verilator/verilator), Yosys 0.66
#     (hdlc/yosys) and nextpnr-ice40 0.11.1 (hdlc/nextpnr).
# Tools on neither machine have outputs written to their documented shape:
# Vivado's (vivado.txt: "SEVERITY: [Tool id-n] message [file:line]"),
# slangc's (slangc.txt: "path(line): severity code: message", notes with no
# code), DXC's -fdiagnostics-format=msvc and FXC's codes (dxc-msvc.txt; the
# dxc on the Mac is a shell alias, not DXC), naga's (naga.txt:
# codespan-reporting's diagrams), tint's (tint.txt: "path:line:col severity:
# message", the source and carets under it), pyright's (pyright.txt: its
# command line's "  path:line:col - severity: message (rule)", a message's
# other lines under it) and pre-commit's (pre-commit.txt: its hooks' lines;
# under them Ruff's own concise line, and pre-commit-hooks'
# trailing-whitespace and check-merge-conflict lines). chatter.txt is lines
# from these outputs (and Ninja's, GCC's) that are not problems.
#
#   tests/buildtools.sh path/to/blit            run the cases
#   tests/buildtools.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/buildtools.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd -P)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
export BLIT_RECENT_FILE="$dir/recent"
export HOME="$dir/home"
mkdir -p "$HOME"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n' >"$dir/init.lua"

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n%s\n\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n  expected:\n%s\n  got:\n%s\n' "$1" "$2" "$got"
        failed=1
    fi
}

# The project: the outputs, and the files a linker names by their bare
# names or objects.
T="$dir/proj"
mkdir -p "$T/src" "$T/shaders"
cp "$here"/buildtools/*.txt "$T/"
printf 'int helper(int);\n' >"$T/src/a.c"
printf 'int dup = 2;\n' >"$T/src/b.c"
cp "$T/src/a.c" "$T/src/a2.c"

# scan NAME [COMMAND]: the problems from the output NAME.txt, as make prints
# it (after COMMAND, as make would echo it, for the tools whose lines leave
# out the file), after "problems: N", a line each.
scan() {
    printf 'out:\n' >"$T/Makefile"
    [ -n "${2:-}" ] && printf '\t@echo '"'"'%s'"'"'\n' "$2" >>"$T/Makefile"
    printf '\t@cat %s.txt; exit 1\n' "$1" >>"$T/Makefile"
    got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$T;frame;buildrun:make out;buildwait;scandump" 2>&1 >/dev/null |
        sed -e "s#$dir#~#g" | sed -n '/^problems: /,$p')
}
# want: what a case should give, the lines given.
want() { expected=$(cat); }

# Linkers. GNU ld with debug information: the source's line, the other
# definition a note; collect2's word after it not a problem of its own.
scan ld-g
want <<'EOF'
problems: 2
E /tmp/m3-tools-gen/src/b.c:1 multiple definition of `dup'
  note /tmp/m3-tools-gen/src/a.c:3 first defined here
E /tmp/m3-tools-gen/src/a.c:6 undefined reference to `helper'
EOF
check "ld with -g: at the source's line" "$expected"
scan ld-nog
want <<'EOF'
problems: 2
E ld:1 multiple definition of `dup' (b.o)
  note first defined in a.o
E src/a.c:1 undefined reference to `helper' in `main'
EOF
check "ld without -g: the object's source (the project's one a.c) and function; an object named for no source, nowhere" "$expected"
scan ld-cmake
want <<'EOF'
problems: 2
E src/a.c:1 undefined reference to `helper' in `main'
E src/a.c:1 undefined reference to `twice' in `main'
EOF
check "ld: the line under \"in function\" with no name before it, and the next with it" "$expected"
scan ld-direct
want <<'EOF'
problems: 3
E ld:1 multiple definition of `dup' (b.o)
  note first defined in a.o
W ld:1 cannot find entry symbol _start; defaulting to 0000000000401000
E src/a.c:1 undefined reference to `helper' in `main'
EOF
check "ld run alone: its warning" "$expected"
scan ld-lib
want <<'EOF'
problems: 1
E ld:1 cannot find -lnosuchlib: No such file or directory
EOF
check "ld: a library not found, with no place" "$expected"
tail -n 1 "$T/ld-g.txt" >"$T/ld-alone.txt"
scan ld-alone
want <<'EOF'
problems: 1
E ld:1 ld returned 1 exit status
EOF
check "collect2's word with no line of ld's before it: a problem" "$expected"
scan ld-mac
want <<'EOF'
problems: 1
E ld:1 undefined symbol: _helper (referenced from _main in a-86729e.o)
EOF
check "macOS's ld: an undefined symbol, in a temporary object" "$expected"
scan ld-mac-cmake
want <<'EOF'
problems: 1
E src/a.c:1 undefined symbol: _helper (referenced from _main in a.c.o)
EOF
check "macOS's ld: in CMake's object, its source" "$expected"
scan ld-mac-gcc
want <<'EOF'
problems: 2
E ld:1 undefined symbol: twice(int) (referenced from _main in ccKEfOmv.o)
E ld:1 undefined symbol: helper(int) (referenced from _main in ccKEfOmv.o)
EOF
check "macOS's ld under GCC: each symbol, collect2's word not one" "$expected"
scan ld-mac-dup
want <<'EOF'
problems: 1
E src/a2.c:1 duplicate symbol '_dup'
  note src/b.c:1 also defined here
EOF
check "macOS's ld: a duplicate symbol, the other object's source a note" "$expected"
scan ld-mac-lib
want <<'EOF'
problems: 1
E ld:1 library 'nosuchlib' not found
EOF
check "macOS's ld: a library not found, Clang's word after it not one" "$expected"
scan as
want <<'EOF'
problems: 3
E bad.s:5 no such instruction: `movz %rax'
E bad.s:6 no such instruction: `bogus %eax'
W bad.s:7 value 0x12c truncated to 0x2c
EOF
check "GNU as" "$expected"

# HDL tools.
scan verilator-lint
want <<'EOF'
problems: 5
W child.v:3:14 [WIDTHTRUNC] Value too large for 3 bit number: 9
W child.v:3:12 [WIDTHEXPAND] Operator ADD expects 8 bits on the LHS, but LHS's VARREF 'd' generates 4 bits.
  note In instance 'top.u0'
W child.v:3:12 [WIDTHEXPAND] Operator ADD expects 8 bits on the RHS, but RHS's CONST '3'h1' generates 3 bits.
  note In instance 'top.u0'
W top.v:5:12 [WIDTHTRUNC] Operator ASSIGNW expects 4 bits on the Assign RHS, but Assign RHS's VARREF 'wide' generates 8 bits.
  note In instance 'top'
W top.v:2:14 [UNUSEDSIGNAL] Bits of signal are not used: 'wide'[7:4]
  note In instance 'top'
EOF
check "Verilator's warnings: their codes, the instance a note" "$expected"
scan verilator-multi
want <<'EOF'
problems: 2
W multi.sv:3:28 [MULTIDRIVEN] Variable written to in always_ff also written by other process (IEEE 1800-2023 9.2.2.4): 'q'
  note In instance 'multi'
  note multi.sv:2:28 Location of other write
W multi.sv:1:76 [MULTIDRIVEN] Signal has multiple driving blocks with different clocking: 'q'
  note multi.sv:2:28 Location of first driving block
  note multi.sv:3:28 Location of other driving block
EOF
check "Verilator: the other places it names, notes" "$expected"
scan verilator-syntax
want <<'EOF'
problems: 1
E syn.v:4:1 syntax error, unexpected endmodule
EOF
check "Verilator's error with no code; its last words not one" "$expected"
scan verilator-miss
want <<'EOF'
problems: 1
E miss.v:2:3 [MODMISSING] Cannot find file containing module: 'nochild'
EOF
check "Verilator's error with a code; the files it looked for not notes" "$expected"
scan yosys-synth
want <<'EOF'
problems: 1
W child.v:3 Literal has a width of 3 bit, but value requires 4 bit.
EOF
check "Yosys: a warning at its place, the rest its log (ABC's warning, its count)" "$expected"
scan yosys-mac
want <<'EOF'
problems: 1
W undef.v:3 Identifier `\nope' is implicitly declared.
EOF
check "Yosys: a warning naming no place not taken" "$expected"
scan yosys-syntax
want <<'EOF'
problems: 1
E syn.v:4 syntax error, unexpected TOK_ENDMODULE
EOF
check "Yosys: an error at its place" "$expected"
scan yosys-q
check "Yosys -q: the same" "$expected"
scan yosys-miss
want <<'EOF'
problems: 1
E yosys:1 Module `\nochild' referenced in module `\miss' in cell `\u0' is not part of the design.
EOF
check "Yosys: an error with no place, after its banner" "$expected"
scan yosys-q-miss 'yosys -q -p "read_verilog miss.v; synth -top top"'
want <<'EOF'
problems: 1
E yosys:1 Module `\child' referenced in module `\top' in cell `\u0' is not part of the design.
EOF
check "and with no banner, after its command" "$expected"
scan yosys-q-miss
check "but a bare ERROR: line of no tool's seen is not one" 'problems: 0'
scan iverilog-syntax
want <<'EOF'
problems: 1
E syn.v:4 syntax error
EOF
check "Icarus: syntax error" "$expected"
scan iverilog-sorry
want <<'EOF'
problems: 3
E sorry.v:2 sorry: Unpacked structs not supported.
E sorry.v:10 sorry: Overriding the default variable lifetime is not yet supported.
E sorry.v:15 sorry: concurrent_assertion_item not supported. Try -gno-assertions or -gsupported-assertions to turn this message off.
EOF
check "Icarus: sorry" "$expected"
scan iverilog-miss
want <<'EOF'
problems: 1
E miss.v:2 Unknown module type: child
EOF
check "Icarus: its error: lines, as a compiler's" "$expected"
scan iverilog-warn
want <<'EOF'
problems: 2
W impl.v:2 implicit definition of wire 'n'.
W impl.v:4 Numeric constant truncated to 5 bits.
EOF
check "Icarus: its warnings" "$expected"
scan vvp
want <<'EOF'
problems: 3
E tb.v:5 x is 3
W tb.v:6 careful
E tb.v:8 stopping
EOF
check "vvp: \$error, \$warning, \$fatal at their places; a \$display saying ERROR: not one" "$expected"
scan verible
want <<'EOF'
problems: 4
E syn.v:4:1 syntax error at token "endmodule"
W lint.sv:1:8 [module-filename] Declared module does not match the first dot-delimited component of file name: "lint"
W lint.sv:2:1 [no-tabs] Use spaces, not tabs.
W lint.sv:5:10 [no-trailing-spaces] Remove trailing spaces.
EOF
check "verible-verilog-lint: its rules" "$expected"
scan vivado
want <<'EOF'
problems: 5
E /work/fpga/src/top.v:12 [Synth 8-439] module 'child' not found
E /work/fpga/src/top.v:1 [Synth 8-6156] failed synthesizing module 'top'
W /work/fpga/src/top.v:12 [Synth 8-7071] port 'q' of module 'child' is unconnected for instance 'u0'
W /work/fpga/constrs/top.xdc:3 [Constraints 18-619] A clock with name 'clk' already exists, overwriting the previous clock with the same name.
E vivado:1 [Common 17-69] Command failed: Synthesis failed - please see the console or run log file for details
EOF
check "Vivado: its ids, its critical warnings; INFO not" "$expected"
pnr='nextpnr-ice40 --up5k --package sg48 --json blink.json --pcf blink.pcf --asc blink.asc'
scan nextpnr-pcf "$pnr"
want <<'EOF'
problems: 2
E blink.pcf:2 package does not have a pin named '999'
E nextpnr:1 Loading PCF failed.
EOF
check "nextpnr: an error on a line of the .pcf its command named" "$expected"
scan nextpnr-warn "$pnr"
want <<'EOF'
problems: 1
W blink.pcf:3 unmatched constraint 'nosuch'
EOF
check "nextpnr: a warning, its Info: lines not" "$expected"
scan nextpnr-pcf
want <<'EOF'
problems: 2
E nextpnr:1 package does not have a pin named '999' (on line 2)
E nextpnr:1 Loading PCF failed.
EOF
check "nextpnr with no command seen: no place" "$expected"
scan nextpnr-timing
want <<'EOF'
problems: 1
E nextpnr:1 Max frequency for clock 'clk$SB_IO_IN_$glb_clk': 79.74 MHz (FAIL at 400.00 MHz)
EOF
check "nextpnr: timing that fails" "$expected"

# Shader compilers.
scan glslang-frag
want <<'EOF'
problems: 1
E shaders/shader.frag:7 'undeclared' : undeclared identifier
EOF
check "glslang: its error, not what follows it (compilation terminated) or its count" "$expected"
scan glslang-col
want <<'EOF'
problems: 1
E shaders/shader.frag:7:21 'undeclared' : undeclared identifier
EOF
check "glslang --error-column" "$expected"
scan glslang-syntax
want <<'EOF'
problems: 1
E shaders/syntax.vert:5 syntax error, unexpected RIGHT_BRACE, expecting COMMA or SEMICOLON
EOF
check "glslang: no token" "$expected"
scan glslang-warn
want <<'EOF'
problems: 1
W shaders/warn.frag:2 '#extension' : extension not supported: GL_FOO_bar
EOF
check "glslang's warning" "$expected"
scan qsb 'qsb --glsl 100es,120,150 --hlsl 50 --msl 12 -o x.qsb shaders/shader2.frag'
want <<'EOF'
problems: 1
E shaders/shader2.frag:6 'missing_fn' : no matching overloaded function found
EOF
check "qsb: glslang's lines with no file, the one its command named" "$expected"
scan qsb-syntax
want <<'EOF'
problems: 1
E qsb:7 syntax error, unexpected IDENTIFIER, expecting COMMA or SEMICOLON
EOF
check "qsb with no command seen" "$expected"
scan spirv-val-vuid 'spirv-val --target-env vulkan1.3 shaders/bad2.spv'
want <<'EOF'
problems: 1
E shaders/bad2.spv:10 [VUID-StandaloneSpirv-Location-04916] Variable must be decorated with a location
  note %2 = OpVariable %_ptr_Output_v4float Output
EOF
check "spirv-val: its VUID, the instruction a note, the module its command named" "$expected"
scan spirv-val
want <<'EOF'
problems: 1
E spirv-val:11 Operand '5[%int]' cannot be a type
  note %7 = OpIAdd %float %int %int
EOF
check "spirv-val with no VUID, and no command seen" "$expected"
scan metal-runtime
want <<'EOF'
problems: 2
E program_source:11:16 use of undeclared identifier 'undefined_fn'
E program_source:13:91 use of undeclared identifier 'b'
EOF
check "Metal at run time, with no .metal file in the project: program_source" "$expected"
printf 'kernel void k() {}\n' >"$T/shaders/shade.metal"
scan metal-runtime
want <<'EOF'
problems: 2
E shaders/shade.metal:11:16 use of undeclared identifier 'undefined_fn'
E shaders/shade.metal:13:91 use of undeclared identifier 'b'
EOF
check "the project's one .metal file" "$expected"
cp "$T/shaders/shade.metal" "$T/shaders/two.metal"
scan metal-runtime
check "two: neither" "$(printf '%s\n' "$expected" | sed 's#shaders/shade.metal#program_source#')"
scan metal-runtime './mc shaders/two.metal'
check "the one its command named" "$(printf '%s\n' "$expected" | sed 's#shaders/shade.metal#shaders/two.metal#')"
rm "$T/shaders/shade.metal" "$T/shaders/two.metal"
scan slangc
want <<'EOF'
problems: 2
E shaders/blur.slang:12 [30015] undefined identifier 'weigths'.
  note shaders/blur.slang:4 see declaration of 'weights'
W shaders/blur.slang:20 [30081] implicit conversion from 'float' to 'int' is not recommended
EOF
check "slangc" "$expected"
scan dxc-msvc
want <<'EOF'
problems: 3
E shaders/light.hlsl:14:12 use of undeclared identifier 'normalz'
  note shaders/light.hlsl:3:8 'normal' declared here
W shaders/light.hlsl:20:5 [X3206] implicit truncation of vector type
E shaders/light.hlsl:31:1 [X3000] syntax error: unexpected token '}'
EOF
check "DXC's and FXC's MSVC form" "$expected"
scan naga
want <<'EOF'
problems: 2
E shaders/fill.wgsl:7:12 no definition in scope for identifier: `undefined_color`
E shaders/fill.wgsl:5:1 Function [0] 'main' is invalid
  note The type of [1] doesn't match the type stored in [0]
EOF
check "naga: the place under its heading, its = lines notes" "$expected"
scan tint
want <<'EOF'
problems: 2
E shaders/fill.wgsl:7:12 unresolved value 'undefined_color'
  note shaders/fill.wgsl:3:5 'color' declared here
W shaders/fill.wgsl:9:3 code is unreachable
EOF
check "tint: its note after a blank line" "$expected"

# Python's tools.
scan ruff-concise
want <<'EOF'
problems: 7
E bad.py:1:7 [invalid-syntax] Expected a parameter or the end of the parameter list
E bad.py:1:8 [invalid-syntax] Expected `)`, found newline
W pkg/mod.py:1:8 [F401] `os` imported but unused
W pkg/mod.py:6:13 [E711] Comparison to `None` should be `cond is None`
W pkg/mod.py:7:16 [F821] Undefined name `undefined_name`
W pkg/mod.py:8:5 [E741] Ambiguous variable name: `l`
W pkg/mod.py:8:5 [F841] Local variable `l` is assigned to but never used
EOF
check "Ruff, concise: its rules warnings, syntax errors errors" "$expected"
scan ruff-full
check "Ruff's full form: the same" "$expected"
scan ruff-pylint
want <<'EOF'
problems: 5
W pkg/mod.py:1 [F401] `os` imported but unused
W pkg/mod.py:6 [E711] Comparison to `None` should be `cond is None`
W pkg/mod.py:7 [F821] Undefined name `undefined_name`
W pkg/mod.py:8 [E741] Ambiguous variable name: `l`
W pkg/mod.py:8 [F841] Local variable `l` is assigned to but never used
EOF
check "Ruff's pylint form" "$expected"
scan ruff-format
want <<'EOF'
problems: 1
W ugly.py:1:3 [unformatted] File would be reformatted
EOF
check "ruff format --check" "$expected"
scan pyright
want <<'EOF'
problems: 3
E /work/py/pkg/mod.py:7:16 [reportUndefinedVariable] "undefined_name" is not defined
E /work/py/pkg/mod.py:9:12 [reportArgumentType] Argument of type "Literal['x']" cannot be assigned to parameter "n" of type "int" in function "f"; "Literal['x']" is not assignable to "int"
W /work/py/pkg/mod.py:1:8 [reportUnusedImport] Import "os" is not accessed
EOF
check "pyright: its rules, a message's lines joined, informations not" "$expected"
scan pre-commit
want <<'EOF'
problems: 3
W pkg/mod.py:1 [trailing-whitespace] fixed by trim trailing whitespace
W pkg/mod.py:1:8 [F401] `os` imported but unused
E pkg/conflict.py:3 [check-merge-conflict] Merge conflict string '<<<<<<< ' found
EOF
check "pre-commit: the files a hook fixed, the lines of the hooks that failed" "$expected"

# Lines like these tools' that are not problems.
scan chatter
check "chatter" 'problems: 0'

exit $failed
