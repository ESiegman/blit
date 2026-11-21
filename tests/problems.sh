#!/bin/sh
# A build's problems from more than the built-in kinds: init.lua's own
# patterns (blit.build.matcher, the manual's for Questa, Xcelium and UVM
# taken from the manual itself) and which a task names (problems = ...).
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

exit $failed
