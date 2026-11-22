#!/bin/sh
# Flashing the board and programming an FPGA (flash.cpp, appflash.cpp):
# each tool's command line for each kind of board, its progress and what it
# came to on the Run button and the configuration's row, "only if changed"
# (and Flash anyway; a failed or stopped flash never remembered as done),
# the build run first, a failure's problem, the keys, the Boards view
# (openFPGALoader's --scan-usb and --detect beside the probe picker's rows),
# and a board on another machine (tests/fake-ssh.sh stands in for ssh).
#
# Each tool is a stand-in on the PATH that writes its name and arguments to
# a log, then replays a sample of the tool's output (tests/replay.sh; the
# samples of tests/progress/, and tests/flash/ for the Boards view, each
# saying where it is from).
#
#   tests/flash.sh path/to/blit            run the cases
#   tests/flash.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/flash.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd -P)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
export BLIT_RECENT_FILE="$dir/recent" HOME="$dir/home" REPLAY_HOLD="$dir/hold"
export BLIT_SSH="$here/fake-ssh.sh" BLIT_FAKE_HOME="$dir/there"
mkdir -p "$HOME" "$REPLAY_HOLD" "$dir/there"
LOG="$dir/log"
SAMPLE="$dir/sample"  # (the sample the next tool run replays)
export LOG SAMPLE

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

# The stand-ins: each writes its name and its arguments (one with a space
# in it quoted) to the log, J-Link's command file under it, then replays
# the sample named in $SAMPLE. openFPGALoader asked for --scan-usb or
# --detect replays $OFL_SCAN or $OFL_DETECT instead.
B="$dir/bin"
mkdir -p "$B"
standin() {
    tool=$1
    cat >"$B/$tool" <<EOF
#!/bin/sh
case "\${1:-}" in
    --scan-usb) exec sh "$here/replay.sh" "\$OFL_SCAN" ;;
    --detect) printf '%s %s\n' "$tool" "\$*" >>"\$OFL_LOG"; exec sh "$here/replay.sh" "\$OFL_DETECT" ;;
esac
{
    printf '%s' "$tool"
    for a in "\$@"; do case \$a in *' '*) printf " '%s'" "\$a" ;; *) printf ' %s' "\$a" ;; esac; done
    echo
    case " \$* " in *" -CommandFile "*) for a in "\$@"; do f=\$a; done; sed 's/^/  | /' "\$f" ;; esac
} >>"\$LOG"
exec sh "$here/replay.sh" "\$(cat "\$SAMPLE")"
EOF
    chmod +x "$B/$tool"
}
for tool in esptool openocd openocd-esp32 probe-rs JLinkExe openFPGALoader; do standin $tool; done
export PATH="$B:$PATH"
export OFL_LOG="$dir/ofl-log"

cat >"$dir/init.lua" <<'EOF'
blit.opt.lsp.enabled = false
blit.opt.git.enabled = false
EOF
export BLIT_INIT="$dir/init.lua"

# mkelf FILE LOADED TAIL: a 32-bit ARM ELF whose one PT_LOAD segment (at
# 0x08000000) is 256 bytes of LOADED, followed by 64 bytes of TAIL that no
# segment loads (as debug information is).
cat >"$dir/mkelf" <<'EOF'
#!/bin/sh
exec perl -e '
    my ($file, $loaded, $tail) = @ARGV;
    my $eh = pack("a4CCCx9vvVVVVVvvvvvv", "\x7fELF", 1, 1, 1, 2, 40, 1, 0x08000000, 52, 0, 0, 52, 32, 1, 40, 0, 0);
    my $ph = pack("VVVVVVVV", 1, 84, 0x08000000, 0x08000000, 256, 256, 5, 4);
    open(my $f, ">", $file) or die; binmode $f;
    print $f $eh, $ph, $loaded x 256, $tail x 64;
' "$@"
EOF
chmod +x "$dir/mkelf"
mkelf() { "$dir/mkelf" "$@"; }

# The project: run.json with one configuration, its board as each case
# writes it (board JSON).
P="$dir/proj"
mkdir -p "$P/.blit" "$P/build"
board() { printf '{"configurations": [{"name": "fw", "program": "build/fw.elf", "board": %s}]}\n' "$1" >"$P/.blit/run.json"; }
mkelf "$P/build/fw.elf" A x

# flash SAMPLE SCRIPT [FOLDER]: the sample the tool replays, and the script
# run in FOLDER (the project); what the script printed, then the log.
# (A run's time, where the tool does not give it, as N.)
flash() {
    rm -f "$REPLAY_HOLD"/* "$LOG"
    printf '%s\n' "$here/progress/$1.txt" >"$SAMPLE"
    script=$(printf '%s' "$2" | sed -e "s#go:\([a-z]*\)#lua:io.open('$REPLAY_HOLD/\1','w'):close()#g")
    case ${3:-} in ssh:*) at="frame;$script" folder=$3 ;; *) at="workdir:${3:-$P};frame;$script" folder= ;; esac
    got=$(BLIT_INIT="${init:-$dir/init.lua}" OFL_SCAN="$here/flash/ofl-scan-${scan:-none}.txt" OFL_DETECT="$here/flash/ofl-detect-${detect:-none}.txt" \
        "$blit" --offscreen 3000x700 --shot "$dir/shot.bmp" --script "$at" ${folder:+"$folder"} 2>&1 >/dev/null |
        sed -e "s#$dir#~#g" -e "s/ in [0-9]*\\.[0-9] s/${timed:- in N s}/g")
    timed= init= scan= detect=
    [ -f "$LOG" ] && got="$got
$(sed -e "s#$dir#~#g" -e 's#-CommandFile [^ ]*#-CommandFile FILE#' "$LOG")"
}
F="command:Flash the board;flashwait"
end="go:a;go:b;buildwait;progressdump"

# ---------------------------------------------------------------- each tool

# An ESP32-S3 on its USB-Serial/JTAG, an ESP-IDF project: esptool (the
# probe's kind says it), the files and addresses flasher_args.json gives
# (written as ESP-IDF v5.5's components/esptool_py/flasher_args.json.in
# writes it), its options and resets in esptool v5's words.
mkdir -p "$P/build/bootloader" "$P/build/partition_table"
printf 'boot' >"$P/build/bootloader/bootloader.bin"
printf 'parts' >"$P/build/partition_table/partition-table.bin"
printf 'app' >"$P/build/app.bin"
cat >"$P/build/flasher_args.json" <<'EOF'
{
    "write_flash_args" : [ "--flash_mode", "dio",
                           "--flash_size", "2MB",
                           "--flash_freq", "80m" ],
    "flash_settings" : {
        "flash_mode": "dio",
        "flash_size": "2MB",
        "flash_freq": "80m"
    },
    "flash_files" : {
        "0x0" : "bootloader/bootloader.bin",
        "0x10000" : "app.bin",
        "0x8000" : "partition_table/partition-table.bin"
    },
    "bootloader" : { "offset" : "0x0", "file" : "bootloader/bootloader.bin", "encrypted" : "false" },
    "app" : { "offset" : "0x10000", "file" : "app.bin", "encrypted" : "false" },
    "extra_esptool_args" : {
        "after"  : "hard_reset",
        "before" : "default_reset",
        "stub"   : true,
        "chip"   : "esp32s3"
    }
}
EOF
ESP='"probe": {"kind": "esp-usb-jtag", "serial": "F4:12:FA:00:00:01", "usb": "303a:1001"}, "serial": {"port": "/dev/ttyACM3"}, "elf": "build/app.elf"'
board "{$ESP}"
timed='&'
flash esptool5 "$F;progresswait:Program 54%;progressdump;$end"
want <<'EOF'
button: ■ Program 54% 12.0/22.0 KB | row: Program 54% 12.0/22.0 KB
button: ▶ fw · Flashed 40.2 KB in 0.3 s, verified | row: done · Flashed 40.2 KB in 0.3 s, verified
esptool --chip esp32s3 -p /dev/ttyACM3 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 2MB --flash-freq 80m 0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/app.bin
EOF
check "ESP32-S3, ESP-IDF: esptool with flasher_args.json's files, its progress, then the summary" "$expected"

# The same board through Espressif's OpenOCD: program_esp for each file,
# the interface and target for the probe and the chip; its failure (no
# board) a problem.
board "{$ESP, \"flash\": \"openocd\"}"
flash openocd-esp32-fail "$F;buildwait;progressdump;scandump"
want <<'EOF'
button: ▶ fw · 1 error | row: failed: exited with code 1
problems: 1
E openocd:1 OpenOCD init failed: esp_usb_jtag: could not find or open device!
openocd-esp32 -f interface/esp_usb_jtag.cfg -c 'adapter serial F4:12:FA:00:00:01' -f target/esp32s3.cfg -c 'program_esp build/bootloader/bootloader.bin 0x0 verify' -c 'program_esp build/partition_table/partition-table.bin 0x8000 verify' -c 'program_esp build/app.bin 0x10000 verify reset exit'
EOF
check "ESP32-S3 through Espressif's OpenOCD: program_esp, and a failure's problem" "$expected"

# An ST-LINK: probe-rs (its kind says it), the probe by its ids and serial
# number, verified and reset; its bars.
STLINK='"probe": {"kind": "stlink", "serial": "004D00283133510F37363734", "usb": "0483:374e"}, "chip": "STM32F411CEUx", "elf": "build/fw.elf"'
board "{$STLINK}"
timed='&'
flash probe-rs "$F;progresswait:Program 62%;progressdump;$end"
want <<'EOF'
button: ■ Program 62% 25.0 KB | row: Erase ✓ · Program 62% 25.0 KB
button: ▶ fw · Flashed 40.2 KB in 1.8 s, verified | row: done · Flashed 40.2 KB in 1.8 s, verified
probe-rs download --chip STM32F411CEUx --probe 0483:374e:004D00283133510F37363734 --non-interactive --verify --reset build/fw.elf
EOF
check "ST-LINK: probe-rs, its bars, the summary" "$expected"

# The same through OpenOCD: the ST-LINK's interface, its serial number, the
# STM32F4's target, program verify reset exit.
board "{$STLINK, \"flash\": \"openocd\"}"
timed='&'
flash openocd "$F;progresswait:Program;progressdump;$end"
want <<'EOF'
button: ■ Program | row: Program
button: ▶ fw · Flashed 40.2 KB in 1.2 s, verified | row: done · Flashed 40.2 KB in 1.2 s, verified
openocd -f interface/stlink.cfg -c 'adapter serial 004D00283133510F37363734' -f target/stm32f4x.cfg -c 'program build/fw.elf verify reset exit'
EOF
check "ST-LINK through OpenOCD" "$expected"

# The Raspberry Pi Debug Probe (CMSIS-DAP) with a raw .bin: probe-rs given
# its format and address.
printf 'raw' >"$P/build/fw.bin"
board '{"probe": {"kind": "cmsis-dap", "serial": "E6633861A3978538", "usb": "2e8a:000c"}, "chip": "RP2040", "elf": "build/fw.bin", "flash": {"address": "0x10000000"}}'
flash probe-rs-fail "$F;buildwait;progressdump;scandump"
want <<'EOF'
button: ▶ fw · 1 error | row: failed: exited with code 1
problems: 1
E probe-rs:1 No connected probes were found.
probe-rs download --chip RP2040 --probe 2e8a:000c:E6633861A3978538 --non-interactive --binary-format bin --base-address 0x10000000 --verify --reset build/fw.bin
EOF
check "CMSIS-DAP with a .bin: its format and address; a failure's problem" "$expected"

# A J-Link: J-Link Commander with a command file made for the run.
board '{"probe": {"kind": "jlink", "serial": "000801234567", "usb": "1366:1015"}, "chip": "STM32F411RE", "elf": "build/fw.elf"}'
timed='&'
flash jlink "$F;$end"
want <<'EOF'
button: ▶ fw · Flashed 128 KB in 3.9 s, verified | row: done · Flashed 128 KB in 3.9 s, verified
JLinkExe -NoGui 1 -ExitOnError 1 -USB 000801234567 -CommandFile FILE
  | device STM32F411RE
  | si SWD
  | speed 4000
  | connect
  | loadfile build/fw.elf
  | r
  | g
  | exit
EOF
check "J-Link: J-Link Commander and its command file" "$expected"
got=$(PATH="$dir/none:/usr/bin:/bin" "$blit" --offscreen 3000x700 --shot "$dir/shot.bmp" --script "workdir:$P;frame;$F;buildwait" 2>&1 >/dev/null | tail -1)
check "a tool not on the PATH: said" 'blit: JLinkExe is not on the PATH'

# A Basys 3: openFPGALoader with the board's name and its cable's serial
# number, the newest bitstream the build made, into SRAM; the FPGA's flash
# with -f and --verify.
mkdir -p "$P/build/old"
printf 'old' >"$P/build/old/top.bit"
touch -t 202601010000 "$P/build/old/top.bit"
printf 'bits' >"$P/build/top.bit"
BASYS='"probe": {"kind": "ftdi", "serial": "210183A8F2C1", "usb": "0403:6010"}'
board "{$BASYS, \"flash\": {\"tool\": \"openfpgaloader\", \"board\": \"basys3\"}}"
flash ofl-sram "$F;progresswait:Load 45%;progressdump;$end"
want <<'EOF'
button: ■ Load 45% | row: Load 45%
button: ▶ fw · Loaded into SRAM in N s | row: done · Loaded into SRAM in N s
openFPGALoader -b basys3 --usb-serial-num 210183A8F2C1 build/top.bit
EOF
check "Basys 3: openFPGALoader into SRAM, the newest bitstream" "$expected"
flash ofl-flash "command:Program the FPGA's flash;flashwait;progresswait:Program 45%;progressdump;$end"
want <<'EOF'
button: ■ Program 45% | row: Load ✓ · Erase ✓ · Program 45%
button: ▶ fw · Flashed in N s, verified | row: done · Flashed in N s, verified
openFPGALoader -b basys3 --usb-serial-num 210183A8F2C1 -f --verify build/top.bit
EOF
check "Basys 3: the FPGA's flash, verified" "$expected"
board "{$BASYS, \"bitstream\": \"build/old/top.bit\"}"
flash ofl-sram "command:Program the FPGA (SRAM);flashwait;$end"
got=$(printf '%s' "$got" | tail -1)
check "an FTDI board with no tool said: openFPGALoader on Digilent's cable; the board's own bitstream" 'openFPGALoader -c digilent --usb-serial-num 210183A8F2C1 build/old/top.bit'
board "{$STLINK}"
got=$("$blit" --offscreen 3000x700 --shot "$dir/shot.bmp" --script "workdir:$P;frame;command:Program the FPGA (SRAM);flashwait" 2>&1 >/dev/null | tail -1)
check "programming an FPGA with an MCU's board: said" "blit: fw's board is not an FPGA's (it is flashed with probe-rs)"

# ---------------------------------------------------------------- only if changed

# A flash that went well is not run again with the same image (the row and
# the button say so); a build that changes only what no segment loads is
# the same image; Flash anyway runs it; a changed image runs.
board "{$STLINK}"
flash probe-rs "$F;$end;$F;buildwait;progressdump"
want <<'EOF'
button: ▶ fw · Flashed 40.2 KB in N s, verified | row: done · Flashed 40.2 KB in N s, verified
button: ▶ fw · Not flashed: unchanged since the last flash | row: Not flashed: unchanged since the last flash
probe-rs download --chip STM32F411CEUx --probe 0483:374e:004D00283133510F37363734 --non-interactive --verify --reset build/fw.elf
EOF
check "flashed again with the same image: not run, and said" "$expected"
flash probe-rs "$F;$end;lua:os.execute('touch $P/build/fw.elf');$F;buildwait;progressdump;command:Flash anyway;flashwait;go:a;buildwait;progressdump"
got=$(printf '%s' "$got" | sed -n '2,$p')
want <<'EOF'
button: ▶ fw · Not flashed: unchanged since the last flash | row: Not flashed: unchanged since the last flash
button: ▶ fw · Flashed 40.2 KB in N s, verified | row: done · Flashed 40.2 KB in N s, verified
probe-rs download --chip STM32F411CEUx --probe 0483:374e:004D00283133510F37363734 --non-interactive --verify --reset build/fw.elf
probe-rs download --chip STM32F411CEUx --probe 0483:374e:004D00283133510F37363734 --non-interactive --verify --reset build/fw.elf
EOF
check "Flash anyway: run" "$expected"
flash probe-rs "$F;go:a;buildwait;lua:os.execute('$dir/mkelf $P/build/fw.elf A y');$F;buildwait;progressdump;lua:os.execute('$dir/mkelf $P/build/fw.elf B y');$F;buildwait;progressdump"
got=$(printf '%s' "$got" | sed -n '1,2p'; printf '%s runs' "$(wc -l <"$LOG" | tr -d ' ')")
want <<'EOF'
button: ▶ fw · Not flashed: unchanged since the last flash | row: Not flashed: unchanged since the last flash
button: ▶ fw · Flashed 40.2 KB in N s, verified | row: done · Flashed 40.2 KB in N s, verified
2 runs
EOF
check "what no segment loads changed: the same image; what one loads changed: flashed" "$expected"
mkelf "$P/build/fw.elf" A x

# A failed flash is not remembered as done (the same image again is
# flashed); nor is one stopped, which also forgets the flash before it.
flash probe-rs-fail "$F;buildwait;progressdump;lua:io.open('$SAMPLE','w'):write('$here/progress/probe-rs.txt'):close();$F;go:a;buildwait;progressdump"
got=$(printf '%s' "$got" | sed -n '1,2p')
want <<'EOF'
button: ▶ fw · 1 error | row: failed: exited with code 1
button: ▶ fw · Flashed 40.2 KB in N s, verified | row: done · Flashed 40.2 KB in N s, verified
EOF
check "a failed flash is not remembered: the same image again is flashed" "$expected"
flash probe-rs "$F;go:a;buildwait;lua:os.remove('$REPLAY_HOLD/a');command:Flash anyway;flashwait;progresswait:Program 62%;command:Stop the build;buildwait;$F;go:a;buildwait;progressdump"
got=$(printf '%s' "$got" | sed -n '1p'; printf '%s runs' "$(wc -l <"$LOG" | tr -d ' ')")
want <<'EOF'
button: ▶ fw · Flashed 40.2 KB in N s, verified | row: done · Flashed 40.2 KB in N s, verified
3 runs
EOF
check "a stopped flash is not remembered, and the one before it is forgotten: the same image again is flashed" "$expected"

# An SRAM load is always run (the board's power may have gone since); the
# FPGA's flash is not run again unchanged.
board "{$BASYS, \"flash\": {\"tool\": \"openfpgaloader\", \"board\": \"basys3\"}}"
flash ofl-sram "$F;go:a;buildwait;lua:os.remove('$REPLAY_HOLD/a');$F;go:a;buildwait;progressdump"
got=$(printf '%s' "$got" | sed -n '1p'; printf '%s runs' "$(wc -l <"$LOG" | tr -d ' ')")
want <<'EOF'
button: ▶ fw · Loaded into SRAM in N s | row: done · Loaded into SRAM in N s
2 runs
EOF
check "an SRAM load twice: run twice" "$expected"
flash ofl-flash "command:Program the FPGA's flash;flashwait;go:a;buildwait;command:Program the FPGA's flash;flashwait;progressdump"
got=$(printf '%s' "$got" | sed -n '1p'; printf '%s runs' "$(wc -l <"$LOG" | tr -d ' ')")
want <<'EOF'
button: ▶ fw · Not programmed: unchanged since the last flash | row: Not programmed: unchanged since the last flash
1 runs
EOF
check "the FPGA's flash twice: the second not run" "$expected"

# ---------------------------------------------------------------- the build first

# The Build tab's Build runs first, then (as it went well) the flash; a
# build that fails flashes nothing.
cat >"$dir/build.lua" <<EOF
blit.opt.lsp.enabled = false
blit.opt.git.enabled = false
blit.build.add("Stand-in", {detect = function() return true end, tasks = {
  {name = "Build", cmd = "echo built >>$LOG; exit \$(cat $dir/status)", default = true},
}})
EOF
board "{$STLINK}"
echo 0 >"$dir/status"
init="$dir/build.lua"
flash probe-rs "$F;go:a;buildwait;progressdump"
want <<'EOF'
button: ▶ fw · Flashed 40.2 KB in N s, verified | row: done · Flashed 40.2 KB in N s, verified
built
probe-rs download --chip STM32F411CEUx --probe 0483:374e:004D00283133510F37363734 --non-interactive --verify --reset build/fw.elf
EOF
check "the build first, then the flash" "$expected"
echo 3 >"$dir/status"
init="$dir/build.lua"
flash probe-rs "$F;buildwait"
want <<'EOF'
blit: Build failed: flashing fw did not start
built
EOF
check "a build that fails: no flash" "$expected"

# ---------------------------------------------------------------- the keys

# gB in Normal mode, Cmd+Ctrl+B (Alt+Shift+B where Ctrl is the editor's
# key) anywhere; named in the command search.
if [ "$(uname -s)" = Darwin ]; then chord=cmd+ctrl+B keys='Cmd+Ctrl+B  gB'; else chord=alt+shift+B keys='Alt+Shift+B  gB'; fi
board "{$STLINK}"
printf 'notes\n' >"$P/notes.txt"  # (a file in front, its text with the keyboard)
flash probe-rs "open:$P/notes.txt;key:cmd+shift+p;text:flash the board;finddump;key:Escape"
got=$(printf '%s' "$got" | sed -e 's/^commands [[][^]]*[]] [0-9]* : >//' -e 's/) .*/)/')
check "the command search: Flash the board first, with its keys" "Flash the board ($keys)"
for k in "vim:gB" "key:$chord"; do
    flash probe-rs "open:$P/notes.txt;$k;flashwait;go:a;buildwait;progressdump"
    want <<'EOF'
button: ▶ fw · Flashed 40.2 KB in N s, verified | row: done · Flashed 40.2 KB in N s, verified
probe-rs download --chip STM32F411CEUx --probe 0483:374e:004D00283133510F37363734 --non-interactive --verify --reset build/fw.elf
EOF
    check "$k flashes the board" "$expected"
done
flash probe-rs "open:$P/notes.txt;key:cmd+shift+p;text:program the fpga;finddump;key:Escape"
got=$(printf '%s' "$got" | sed -e 's/^commands [[][^]]*[]] [0-9]* : >//' -e "s/ Record .*//")
check "the command search: programming the FPGA, both ways, first" "Program the FPGA (SRAM) Program the FPGA's flash"

# ---------------------------------------------------------------- the Boards view

# The devices: a Basys 3's FT2232 (its console on interface B) and an
# ST-LINK, as Linux's /sys/bus/usb/devices has them (tests/probes.sh's).
# dev FOLDER VID PID MANUFACTURER PRODUCT SERIAL; iface FOLDER NUMBER TTY
# (cdc-acm's tty/ttyACM0, a usb-serial driver's ttyUSB0/tty/ttyUSB0).
U="$dir/usb"
dev() {
    mkdir -p "$U/$1"
    printf '%s\n' "$2" >"$U/$1/idVendor"
    printf '%s\n' "$3" >"$U/$1/idProduct"
    printf '00\n' >"$U/$1/bDeviceClass"
    printf '%s\n' "$4" >"$U/$1/manufacturer"
    printf '%s\n' "$5" >"$U/$1/product"
    printf '%s\n' "$6" >"$U/$1/serial"
}
iface() {
    mkdir -p "$U/$1/$3"
    printf '%s\n' "$2" >"$U/$1/bInterfaceNumber"
}
dev 1-1 0483 374e STMicroelectronics STLINK-V3 004D00283133510F37363734
iface 1-1:1.1 01 tty/ttyACM0
dev 1-7 0403 6010 Digilent "Digilent USB Device" 210183A8F2C1
iface 1-7:1.0 00 ttyUSB0/tty/ttyUSB0
iface 1-7:1.1 01 ttyUSB1/tty/ttyUSB1
export BLIT_USB_ROOT="$U"
rm -rf "$P/.blit"
mkdir -p "$P/.blit"
scan=basys3 detect=basys3
flash probe-rs "command:Choose the probe;probedump"
want <<'EOF'
fpga ftdi 0403:6010: Digilent FT2232 (FPGA)  210183A8F2C1  /dev/ttyUSB1 | fpga ftdi 0403:6010: Basys 3 (xc7a35t)  210183A8F2C1  /dev/ttyUSB1 | fpga ftdi 0403:6010: Arty A7-35T (xc7a35t)  210183A8F2C1  /dev/ttyUSB1 | fpga ftdi 0403:6010: Cmod A7-35T (xc7a35t)  210183A8F2C1  /dev/ttyUSB1 | probe stlink 0483:374e: ST-LINK V3E  004D00283133510F37363734  /dev/ttyACM0
EOF
check "a Basys 3 seen by openFPGALoader: the boards with its FPGA, after its cable's row" "$expected"
got=$(cat "$OFL_LOG")
check "(--detect asked through Digilent's cable, by its serial number)" 'openFPGALoader --detect -c digilent --usb-serial-num 210183A8F2C1'
scan=basys3 detect=basys3
flash probe-rs "command:Choose the probe;probewait;text:Basys 3;key:Return;probewait;flashdump"
want <<'EOF'
blit: proj's board: Basys 3 (xc7a35t) 210183A8F2C1, /dev/ttyUSB1
proj: tool openfpgaloader | board basys3 | cable  | interface  | target  | device  | address  | bitstream 
EOF
check "Enter on the Basys 3: the board's flash openFPGALoader with -b basys3, its probe the cable" "$expected"
got=$(tr '\n' '|' <"$P/.blit/run.json")
check "(the file)" '{|  "configurations": [|    {|      "name": "proj",|      "program": "build/proj",|      "board": {|        "probe": {"kind": "ftdi", "serial": "210183A8F2C1", "usb": "0403:6010"},|        "serial": {"port": "/dev/ttyUSB1"},|        "flash": {"tool": "openfpgaloader", "board": "basys3"}|      }|    }|  ]|}|'
printf '{"configurations": [{"name": "proj", "program": "build/proj", "board": {"flash": {"tool": "probe-rs", "address": "0x0", "cable": "ft2232"}}}]}\n' >"$P/.blit/run.json"
scan=basys3 detect=basys3
flash probe-rs "command:Choose the probe;probewait;text:Cmod;key:Return;probewait;flashdump"
got=$(printf '%s' "$got" | tail -1)
check "a flash object there already: its tool and board set, its cable gone, the rest kept" 'proj: tool openfpgaloader | board cmoda7_35t | cable  | interface  | target  | device  | address 0x0 | bitstream '

# openFPGALoader's own output with no board (captured on the Mac and on the
# Linux box): nothing is added; the same with openFPGALoader itself, where
# it is installed.
flash probe-rs "command:Choose the probe;probedump"
check "openFPGALoader seeing nothing (its real output): the picker's rows alone" 'fpga ftdi 0403:6010: Digilent FT2232 (FPGA)  210183A8F2C1  /dev/ttyUSB1 | probe stlink 0483:374e: ST-LINK V3E  004D00283133510F37363734  /dev/ttyACM0'
scan=basys3
flash probe-rs "command:Choose the probe;probedump"
check "a cable seen but no FPGA on it (--detect's real failure): the picker's rows alone" 'fpga ftdi 0403:6010: Digilent FT2232 (FPGA)  210183A8F2C1  /dev/ttyUSB1 | probe stlink 0483:374e: ST-LINK V3E  004D00283133510F37363734  /dev/ttyACM0'
if ofl=$(PATH=$(printf '%s' "$PATH" | sed "s#^$B:##") command -v openFPGALoader) && ! "$ofl" --detect -c digilent >/dev/null 2>&1; then
    rm "$B/openFPGALoader"
    ln -s "$ofl" "$B/openFPGALoader"
    flash probe-rs "command:Choose the probe;probedump"
    check "openFPGALoader itself, with no board: the picker's rows alone" 'fpga ftdi 0403:6010: Digilent FT2232 (FPGA)  210183A8F2C1  /dev/ttyUSB1 | probe stlink 0483:374e: ST-LINK V3E  004D00283133510F37363734  /dev/ttyACM0'
    rm "$B/openFPGALoader"
    standin openFPGALoader
fi

# ---------------------------------------------------------------- another machine

# A board on another machine: the tool found and run there (its stand-in
# on the PATH there), the image read there; the Boards view from
# openFPGALoader there, each row saying where.
T="$dir/there/fw"
mkdir -p "$T/.blit" "$T/build"
mkelf "$T/build/fw.elf" A x
# (A login there finds the stand-ins first: blit takes the PATH a login there has.)
for f in .profile .zprofile .bash_profile; do printf 'PATH="%s:$PATH"\n' "$B" >"$dir/there/$f"; done
printf '{"configurations": [{"name": "fw", "program": "build/fw.elf", "board": {%s}}]}\n' "$STLINK" >"$T/.blit/run.json"
timed='&'
flash probe-rs "remotewait;frame;boarddump;$F;go:a;buildwait;progressdump" "ssh://box$T"
got=$(printf '%s' "$got" | tail -2)
want <<'EOF'
button: ▶ fw · Flashed 40.2 KB in 1.8 s, verified | row: done · Flashed 40.2 KB in 1.8 s, verified
probe-rs download --chip STM32F411CEUx --probe 0483:374e:004D00283133510F37363734 --non-interactive --verify --reset build/fw.elf
EOF
check "a board on another machine: flashed there" "$expected"
flash probe-rs "remotewait;frame;boarddump;$F;go:a;buildwait;$F;buildwait;progressdump" "ssh://box$T"
got=$(printf '%s' "$got" | tail -2 | head -1)
check "(and not again, unchanged)" 'button: ▶ fw · Not flashed: unchanged since the last flash | row: Not flashed: unchanged since the last flash'
# Asked for as soon as the connection is up, its run configurations still
# being read there: the board is looked for once they have been, not said
# to be missing. (A new image, so that it flashes again.)
mkelf "$T/build/fw.elf" B y
timed='&'
flash probe-rs "remotewait;$F;go:a;buildwait;progressdump" "ssh://box$T"
got=$(printf '%s' "$got" | tail -2)
check "a board on another machine, asked for before its folder is read" "$expected"
scan=basys3 detect=basys3
flash probe-rs "remotewait;command:Choose the probe;probedump" "ssh://box$T"
check "the Boards view there" 'fpga ftdi 0403:6010: Digilent FT2232 (FPGA)  210183A8F2C1  /dev/ttyUSB1  on box | fpga ftdi 0403:6010: Basys 3 (xc7a35t)  210183A8F2C1  /dev/ttyUSB1  on box | fpga ftdi 0403:6010: Arty A7-35T (xc7a35t)  210183A8F2C1  /dev/ttyUSB1  on box | fpga ftdi 0403:6010: Cmod A7-35T (xc7a35t)  210183A8F2C1  /dev/ttyUSB1  on box | probe stlink 0483:374e: ST-LINK V3E  004D00283133510F37363734  /dev/ttyACM0  on box'

[ "$failed" = 0 ] && echo "flash: all passed"
exit "$failed"
