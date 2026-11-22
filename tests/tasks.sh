#!/bin/sh
# A folder's own tasks (.blit/tasks.json) in the Build tab: listed first,
# with their kinds; the variables their commands name, quoted as each
# place needs; long tasks in tabs of their own, which other runs leave
# alone, asked about before anything ends them, with their time on the Run
# button and their rows; Stop, which keeps the text and the problems;
# and BuildStart and BuildDone for init.lua.
#
#   tests/tasks.sh path/to/blit            run the cases
#   tests/tasks.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/tasks.sh path/to/blit [--print]}
print=${2:-}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
export BLIT_RECENT_FILE="$dir/recent"
export HOME="$dir/home"
export SHELL=/bin/sh  # (the tasks' commands run by the same shell everywhere)
mkdir -p "$HOME"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run FOLDER SCRIPT: the last line the run printed (the folders made here
# written ~, a run's time T). INIT, if set, is the init.lua for this run only.
INIT=
run() {
    got=$(BLIT_INIT="${INIT:-$dir/init.lua}" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$1;frame;$2" 2>&1 >/dev/null |
        sed -e "s#$dir#~#g" -e 's/after [0-9.]* s\]/after T]/' -e 's/in [0-9.]* s\]/in T]/' | tail -1)
    INIT=
}

# The folder's tasks: first, before its run configurations and what it
# builds with; a kind on each row; init.lua sees them.
P="$dir/proj"
mkdir -p "$P/.blit" "$P/build"
printf 'all:\n\ttrue\n' >"$P/Makefile"
printf '{ "configurations": [ { "name": "hello", "program": "build/hello" } ] }\n' >"$P/.blit/run.json"
cat >"$P/.blit/tasks.json" <<'EOF'
{ "tasks": [
  { "label": "Flash", "command": "printf 'flashing %s\\n'", "args": ["the board"], "kind": "flash" },
  { "label": "Sim", "command": "printf 'simulating\\n'", "long": true, "kind": "simulation" },
  { "label": "Here", "command": "pwd", "cwd": "build", "env": { "WHO": "a b" } } ] }
EOF
run "$P" "builddump"
check "tasks.json's tasks first; the run configuration still the Run button's" 'Tasks: Flash Sim Here | Run: >hello | Make: make make all'
run "$P" "lua:local s = '' for _, t in ipairs(blit.build.tasks()) do if t.system == 'Tasks' then s = s .. t.name .. '=' .. t.kind .. '/' .. tostring(t.long) .. '/' .. t.cmd .. ' ' end end print(s)"
check "blit.build.tasks(): their kind, whether they are long, and their command (env, then each argument a word)" "blit: Flash=flash/false/printf 'flashing %s\\n' 'the board' Sim=simulation/true/printf 'simulating\\n' Here=/false/env 'WHO=a b' pwd "
run "$P" "buildpanel;builddump:rows"
check "a task's row: its kind, then its command" "Flash: flash · printf 'flashing %s\\n' 'the board' | Sim: simulation · printf 'simulating\\n' | Here: env 'WHO=a b' pwd | hello: ~/proj/build/hello | make: make | make all: make all"
run "$P" "buildpanel;buildrun:Flash;buildwait;builddump:rows"
check "and how its last run went, after its kind" "Flash: flash · done | Sim: simulation · printf 'simulating\\n' | Here: env 'WHO=a b' pwd | hello: ~/proj/build/hello | make: make | make all: make all"
run "$P" "buildrun:Flash;buildwait;builddump:text"
check "its arguments, each one word" '|flashing the board|[done in T]'
run "$P" "buildrun:Here;buildwait;builddump:text"
check "in its folder" '|~/proj/build|[done in T]'
# (That list alone, as Zed has it, is read too.)
mkdir -p "$dir/bare/.blit"
printf '[ { "label": "Only", "command": "true" } ]\n' >"$dir/bare/.blit/tasks.json"
run "$dir/bare" "builddump"
check "a list alone: the tasks; with nothing else, the first is the Run button's" 'Tasks: >Only'

# A file that is wrong says so, as run.json does.
B="$dir/bad"
mkdir -p "$B/.blit"
said() {
    got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$B;frame;builddump" 2>&1 >/dev/null | grep '^blit: ' | tail -1)
}
printf '{ "tasks": [ \n' >"$B/.blit/tasks.json"
said
check "not JSON" 'blit: .blit/tasks.json is not JSON'
printf '{ "jobs": [] }\n' >"$B/.blit/tasks.json"
said
check "no list" 'blit: .blit/tasks.json has no "tasks" list'
printf '{ "tasks": [ { "label": "x" } ] }\n' >"$B/.blit/tasks.json"
said
check "a task without its command" 'blit: .blit/tasks.json: a task needs a "label" and a "command"'
printf '{ "tasks": [ { "label": "x", "command": "true", "kind": "bake" } ] }\n' >"$B/.blit/tasks.json"
said
check "a kind that is none of them" 'blit: .blit/tasks.json: x'"'"'s kind is one of "flash", "program", "synthesis", "simulation", "gpu-profile"'

# Variables: the file in front (a name with a space, quotes and a $ in
# it), its cursor's line and word, what is selected, the folder; each put
# in as the place it is in needs: bare, in '...', in "...", or as an
# argument of the list.
V="$dir/vars"
mkdir -p "$V/.blit" "$V/src"
F="$V/src/it's a \"q\" \$x.c"
printf 'int a;\nint counter = 0;\n' >"$F"
cat >"$V/.blit/tasks.json" <<'EOF'
{ "tasks": [
  { "label": "Bare", "command": "printf '<%s>\\n' ${file} x${line}y" },
  { "label": "Single", "command": "printf '<%s>\\n' 'in ${file} quotes'" },
  { "label": "Double", "command": "printf '<%s>\\n' \"in ${file} quotes\"" },
  { "label": "Args", "command": "printf '<%s>\\n'", "args": ["${fileDirname}", "${fileBasename}", "${fileBasenameNoExtension}", "${line}:${word}", "${workdir}"] },
  { "label": "Picked", "command": "printf '<%s>\\n' \"${selection}\" ${selection}" },
  { "label": "Left", "command": "printf '<%s>\\n' \\${file} \"${HOME}\" ${nothing}" } ] }
EOF
here="open:$F;key:Down;key:Right;key:Right;key:Right;key:Right"
Fs="~/vars/src/it's a \"q\" \$x.c"  # (as the run prints it, the folder written ~)
run "$V" "$here;buildrun:Bare;buildwait;builddump:text"
check "bare: one word, whatever is in it" "|<$Fs>|<x2y>|[done in T]"
run "$V" "$here;buildrun:Single;buildwait;builddump:text"
check "in single quotes" "|<in $Fs quotes>|[done in T]"
run "$V" "$here;buildrun:Double;buildwait;builddump:text"
check "in double quotes" "|<in $Fs quotes>|[done in T]"
run "$V" "$here;buildrun:Args;buildwait;builddump:text"
check "in the arguments; its folder, name and name without the extension; the line and the word at the cursor; the folder" "|<~/vars/src>|<it's a \"q\" \$x.c>|<it's a \"q\" \$x>|<2:counter>|<~/vars>|[done in T]"
run "$V" "$here;key:shift+End;buildrun:Picked;buildwait;builddump:text"
check "what is selected" '|<counter = 0;>|<counter = 0;>|[done in T]'
run "$V" "$here;buildrun:Picked;buildwait;builddump:text"
check "nothing selected: empty, still a word" '|<>|<>|[done in T]'
run "$V" "$here;buildrun:Left;buildwait;builddump:text"
check "\\\${file}, and names that are not blit's, left for the shell" '|<${file}>|<~/home>|[done in T]'

# Long tasks: a tab of their own, which another run leaves alone; asked
# about before anything ends them once they have run more than 10 s; their
# time on the Run button and their rows.
L="$dir/long"
mkdir -p "$L/.blit"
printf 'all:\n\t@echo built\n' >"$L/Makefile"
cat >"$L/.blit/tasks.json" <<'EOF'
{ "tasks": [
  { "label": "Sim", "command": "printf 'simulating\\n'; sleep 30", "long": true, "kind": "simulation" },
  { "label": "Talk", "command": "printf 'src/a.c:1:5: error: boom\\n'; sleep 30" },
  { "label": "Quick", "command": "printf 'quick\\n'", "long": true } ] }
EOF
mkdir -p "$L/src"
printf 'int a;\n' >"$L/src/a.c"
run "$L" "buildrun:Sim;buildrun:make;buildwait;builddump"
check "a long task left running while another task runs (and ends)" 'Tasks: Sim Talk Quick | Make: >make make all (last: 0) [Sim: running 0:00]'
run "$L" "buildrun:Sim;buildrun:make;buildwait;buildrun:make all;buildwait;dump"
case "$got" in *'Sim'*'make all'*) got='both tabs' ;; esac
check "its tab beside the other runs' terminal, which the next run used again" 'both tabs'
run "$L" "buildrun:Sim;buildrun:Sim;asked"
check "run again within 10 s: not asked" 'nothing asked'
run "$L" "buildrun:Sim;buildolder:72;buildrun:Sim;asked"
check "run again after more than 10 s: asked first" 'Sim has been running for 1:12. End it and run it again?'
run "$L" "buildrun:Sim;buildolder:72;keyof:Stop the build;asked"
check "Stop after more than 10 s: asked first" 'Sim has been running for 1:12. Stop it?'
run "$L" "buildrun:Sim;buildrun:make;buildwait;quit;asked"
check "quitting: asked, as for any program still running" '1 program is still running in a terminal. Quit and end it?'
run "$L" "buildrun:Sim;buildolder:72;problemsdump"
check "its time on the Run button" 'button: ■ Stop Sim 1:12 | file 0'
run "$L" "buildpanel;buildrun:Sim;buildolder:3725;builddump:rows"
check "and on its row (over an hour: hours too)" "Sim: simulation · running… 1:02:05 | Talk: printf 'src/a.c:1:5: error: boom\\n'; sleep 30 | Quick: printf 'quick\\n' | make: make | make all: make all"
run "$L" "buildpanel;buildrun:Sim;buildolder:65;buildrun:make;buildwait;builddump:rows"
check "its row with its time while another run is the last" "Sim: simulation · running… 1:05 | Talk: printf 'src/a.c:1:5: error: boom\\n'; sleep 30 | Quick: printf 'quick\\n' | make: done | make all: make all"
run "$L" "buildrun:Sim;frame;tickdump"
case "$got" in [0-9]*) [ "$got" -le 1001 ] && got='within a second' ;; esac
check "drawn again within a second while it runs and blit is in front" 'within a second'
run "$L" "buildrun:Sim;frame;blur;tickdump"
check "behind another window: nothing ticks" '-1'
run "$L" "buildrun:Sim;frame;blur;focus;tickdump"
case "$got" in [0-9]*) got='ticks' ;; esac
check "in front again: it does" 'ticks'
run "$L" "buildrun:make;buildwait;frame;tickdump"
check "no long task: nothing ticks" '-1'
run "$L" "buildrun:Quick;buildwait;builddump"
check "a long task that ends: its tab stays, saying how" 'Tasks: Sim Talk >Quick | Make: make make all (last: 0) [Quick: ended 0]'

# Stop: the program ends, its tab stays with what it wrote (saying it was
# stopped), and so do its problems; it is no failure.
run "$L" "buildrun:Talk;wait:500;keyof:Stop the build;buildwait;builddump:text"
check "Stop keeps the text, and says so" '|src/a.c:1:5: error: boom|[stopped after T]'
run "$L" "buildrun:Talk;wait:500;keyof:Stop the build;buildwait;problemsdump"
check "and the problems" 'button: ▶ Talk · 1 error | Build 1 | file src/a.c | >E 1:5 boom'
run "$L" "buildrun:Talk;wait:500;keyof:Stop the build;buildwait;builddump"
case "$got" in *'(last: '*') (stopped)') got='stopped' ;; esac
check "the run: stopped" 'stopped'
run "$L" "buildpanel;buildrun:Talk;wait:500;keyof:Stop the build;buildwait;builddump:rows"
check "its row says so" "Sim: simulation · printf 'simulating\\n'; sleep 30 | Talk: stopped | Quick: printf 'quick\\n' | make: make | make all: make all"
run "$L" "buildrun:Sim;wait:300;keyof:Stop the build;buildwait;builddump"
check "a long task stopped within 10 s: not asked, its tab kept" 'Tasks: >Sim Talk Quick | Make: make make all (last: 130) (stopped) [Sim: stopped]'

# BuildStart and BuildDone: the task's name (as the pattern is matched
# against), kind and command; how it went, its problems and whether it was
# stopped.
cat >"$dir/events.lua" <<'EOF'
blit.opt.lsp.enabled = false
blit.opt.git.enabled = false
blit.on("BuildStart", function(e) print("start " .. e.match .. " " .. e.data.name .. " [" .. e.data.kind .. "] " .. e.data.command) end)
blit.on("BuildDone", { pattern = "T*" }, function(e)
  local d = e.data
  print("done " .. d.name .. " [" .. d.kind .. "] " .. d.status .. " " .. d.errors .. " " .. d.warnings .. " " .. tostring(d.stopped) .. " " .. type(d.seconds))
end)
EOF
INIT="$dir/events.lua"
run "$L" "buildrun:Sim"
check "BuildStart" "blit: start Sim Sim [simulation] printf 'simulating\\n'; sleep 30"
INIT="$dir/events.lua"
run "$L" "buildrun:Talk;wait:500;keyof:Stop the build;buildwait"
case "$got" in 'blit: done Talk [] '*' 1 0 true number') got='stopped, 1 error' ;; esac
check "BuildDone: the status, errors, warnings, stopped" 'stopped, 1 error'
INIT="$dir/events.lua"
run "$L" "buildrun:Quick;buildwait"
check "BuildDone for another name: not this pattern's" "blit: start Quick Quick [] printf 'quick\\n'"
mkdir -p "$dir/t2/.blit"
printf '{ "tasks": [ { "label": "Tfail", "command": "printf \\"x.c:2:1: warning: w\\\\n\\"; exit 3", "kind": "program" } ] }\n' >"$dir/t2/.blit/tasks.json"
printf 'a\nb\n' >"$dir/t2/x.c"
INIT="$dir/events.lua"
run "$dir/t2" "buildrun:Tfail;buildwait"
check "a task that fails" 'blit: done Tfail [program] 3 0 1 false number'

exit $failed
