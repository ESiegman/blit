#!/bin/sh
# Crash dumps in a run's output decoded in place (src/crashscan.*): a
# firmware built here from tests/elf's sources (Arm's GNU toolchain for a
# Cortex-M0+, else this machine's cc where it makes ELFs), and dumps printed
# by tasks in the shapes ESP-IDF v6.1.1 and Zephyr v4.5.0 print them (their
# sources are cited in crashscan.cpp; no board is run here), with that
# firmware's addresses in them. Each address's frames go into the terminal
# under the dump's line, dim; the innermost frame of the project's own code
# is a problem with the frames as its notes; Cmd+click on a decoded line
# opens its place.
#
#   tests/crash.sh path/to/blit            run the cases
#   tests/crash.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/crash.sh path/to/blit [--print]}
print=${2:-}
src=$(cd "$(dirname "$0")/.." && pwd -P)
elftest="$(cd "$(dirname "$blit")" && pwd)/blit_elf_test"
[ -x "$elftest" ] || { echo "FAIL: no $elftest"; exit 1; }
real_home=$HOME
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
export BLIT_RECENT_FILE="$dir/recent"
export HOME="$dir/home"
export SHELL=/bin/sh
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

# The firmware: the project's own code in main/, a fetched component's in
# managed_components/, built in the project (so that its debug information
# names its files there).
P="$dir/proj"
mkdir -p "$P/main" "$P/managed_components/ring" "$P/build" "$P/.blit"
cp "$src/tests/elf/main.c" "$src/tests/elf/motor.c" "$src/tests/elf/motor.h" "$P/main/"
cp "$src/tests/elf/ring.c" "$P/managed_components/ring/"
gcc=$(command -v arm-none-eabi-gcc 2>/dev/null || ls -d "$real_home"/.local/opt/arm-gnu-toolchain-*/bin/arm-none-eabi-gcc 2>/dev/null | head -1)
if [ -n "$gcc" ]; then
    (cd "$P" && "$gcc" -mcpu=cortex-m0plus -mthumb -O2 -g -nostdlib -e _start main/main.c main/motor.c managed_components/ring/ring.c -o build/fw.elf) || failed=1
elif [ "$(uname -s)" = Linux ] && command -v cc >/dev/null; then
    (cd "$P" && cc -O2 -g -nostdlib -static -no-pie -fno-stack-protector -e _start main/main.c main/motor.c managed_components/ring/ring.c -o build/fw.elf) || failed=1
else
    echo "SKIP: crash.sh needs arm-none-eabi-gcc (or, on Linux, cc) to build its firmware, and this machine has neither"
    exit 0
fi
[ -f "$P/build/fw.elf" ] || { echo "FAIL: the firmware was not built"; exit 1; }

# Its addresses: where the write through a null pointer is (store, inlined
# into pid_step), the call in motor_run, the call in main, and ring_put.
at() { "$elftest" at "$P/build/fw.elf" "$@" | sed 's/^0x//'; }
STORE=$(at motor.c 9)
CALL=$(at motor.c 22)
MAIN=$(at main.c 14)
RING=$(at ring.c 5)
for a in "$STORE" "$CALL" "$MAIN" "$RING"; do [ -n "$a" ] || { echo "FAIL: an address in the firmware was not found"; exit 1; }; done
pad() { printf '%08x' "0x$1"; }

# The board's image in run.json, and each dump as a task's output.
printf '{ "configurations": [ { "name": "fw", "program": "build/fw.elf", "board": { "chip": "esp32s3", "elf": "build/fw.elf" } } ] }\n' >"$P/.blit/run.json"
cat >"$P/xtensa.sh" <<EOF
printf 'I (312) main_task: Calling app_main()\r\n'
printf "Guru Meditation Error: Core  0 panic'ed (StoreProhibited). Exception was unhandled.\r\n\r\n"
printf 'Core  0 register dump:\r\nPC      : 0x$(pad "$STORE")  PS      : 0x00060730  A0      : 0x8$(pad "$CALL" | cut -c2-)  A1      : 0x3fc9a0a0  \r\n'
printf 'EXCVADDR: 0x00000000  LBEG    : 0x400556d5  LEND    : 0x400556e5  LCOUNT  : 0xfffffffd  \r\n'
printf '\r\n\r\nBacktrace: 0x$(pad "$STORE"):0x3fc9a0a0 0x$(pad "$CALL"):0x3fc9a0c0 0x$(pad "$MAIN"):0x3fc9a0e0 0x40379a2d:0x3fc9a100 |<-CORRUPTED\r\n\r\n'
printf '\r\n\r\nELF file SHA256: 2bd7d9c6b\r\n\r\nRebooting...\r\n'
EOF
cat >"$P/riscv.sh" <<EOF
printf "Guru Meditation Error: Core  0 panic'ed (Store access fault). Exception was unhandled.\r\n\r\n"
printf 'Core  0 register dump:\r\nMEPC    : 0x$(pad "$STORE")  RA      : 0x$(pad "$CALL")  SP      : 0x3fc8f3b0  GP      : 0x3fc8b200  \r\n'
printf 'MSTATUS : 0x00001881  MTVEC   : 0x40380001  MCAUSE  : 0x00000007  MTVAL   : 0x00000000  \r\nMHARTID : 0x00000000  \r\n'
printf '\r\nStack memory:\r\n3fc8f3b0: 0x00000000 0x00000000\r\n\r\nELF file SHA256: 2bd7d9c6b\r\n'
EOF
cat >"$P/zephyr.sh" <<EOF
printf '[00:00:00.010,000] <err> os: ***** HARD FAULT *****\r\n'
printf '[00:00:00.010,000] <err> os: r0/a1:  0x00000000  r1/a2:  0x00000004  r2/a3:  0x00000000\r\n'
printf '[00:00:00.010,000] <err> os: r3/a4:  0x00000000 r12/ip:  0x00000000 r14/lr:  0x$(pad "$CALL")\r\n'
printf '[00:00:00.010,000] <err> os:  xpsr:  0x61000000\r\n'
printf '[00:00:00.010,000] <err> os: Faulting instruction address (r15/pc): 0x$(pad "$STORE")\r\n'
printf '[00:00:00.010,000] <err> os: >>> ZEPHYR FATAL ERROR 0: CPU exception on CPU 0\r\n'
printf '[00:00:00.010,000] <err> os: Current thread: 0x20000598 (main)\r\n'
EOF
cat >"$P/own.sh" <<EOF
printf 'tick 41\r\nPANIC: hard fault\r\nbt: 0x$STORE 0x$CALL\r\nbt: 0x$MAIN\r\nhalted\r\n'
EOF
cat >"$P/abort.sh" <<EOF
printf '\r\nabort() was called at PC 0x$CALL on core 0\r\n\r\n\r\nBacktrace: 0x$(pad "$CALL"):0x3fc9a0c0 0x$(pad "$MAIN"):0x3fc9a0e0\r\n\r\n'
printf 'Rebooting...\r\n'
EOF
cat >"$P/vendor.sh" <<EOF
printf "Guru Meditation Error: Core  0 panic'ed (StoreProhibited). Exception was unhandled.\r\n"
printf '\r\n\r\nBacktrace: 0x$(pad "$RING"):0x3fc9a0a0 0x$(pad "$CALL"):0x3fc9a0c0\r\n\r\n'
EOF
cat >"$P/twice.sh" <<EOF
for i in 1 2; do
printf "Guru Meditation Error: Core  0 panic'ed (StoreProhibited). Exception was unhandled.\r\n"
printf '\r\n\r\nBacktrace: 0x$(pad "$STORE"):0x3fc9a0a0 0x$(pad "$CALL"):0x3fc9a0c0\r\n\r\nRebooting...\r\n'
done
EOF
cat >"$P/wdt.sh" <<EOF
printf 'E (5312) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:\r\n'
printf 'E (5312) task_wdt: CPU 0: main\r\n\r\nBacktrace: 0x$(pad "$CALL"):0x3fc9a0c0 0x$(pad "$MAIN"):0x3fc9a0e0\r\n\r\n'
EOF
# idf.py monitor decodes its own: a stand-in that prints the dump and its decoding as esp-idf-monitor does.
cat >"$P/idf.py" <<EOF
printf "Guru Meditation Error: Core  0 panic'ed (StoreProhibited). Exception was unhandled.\r\n"
printf '\r\n\r\nBacktrace: 0x$(pad "$STORE"):0x3fc9a0a0 0x$(pad "$CALL"):0x3fc9a0c0\r\n'
printf -- '--- 0x$(pad "$STORE"): pid_step at $P/main/motor.c:15\r\n\r\nRebooting...\r\n'
EOF
cat >"$P/.blit/tasks.json" <<'EOF'
{ "tasks": [
  { "label": "xtensa", "command": "sh xtensa.sh" },
  { "label": "riscv", "command": "sh riscv.sh" },
  { "label": "zephyr", "command": "sh zephyr.sh" },
  { "label": "own", "command": "sh own.sh" },
  { "label": "abort", "command": "sh abort.sh" },
  { "label": "vendor", "command": "sh vendor.sh" },
  { "label": "twice", "command": "sh twice.sh" },
  { "label": "wdt", "command": "sh wdt.sh" },
  { "label": "monitor", "command": "sh ./idf.py monitor" } ] }
EOF

# run SCRIPT: the run's output (stderr), the folders made here written ~,
# and addresses without their leading zeros.
run() {
    BLIT_INIT="$dir/init.lua" "$blit" --offscreen 4800x1800 --shot "$dir/shot.bmp" --script "workdir:$P;frame;$1" 2>&1 >/dev/null |
        sed -e "s#$dir#~#g" -E -e 's/0x0+([0-9a-f])/0x\1/g'
}
# decoded TASK [FROM]: the task run, and the decoded frames on the
# terminal's rows after the one with FROM in it (the dump's line), to the
# first row after them that is not one.
decoded() {
    run "buildrun:$1;buildwait;frame;key:cmd+alt+Down;frame;termdump" | tail -1 | tr '|' '\n' |
        awk -v from="${2:-Backtrace:}" 'index($0, from) { on = 1; next } on && /^(0x|MEPC 0x|RA 0x|pc 0x|lr 0x| \(inlined by\))/ { printf "%s%s", sep, $0; sep = " | "; next } on && sep != "" { exit }'
}
problems() { run "buildrun:$1;buildwait;frame;scandump" | sed -n '/^problems:/,$p' | tr '\n' '|' | sed 's/|$//'; }
x() { printf '0x%x' "0x$1"; }  # (an address as the dumps above are made to read here)

M="~/proj/main"
got=$(decoded xtensa)
check "ESP-IDF on Xtensa: each PC of the backtrace decoded under it (inlined frames too); a PC not in the ELF (the ROM's) left out" \
    "$(x "$STORE"): store at $M/motor.c:9 |  (inlined by) pid_step at $M/motor.c:15 | $(x "$CALL"): motor_run at $M/motor.c:22 | $(x "$MAIN"): main at $M/main.c:14"
got=$(problems xtensa)
check "its problem: the innermost frame of the project's own code, the exception as its code, every frame a note" \
    "problems: 1|E main/motor.c:9:13 [StoreProhibited] Crash at motor.c:9 in store (StoreProhibited)|  note main/motor.c:9:13 store (inlined)|  note main/motor.c:15:5 pid_step|  note main/motor.c:22:9 motor_run|  note main/main.c:14:5 main"
got=$(decoded riscv "register dump:")
check "ESP-IDF on RISC-V: the register dump's MEPC and RA decoded under their line" \
    "MEPC $(x "$STORE"): store at $M/motor.c:9 |  (inlined by) pid_step at $M/motor.c:15 | RA $(x "$CALL"): motor_run at $M/motor.c:22"
got=$(problems riscv)
check "its problem" "problems: 1|E main/motor.c:9:13 [Store access fault] Crash at motor.c:9 in store (Store access fault)|  note main/motor.c:9:13 store (inlined)|  note main/motor.c:15:5 pid_step|  note main/motor.c:22:9 motor_run"
got=$(decoded zephyr "r12/ip:")
check "Zephyr on Cortex-M: lr decoded under its line" "lr $(x "$CALL"): motor_run at $M/motor.c:22"
got=$(run "buildrun:zephyr;buildwait;frame;key:cmd+alt+Down;frame;termdump" | tail -1 | tr '|' '\n' | grep -A1 'Faulting instruction' | tail -1)
check "and the faulting instruction's address under its own" "pc $(x "$STORE"): store at $M/motor.c:9"
got=$(problems zephyr)
check "its problem, at the faulting instruction, with the reason its fatal error gives" \
    "problems: 1|E main/motor.c:9:13 [CPU exception] Crash at motor.c:9 in store (CPU exception)|  note main/motor.c:9:13 store (inlined)|  note main/motor.c:15:5 pid_step|  note main/motor.c:22:9 motor_run"

# The program's own format: a pattern in run.json, its lines one dump.
printf '{ "configurations": [ { "name": "fw", "program": "build/fw.elf", "board": { "elf": "build/fw.elf" }, "crash": "^bt:(.*)$" } ] }\n' >"$P/.blit/run.json"
got=$(run "buildrun:own;buildwait;frame;key:cmd+alt+Down;frame;termdump" | tail -1 | tr '|' '\n' | sed -n '/^bt:/,/^halted/p' | tr '\n' '|' | sed 's/|$//')
check "the program's own panic format (a pattern in run.json): each line's addresses under it" \
    "bt: 0x$STORE 0x$CALL|$(x "$STORE"): store at $M/motor.c:9| (inlined by) pid_step at $M/motor.c:15|$(x "$CALL"): motor_run at $M/motor.c:22|bt: 0x$MAIN|$(x "$MAIN"): main at $M/main.c:14|halted"
got=$(problems own)
check "and the lines one after another one crash" \
    "problems: 1|E main/motor.c:9:13 [panic] Crash at motor.c:9 in store|  note main/motor.c:9:13 store (inlined)|  note main/motor.c:15:5 pid_step|  note main/motor.c:22:9 motor_run|  note main/main.c:14:5 main"
printf '{ "configurations": [ { "name": "fw", "program": "build/fw.elf", "crash": ["^bt:(.*)$", "%%b()"] } ] }\n' >"$P/.blit/run.json"
got=$(run "builddump" | grep '^blit: ' | tail -1)
check "a pattern that is not one is said" "blit: .blit/run.json: fw's crash pattern \"%b()\": %b and %f are not supported"

# No board ELF named: the newest ELF the build made.
printf '{ "configurations": [ { "name": "fw", "program": "build/fw.elf" } ] }\n' >"$P/.blit/run.json"
got=$(decoded abort "abort() was called")
check "abort()'s PC decoded; with no board's elf, the newest the build made is used" "$(x "$CALL"): motor_run at $M/motor.c:22"
got=$(problems vendor)
check "a fetched component's frame (managed_components) is not the project's own: the problem is at the first that is" \
    "problems: 1|E main/motor.c:22:9 [StoreProhibited] Crash at motor.c:22 in motor_run (StoreProhibited)|  note managed_components/ring/ring.c:5:11 ring_put|  note main/motor.c:22:9 motor_run"
got=$(run "buildrun:twice;buildwait;frame;problemsdump" | tail -1)
check "the same crash again (a board that crashes at each start) is the one problem, counted" \
    "button: ▶ twice · 1 error | Build 1 | file main/motor.c | >▸E 9:13 Crash at motor.c:9 in store (StoreProhibited) ×2"
got=$(decoded wdt)
check "a backtrace with no crash (the task watchdog's) is decoded" "$(x "$CALL"): motor_run at $M/motor.c:22 | $(x "$MAIN"): main at $M/main.c:14"
got=$(problems wdt)
check "but is no problem" "problems: 0"
got=$(decoded monitor)
check "idf.py monitor decodes its own: nothing added" ""
got=$(problems monitor)
check "the problem still is" "problems: 1|E main/motor.c:9:13 [StoreProhibited] Crash at motor.c:9 in store (StoreProhibited)|  note main/motor.c:9:13 store (inlined)|  note main/motor.c:15:5 pid_step|  note main/motor.c:22:9 motor_run"

# Cmd+click on a decoded line opens its place.
got=$(run "buildrun:xtensa;buildwait;frame;termclick:main/motor.c:22;frame;statusdump" | tail -1)
got=${got% LF *}
check "Cmd+click on a decoded frame opens the file at its line" "main/motor.c | × 1 ▲ 0 Ln 22, Col 1 Spaces: 4"

# No ELF anywhere: said once, under the dump, and no problem.
rm "$P/build/fw.elf"
got=$(run "buildrun:xtensa;buildwait;frame;key:cmd+alt+Down;frame;termdump" | tail -1 | tr '|' '\n' | grep -c "Not decoded: no ELF (\*.elf, \*.axf) under ~/proj, and no board elf in .blit/run.json")
check "no ELF to decode with: said once" "1"
got=$(problems xtensa)
check "and no problem" "problems: 0"

[ "$failed" = 0 ] && echo "crash: all passed"
exit "$failed"
