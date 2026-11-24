#!/bin/sh
# The fault report (fault.cpp, appdebug.cpp's board part): a board stopped
# in a fault handler shows a "Fault" section at the top of the Debug tab,
# what the fault is in a line, and "Show the fault report" opens it wide in
# a tab, where Enter on the stacked PC opens its source and disassembly.
# Against QEMU: Cortex-M3 (mps2-an385) UsageFaults, bus faults, a
# MemManage fault and a HardFault escalated, on the main stack and the
# process stack; a Cortex-M4 with its FPU frame (mps2-an386); a Cortex-M33
# (mps2-an505, ARMv8-M); a Cortex-M0 with no fault status registers
# (microbit); RISC-V (sifive_e) access faults and illegal instructions.
# Each firmware's handler spins, and the session is paused there (F6), as
# one looks at a board that has hung. Skipped, saying why, where QEMU, a
# toolchain or a gdb that speaks DAP (14 or later, built with Python) is
# not installed.
#
#   tests/faults.sh path/to/blit            run the cases
#   tests/faults.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/faults.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd -P)
dap_gdb() {  # the first of these that is built with Python
    for g in "$@"; do
        command -v "$g" >/dev/null 2>&1 && [ "$("$g" -q -batch -ex 'python print(41 + 1)' 2>/dev/null)" = 42 ] && { command -v "$g"; return 0; }
    done
    return 1
}
cc=$(command -v arm-none-eabi-gcc || ls -d "$HOME"/.local/opt/arm-gnu-toolchain-*/bin/arm-none-eabi-gcc 2>/dev/null | tail -1)
gdb=$(dap_gdb arm-none-eabi-gdb gdb-multiarch)
rvcc=$(command -v riscv64-elf-gcc || command -v riscv64-unknown-elf-gcc || command -v riscv-none-elf-gcc)
rvgdb=$(dap_gdb riscv64-elf-gdb riscv64-unknown-elf-gdb riscv-none-elf-gdb gdb-multiarch)
dir=$(mktemp -d)
trap 'pkill -f "$dir/" 2>/dev/null; rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
P="$dir/fw"
mkdir -p "$P/.blit"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua" BLIT_RECENT_FILE="$dir/recent" HOME="$dir/home" REPLAY_HOLD="$dir/hold"
mkdir -p "$HOME" "$REPLAY_HOLD"

failed=0
ran=0
check() {
    ran=$((ran + 1))
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run SOURCE SCRIPT: the lines it printed, joined by " ;; ". What the
# compiler's code decides (where the code is and the stack, what the
# registers held) is written #: the source lines the stacked PC and LR
# are at, and every word of the status registers, are what is checked.
run() {
    got=$(cd "$P" && "$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "workdir:$P;$2" "$P/$1" 2>&1 >/dev/null | sed -e "s#$dir#@#g" | tr "\t" " " |
        sed -E -e 's/(stacked PC|mepc|stacked at|SP before) 0x[0-9a-f]{8}/\1 #/g' \
            -e 's/(R0|R1|R2|R3|R12|LR|PC|xPSR|mepc|mtvec) ( *)0x[0-9a-f]{8}/\1 \2#/g' -e 's/; a word of padding above the frame//' \
            -e 's/^0x[0-9a-f]+ /# /' |
        awk 'NR > 1 { printf " ;; " } { printf "%s", $0 }')
}
# board MACHINE [GDB]: the run configuration "fw", its program fw.elf, QEMU's MACHINE.
board() {
    g=${2:+, \"gdb\": \"$2\"}
    printf '{ "configurations": [ { "name": "fw", "program": "fw.elf", "board": { "chip": "%s", "debug": { "preset": "qemu"%s } } } ] }\n' "$1" "$g" >"$P/.blit/run.json"
}
# F5 to main, on into the fault, paused in its handler (F6): the Debug
# tab's line (FAULT), and the report's lines too (REPORT); then Shift+F5.
GO="key:F5;debugwait:stopped;key:F5;wait:300;key:F6;debugwait:stopped"
FAULT="$GO;debugfault;key:shift+F5;debugwait:idle"
REPORT="$GO;debugfault;debugfault:report;key:shift+F5;debugwait:idle"

# ---- Cortex-M
if [ -z "$cc" ]; then
    echo "faults: Cortex-M skipped: no arm-none-eabi-gcc (Arm's GNU toolchain)"
elif ! command -v qemu-system-arm >/dev/null 2>&1; then
    echo "faults: Cortex-M skipped: no qemu-system-arm (QEMU)"
elif [ -z "$gdb" ]; then
    echo "faults: Cortex-M skipped: no arm-none-eabi-gdb or gdb-multiarch built with Python (gdb -i dap needs it)"
else
    cp "$here/faults/cm.c" "$P/"
    # fw MODE CPU LINKER [FLAGS]: fw.elf built.
    fw() { (cd "$P" && "$cc" -mcpu="$2" -mthumb -g -O0 -nostdlib -Wl,--no-warn-rwx-segments -T "$here/$3" -DMODE="$1" ${4:-} -o fw.elf cm.c) || exit 1; }

    board mps2-an385 "$gdb"
    fw 1 cortex-m3 boards/cm.ld
    run cm.c "$REPORT"
    check "Cortex-M3, UDF: a UsageFault, UNDEFINSTR, on the main stack; the report" \
        'UsageFault: UNDEFINSTR, undefined instruction; main stack (MSP); no FPU frame; stacked PC # in cm.c:32 ;; UsageFault: UNDEFINSTR, undefined instruction  (Cortex-M3 r0p1) | Fault status | CFSR        0x00010000 | UNDEFINSTR (UsageFault): undefined instruction | HFSR        0x00000000  none set | MMFAR       0x00000000  not valid (MMARVALID is clear) | BFAR        0x00000000  not valid (BFARVALID is clear) | AFSR        0x00000000  the implementation'"'"'s own | Exception return | EXC_RETURN  0xfffffff9  thread mode, main stack (MSP), no FPU frame (8 words) | stacked at # (MSP at the exception'"'"'s entry) | Stacked frame | R0          # | R1          # | R2          # | R3          # | R12         # | LR          #  main, cm.c:67 | PC          #  motor_step, cm.c:32 | xPSR        #  thread mode | SP before   0x203fffe4  the stack pointer before the exception'
    # (The Debug tab's section; "Show the fault report"; Enter on its stacked
    # PC: the faulting frame chosen, its source in front, its instruction in
    # the disassembly beside it.)
    run cm.c "$GO;debugpanel;debugrows:Fault:4;debugpick:fault;tabsdump;debugfaultline:PC;key:Return;debugwait:answers;views;debugdisasm;debugrows:Call stack:4;key:shift+F5;debugwait:idle;debugfault"
    check "the Fault section, the report's tab, Enter on the stacked PC" \
        'Fault | UsageFault: UNDEFINSTR, | undefined instruction; main | stack (MSP); no FPU frame; ;; cm.c | Fault report ;; views: [cm.c#1 31:0     __asm volatile("udf #0");]< ;; #  udf #0 ;; Call stack 4 | UsageFault_Handler cm.c:20 | <signal handler called> | ▸ motor_step cm.c:32 ;; no fault'
    # (Not at a fault: nothing said.)
    run cm.c "key:F5;debugwait:stopped;debugfault;debugpanel;debugrows:Fault:2;key:shift+F5;debugwait:idle"
    check "stopped at main: no fault" 'no fault ;; no Fault'
    # (A breakpoint in the handler, rather than a pause: the same.)
    run cm.c "break:20;key:F5;debugwait:stopped;key:F5;debugwait:stopped;debugdump;debugfault;key:shift+F5;debugwait:idle"
    check "stopped at a breakpoint in the handler" 'stopped (breakpoint); stack: UsageFault_Handler@20 <signal handler called>; vars: ;; UsageFault: UNDEFINSTR, undefined instruction; main stack (MSP); no FPU frame; stacked PC # in cm.c:32'

    fw 1 cortex-m3 boards/cm.ld -DPSP
    run cm.c "$REPORT"
    check "UDF in thread mode on the process stack" 'UsageFault: UNDEFINSTR, undefined instruction; thread stack (PSP); no FPU frame; stacked PC # in cm.c:32 ;; UsageFault: UNDEFINSTR, undefined instruction  (Cortex-M3 r0p1) | Fault status | CFSR        0x00010000 | UNDEFINSTR (UsageFault): undefined instruction | HFSR        0x00000000  none set | MMFAR       0x00000000  not valid (MMARVALID is clear) | BFAR        0x00000000  not valid (BFARVALID is clear) | AFSR        0x00000000  the implementation'"'"'s own | Exception return | EXC_RETURN  0xfffffffd  thread mode, process stack (PSP), no FPU frame (8 words) | stacked at # (PSP) | Stacked frame | R0          # | R1          # | R2          # | R3          # | R12         # | LR          #  main, cm.c:67 | PC          #  motor_step, cm.c:32 | xPSR        #  thread mode | SP before   0x20000404  the stack pointer before the exception'
    fw 2 cortex-m3 boards/cm.ld
    run cm.c "$FAULT"
    check "divide by zero (CCR.DIV_0_TRP): DIVBYZERO" 'UsageFault: DIVBYZERO, divide by zero; main stack (MSP); no FPU frame; stacked PC # in cm.c:34'
    fw 3 cortex-m3 boards/cm.ld
    run cm.c "$REPORT"
    check "a load from an unmapped address: a precise BusFault, BFAR valid" 'BusFault: PRECISERR, precise data bus error at 0x5ff00000; main stack (MSP); no FPU frame; stacked PC # in cm.c:36 ;; BusFault: PRECISERR, precise data bus error at 0x5ff00000  (Cortex-M3 r0p1) | Fault status | CFSR        0x00008200 | PRECISERR (BusFault): precise data bus error: the stacked PC is the instruction that faulted | BFARVALID: BFAR holds the address that faulted | HFSR        0x00000000  none set | MMFAR       0x00000000  not valid (MMARVALID is clear) | BFAR        0x5ff00000  the address of the bus fault | AFSR        0x00000000  the implementation'"'"'s own | Exception return | EXC_RETURN  0xfffffff9  thread mode, main stack (MSP), no FPU frame (8 words) | stacked at # (MSP at the exception'"'"'s entry) | Stacked frame | R0          # | R1          # | R2          # | R3          # | R12         # | LR          #  main, cm.c:67 | PC          #  motor_step, cm.c:36 | xPSR        #  thread mode | SP before   0x203fffe4  the stack pointer before the exception'
    fw 4 cortex-m3 boards/cm.ld
    run cm.c "$FAULT"
    check "a store to an unmapped address: precise on QEMU (a real M3's write buffer makes it imprecise)" 'BusFault: PRECISERR, precise data bus error at 0x5ff00000; main stack (MSP); no FPU frame; stacked PC # in cm.c:38'
    fw 5 cortex-m3 boards/cm.ld
    run cm.c "$FAULT"
    check "a load the MPU forbids: MemManage, DACCVIOL, MMFAR valid" 'MemManage: DACCVIOL, data access violation at 0x20000100; main stack (MSP); no FPU frame; stacked PC # in cm.c:40'
    fw 6 cortex-m3 boards/cm.ld
    run cm.c "$REPORT"
    check "UDF with the fault handlers off: a HardFault, forced from the UsageFault" 'HardFault, forced from UsageFault: UNDEFINSTR, undefined instruction; main stack (MSP); no FPU frame; stacked PC # in cm.c:32 ;; HardFault, forced from UsageFault: UNDEFINSTR, undefined instruction  (Cortex-M3 r0p1) | Fault status | CFSR        0x00010000 | UNDEFINSTR (UsageFault): undefined instruction | HFSR        0x40000000 | FORCED (HardFault): forced: a configurable fault escalated to HardFault (its handler disabled, or no higher in priority than what ran) | MMFAR       0x00000000  not valid (MMARVALID is clear) | BFAR        0x00000000  not valid (BFARVALID is clear) | AFSR        0x00000000  the implementation'"'"'s own | Exception return | EXC_RETURN  0xfffffff9  thread mode, main stack (MSP), no FPU frame (8 words) | stacked at # (MSP at the exception'"'"'s entry) | Stacked frame | R0          # | R1          # | R2          # | R3          # | R12         # | LR          #  main, cm.c:67 | PC          #  motor_step, cm.c:32 | xPSR        #  thread mode | SP before   0x203fffe4  the stack pointer before the exception'

    board mps2-an386 "$gdb"
    fw 1 cortex-m4 boards/cm.ld "-mfloat-abi=hard -mfpu=fpv4-sp-d16 -DFPU"
    run cm.c "$REPORT"
    check "Cortex-M4 with its FPU on: the FP state stacked (26 words)" 'UsageFault: UNDEFINSTR, undefined instruction; main stack (MSP); FPU frame; stacked PC # in cm.c:32 ;; UsageFault: UNDEFINSTR, undefined instruction  (Cortex-M4 r0p0) | Fault status | CFSR        0x00010000 | UNDEFINSTR (UsageFault): undefined instruction | HFSR        0x00000000  none set | MMFAR       0x00000000  not valid (MMARVALID is clear) | BFAR        0x00000000  not valid (BFARVALID is clear) | AFSR        0x00000000  the implementation'"'"'s own | Exception return | EXC_RETURN  0xffffffe9  thread mode, main stack (MSP), FPU frame (26 words) | stacked at # (MSP at the exception'"'"'s entry) | Stacked frame | R0          # | R1          # | R2          # | R3          # | R12         # | LR          #  main, cm.c:67 | PC          #  motor_step, cm.c:32 | xPSR        #  thread mode | SP before   0x203fffe4  the stack pointer before the exception'

    board mps2-an505 "$gdb"
    fw 1 cortex-m33 faults/an505.ld
    run cm.c "$REPORT"
    check "Cortex-M33 (ARMv8-M, secure): EXC_RETURN's secure stack, SFSR read" 'UsageFault: UNDEFINSTR, undefined instruction; main stack (MSP); no FPU frame; stacked PC # in cm.c:32 ;; UsageFault: UNDEFINSTR, undefined instruction  (Cortex-M33 r0p3) | Fault status | CFSR        0x00010000 | UNDEFINSTR (UsageFault): undefined instruction | HFSR        0x00000000  none set | MMFAR       0x00000000  not valid (MMARVALID is clear) | BFAR        0x00000000  not valid (BFARVALID is clear) | AFSR        0x00000000  the implementation'"'"'s own | SFSR        0x00000000  none set | SFAR        0x00000000  not valid (SFARVALID is clear) | Exception return | EXC_RETURN  0xfffffff9  thread mode, main stack (MSP), no FPU frame (8 words); secure stack, secure exception | stacked at # (MSP at the exception'"'"'s entry) | Stacked frame | R0          # | R1          # | R2          # | R3          # | R12         # | LR          #  main, cm.c:67 | PC          #  motor_step, cm.c:32 | xPSR        #  thread mode | SP before   0x381fffe4  the stack pointer before the exception'

    board microbit "$gdb"
    fw 6 cortex-m0 faults/microbit.ld
    run cm.c "$REPORT"
    check "Cortex-M0: a HardFault, no fault status registers, the stacked frame" 'HardFault (Cortex-M0: no fault status registers); main stack (MSP); no FPU frame; stacked PC # in cm.c:32 ;; HardFault  (Cortex-M0 r0p0: no fault status registers) | Fault status | ARMv6-M and v8-M Baseline have no CFSR, HFSR or fault address registers: | every fault is a HardFault, and the stacked frame is all there is. | Exception return | EXC_RETURN  0xfffffff9  thread mode, main stack (MSP), no FPU frame (8 words) | stacked at # (MSP at the exception'"'"'s entry) | Stacked frame | R0          # | R1          # | R2          # | R3          # | R12         # | LR          #  main, cm.c:67 | PC          #  motor_step, cm.c:32 | xPSR        #  thread mode | SP before   0x20003fe0  the stack pointer before the exception'
fi

# ---- RISC-V
if [ -z "$rvcc" ]; then
    echo "faults: RISC-V skipped: no riscv64-elf-gcc (nor riscv64-unknown-elf-gcc, riscv-none-elf-gcc)"
elif ! command -v qemu-system-riscv32 >/dev/null 2>&1; then
    echo "faults: RISC-V skipped: no qemu-system-riscv32 (QEMU)"
elif [ -z "$rvgdb" ]; then
    echo "faults: RISC-V skipped: no riscv64-elf-gdb (nor riscv64-unknown-elf-gdb, riscv-none-elf-gdb, gdb-multiarch) built with Python"
else
    cp "$here/faults/rv.c" "$P/"
    rvfw() { (cd "$P" && "$rvcc" -march=rv32imac_zicsr -mabi=ilp32 -g -O0 -nostdlib -Wl,--no-warn-rwx-segments -T "$here/faults/sifive_e.ld" -DMODE="$1" -o fw.elf rv.c) || exit 1; }
    board sifive_e "$rvgdb"
    rvfw 1
    run rv.c "$REPORT"
    check "RISC-V, a load from an unmapped address: load access fault, mtval the address" 'Load access fault (mcause 5) at 0x60000000 (mtval); mepc # in rv.c:15 ;; Load access fault (mcause 5) | Trap | mcause      0x00000005  exception 5: Load access fault | mepc        #  motor_step, rv.c:15 | mtval       0x60000000  the address that faulted | mtvec       #  trap_handler, rv.c:9 (direct) | mstatus     0x00001800  MPP 3: it was in machine mode'
    # (Enter on mepc, which is in no frame of the stack: the ELF's line, and the instruction there.)
    run rv.c "$GO;debugpick:fault;debugfaultline:mepc;key:Return;debugwait:answers;views;debugdisasm;key:shift+F5;debugwait:idle"
    check "Enter on mepc: its source and its instruction" 'views: [rv.c#1 14:0     sink = *(volatile u32*)0x60000000;]< ;; #  lw a4,0(a5)'
    rvfw 2
    run rv.c "$FAULT"
    check "RISC-V, an illegal instruction" 'Illegal instruction (mcause 2); mepc # in rv.c:13'
    run rv.c "key:F5;debugwait:stopped;debugfault;key:shift+F5;debugwait:idle"
    check "RISC-V stopped at main: no fault" 'no fault'
fi

# ---- the reports' tables, for what QEMU cannot make (an imprecise bus
# fault, a SecureFault, ESP-IDF's panic frames, an ESP32-S3 stopped by
# its panic handler's breakpoint: blit_fault_test beside blit)
t="$(cd "$(dirname "$blit")" && pwd)/blit_fault_test"
if [ ! -x "$t" ]; then
    echo "faults: the tables skipped: no blit_fault_test beside blit (a Debug build makes it)"
else
    table() {
        got=$("$t" "$1" 2>&1 | awk 'NR > 1 { printf " ;; " } { printf "%s", $0 }')
        check "table: $1" "$2"
    }
    table imprecise \
        'BusFault: IMPRECISERR, imprecise data bus error; main stack (MSP); no FPU frame; stacked PC 0x08001a40 in motor.c:212 ;; BusFault: IMPRECISERR, imprecise data bus error  (Cortex-M3 r2p1) | Fault status | CFSR        0x00000400 | IMPRECISERR (BusFault): imprecise data bus error: the stacked PC is after the instruction that faulted | HFSR        0x00000000  none set | MMFAR       0x00000000  not valid (MMARVALID is clear) | BFAR        0x00000000  not valid (BFARVALID is clear) | AFSR        0x00000000  the implementation'"'"'s own | Exception return | EXC_RETURN  0xfffffff9  thread mode, main stack (MSP), no FPU frame (8 words) | stacked at 0x20001fe0 (MSP at the exception'"'"'s entry) | Stacked frame | R0          0x0000002a | R1          0x40011000 | R2          0x00000000 | R3          0x00000000 | R12         0x00000000 | LR          0x08001b03  main, main.c:30 [->] | PC          0x08001a40  motor_step, motor.c:212 [->] | xPSR        0x21000000  thread mode | SP before   0x20002000  the stack pointer before the exception | Imprecise bus fault | The write buffer let the program run on before the error came back: | the store that faulted is before the stacked PC. | Setting ACTLR.DISDEFWBUF (bit 1 of 0xE000E008) turns the write buffer off on | Cortex-M3 and M4, which makes such faults precise, at a cost in speed.'
    table imprecise-disdefwbuf \
        'BusFault: IMPRECISERR, imprecise data bus error; main stack (MSP); no FPU frame; stacked PC 0x08001a40 in motor.c:212 ;; BusFault: IMPRECISERR, imprecise data bus error  (Cortex-M3 r2p1) | Fault status | CFSR        0x00000400 | IMPRECISERR (BusFault): imprecise data bus error: the stacked PC is after the instruction that faulted | HFSR        0x00000000  none set | MMFAR       0x00000000  not valid (MMARVALID is clear) | BFAR        0x00000000  not valid (BFARVALID is clear) | AFSR        0x00000000  the implementation'"'"'s own | Exception return | EXC_RETURN  0xfffffff9  thread mode, main stack (MSP), no FPU frame (8 words) | stacked at 0x20001fe0 (MSP at the exception'"'"'s entry) | Stacked frame | R0          0x0000002a | R1          0x40011000 | R2          0x00000000 | R3          0x00000000 | R12         0x00000000 | LR          0x08001b03  main, main.c:30 [->] | PC          0x08001a40  motor_step, motor.c:212 [->] | xPSR        0x21000000  thread mode | SP before   0x20002000  the stack pointer before the exception | Imprecise bus fault | The write buffer let the program run on before the error came back: | the store that faulted is before the stacked PC. | ACTLR.DISDEFWBUF is set already (ACTLR 0x00000002).'
    table securefault \
        'SecureFault: INVEP, invalid entry point; thread stack (PSP); no FPU frame; stacked PC 0x10000800 ;; SecureFault: INVEP, invalid entry point  (Cortex-M33 r0p4) | Fault status | CFSR        0x00000000  none set | HFSR        0x00000000  none set | MMFAR       0x00000000  not valid (MMARVALID is clear) | BFAR        0x00000000  not valid (BFARVALID is clear) | AFSR        0x00000000  the implementation'"'"'s own | SFSR        0x00000001 | INVEP (SecureFault): invalid entry point: non-secure code called secure code where there is no SG instruction | SFAR        0x00000000  not valid (SFARVALID is clear) | Exception return | EXC_RETURN  0xffffffbd  thread mode, process stack (PSP), no FPU frame (8 words); non-secure stack, secure exception | stacked at 0x20003f00 (PSP) | Stacked frame | R0          0x00000000 | R1          0x00000000 | R2          0x00000000 | R3          0x00000000 | R12         0x00000000 | LR          0x00200101 [->] | PC          0x10000800 [->] | xPSR        0x01000000  thread mode | SP before   0x20003f20  the stack pointer before the exception'
    table m0-forced \
        'HardFault (Cortex-M0+: no fault status registers); main stack (MSP); no FPU frame; stacked PC 0x10000410 in motor.c:40 ;; HardFault  (Cortex-M0+ r0p1: no fault status registers) | Fault status | ARMv6-M and v8-M Baseline have no CFSR, HFSR or fault address registers: | every fault is a HardFault, and the stacked frame is all there is. | Exception return | EXC_RETURN  0xfffffff9  thread mode, main stack (MSP), no FPU frame (8 words) | stacked at 0x20041f08 (MSP at the exception'"'"'s entry) | Stacked frame | R0          0x00000001 | R1          0x00000002 | R2          0x00000003 | R3          0x00000004 | R12         0x0000000c | LR          0x10000451 [->] | PC          0x10000410  motor_step, motor.c:40 [->] | xPSR        0x01000000  thread mode | SP before   0x20041f28  the stack pointer before the exception'
    table xtensa-rerun \
        'StoreProhibited (EXCCAUSE 29), cache attribute does not allow store at 0x00000000 (EXCVADDR); EPC1 0x42009126 in main.c:10 ;; StoreProhibited (EXCCAUSE 29), cache attribute does not allow store | Exception | EXCCAUSE    29  StoreProhibited: cache attribute does not allow store | EPC1        0x42009126  motor_step, main.c:10 [->] | EXCVADDR    0x00000000  the address that faulted | ESP-IDF'"'"'s panic handler set a breakpoint at the instruction that faulted | and returned to it: the core stopped there.'
    table xtensa-user-break \
        'no fault'
    table xtensa-panic \
        'LoadProhibited (EXCCAUSE 28), cache attribute does not allow load at 0x00000004 (EXCVADDR); EPC1 0x42009210 in sensor.c:77 ;; LoadProhibited (EXCCAUSE 28), cache attribute does not allow load | Exception | EXCCAUSE    28  LoadProhibited: cache attribute does not allow load | EPC1        0x42009210  read_sensor, sensor.c:77 [->] | EXCVADDR    0x00000004  the address that faulted | A0          0x82009300  returns to 0x42009300 [->] | A1          0x3fc9a1c0  the stack pointer | from ESP-IDF'"'"'s panic frame at 0x3fc9a0b0'
    table xtensa-pseudo \
        'Interrupt wdt timeout on CPU0 (ESP-IDF'"'"'s cause 5); EPC1 0x42009210 in sensor.c:77 ;; Interrupt wdt timeout on CPU0 (ESP-IDF'"'"'s cause 5) | Exception | EXCCAUSE    5  written by ESP-IDF'"'"'s panicHandler | EPC1        0x42009210  read_sensor, sensor.c:77 [->] | EXCVADDR    0x00000000  not about an address for this cause | A0          0x82009300  returns to 0x42009300 [->] | A1          0x3fc9a1c0  the stack pointer | from ESP-IDF'"'"'s panic frame at 0x3fc9a0b0'
    table xtensa-entry \
        'StoreProhibited (EXCCAUSE 29), cache attribute does not allow store at 0x00000000 (EXCVADDR); EPC1 0x42009126 in main.c:10 ;; StoreProhibited (EXCCAUSE 29), cache attribute does not allow store | Exception | EXCCAUSE    29  StoreProhibited: cache attribute does not allow store | EPC1        0x42009126  motor_step, main.c:10 [->] | EXCVADDR    0x00000000  the address that faulted | from the core'"'"'s registers, in ESP-IDF'"'"'s panic handler (its frame could not be read here)'
    table riscv-panic \
        'Store/AMO access fault (mcause 7); mepc 0x42005d6a in motor.c:12 ;; Store/AMO access fault (mcause 7) | Trap | mcause      0x00000007  exception 7: Store/AMO access fault | mepc        0x42005d6a  motor_step, motor.c:12 [->] | mtval       0x00000000  the address 0, or the core gives none | mtvec       0x40380001  _vector_table (vectored) | mstatus     0x00001880  MPP 3: it was in machine mode | from ESP-IDF'"'"'s panic frame at 0x3fc8e0a0'
    table riscv-interrupt \
        'no fault'
    table riscv-rerun \
        'Load access fault (mcause 5); mepc 0x42005d6a in motor.c:12 ;; Load access fault (mcause 5) | Trap | mcause      0x00000005  exception 5: Load access fault | mepc        0x42005d6a  motor_step, motor.c:12 [->] | mtval       0x00000000  the address 0, or the core gives none | mtvec       0x40380001  (vectored) | ESP-IDF'"'"'s panic handler had a breakpoint set at the instruction that faulted | and returned to it: the core stopped there.'
    table riscv-step \
        'no fault'
fi

[ "$failed" = 0 ] && [ -z "$print" ] && echo "faults: all passed"
[ "$ran" -gt 0 ] || { echo "faults: nothing run"; exit 0; }
exit "$failed"
