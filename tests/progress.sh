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
flash openFPGALoader openFPGALoader ofl-flash "progresswait:Program 45%;progressdump;$end"
want <<'EOF'
button: ■ Program 45% | row: Load ✓ · Erase ✓ · Program 45%
button: ▶ openFPGALoader · Flashed in N s, verified | row: done · Flashed in N s, verified
problems: 0
EOF
check "openFPGALoader: into flash, verified" "$expected"

# picotool: "Loading into Flash: [====   ]  62%\r", the next drawn over it.
flash picotool picotool picotool "progresswait:Program 62%;progressdump;$end"
want <<'EOF'
button: ■ Program 62% | row: Program 62%
button: ▶ picotool · Flashed in N s, verified | row: done · Flashed in N s, verified
problems: 0
EOF
check "picotool: the bar, then the summary" "$expected"

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
EOF
check "picotool with no board" "$expected"

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
