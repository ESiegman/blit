#!/bin/sh
# A build's problems from more than the built-in kinds: init.lua's own
# patterns (blit.build.matcher, the manual's for Questa, Xcelium and UVM
# taken from the manual itself) and which a task names (problems = ...);
# a run that failed with none; and problems read from a file (SARIF, GCC's
# JSON diagnostics, a log).
#
#   tests/problems.sh path/to/blit            run the cases
#   tests/problems.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/problems.sh path/to/blit [--print]}
print=${2:-}
manual="$(cd "$(dirname "$0")/.." && pwd -P)/docs/manual.md"
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
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run FOLDER SCRIPT: the last line the run printed. INIT, if set, is the
# init.lua for this run only.
INIT=
run() {
    got=$(BLIT_INIT="${INIT:-$dir/init.lua}" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$1;frame;$2" 2>&1 >/dev/null | sed -e "s#$dir#~#g" | tail -1)
    INIT=
}

# The manual's patterns for the commercial simulators, as it has them.
{
    cat "$dir/init.lua"
    sed -n '/^-- Questa and ModelSim:/,/^```$/p' "$manual" | sed '$d'
} >"$dir/sims.lua"
grep -q 'blit.build.matcher("uvm"' "$dir/sims.lua" || { echo "FAIL: the manual's patterns for the simulators were not found"; failed=1; }

# What each prints, in the shapes their documentation gives: neither
# Questa, Xcelium nor a UVM library is on the machines these tests run on,
# so these lines are not any simulator's real output.
H="$dir/hdl"
mkdir -p "$H"
printf 'module tb;\n  logic a;\n  assign a = nope;\n  logic x;\n  logic x;\nendmodule\n' >"$H/tb.sv"
cat >"$H/questa.sh" <<'EOF'
printf -- '-- Compiling module tb\n'
printf "** Error: tb.sv(3): (vlog-2730) Undefined variable: 'nope'.\n"
printf "** Error (suppressible): tb.sv(5): (vlog-2388) 'x' already declared in this scope (tb).\n"
printf "** Warning: tb.sv(4): (vlog-2623) Undefined variable: 'w'.\n"
printf '** Note: (vlog-2286) Using implicit +incdir+/opt/questa/verilog_src/uvm-1.2/src from import uvm_pkg\n'
printf 'Errors: 2, Warnings: 1\n'
exit 2
EOF
cat >"$H/vsim.sh" <<'EOF'
printf '# ** Note: (vsim-3812) Design is being optimized...\n'
printf '# ** Warning: (vsim-3015) [PCDPC] - Port size (1) does not match connection size (4) for port '"'"'a'"'"'.\n'
printf '#    Time: 0 ps  Iteration: 0  Instance: /tb/dut File: ./tb.sv Line: 2\n'
printf '# ** Error: Assertion error.\n'
printf '#    Time: 50 ns Started: 50 ns  Scope: tb.a1 File: tb.sv Line: 6 Expr: a==1\n'
printf '# ** Error: tb.sv(3): (vlog-2730) Undefined variable: '"'"'nope'"'"'.\n'
printf '# ** Error: lost its place\n'
printf '# End time: 12:00:00 on Nov 21,2026, Elapsed time: 0:00:01\n'
printf '# ** Fatal: (vsim-3421) Value 9 for idx is out of range [0:3].\n'
printf '#    Time: 10 ns  Iteration: 0  Process: /tb/p File: tb.sv Line: 4\n'
exit 1
EOF
cat >"$H/xcelium.sh" <<'EOF'
printf "xmvlog: *E,UNDIDN (./tb.sv,3|13): 'nope': undeclared identifier [12.5(IEEE)].\n"
printf 'xmelab: *W,CUVWSP (./tb.sv,2|8): 1 output port was not connected:\n'
printf 'xmelab: *N,SPDUSD (./tb.sv,1|7): Include directory ./inc given but not used.\n'
printf 'xmsim: *E,ASRTST (./tb.sv,6): (time 50 NS) Assertion tb.a1 has failed\n'
exit 1
EOF
cat >"$H/uvm.sh" <<'EOF'
printf 'UVM_INFO @ 0: reporter [RNTST] Running test my_test...\n'
printf 'UVM_INFO ./tb.sv(1) @ 10: uvm_test_top [TRACE] not a problem\n'
printf 'UVM_WARNING ./tb.sv(4) @ 50: uvm_test_top [CFG] no config for x\n'
printf '# UVM_ERROR ./tb.sv(5) @ 100: uvm_test_top.env.sb [SCBD] mismatch: got 3, expected 4\n'
printf 'UVM_FATAL ./tb.sv(6) @ 200: uvm_test_top [TIMEOUT] watchdog expired\n'
printf -- '--- UVM Report Summary ---\nUVM_ERROR :    1\nUVM_FATAL :    1\n'
exit 1
EOF
cat >"$H/mixed.sh" <<'EOF'
printf "** Error: tb.sv(3): (vlog-2730) Undefined variable: 'nope'.\n"
printf 'UVM_ERROR ./tb.sv(5) @ 100: uvm_test_top [SCBD] mismatch\n'
printf 'tb.sv:2:1: error: a compiler'"'"'s\n'
exit 1
EOF
printf 'questa:\n\tsh questa.sh\nvsim:\n\tsh vsim.sh\nxcelium:\n\tsh xcelium.sh\nuvm:\n\tsh uvm.sh\nmixed:\n\tsh mixed.sh\n' >"$H/Makefile"

INIT="$dir/sims.lua"
run "$H" "buildrun:make questa;buildwait;problemsdump"
check "Questa's compiler: errors and warnings at their lines, its notes not" 'button: ▶ make questa · 2 errors | Build 3 | file tb.sv | >E 3 (vlog-2730) Undefined variable: '"'"'nope'"'"'. | E 5 (vlog-2388) '"'"'x'"'"' already declared in this scope (tb). | W 4 (vlog-2623) Undefined variable: '"'"'w'"'"'.'
INIT="$dir/sims.lua"
run "$H" "buildrun:make vsim;buildwait;buildproblemsdump"
check "vsim's transcript: a message and its place on the line under it; one with none is no problem" 'W ~/hdl/tb.sv:2 (vsim-3015) [PCDPC] - Port size (1) does not match connection size (4) for port '"'"'a'"'"'. | E ~/hdl/tb.sv:6 Assertion error. | E ~/hdl/tb.sv:3 (vlog-2730) Undefined variable: '"'"'nope'"'"'. | E ~/hdl/tb.sv:4 (vsim-3421) Value 9 for idx is out of range [0:3].'
INIT="$dir/sims.lua"
run "$H" "buildrun:make xcelium;buildwait;buildproblemsdump"
check "Xcelium: its code, a column when it gives one, its notes not" 'E ~/hdl/tb.sv:3:13 [UNDIDN] '"'"'nope'"'"': undeclared identifier [12.5(IEEE)]. | W ~/hdl/tb.sv:2:8 [CUVWSP] 1 output port was not connected: | E ~/hdl/tb.sv:6 [ASRTST] (time 50 NS) Assertion tb.a1 has failed'
INIT="$dir/sims.lua"
run "$H" "buildrun:make uvm;buildwait;buildproblemsdump"
check "UVM: its id as the code, UVM_INFO and the summary not" 'W ~/hdl/tb.sv:4 [CFG] no config for x | E ~/hdl/tb.sv:5 [SCBD] mismatch: got 3, expected 4 | E ~/hdl/tb.sv:6 [TIMEOUT] watchdog expired'
INIT="$dir/sims.lua"
run "$H" "buildrun:make uvm;buildwait;key:F8;frame;statusdump"
got=${got% LF *}  # (not the file's language)
check "F8 goes to them" 'NORMAL tb.sv | × 2 ▲ 1 Ln 4, Col 1'

# Which a task's output is read with: all of them, those it names, or none.
cat "$dir/sims.lua" >"$dir/named.lua"
cat >>"$dir/named.lua" <<'EOF'
blit.build.add("Sim", {detect = "tb.sv", tasks = {
  {name = "all", cmd = "sh mixed.sh"},
  {name = "uvm only", cmd = "sh mixed.sh", problems = {"uvm"}},
  {name = "none", cmd = "sh mixed.sh", problems = "none"},
  {name = "unknown", cmd = "sh mixed.sh", problems = {"vcs"}},
}})
EOF
INIT="$dir/named.lua"
run "$H" "buildrun:all;buildwait;buildproblemsdump"
check "a task that names none: all of init.lua's, and the built-in kinds" 'E ~/hdl/tb.sv:3 (vlog-2730) Undefined variable: '"'"'nope'"'"'. | E ~/hdl/tb.sv:5 [SCBD] mismatch | E ~/hdl/tb.sv:2:1 a compiler'"'"'s'
INIT="$dir/named.lua"
run "$H" "buildrun:uvm only;buildwait;buildproblemsdump"
check "problems = {\"uvm\"}: that one alone of init.lua's" 'E ~/hdl/tb.sv:5 [SCBD] mismatch | E ~/hdl/tb.sv:2:1 a compiler'"'"'s'
INIT="$dir/named.lua"
run "$H" "buildrun:none;buildwait;buildproblemsdump"
check "problems = \"none\": nothing in its output" 'none'
INIT="$dir/named.lua"
got=$(BLIT_INIT="$dir/named.lua" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$H;frame;buildrun:unknown;buildwait;buildproblemsdump" 2>&1 >/dev/null | sed -e "s#$dir#~#g" | tail -2 | tr '\n' '/')
check "a name init.lua has no pattern for: said" 'blit: No pattern called vcs (blit.build.matcher)/E ~/hdl/tb.sv:2:1 a compiler'"'"'s/'
# A run configuration names them as a task does.
mkdir -p "$H/.blit"
printf '{"configurations": [{"name": "sim", "program": "/bin/sh", "args": ["uvm.sh"], "problems": ["questa"]}]}\n' >"$H/.blit/run.json"
INIT="$dir/sims.lua"
run "$H" "buildrun:sim;buildwait;buildproblemsdump"
check ".blit/run.json's \"problems\": a list" 'none'
INIT="$dir/sims.lua"
run "$H" "buildrun:Run: sim;buildwait;builddump"
case $got in *"(last: 1)"*) got=ran ;; esac
check "(and it ran)" 'ran'
rm -r "$H/.blit"

# What init.lua is told when a pattern is not one.
bad() {
    printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nprint(select(2, pcall(blit.build.matcher, %s)))\n' "$1" >"$dir/bad.lua"
    INIT="$dir/bad.lua"
    run "$H" "frame"
}
bad '"x", {pattern = "(%a+"}'
check "a pattern that is not one" 'blit: bad.lua:3: blit.build.matcher("x"): its pattern is not one: a ( is not closed'
bad '"x", {pattern = "(%a+): (%d+)", file = 1, line = 3}'
check "a capture it does not have" 'blit: bad.lua:3: blit.build.matcher("x"): line is the number of one of its 2 captures'
bad '"x", {pattern = "(%a+): (%d+)", line = 2}'
check "no file" 'blit: bad.lua:3: blit.build.matcher("x"): file is a capture'"'"'s number, or a file'"'"'s name'
bad '"x", {pattern = "(%a+)", file = "a.c", severity = "loud"}'
check "a severity that is neither" 'blit: bad.lua:3: blit.build.matcher("x"): severity is "error" or "warning", or a capture'"'"'s number'
bad '"x", {pattern = "(%a+)", file = 1, next = {pattern = "%b()"}}'
check "a next pattern that is not one" 'blit: bad.lua:3: blit.build.matcher("x"): next: its pattern is not one: %b and %f are not supported'

# A tool that names no file, and one a pattern gives for a fixed file.
cat >"$dir/fixed.lua" <<'EOF'
blit.opt.lsp.enabled = false
blit.opt.git.enabled = false
blit.build.matcher("qsb", {pattern = "^ERROR: :(%d+): (.*)$", file = "shader.frag", line = 1, message = 2})
blit.build.matcher("lint", {pattern = "^lint (%d+) (.*)$", file = "tb.sv", severity = "warning", line = 1})
EOF
printf 'void main() {}\n' >"$H/shader.frag"
printf 'fixed:\n\t@printf "ERROR: :1: '"'"'x'"'"' : undeclared identifier\\nlint 2 a long line\\n"; exit 1\n' >>"$H/Makefile"
INIT="$dir/fixed.lua"
run "$H" "buildrun:make fixed;buildwait;buildproblemsdump"
check "file and severity given, and no message: the line" 'E ~/hdl/shader.frag:1 '"'"'x'"'"' : undeclared identifier | W ~/hdl/tb.sv:2 lint 2 a long line'

# A run that fails with no error in its output says so (the Run button,
# the Problems tab, F8), never "No problems" under a red button.
F="$dir/fail"
mkdir -p "$F"
printf 'int a;\n' >"$F/a.c"
printf 'quiet:\n\t@echo linking; exit 3\nwarned:\n\t@echo "a.c:1:5: warning: unused"; exit 1\nfine:\n\t@echo "a.c:1:5: warning: unused"\nerrs:\n\t@echo "a.c:1:5: error: bad"; exit 1\n' >"$F/Makefile"
run "$F" "buildrun:make quiet;buildwait;problemsdump"
check "failed, nothing found: the Run button and the Problems tab say so" 'button: ▶ make quiet · failed (exit 2) | make quiet failed with exit code 2: no problem found in its output | file 0'
got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$F;frame;buildrun:make quiet;buildwait;key:F8" 2>&1 >/dev/null | tail -1)
check "and F8" 'blit: make quiet failed with exit code 2: no problem found in its output'
run "$F" "buildrun:make warned;buildwait;problemsdump"
check "failed with warnings alone: the button says it failed, and how many" 'button: ▶ make warned · failed (exit 2), 1 warning | Build 1 | file a.c | >W 1:5 unused'
run "$F" "buildrun:make fine;buildwait;problemsdump"
check "warnings, and it went well: as before" 'button: ▶ make fine · 1 warning | Build 1 | file a.c | >W 1:5 unused'
run "$F" "buildrun:make errs;buildwait;problemsdump"
check "failed with errors: they say it" 'button: ▶ make errs · 1 error | Build 1 | file a.c | >E 1:5 bad'
run "$F" "buildrun:make quiet;buildwait;buildrun:make fine;buildwait;problemsdump"
check "the next run starts afresh" 'button: ▶ make fine · 1 warning | Build 1 | file a.c | >W 1:5 unused'

# Problems from a file a tool wrote ("Read the problems from this file"):
# SARIF and GCC's JSON diagnostics, each problem's notes after it (<), and
# a log read as a build's output is. tests/problems holds real output, made
# from its src: gcc15.json and gcc15-a.json by GCC 15.3 (-fdiagnostics-format=json,
# Arch Linux), gcc16.sarif by GCC 16.2.1 (-fdiagnostics-format=sarif-file,
# Arch Linux); g++-16 and clang's analyzer make theirs here, when they are here.
samples="$(cd "$(dirname "$0")" && pwd -P)/problems"
R="$dir/read"
mkdir -p "$R/out"
cp -R "$samples/src" "$R/"
cp "$samples/gcc15.json" "$samples/gcc15-a.json" "$R/"
# (Made in a folder on that machine: its base, originalUriBaseIds, is here a folder that is not there.)
sed "s#file:///tmp/m3-match-s/#file://$dir/elsewhere/#" "$samples/gcc16.sarif" >"$R/gcc16.sarif"
run "$R" "problemsread:$R/gcc15.json;buildwait;buildproblemsdump"
check "GCC's JSON: its option as the code, its children and the notes after a problem as notes" 'E ~/read/src/d.hpp:3:27 '"'"'nothing'"'"' was not declared in this scope | E ~/read/src/c.cpp:5:9 too few arguments to function '"'"'void take(int, int)'"'"' < ~/read/src/c.cpp:2:6 declared here | E ~/read/src/c.cpp:6:18 [-fpermissive] invalid conversion from '"'"'const char*'"'"' to '"'"'int'"'"' < ~/read/src/d.hpp:2:22 initializing argument 1 of '"'"'int twice(int)'"'"' | W ~/read/src/c.cpp:4:9 [-Wunused-variable] unused variable '"'"'unused'"'"''
run "$R" "problemsread:$R/gcc16.sarif;buildwait;buildproblemsdump"
check "SARIF from GCC: relative to the file's folder when its base is not here; the include chain and notes" 'E ~/read/src/d.hpp:3:27 '"'"'nothing'"'"' was not declared in this scope < ~/read/src/c.cpp:1 included here | E ~/read/src/c.cpp:5:5 too few arguments to function '"'"'void take(int, int)'"'"' < ~/read/src/c.cpp:2:6 declared here | E ~/read/src/c.cpp:6:18 [-fpermissive] invalid conversion from '"'"'const char*'"'"' to '"'"'int'"'"' < ~/read/src/d.hpp:2:18 initializing argument 1 of '"'"'int twice(int)'"'"' | W ~/read/src/c.cpp:4:9 [-Wunused-variable] unused variable '"'"'unused'"'"''
got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$R;frame;problemsread:$R/gcc16.sarif;buildwait;problemsdump;key:F8;frame;statusdump" 2>&1 >/dev/null | sed -e "s#$dir#~#g" | tail -4 | tr '\n' '/')
got=$(printf '%s' "$got" | sed 's#\(Col 27\)[^/]*/#\1/#')
check "said, listed, counted, and F8 goes to them" 'blit: gcc16.sarif (SARIF): 4 problems/button: ▶  · 3 errors | Build 4 | file src/d.hpp | >E 3:27 '"'"'nothing'"'"' was not declared in this scope | file src/c.cpp | E 5:5 too few arguments to function '"'"'void take(int, int)'"'"' | E 6:18 invalid conversion from '"'"'const char*'"'"' to '"'"'int'"'"' | W 4:9 unused variable '"'"'unused'"'"'/blit: Problem 1 of 4: '"'"'nothing'"'"' was not declared in this scope/NORMAL src/d.hpp | × 3 ▲ 1 Ln 3, Col 27/'
# A build's log of GCC's JSON: an array for each file, make's lines between.
{ printf 'g++ -c src/a.cpp\n'; cat "$R/gcc15-a.json"; printf '\ng++ -c src/c.cpp\n'; cat "$R/gcc15.json"; } >"$R/build.log"
run "$R" "problemsread:$R/build.log;buildwait;buildproblemsdump"
got=$(printf '%s' "$got" | tr '|' '\n' | grep -c '^ *[EW] ')
check "a log of GCC's JSON, one array after another" '7'
# A plain log: as a build's output, from the log's folder.
printf 'make: Entering directory\nsrc/c.cpp:5:5: error: from a log\n' >"$R/out/build.txt"
cp -R "$R/src" "$R/out/"
run "$R" "problemsread:$R/out/build.txt;buildwait;buildproblemsdump"
check "a plain log: read as a build's output, from its folder" 'E ~/read/out/src/c.cpp:5:5 from a log'
printf '{"name": "x"}\n' >"$R/other.json"
run "$R" "problemsread:$R/other.json;buildwait"
check "JSON that is neither: said" 'blit: other.json is neither SARIF nor GCC'"'"'s JSON diagnostics'
run "$R" "problemsread:$R/nothing-here.sarif;buildwait"
check "a file that is not there" 'blit: Cannot read ~/read/nothing-here.sarif: there is no such file'
# They take the place of the last run's, and a run's failure is not said over them.
printf 'quiet:\n\t@exit 1\n' >"$R/Makefile"
run "$R" "buildrun:make quiet;buildwait;problemsread:$R/out/build.txt;buildwait;problemsdump"
check "in place of the last run's (its failure no longer said)" 'button: ▶ make quiet · 1 error | Build 1 | file out/src/c.cpp | >E 5:5 from a log'
run "$R" "problemsread:$R/out/build.txt;buildwait;buildrun:make quiet;buildwait;problemsdump"
check "and the next run's take theirs" 'button: ▶ make quiet · failed (exit 2) | make quiet failed with exit code 2: no problem found in its output | file 0'
# The command: the file in front to begin with.
run "$R" "open:$R/gcc16.sarif;command:Read the problems from this file…;key:Return;buildwait;buildproblemsdump"
got=${got%% |*}
check "the command, Enter: the file in front read" 'E ~/read/src/d.hpp:3:27 '"'"'nothing'"'"' was not declared in this scope < ~/read/src/c.cpp:1 included here'
# Made here: GCC 16's SARIF (its base the folder it ran in, which is here;
# the file moved away from it), and clang's analyzer's (its paths whole,
# its path's steps as notes).
if command -v g++-16 >/dev/null 2>&1; then gxx=g++-16; else gxx=g++; fi
if (cd "$R" && $gxx -Wall -c src/c.cpp -o /dev/null -fdiagnostics-format=sarif-file 2>/dev/null; test -f c.cpp.sarif); then
    mv "$R/c.cpp.sarif" "$R/out/c.sarif"
    run "$R" "problemsread:$R/out/c.sarif;buildwait;buildproblemsdump"
    check "SARIF from $gxx here: from its base, which is here" 'E ~/read/src/d.hpp:3:27 '"'"'nothing'"'"' was not declared in this scope < ~/read/src/c.cpp:1 included here | E ~/read/src/c.cpp:5:5 too few arguments to function '"'"'void take(int, int)'"'"' < ~/read/src/c.cpp:2:6 declared here | E ~/read/src/c.cpp:6:18 [-fpermissive] invalid conversion from '"'"'const char*'"'"' to '"'"'int'"'"' < ~/read/src/d.hpp:2:18 initializing argument 1 of '"'"'int twice(int)'"'"' | W ~/read/src/c.cpp:4:9 [-Wunused-variable] unused variable '"'"'unused'"'"''
fi
if (cd "$R" && clang --analyze --analyzer-output sarif -o out/leak.sarif src/leak.c 2>/dev/null; test -s out/leak.sarif); then
    run "$R" "problemsread:$R/out/leak.sarif;buildwait;buildproblemsdump"
    check "SARIF from clang's analyzer: its checker as the code, its path as notes" 'W ~/read/src/leak.c:6:16 [unix.Malloc] Potential leak of memory pointed to by '"'"'p'"'"' < ~/read/src/leak.c:4:14 Memory is allocated < ~/read/src/leak.c:5:9 Assuming '"'"'k'"'"' is > 2 < ~/read/src/leak.c:5:5 Taking true branch | W ~/read/src/leak.c:17:14 [core.DivideZero] Division by zero < ~/read/src/leak.c:14:5 '"'"'z'"'"' initialized to 0 < ~/read/src/leak.c:15:9 Assuming '"'"'k'"'"' is 0 < ~/read/src/leak.c:15:5 Taking false branch'
fi

exit $failed
