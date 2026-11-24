#!/bin/sh
# How far a flash or an FPGA's load has got, read from the engine's output
# as it comes (buildprogress.cpp): the Run button's words and the task's
# row at points in the run (the bar still being redrawn, with no newline
# yet), what the run came to kept on the row after, and a failure's
# problem. Each engine is a stand-in (tests/replay.sh) writing a sample of
# its output (tests/progress/, each saying where it is from) on the task's
# terminal, at the place a test waits for before it goes on.
#
#   tests/progress.sh path/to/blit            run the cases
#   tests/progress.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/progress.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd -P)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
export BLIT_RECENT_FILE="$dir/recent"
export HOME="$dir/home"
export REPLAY_HOLD="$dir/hold"
mkdir -p "$HOME" "$REPLAY_HOLD"

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n%s\n\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n  expected:\n%s\n  got:\n%s\n' "$1" "$2" "$got"
        failed=1
    fi
}
want() { expected=$(cat); }

# The project: a task for each engine, run as its program by its path (as
# a board's task would name it), which is the stand-in.
T="$dir/proj"
B="$dir/bin"  # (outside the project: a problem placed at the program's name is at no file of it)
mkdir -p "$T" "$B"
cat >"$dir/init.lua" <<EOF
blit.opt.lsp.enabled = false
blit.opt.git.enabled = false
blit.build.add("Flash", {detect = function() return true end, tasks = {
  {name = "esptool", cmd = "$B/esptool --chip esp32s3 write-flash 0x10000 app.bin", default = true},
  {name = "openocd", cmd = "$B/openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c 'program app.bin 0x08000000 verify reset exit'"},
  {name = "probe-rs", cmd = "$B/probe-rs download --chip STM32F411RETx --binary-format bin --base-address 0x08000000 app.bin"},
  {name = "J-Link", cmd = "$B/JLinkExe -device STM32F411RE -if SWD -speed 4000 -autoconnect 1 -CommandFile flash.jlink"},
  {name = "openFPGALoader", cmd = "$B/openFPGALoader -b basys3 top.bit"},
  {name = "picotool", cmd = "$B/picotool load -v -x app.uf2"},
  {name = "Vivado", cmd = "$B/vivado -mode batch -nojournal -nolog -notrace -source load.tcl"},
  {name = "picotool -f", cmd = "$B/picotool load -v -x -f app.uf2"},
  {name = "UF2", cmd = "$B/uf2copy"},
  {name = "dfu-util", cmd = "$B/dfu-util -d 0483:df11 -a 0 -s 0x08000000:leave -D fw.bin"},
  {name = "pyOCD", cmd = "$B/pyocd load -W -t stm32f411ce -u SERIAL build/fw.elf"},
  {name = "STM32CubeProgrammer", cmd = "$B/STM32_Programmer_CLI -c port=SWD sn=SERIAL -w build/fw.elf -v -rst"},
  {name = "iceprog", cmd = "$B/iceprog build/top.bin"},
  {name = "ecpprog", cmd = "$B/ecpprog build/top.bit"},
  {name = "pgrcmd", cmd = "$B/pgrcmd -infile impl/top.xcf"},
  {name = "make", cmd = "make flash"},
  {name = "plain", cmd = "$B/plain"},
}})
EOF

# flash TASK PROGRAM SAMPLE SCRIPT: the task run, PROGRAM being the stand-in
# writing SAMPLE; SCRIPT is what is done meanwhile (go:NAME lets the sample
# go on past its "@hold NAME"). What the script printed, a line each; the
# time a run took (the time its lines came over, where the tool does not
# say it) as N, unless timed is set (to "&": the tool's own count shown).
flash() {
    rm -f "$REPLAY_HOLD"/*
    printf '#!/bin/sh\nexec sh "%s" "%s"\n' "$here/replay.sh" "$here/progress/$3.txt" >"$B/$2"
    chmod +x "$B/$2"
    script=$(printf '%s' "$4" | sed -e "s#go:\([a-z]*\)#lua:io.open('$REPLAY_HOLD/\1','w'):close()#g")
    got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen ${width:-3000}x700 --shot "$dir/shot.bmp" --script "workdir:$T;frame;buildrun:$1;$script" 2>&1 >/dev/null |
        sed -e "s#$dir#~#g" -e "s/ in [0-9]*\\.[0-9] s/${timed:- in N s}/g")
    timed= width=
}
end="go:a;go:b;buildwait;progressdump;scandump"

# esptool v5: its bar redrawn in place, no newline after it while it is
# being written; then what it came to (its own time for the writing). Its
# third file (ESP-IDF's app, after the bootloader and the partition table,
# each written and verified): Program under way again, with that file's
# bytes; the summary the three files'.
timed='&'
flash esptool esptool esptool5 "progresswait:Program 38%;progressdump;$end"
want <<'EOF'
button: ■ Program 38% 32.0/82.8 KB | row: Program 38% 32.0/82.8 KB
button: ▶ esptool · Flashed 170.5 KB in 8.8 s, verified | row: done · Flashed 170.5 KB in 8.8 s, verified
problems: 0
EOF
check "esptool v5: the bar being redrawn, a later file's, then the summary" "$expected"

# esptool v5 on a pipe (as blit runs it): a line for each step of the bar.
timed='&'
flash esptool esptool esptool5-pipe "progresswait:Program 38%;progressdump;$end"
want <<'EOF'
button: ■ Program 38% 32.0/82.8 KB | row: Program 38% 32.0/82.8 KB
button: ▶ esptool · Flashed 170.5 KB in 8.8 s, verified | row: done · Flashed 170.5 KB in 8.8 s, verified
problems: 0
EOF
check "esptool v5 on a pipe: the same" "$expected"

# A window too narrow for the whole summary on the Run button (a third of
# it at most): without its time, else its words before it; the row keeps
# it whole.
width=1600
flash esptool esptool esptool5-pipe "$end"
want <<'EOF'
button: ▶ esptool · Flashed 170.5 KB | row: done · Flashed 170.5 KB in N s, verified
problems: 0
EOF
check "a narrow window: the summary's words before its time" "$expected"
width=1800
flash openFPGALoader openFPGALoader ofl-flash "$end"
want <<'EOF'
button: ▶ openFPGALoader · Flashed, verified | row: done · Flashed in N s, verified
problems: 0
EOF
check "a narrow window: the summary without its time" "$expected"

# Stopped mid-write (the app, its third file): not "Flashed" with what the
# first two wrote, but where it stopped.
flash esptool esptool esptool5-pipe "progresswait:Program 38%;command:Stop the build;buildwait;progressdump"
want <<'EOF'
button: ▶ esptool · stopped in Program at 38% | row: stopped in Program at 38%
EOF
check "esptool stopped mid-write: where it stopped" "$expected"

# esptool v4: "Writing at 0x00010000... (50 %)" after a \r, no newline.
timed='&'
flash esptool esptool esptool4 "progresswait:Program 50%;progressdump;$end"
want <<'EOF'
button: ■ Program 50% | row: Program 50%
button: ▶ esptool · Flashed 40.2 KB in 1.8 s, verified | row: done · Flashed 40.2 KB in 1.8 s, verified
problems: 0
EOF
check "esptool v4: the percent, then the summary" "$expected"

# OpenOCD's program: its stages (it prints no percent), the bytes written
# and the time that took.
timed='&'
flash openocd openocd openocd "progresswait:Program;progressdump;go:a;progresswait:Verify;progressdump;$end"
want <<'EOF'
button: ■ Program | row: Program
button: ■ Verify | row: Program ✓ · Verify
button: ▶ openocd · Flashed 40.2 KB in 1.2 s, verified | row: done · Flashed 40.2 KB in 1.2 s, verified
problems: 0
EOF
check "OpenOCD: program, verify, the summary" "$expected"

# Espressif's OpenOCD, program_esp for each of ESP-IDF's files: the app
# written (the other two already there), then verified, the tool's own
# time; all three already there: unchanged, nothing written.
timed='&'
flash openocd openocd openocd-esp32 "progresswait:Program;progressdump;$end"
want <<'EOF'
button: ■ Program | row: Program
button: ▶ openocd · Flashed in 2.3 s, verified | row: done · Flashed in 2.3 s, verified
problems: 0
EOF
check "Espressif's OpenOCD: one file of three written, verified" "$expected"
flash openocd openocd openocd-esp32-same "$end"
want <<'EOF'
button: ▶ openocd · Unchanged, verified | row: done · Unchanged, verified
problems: 0
EOF
check "Espressif's OpenOCD: all already there" "$expected"

# probe-rs: three bars drawn again and again (moved up to, cleared,
# written), an idle one not under way, a done one staying done.
timed='&'
flash probe-rs probe-rs probe-rs "progresswait:Program 62%;progressdump;$end"
want <<'EOF'
button: ■ Program 62% 25.0 KB | row: Erase ✓ · Program 62% 25.0 KB
button: ▶ probe-rs · Flashed 40.2 KB in 1.8 s, verified | row: done · Flashed 40.2 KB in 1.8 s, verified
problems: 0
EOF
check "probe-rs: its bars, then the summary" "$expected"

# J-Link Commander: its stages done, the bytes and its time.
timed='&'
flash J-Link JLinkExe jlink "$end"
want <<'EOF'
button: ▶ J-Link · Flashed 128 KB in 3.9 s, verified | row: done · Flashed 128 KB in 3.9 s, verified
problems: 0
EOF
check "J-Link: the summary" "$expected"

# openFPGALoader into SRAM, and into flash (the bridge loaded first) with
# --verify.
flash openFPGALoader openFPGALoader ofl-sram "progresswait:Load 32%;progressdump;$end"
want <<'EOF'
button: ■ Load 32% | row: Load 32%
button: ▶ openFPGALoader · Loaded into SRAM in N s | row: done · Loaded into SRAM in N s
problems: 0
EOF
check "openFPGALoader: into SRAM" "$expected"
flash openFPGALoader openFPGALoader ofl-sram-pipe "progresswait:Load 32%;progressdump;$end"
want <<'EOF'
button: ■ Load 32% | row: Load 32%
button: ▶ openFPGALoader · Loaded into SRAM in N s | row: done · Loaded into SRAM in N s
problems: 0
EOF
check "openFPGALoader on a pipe: into SRAM" "$expected"
flash openFPGALoader openFPGALoader ofl-flash "progresswait:Program 38%;progressdump;$end"
want <<'EOF'
button: ■ Program 38% | row: Load ✓ · Erase ✓ · Program 38%
button: ▶ openFPGALoader · Flashed in N s, verified | row: done · Flashed in N s, verified
problems: 0
EOF
check "openFPGALoader: into flash, verified" "$expected"
flash openFPGALoader openFPGALoader ofl-flash-pipe "progresswait:Program 38%;progressdump;$end"
want <<'EOF'
button: ■ Program 38% | row: Load ✓ · Erase ✓ · Program 38%
button: ▶ openFPGALoader · Flashed in N s, verified | row: done · Flashed in N s, verified
problems: 0
EOF
check "openFPGALoader on a pipe: into flash, verified" "$expected"

# picotool (its real output, on a pipe and on a terminal): with -f, the
# board rebooted into BOOTSEL first; "Loading into Flash: [====   ]  52%\r",
# the next drawn over it; "Verifying Flash:", then "  OK".
flash "picotool -f" picotool picotool "progresswait:Program 52%;progressdump;$end"
want <<'EOF'
button: ■ Program 52% | row: Reboot ✓ · Program 52%
button: ▶ picotool -f · Flashed in N s, verified | row: done · Flashed in N s, verified
problems: 0
EOF
check "picotool -f: the reboot into BOOTSEL, the bar, then the summary" "$expected"
flash picotool picotool picotool-pty "progresswait:Program 52%;progressdump;$end"
want <<'EOF'
button: ■ Program 52% | row: Program 52%
button: ▶ picotool · Flashed in N s, verified | row: done · Flashed in N s, verified
problems: 0
EOF
check "picotool on a terminal, a board in BOOTSEL: the bar, then the summary" "$expected"

# blit's own copy of a .uf2 to a boot ROM's volume: the bytes for the
# flash as it goes, then the board restarting, the summary.
flash UF2 uf2copy uf2 "progresswait:Program 52%;progressdump;go:a;progresswait:Restart;buildwait;progressdump;scandump"
want <<'EOF'
button: ■ Program 52% 16.0/30.8 KB | row: Program 52% 16.0/30.8 KB
button: ▶ UF2 · Flashed 30.8 KB in N s | row: done · Flashed 30.8 KB in N s
problems: 0
EOF
check "the UF2 copy: its bytes, then the summary" "$expected"
flash UF2 uf2copy uf2-fail "$end"
want <<'EOF'
button: ▶ UF2 · 1 error · stopped in Restart | row: failed: exited with code 1 · stopped in Restart
problems: 1
E uf2:1 /Volumes/RPI-RP2 is still there 10 s after the copy: the board did not take the image
EOF
check "the UF2 copy, the board not restarting" "$expected"

# dfu-util (written from its source): Erase, then Download, its bytes.
flash dfu-util dfu-util dfu-util "progresswait:Program 50%;progressdump;$end"
want <<'EOF'
button: ■ Program 50% 20.1 KB | row: Erase ✓ · Program 50% 20.1 KB
button: ▶ dfu-util · Flashed 40.2 KB in N s | row: done · Flashed 40.2 KB in N s
problems: 0
EOF
check "dfu-util: erase, download, the summary" "$expected"

# pyOCD on a terminal (written from its source): its log lines, the same
# bar for each stage, the summary's bytes programmed.
flash pyOCD pyocd pyocd "progresswait:Program 52%;progressdump;$end"
want <<'EOF'
button: ■ Program 52% | row: Erase ✓ · Program 52%
button: ▶ pyOCD · Flashed 16 KB in N s | row: done · Flashed 16 KB in N s
problems: 0
EOF
check "pyOCD: erase, program, the summary" "$expected"

# STM32CubeProgrammer (only the lines others' code reads it by).
flash STM32CubeProgrammer STM32_Programmer_CLI stm32cube "progresswait:Program;go:a;$end"
want <<'EOF'
button: ▶ STM32CubeProgrammer · Flashed in N s, verified | row: done · Flashed in N s, verified
problems: 0
EOF
check "STM32CubeProgrammer: downloaded, verified" "$expected"

# iceprog (written from its source): the flash erased, written and read
# back, its percent; into SRAM, CDONE high.
flash iceprog iceprog iceprog "progresswait:Program 49%;progressdump;$end"
want <<'EOF'
button: ■ Program 49% | row: Erase ✓ · Program 49%
button: ▶ iceprog · Flashed 31.5 KB in N s, verified | row: done · Flashed 31.5 KB in N s, verified
problems: 0
EOF
check "iceprog: the flash written, verified" "$expected"
flash iceprog iceprog iceprog-sram "progresswait:Load;progressdump;$end"
want <<'EOF'
button: ■ Load | row: Load
button: ▶ iceprog · Loaded into SRAM in N s | row: done · Loaded into SRAM in N s
problems: 0
EOF
check "iceprog -S: into SRAM" "$expected"

# ecpprog on a terminal (written from its source): its counts of bytes;
# into SRAM, the status register's DONE bit.
flash ecpprog ecpprog ecpprog "progresswait:Program 50%;progressdump;$end"
want <<'EOF'
button: ■ Program 50% 96.0/192.0 KB | row: Erase ✓ · Program 50% 96.0/192.0 KB
button: ▶ ecpprog · Flashed 192 KB in N s, verified | row: done · Flashed 192 KB in N s, verified
problems: 0
EOF
check "ecpprog: the flash written, verified" "$expected"
flash ecpprog ecpprog ecpprog-sram "progresswait:Load;progressdump;$end"
want <<'EOF'
button: ■ Load | row: Load
button: ▶ ecpprog · Loaded into SRAM in N s | row: done · Loaded into SRAM in N s
problems: 0
EOF
check "ecpprog -S: into SRAM, DONE set" "$expected"
flash ecpprog ecpprog ecpprog-sram-fail "$end"
want <<'EOF'
button: ▶ ecpprog · 1 error · stopped in Load | row: done · stopped in Load
problems: 1
E ecpprog:1 ECP5 Status Register: 0x00202000 (DONE is not set)
EOF
check "ecpprog -S: DONE not set (ecpprog itself ends with 0)" "$expected"

# Failures: the tool's own words, a problem at the program (not a file).
flash openocd openocd openocd-fail "$end"
want <<'EOF'
button: ▶ openocd · 1 error | row: failed: exited with code 1
problems: 1
E openocd:1 OpenOCD init failed: open failed
EOF
check "OpenOCD with no probe" "$expected"
flash openocd openocd openocd-esp32-fail "$end"
want <<'EOF'
button: ▶ openocd · 1 error | row: failed: exited with code 1
problems: 1
E openocd:1 OpenOCD init failed: esp_usb_jtag: could not find or open device!
EOF
check "Espressif's OpenOCD with no ESP32-S3" "$expected"
flash esptool esptool esptool-fail "$end"
want <<'EOF'
button: ▶ esptool · 1 error | row: failed: exited with code 2
problems: 1
E esptool:1 Could not open /dev/cu.nothere, the port is busy or doesn't exist.
EOF
check "esptool with no board" "$expected"
flash probe-rs probe-rs probe-rs-fail "$end"
want <<'EOF'
button: ▶ probe-rs · 1 error | row: failed: exited with code 1
problems: 1
E probe-rs:1 No connected probes were found.
EOF
check "probe-rs with no probe (its command says whose the bare Error: is)" "$expected"
flash J-Link JLinkExe jlink-fail "$end"
want <<'EOF'
button: ▶ J-Link · 1 error | row: failed: exited with code 1
problems: 1
E JLinkExe:1 Writing target memory failed.
EOF
check "J-Link failing to write" "$expected"
flash openFPGALoader openFPGALoader ofl-fail "$end"
want <<'EOF'
button: ▶ openFPGALoader · 1 error | row: failed: exited with code 1
problems: 1
E openFPGALoader:1 JTAG init failed with: unable to open ftdi device
EOF
check "openFPGALoader with no board" "$expected"
flash picotool picotool picotool-fail "$end"
want <<'EOF'
button: ▶ picotool · 1 error | row: failed: exited with code 249
problems: 1
E picotool:1 No accessible RP-series devices in BOOTSEL mode were found.
  note RP2040 device at bus 1, address 2 appears to have a USB serial connection, so consider -f (or -F) to force reboot in order to run the command.
EOF
check "picotool with no board in BOOTSEL: the board it saw, and its advice, a note" "$expected"
flash "picotool -f" picotool picotool-nofile "$end"
want <<'EOF'
button: ▶ picotool -f · 1 error · stopped in Reboot | row: failed: exited with code 252 · stopped in Reboot
problems: 1
E picotool:1 Could not open '/nonexistent.uf2'
EOF
check "picotool -f with no such file (after rebooting the board)" "$expected"
cat >"$dir/held.txt" <<'EOF'
No accessible RP-series devices in BOOTSEL mode were found with serial number
E0C9125B0D9B.
@exit 249
EOF
printf '#!/bin/sh\nexec sh "%s" "%s"\n' "$here/replay.sh" "$dir/held.txt" >"$B/picotool"
chmod +x "$B/picotool"
got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 3000x700 --shot "$dir/shot.bmp" --script "workdir:$T;frame;buildrun:picotool;buildwait;scandump" 2>&1 >/dev/null)
want <<'EOF'
problems: 1
E picotool:1 No accessible RP-series devices in BOOTSEL mode were found with serial number E0C9125B0D9B.
EOF
check "picotool's words wrapped at 80 columns: one problem" "$expected"
flash dfu-util dfu-util dfu-util-fail "$end"
want <<'EOF'
button: ▶ dfu-util · 1 error | row: failed: exited with code 74
problems: 1
E dfu-util:1 No DFU capable USB device available
EOF
check "dfu-util with no DFU device (its warnings no problem)" "$expected"
flash pyOCD pyocd pyocd-fail "$end"
want <<'EOF'
button: ▶ pyOCD · 1 error | row: failed: exited with code 1
problems: 1
E pyocd:1 No connected debug probe matches unique ID '0670FF485550755187121723'
EOF
check "pyOCD with no probe" "$expected"
flash STM32CubeProgrammer STM32_Programmer_CLI stm32cube-fail "$end"
want <<'EOF'
button: ▶ STM32CubeProgrammer · 1 error | row: failed: exited with code 1
problems: 1
E STM32_Programmer_CLI:1 No debug probe detected
EOF
check "STM32CubeProgrammer with no probe" "$expected"
flash iceprog iceprog iceprog-fail "$end"
want <<'EOF'
button: ▶ iceprog · 1 error | row: failed: exited with code 2
problems: 1
E iceprog:1 Can't find iCE FTDI USB device (device string s:0x0403:0x6010:NOSUCHSERIAL).
EOF
check "iceprog with no such chip (its real output)" "$expected"
flash pgrcmd pgrcmd pgrcmd-fail "$end"
want <<'EOF'
button: ▶ pgrcmd · 1 error | row: failed: exited with code 251
problems: 1
E pgrcmd:1 cable not detected (return code -5)
EOF
check "pgrcmd with no cable: its return code in words" "$expected"

# Vivado's hardware manager: its steps (no percent), the load done when the
# DONE pin is high; a failure the ERROR line's message, without its id.
flash Vivado vivado vivado "progresswait:Load;progressdump;$end"
want <<'EOF'
button: ■ Load | row: Connect ✓ · Load
button: ▶ Vivado · Loaded into SRAM in N s | row: done · Loaded into SRAM in N s
problems: 0
EOF
check "Vivado: connecting, loading, the summary" "$expected"
flash Vivado vivado vivado-fail "$end"
want <<'EOF'
button: ▶ Vivado · 1 error · stopped in Load | row: failed: exited with code 1 · stopped in Load
problems: 1
E vivado:1 File missing.bit not found. Check file name and file permissions.
EOF
check "Vivado with no bitstream" "$expected"
flash Vivado vivado vivado-notarget "$end"
want <<'EOF'
button: ▶ Vivado · 1 error · stopped in Connect | row: failed: exited with code 1 · stopped in Connect
problems: 1
E vivado:1 No matching targets found on connected servers: localhost
EOF
check "Vivado with no board on the cable asked for" "$expected"

# Vivado writing the FPGA's flash: its SRAM loaded with Vivado's design for
# it, the flash erased, written and read back (one step, as it prints
# it), the FPGA booted from it; the time program_hw_cfgmem gives itself.
timed='&'
flash Vivado vivado vivado-flash "progresswait:Program;progressdump;$end"
want <<'EOF'
button: ■ Program | row: Connect ✓ · Load ✓ · Erase ✓ · Program
button: ▶ Vivado · Flashed in 54.0 s, verified | row: done · Flashed in 54.0 s, verified
problems: 0
EOF
check "Vivado: the FPGA's flash erased, written and verified" "$expected"
flash Vivado vivado vivado-flash-verify "progresswait:Verify;progressdump;$end"
want <<'EOF'
button: ■ Verify | row: Connect ✓ · Load ✓ · Verify
button: ▶ Vivado · 1 error · stopped in Verify | row: failed: exited with code 1 · stopped in Verify
problems: 1
E vivado:1 Flash Programming Unsuccessful: Byte 10 does not match (CF != FF) Verify that the selected flash part matches the actual flash device connected to the board.
EOF
check "Vivado: the flash not what was written" "$expected"
flash Vivado vivado vivado-flash-wrong "$end"
want <<'EOF'
button: ▶ Vivado · 1 error | row: failed: exited with code 1
problems: 1
E vivado:1 Flash Programming Unsuccessful. Part selected s25fl032p, but part mx25l3273f detected.
EOF
check "Vivado: told the wrong flash part" "$expected"
flash Vivado vivado vivado-flash-nopart "$end"
want <<'EOF'
button: ▶ Vivado · 1 error | row: failed: exited with code 1
problems: 1
E vivado:1 Unrecognized config mem part - mx25l3233f-spi-x1_x2_x4. Please run get_cfgmem_parts and select a valid part or use the cfgmem GUI to assist in selecting a valid device.
EOF
check "Vivado: a flash part it has none of" "$expected"

# The real openFPGALoader, where it is installed, with no board plugged in
# (skipped where it is not, or where one answers).
if ofl=$(command -v openFPGALoader) && ! "$ofl" --detect -b basys3 >/dev/null 2>&1; then
    : >"$T/top.bit"
    cat >"$dir/real.lua" <<EOF
blit.opt.lsp.enabled = false
blit.opt.git.enabled = false
blit.build.add("Flash", {detect = function() return true end, tasks = {{name = "Basys 3", cmd = "$ofl -b basys3 -f top.bit"}}})
EOF
    got=$(BLIT_INIT="$dir/real.lua" "$blit" --offscreen 3000x700 --shot "$dir/shot.bmp" --script "workdir:$T;frame;buildrun:Basys 3;buildwait;progressdump;scandump" 2>&1 >/dev/null)
    want <<'EOF'
button: ▶ Basys 3 · 1 error | row: failed: exited with code 1
problems: 1
E openFPGALoader:1 JTAG init failed with: unable to open ftdi device
EOF
    check "openFPGALoader itself, with no board" "$expected"
    rm -f "$T/top.bit"
fi

# A tool run by make, which echoes its command: whose output it is.
printf 'flash:\n\t%s/probe-rs download --chip STM32F411RETx app.elf\n' "$B" >"$T/Makefile"
flash make probe-rs probe-rs-fail "$end"
got=$(printf '%s' "$got" | sed -e 's/^make: \*\*\*.*$//' | grep .)
want <<'EOF'
button: ▶ make · 1 error | row: failed: exited with code 2
problems: 1
E probe-rs:1 No connected probes were found.
EOF
check "make echoing probe-rs's command" "$expected"
rm -f "$T/Makefile"

# The task's row as the Build tab fits it (panel.width), at the stage under
# way: that stage and its percent stay, whatever the room. The stages done
# are said more briefly first ("✓ 2"), then left out, then the kind of
# work, then the stage's sizes. (A task of the folder's own, for its kind.)
K="$dir/kinds"
mkdir -p "$K/.blit"
cat >"$K/.blit/tasks.json" <<EOF
{ "tasks": [
  { "label": "FPGA", "command": "$B/openFPGALoader -b basys3 -f top.bit", "kind": "flash" },
  { "label": "ESP", "command": "$B/esptool --chip esp32s3 write-flash 0x10000 app.bin", "kind": "flash" } ] }
EOF
# fitted TASK PROGRAM SAMPLE STAGE WIDTH: the task's row as shown, held at STAGE.
fitted() {
    rm -f "$REPLAY_HOLD"/*
    printf '#!/bin/sh\nexec sh "%s" "%s"\n' "$here/replay.sh" "$here/progress/$3.txt" >"$B/$2"
    chmod +x "$B/$2"
    printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.panel.width = %s\n' "$5" >"$dir/wide.lua"
    go="lua:io.open('$REPLAY_HOLD/a','w'):close();lua:io.open('$REPLAY_HOLD/b','w'):close()"
    got=$(BLIT_INIT="$dir/wide.lua" "$blit" --offscreen 3000x700 --shot "$dir/shot.bmp" --script "workdir:$K;frame;buildpanel;buildrun:$1;progresswait:$4;builddump:shown;$go;buildwait" 2>&1 >/dev/null |
        grep '^FPGA: ' | awk -v t="$1: " 'BEGIN { RS = "\n" } { n = split($0, r, / \| /); for (i = 1; i <= n; ++i) if (index(r[i], t) == 1) print r[i] }')
}
fitted FPGA openFPGALoader ofl-flash 'Program 38%' 50
check "room for it all: the stages done, then the one under way" 'FPGA: flash · Load ✓ · Erase ✓ · Program 38%'
fitted FPGA openFPGALoader ofl-flash 'Program 38%' 40
check "less: the stages done counted" 'FPGA: flash · ✓ 2 · Program 38%'
fitted FPGA openFPGALoader ofl-flash 'Program 38%' 32
check "the panel's own width: the stages done left out" 'FPGA: flash · Program 38%'
fitted FPGA openFPGALoader ofl-flash 'Program 38%' 20
check "narrower: the kind of work left out too" 'FPGA: Program 38%'
fitted ESP esptool esptool5 'Program 38%' 40
check "the stage's sizes, where they fit" 'ESP: flash · Program 38% 32.0/82.8 KB'
fitted ESP esptool esptool5 'Program 38%' 32
check "the kind of work left out before them" 'ESP: Program 38% 32.0/82.8 KB'
fitted ESP esptool esptool5 'Program 38%' 24
check "and where they do not fit, its percent alone" 'ESP: Program 38%'

# Output that is no engine's leaves the button and the row as they were.
cat >"$dir/plain.txt" <<'EOF'
Erasing the old build folder
Done
Writing at the end: 50%
OK
EOF
printf '#!/bin/sh\ncat "%s"\n' "$dir/plain.txt" >"$B/plain"
chmod +x "$B/plain"
got=$(BLIT_INIT="$dir/init.lua" "$blit" --offscreen 3000x700 --shot "$dir/shot.bmp" --script "workdir:$T;frame;buildrun:plain;buildwait;progressdump;scandump" 2>&1 >/dev/null)
want <<'EOF'
button: ▶ plain | row: done
problems: 0
EOF
check "lines that are no engine's" "$expected"

exit $failed
