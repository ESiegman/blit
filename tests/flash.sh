#!/bin/sh
# Flashing the board and programming an FPGA (flash.cpp, appflash.cpp):
# each tool's command line for each kind of board, its progress and what it
# came to on the Run button and the configuration's row, "only if changed"
# (and Flash anyway; a failed or stopped flash never remembered as done),
# the build run first, a failure's problem, the keys, the Boards view
# (openFPGALoader's --scan-usb and --detect beside the probe picker's rows),
# an RP2040's .uf2 copied to its boot ROM's volume, and a board on another
# machine (tests/fake-ssh.sh stands in for ssh).
#
# Each tool is a stand-in on the PATH that writes its name and arguments to
# a log, then replays a sample of the tool's output (tests/replay.sh; the
# samples of tests/progress/, and tests/flash/ for the Boards view, each
# saying where it is from). The boot ROMs' volumes are folders here
# (BLIT_UF2_VOLUMES), each with the INFO_UF2.TXT its boot ROM writes, whose
# board "restarts" (the file taken away) once the copy is whole.
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
    case " \$* " in *" -CommandFile "*|*" -source "*) for a in "\$@"; do f=\$a; done; sed 's/^/  | /' "\$f" ;; esac
} >>"\$LOG"
[ -n "\${MCS:-}" ] && : >"\$f.mcs" && : >"\$f.prm"  # (what Vivado's write_cfgmem writes beside the Tcl file)
exec sh "$here/replay.sh" "\$(cat "\$SAMPLE")"
EOF
    chmod +x "$B/$tool"
}
for tool in esptool openocd openocd-esp32 probe-rs JLinkExe openFPGALoader vivado picotool dfu-util pyocd STM32_Programmer_CLI iceprog ecpprog pgrcmd; do standin $tool; done
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
$(sed -e "s#$dir#~#g" -e 's#-CommandFile [^ ]*#-CommandFile FILE#' -e 's#-source [^ ]*#-source FILE#' "$LOG")"
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
flash esptool5 "$F;progresswait:Program 38%;progressdump;$end"
want <<'EOF'
button: ■ Program 38% 32.0/82.8 KB | row: Program 38% 32.0/82.8 KB
button: ▶ fw · Flashed 170.5 KB in 8.8 s, verified | row: done · Flashed 170.5 KB in 8.8 s, verified
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

# The same board on its USB serial bridge (a CH343 on the ESP32-S3 DevKit's
# UART port), chosen in the picker: a serial port with no probe and no
# chip. esptool, since the ESP-IDF build beside the configuration's
# program wrote flasher_args.json, whose chip it takes; without one, said.
board '{"serial": {"port": "/dev/ttyACM0"}}'
timed='&'
flash esptool5 "$F;$end"
want <<'EOF'
button: ▶ fw · Flashed 170.5 KB in 8.8 s, verified | row: done · Flashed 170.5 KB in 8.8 s, verified
esptool --chip esp32s3 -p /dev/ttyACM0 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 2MB --flash-freq 80m 0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/app.bin
EOF
check "a serial bridge with an ESP-IDF build: esptool, the chip from flasher_args.json" "$expected"
mv "$P/build/flasher_args.json" "$P/build/flasher_args.away"
got=$("$blit" --offscreen 3000x700 --shot "$dir/shot.bmp" --script "workdir:$P;frame;$F" 2>&1 >/dev/null | tail -1)
mv "$P/build/flasher_args.away" "$P/build/flasher_args.json"
check "a serial bridge with no ESP-IDF build: said" "blit: fw's board is a serial port with no flash tool or chip, and no ESP-IDF build says one (board.flash, or board.chip)"

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
# An ST-LINK V2, the lab box's clone: its serial number 12 bytes of binary,
# which Linux reads as these (sysfs, in UTF-8) and OpenOCD in a language of
# its own as others, so OpenOCD is given where it is plugged in; the
# H743's target. Not plugged in: its serial number, as ever.
mkdir -p "$dir/usb-v2/6-3"
printf '0483\n' >"$dir/usb-v2/6-3/idVendor"; printf '3748\n' >"$dir/usb-v2/6-3/idProduct"; printf '00\n' >"$dir/usb-v2/6-3/bDeviceClass"
printf 'STM32 STLink\n' >"$dir/usb-v2/6-3/product"
printf '\140\150\303\277\050\140\150\303\277\050\140\150\303\277\050\n' >"$dir/usb-v2/6-3/serial"
V2='"probe": {"kind": "stlink", "serial": "6068C3BF286068C3BF286068C3BF28", "usb": "0483:3748"}, "chip": "STM32H743XIHx", "elf": "build/fw.elf", "flash": "openocd"'
board "{$V2}"
BLIT_USB_ROOT="$dir/usb-v2"; export BLIT_USB_ROOT
flash openocd "$F;$end"
got=$(printf '%s' "$got" | tail -1)
check "an ST-LINK V2 through OpenOCD: by where it is plugged in" "openocd -f interface/stlink.cfg -c 'adapter usb location 6-3' -f target/stm32h7x.cfg -c 'program build/fw.elf verify reset exit'"
rm -rf "$dir/usb-v2/6-3"
flash openocd "$F;$end"
got=$(printf '%s' "$got" | tail -1)
check "(not plugged in: its serial number)" "openocd -f interface/stlink.cfg -c 'adapter serial 6068C3BF286068C3BF286068C3BF28' -f target/stm32h7x.cfg -c 'program build/fw.elf verify reset exit'"
unset BLIT_USB_ROOT

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
case $(uname -s) in Darwin) this="this Mac" ;; *) this="this machine" ;; esac
check "a tool not on the PATH: said, naming this machine" "blit: JLinkExe is not installed on $this"

# A Basys 3: openFPGALoader with the board's name and its cable's serial
# number, the newest bitstream the build made, into SRAM; the FPGA's flash
# with -f and --verify.
mkdir -p "$P/build/old"
printf 'old' >"$P/build/old/top.bit"
touch -t 202601010000 "$P/build/old/top.bit"
printf 'bits' >"$P/build/top.bit"
BASYS='"probe": {"kind": "ftdi", "serial": "210183A8F2C1", "usb": "0403:6010"}'
board "{$BASYS, \"flash\": {\"tool\": \"openfpgaloader\", \"board\": \"basys3\"}}"
flash ofl-sram "$F;progresswait:Load 32%;progressdump;$end"
want <<'EOF'
button: ■ Load 32% | row: Load 32%
button: ▶ fw · Loaded into SRAM in N s | row: done · Loaded into SRAM in N s
openFPGALoader -b basys3 --usb-serial-num 210183A8F2C1 build/top.bit
EOF
check "Basys 3: openFPGALoader into SRAM, the newest bitstream" "$expected"
flash ofl-flash "command:Program the FPGA's flash;flashwait;progresswait:Program 38%;progressdump;$end"
want <<'EOF'
button: ■ Program 38% | row: Load ✓ · Erase ✓ · Program 38%
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

# The Basys 3 through Vivado (its board.flash): batch mode with a Tcl file
# made for the run, the cable by its serial number, the device whose part
# begins the board's chip; into SRAM, its progress and summary.
board "{$BASYS, \"chip\": \"xc7a35tcpg236-1\", \"flash\": \"vivado\"}"
flash vivado "command:Program the FPGA (SRAM);flashwait;progresswait:Load;progressdump;$end"
want <<'EOF'
button: ■ Load | row: Connect ✓ · Load
button: ▶ fw · Loaded into SRAM in N s | row: done · Loaded into SRAM in N s
vivado -mode batch -nojournal -nolog -notrace -source FILE
  | set ours [catch {close [socket localhost 3121]}]
  | open_hw_manager
  | connect_hw_server -allow_non_jtag
  | open_hw_target [lindex [get_hw_targets */210183A8F2C1*] 0]
  | set d [lindex [get_hw_devices -filter {PART =~ x*}] 0]
  | foreach h [get_hw_devices] {if {[string match [get_property PART $h]* xc7a35tcpg236-1]} {set d $h}}
  | if {[string match [get_property PART $d]* xc7a35tcpg236-1] == 0} {puts {ERROR: No device on the cable is a xc7a35tcpg236-1}; exit 1}
  | current_hw_device $d
  | set_property PROGRAM.FILE build/top.bit [current_hw_device]
  | program_hw_devices [current_hw_device]
  | close_hw_target
  | disconnect_hw_server
  | close_hw_manager
  | if {$ours} {catch {exec pkill -u $::env(USER) -x hw_server}; catch {exec pkill -u $::env(USER) -x cs_server}}
EOF
check "Basys 3 through Vivado: its Tcl, into SRAM" "$expected"
board "{$BASYS, \"flash\": \"vivado\"}"
flash vivado "command:Program the FPGA (SRAM);flashwait;$end"
got=$(printf '%s' "$got" | grep -e 'hw_device' -e 'string match')
want <<'EOF'
  | set d [lindex [get_hw_devices -filter {PART =~ x*}] 0]
  | current_hw_device $d
  | set_property PROGRAM.FILE build/top.bit [current_hw_device]
  | program_hw_devices [current_hw_device]
EOF
check "Vivado with no chip said: the first of AMD's devices" "$expected"

# A load that fails: the task failed with Vivado's exit status (not done),
# the problem in its words.
flash vivado-fail "command:Program the FPGA (SRAM);flashwait;$end;scandump"
got=$(printf '%s' "$got" | grep -v -e '^vivado' -e '^  |')
want <<'EOF'
button: ▶ fw · 1 error · stopped in Load | row: failed: exited with code 1 · stopped in Load
problems: 1
E vivado:1 File missing.bit not found. Check file name and file permissions.
EOF
check "Vivado failing: its exit status kept" "$expected"

# The FPGA's flash through Vivado: the flash part blit knows for the board
# (the Basys 3's, by its flash.board), the bitstream made a configuration
# memory file with its own SPI bus width (none said: 1), written, verified
# and booted from; its progress and summary.
board "{$BASYS, \"chip\": \"xc7a35tcpg236-1\", \"flash\": {\"tool\": \"vivado\", \"board\": \"basys3\"}}"
timed='&'
flash vivado-flash "command:Program the FPGA's flash;flashwait;progresswait:Program;progressdump;$end"
want <<'EOF'
button: ■ Program | row: Connect ✓ · Load ✓ · Erase ✓ · Program
button: ▶ fw · Flashed in 54.0 s, verified | row: done · Flashed in 54.0 s, verified
vivado -mode batch -nojournal -nolog -notrace -source FILE
  | set ours [catch {close [socket localhost 3121]}]
  | set p [lindex [get_cfgmem_parts mx25l3273f-spi-x1_x2_x4] 0]
  | set m [info script].mcs
  | write_cfgmem -force -format mcs -size [expr {max(1, [get_property MEM_DENSITY $p] / 8)}] -interface SPIx1 -loadbit [list up 0x0 build/top.bit] -file $m
  | open_hw_manager
  | connect_hw_server -allow_non_jtag
  | open_hw_target [lindex [get_hw_targets */210183A8F2C1*] 0]
  | set d [lindex [get_hw_devices -filter {PART =~ x*}] 0]
  | foreach h [get_hw_devices] {if {[string match [get_property PART $h]* xc7a35tcpg236-1]} {set d $h}}
  | if {[string match [get_property PART $d]* xc7a35tcpg236-1] == 0} {puts {ERROR: No device on the cable is a xc7a35tcpg236-1}; exit 1}
  | current_hw_device $d
  | create_hw_cfgmem -hw_device $d $p
  | set c [get_property PROGRAM.HW_CFGMEM $d]
  | set_property PROGRAM.FILES [list $m] $c
  | set_property PROGRAM.ADDRESS_RANGE use_file $c
  | set_property PROGRAM.UNUSED_PIN_TERMINATION pull-none $c
  | set_property PROGRAM.BLANK_CHECK 0 $c
  | set_property PROGRAM.ERASE 1 $c
  | set_property PROGRAM.CFG_PROGRAM 1 $c
  | set_property PROGRAM.VERIFY 1 $c
  | set_property PROGRAM.CHECKSUM 0 $c
  | create_hw_bitstream -hw_device $d [get_property PROGRAM.HW_CFGMEM_BITFILE $d]
  | program_hw_devices $d
  | program_hw_cfgmem -hw_cfgmem $c
  | boot_hw_device $d
  | close_hw_target
  | disconnect_hw_server
  | close_hw_manager
  | if {$ours} {catch {exec pkill -u $::env(USER) -x hw_server}; catch {exec pkill -u $::env(USER) -x cs_server}}
EOF
check "Basys 3 through Vivado: its Tcl, into its flash" "$expected"
flash vivado-flash "command:Program the FPGA's flash;flashwait;$end;command:Program the FPGA's flash;flashwait;buildwait;progressdump"
got=$(printf '%s' "$got" | grep -e '^button' -e '^vivado')
want <<'EOF'
button: ▶ fw · Flashed in N s, verified | row: done · Flashed in N s, verified
button: ▶ fw · Not programmed: unchanged since the last flash | row: Not programmed: unchanged since the last flash
vivado -mode batch -nojournal -nolog -notrace -source FILE
EOF
check "(the same bitstream again: not written again)" "$expected"

# The Tcl file, and the .mcs and .prm Vivado writes beside it, gone after
# the run (into flash, and into SRAM, where there are none), in zsh too.
left() { for f in $(sed -n 's/.* -source //p' "$LOG"); do ls "$f" "$f.mcs" "$f.prm" 2>/dev/null; done; }
was=${SHELL:-/bin/sh}
export MCS=1 SHELL="$(command -v zsh || printf '%s' "$was")"
flash vivado-flash "command:Program the FPGA's flash;flashwait;$end"
was_left=$(left)
unset MCS
flash vivado "command:Program the FPGA (SRAM);flashwait;$end"
got=$was_left$(left)
export SHELL="$was"
check "(the Tcl file, and the .mcs and .prm beside it, gone after)" ""

# The board's own flash part (flash.cfgmem) before the table's; a
# bitstream made for quad SPI (its header as Vivado wrote it) written as
# SPIx4, one with no width said as SPIx1.
hexfile() { grep -v '^#' "$1" | perl -ne 'print pack("H*", join("", split))' >"$2"; }
hexfile "$here/flash/vivado-head-x4.txt" "$P/build/quad.bit"
hexfile "$here/flash/vivado-head-x1.txt" "$P/build/single.bit"
board "{$BASYS, \"bitstream\": \"build/quad.bit\", \"flash\": {\"tool\": \"vivado\", \"board\": \"basys3\", \"cfgmem\": \"s25fl032p-spi-x1_x2_x4\"}}"
flash vivado-flash "command:Program the FPGA's flash;flashwait;$end"
got=$(printf '%s' "$got" | grep -e 'cfgmem_parts' -e 'write_cfgmem')
want <<'EOF'
  | set p [lindex [get_cfgmem_parts s25fl032p-spi-x1_x2_x4] 0]
  | write_cfgmem -force -format mcs -size [expr {max(1, [get_property MEM_DENSITY $p] / 8)}] -interface SPIx4 -loadbit [list up 0x0 build/quad.bit] -file $m
EOF
check "the board's flash.cfgmem, and a quad SPI bitstream" "$expected"
board "{$BASYS, \"bitstream\": \"build/single.bit\", \"flash\": {\"tool\": \"vivado\", \"board\": \"basys3\"}}"
flash vivado-flash "command:Program the FPGA's flash;flashwait;$end"
got=$(printf '%s' "$got" | grep -e 'write_cfgmem')
check "(a bitstream for one-bit SPI as Vivado writes it)" '  | write_cfgmem -force -format mcs -size [expr {max(1, [get_property MEM_DENSITY $p] / 8)}] -interface SPIx1 -loadbit [list up 0x0 build/single.bit] -file $m'

# Vivado's failure, in its words; a board blit knows no flash part for.
board "{$BASYS, \"flash\": {\"tool\": \"vivado\", \"board\": \"basys3\", \"cfgmem\": \"s25fl032p-spi-x1_x2_x4\"}}"
flash vivado-flash-wrong "command:Program the FPGA's flash;flashwait;$end;scandump"
got=$(printf '%s' "$got" | grep -v -e '^vivado' -e '^  |')
want <<'EOF'
button: ▶ fw · 1 error | row: failed: exited with code 1
problems: 1
E vivado:1 Flash Programming Unsuccessful. Part selected s25fl032p, but part mx25l3273f detected.
EOF
check "Vivado told the wrong flash part: its words" "$expected"
board "{$BASYS, \"flash\": \"vivado\"}"
got=$("$blit" --offscreen 3000x700 --shot "$dir/shot.bmp" --script "workdir:$P;frame;command:Program the FPGA's flash;flashwait" 2>&1 >/dev/null | tail -1)
check "the FPGA's flash through Vivado with no flash part: said" "blit: Vivado needs the board's flash part to write its flash: flash.cfgmem, as get_cfgmem_parts names it"
board "{$BASYS, \"flash\": {\"tool\": \"vivado\", \"board\": \"cmoda7_35t\"}}"
got=$("$blit" --offscreen 3000x700 --shot "$dir/shot.bmp" --script "workdir:$P;frame;command:Program the FPGA's flash;flashwait" 2>&1 >/dev/null | tail -1)
check "(nor for a board blit knows none for)" "blit: Vivado needs the board's flash part to write its flash: flash.cfgmem, as get_cfgmem_parts names it"
rm -f "$P/build/quad.bit" "$P/build/single.bit"

# Vivado not on the PATH: where AMD installs it, the newest version (the
# folders as 2025.1 on lay them out, and as before), else said.
X="$dir/xilinx"
for v in 2025.1/Vivado/bin 2026.2/Vivado/bin Vivado/2024.2/bin; do
    mkdir -p "$X/$v"
    printf '#!/bin/sh\nprintf "%%s\\n" "$0" >>"$LOG"\nexec sh "%s" "%s"\n' "$here/replay.sh" "$here/progress/vivado.txt" >"$X/$v/vivado"
    chmod +x "$X/$v/vivado"
done
mkdir -p "$X/2026.3/Vivado"  # (a version with no vivado in it)
rm -f "$LOG"
got=$(PATH="$dir/none:/usr/bin:/bin" BLIT_VIVADO_HOMES="$dir/none:$X" "$blit" --offscreen 3000x700 --shot "$dir/shot.bmp" --script "workdir:$P;frame;command:Program the FPGA (SRAM);flashwait;buildwait;progressdump" 2>&1 >/dev/null | sed -e "s#$dir#~#g" -e "s/ in [0-9]*\\.[0-9] s/ in N s/g"; sed -e "s#$dir#~#g" "$LOG")
want <<'EOF'
button: ▶ fw · Loaded into SRAM in N s | row: done · Loaded into SRAM in N s
~/xilinx/2026.2/Vivado/bin/vivado
EOF
check "Vivado where AMD installs it: the newest" "$expected"
rm -f "$LOG"
got=$(PATH="$dir/none:/usr/bin:/bin" BLIT_VIVADO_HOMES="$dir/none" "$blit" --offscreen 3000x700 --shot "$dir/shot.bmp" --script "workdir:$P;frame;command:Program the FPGA (SRAM);flashwait" 2>&1 >/dev/null | tail -1)
check "Vivado nowhere: said" "blit: vivado is not installed on $this (nor in /tools/Xilinx or /opt/Xilinx)"

# ---------------------------------------------------------------- RP2040 and RP2350

# mkuf2 FILE BLOCKS [FAMILY]: a UF2 of BLOCKS 512-byte blocks, 256 bytes
# of each for the flash from 0x10000000, of FAMILY (the RP2040's,
# 0xe48bff56, unless said).
cat >"$dir/mkuf2" <<'EOF'
#!/bin/sh
exec perl -e '
    my ($file, $n, $family) = @ARGV;
    $family = defined $family ? hex($family) : 0xe48bff56;
    open(my $f, ">", $file) or die; binmode $f;
    for my $i (0 .. $n - 1) {
        print $f pack("VVVVVVVV", 0x0A324655, 0x9E5D5157, 0x2000, 0x10000000 + 256 * $i, 256, $i, $n, $family), chr($i % 256) x 256, "\0" x 220, pack("V", 0x0AB16F30);
    }
' "$@"
EOF
chmod +x "$dir/mkuf2"
mkuf2() { "$dir/mkuf2" "$@"; }

# A Pico SDK build's outputs beside each other, as on the owner's Pico's
# blinky (pico-sdk 2.3.1: NAME.elf, NAME.uf2, NAME.bin); its CMakeCache.txt.
mkelf "$P/build/blinky.elf" P x
mkuf2 "$P/build/blinky.uf2" 16
printf 'raw' >"$P/build/blinky.bin"
printf 'PICO_SDK_PATH:PATH=/Users/eren/pico/pico-sdk\nPICO_PLATFORM:STRING=rp2040\n' >"$P/build/CMakeCache.txt"
PICO='"probe": {"kind": "pico-serial", "serial": "A58565170D696C53", "usb": "2e8a:000a"}, "serial": {"port": "/dev/ttyACM7"}'
BOOT='"probe": {"kind": "rp-boot", "usb": "2e8a:0003"}'
picoboard() { printf '{"configurations": [{"name": "blinky", "program": "build/blinky", "board": %s}]}\n' "$1" >"$P/.blit/run.json"; }

# The Pico running the SDK's USB serial, chosen in the picker: picotool,
# its serial number, -f (it reboots the board into BOOTSEL), the ELF the
# build made beside the configuration's program; its real output.
picoboard "{$PICO}"
flash picotool "$F;progresswait:Program 52%;progressdump;$end"
want <<'EOF'
button: ■ Program 52% | row: Reboot ✓ · Program 52%
button: ▶ blinky · Flashed in N s, verified | row: done · Flashed in N s, verified
picotool load -v -x build/blinky.elf --ser A58565170D696C53 -f
EOF
check "a Pico on its SDK's USB serial: picotool -f, its serial number, the ELF" "$expected"
# Its boot ROM (BOOTSEL), which has no serial number of its own: -f still
# (picotool takes a board in BOOTSEL as it is); a .uf2 named, or a .bin
# at its address.
picoboard "{$BOOT, \"elf\": \"build/blinky.uf2\"}"
flash picotool-pty "$F;$end"
got=$(printf '%s' "$got" | tail -1)
check "the Pico in BOOTSEL: picotool without --ser, a .uf2" 'picotool load -v -x build/blinky.uf2 -f'
picoboard "{$PICO, \"elf\": \"build/blinky.bin\", \"flash\": {\"address\": \"0x10004000\"}}"
flash picotool "$F;$end"
got=$(printf '%s' "$got" | tail -1)
check "a .bin, at its address" 'picotool load -v -x build/blinky.bin -o 0x10004000 --ser A58565170D696C53 -f'
picoboard "{$PICO, \"elf\": \"build/blinky.bin\"}"
flash picotool "$F;$end"
got=$(printf '%s' "$got" | tail -1)
check "a .bin with no address: picotool's own, the start of flash" 'picotool load -v -x build/blinky.bin --ser A58565170D696C53 -f'
# A file that is not there: said before picotool runs (with -f it would
# have rebooted the board into BOOTSEL to find it missing).
picoboard "{$PICO, \"elf\": \"build/gone.uf2\"}"
flash picotool "$F;buildwait"
check "a Pico's image not there: said, and picotool not run" "blit: Cannot read build/gone.uf2: there is no such file"
# A board with only a serial port, or nothing, and a Pico SDK build (no
# ESP-IDF one beside it): picotool.
mv "$P/build/flasher_args.json" "$P/build/flasher_args.away"
for b in '{"serial": {"port": "/dev/ttyACM7"}}' '{"chip": ""}'; do
    picoboard "$b"
    flash picotool "$F;$end"
    got=$(printf '%s' "$got" | tail -1)
    check "a board of $b, a Pico SDK build beside the ELF: picotool" 'picotool load -v -x build/blinky.elf -f'
done
mv "$P/build/CMakeCache.txt" "$P/build/CMakeCache.away"
picoboard '{"chip": ""}'
flash picotool "$F"
check "neither build: said" "blit: blinky's board has no flash tool, and its probe says none"
mv "$P/build/CMakeCache.away" "$P/build/CMakeCache.txt"
mv "$P/build/flasher_args.away" "$P/build/flasher_args.json"

# The UF2 copy ("flash": "uf2"): the .uf2 beside the ELF copied to the
# boot ROM's volume (found by its INFO_UF2.TXT), its bytes for the flash as
# it goes, then done once the volume goes (the board restarting: here a
# watcher takes the volume's INFO_UF2.TXT away once the copy is whole).
V="$dir/volumes"
mkdir -p "$V/RPI-RP2" "$V/Other" "$V/RP2350"
printf 'UF2 Bootloader v3.0\nModel: Raspberry Pi RP2\nBoard-ID: RPI-RP2\n' >"$V/RPI-RP2/INFO_UF2.TXT"
printf 'UF2 Bootloader v1.0\nModel: Some Other Board\nBoard-ID: OTHER-1\n' >"$V/Other/INFO_UF2.TXT"
export BLIT_UF2_VOLUMES="$dir/none:$V"
restarts() {  # (the board taking the copy and restarting, as the copy finishes)
    (for i in $(seq 1 200); do [ "$( (wc -c <"$1/blinky.uf2") 2>/dev/null | tr -d " ")" = 8192 ] && { rm -f "$1/INFO_UF2.TXT"; exit 0; }; sleep 0.05; done) &
}
picoboard "{$BOOT, \"flash\": \"uf2\"}"
restarts "$V/RPI-RP2"
flash picotool "$F;buildwait;progressdump;scandump"
want <<'EOF'
button: ▶ blinky · Flashed 4 KB in N s | row: done · Flashed 4 KB in N s
problems: 0
EOF
check "the UF2 copy: to the RPI-RP2 volume, its bytes for the flash, done as the volume goes" "$expected"
cmp -s "$P/build/blinky.uf2" "$V/RPI-RP2/blinky.uf2" && got=same || got=different
check "(the copy, whole; picotool not run)" same
printf 'UF2 Bootloader v3.0\nModel: Raspberry Pi RP2\nBoard-ID: RPI-RP2\n' >"$V/RPI-RP2/INFO_UF2.TXT"
rm -f "$V/RPI-RP2/blinky.uf2"
# What cannot be copied, said: a .uf2 for another family, a file that is no
# UF2, no volume, two.
mkuf2 "$P/build/other.uf2" 4 e48bff59
picoboard "{$BOOT, \"flash\": \"uf2\", \"elf\": \"build/other.uf2\"}"
flash picotool "$F"
check "a .uf2 that is not the RP2040's: said" "blit: build/other.uf2 is not for the RP2040 (no block of it is of the RP2040's family, 0xe48bff56)"
picoboard "{$BOOT, \"flash\": \"uf2\", \"elf\": \"build/blinky.bin\"}"
printf '%512s' x >"$P/build/blinky.uf2.away"
mv "$P/build/blinky.uf2" "$P/build/blinky.uf2.keep"
mv "$P/build/blinky.uf2.away" "$P/build/blinky.uf2"
flash picotool "$F"
check "a .uf2 that is no UF2: said" "blit: build/blinky.uf2 is not a UF2 (a block's magic numbers are wrong)"
mv "$P/build/blinky.uf2.keep" "$P/build/blinky.uf2"
export BLIT_UF2_VOLUMES="$dir/none"
flash picotool "$F"
export BLIT_UF2_VOLUMES="$dir/none:$V"
check "no volume: said" "blit: No RPI-RP2 volume is mounted on $this: hold BOOTSEL while plugging the board in"
printf 'UF2 Bootloader v1.1\nModel: Raspberry Pi RP2350\nBoard-ID: RP2350\n' >"$V/RP2350/INFO_UF2.TXT"
flash picotool "$F"
check "two: said" "blit: 2 RP2 boot ROM volumes are mounted on $this (~/volumes/RP2350, ~/volumes/RPI-RP2): which is the board's is not known"
rm "$V/RP2350/INFO_UF2.TXT"

# ---------------------------------------------------------------- the other flash engines

# dfu-util for an STM32's DFU boot ROM: the .bin beside the ELF, DfuSe's
# address (then leave), the device by its ids and serial number; with no
# .bin, said.
printf 'raw' >"$P/build/fw.bin"
DFU='"probe": {"kind": "dfu", "serial": "205F34915741", "usb": "0483:df11"}, "elf": "build/fw.elf", "flash": {"address": "0x08000000"}'
board "{$DFU}"
flash dfu-util "$F;progresswait:Program 50%;progressdump;$end"
want <<'EOF'
button: ■ Program 50% 20.1 KB | row: Erase ✓ · Program 50% 20.1 KB
button: ▶ fw · Flashed 40.2 KB in N s | row: done · Flashed 40.2 KB in N s
dfu-util -d 0483:df11 -S 205F34915741 -a 0 -s 0x08000000:leave -D build/fw.bin
EOF
check "dfu-util: the .bin beside the ELF, its address" "$expected"
board '{"probe": {"kind": "dfu", "usb": "1d50:6017"}, "elf": "build/fw.bin", "flash": {"tool": "dfu-util", "alt": "1"}}'
flash dfu-util "$F;$end"
got=$(printf '%s' "$got" | tail -1)
check "dfu-util, a DFU device (not DfuSe): its alternate setting, reset after" 'dfu-util -d 1d50:6017 -a 1 -D build/fw.bin -R'
mv "$P/build/fw.bin" "$P/build/fw.away"
board "{$DFU}"
flash dfu-util "$F"
mv "$P/build/fw.away" "$P/build/fw.bin"
check "dfu-util with no .bin: said" "blit: dfu-util writes a .bin or a .dfu: no build/fw.bin beside build/fw.elf"
# ST's DFU boot ROM as the picker writes it, with no flash.address: the .bin
# where the ELF loads first (objcopy -O binary starts it there); a .bin
# with no ELF, and no address: said (dfu-util itself says only "Only DfuSe
# file version 1.1a is supported").
board '{"probe": {"kind": "dfu", "serial": "200364500000", "usb": "0483:df11"}}'
flash dfu-util "$F;$end"
got=$(printf '%s' "$got" | tail -1)
check "dfu-util, ST's boot ROM with no address: the ELF's" 'dfu-util -d 0483:df11 -S 200364500000 -a 0 -s 0x08000000:leave -D build/fw.bin'
printf '{"configurations": [{"name": "fw", "program": "build/fw.bin", "board": {"probe": {"kind": "dfu", "usb": "0483:df11"}}}]}\n' >"$P/.blit/run.json"
flash dfu-util "$F"
check "and a .bin alone, with no address: said" "blit: build/fw.bin is a .bin: the board's flash.address says where it goes"

# pyOCD: its target (flash.device, else the chip), the probe's unique ID.
board '{"probe": {"kind": "cmsis-dap", "serial": "E6633861A3978538", "usb": "2e8a:000c"}, "chip": "STM32F411CEUx", "elf": "build/fw.elf", "flash": {"tool": "pyocd", "device": "STM32F411CE"}}'
flash pyocd "$F;progresswait:Program 52%;progressdump;$end"
want <<'EOF'
button: ■ Program 52% | row: Erase ✓ · Program 52%
button: ▶ fw · Flashed 16 KB in N s | row: done · Flashed 16 KB in N s
pyocd load -W -t stm32f411ce -u E6633861A3978538 build/fw.elf
EOF
check "pyOCD: its target, the probe, its progress" "$expected"
board '{"probe": {"kind": "cmsis-dap", "serial": "E6633861A3978538", "usb": "2e8a:000c"}, "elf": "build/fw.bin", "flash": {"tool": "pyocd", "address": "0x08000000"}}'
flash pyocd "$F"
check "pyOCD with no chip: said" "blit: pyOCD needs the board's chip (its target), or flash.device"

# STM32CubeProgrammer: the ST-LINK by its serial number, verified, reset.
board "{$STLINK, \"flash\": \"stm32cubeprogrammer\"}"
flash stm32cube "$F;$end"
got=$(printf '%s' "$got" | tail -2)
want <<'EOF'
button: ▶ fw · Flashed in N s, verified | row: done · Flashed in N s, verified
STM32_Programmer_CLI -c port=SWD sn=004D00283133510F37363734 -w build/fw.elf -v -rst
EOF
check "STM32CubeProgrammer: the probe, -v, -rst" "$expected"
board "{\"probe\": {\"kind\": \"stlink\", \"serial\": \"004D00283133510F37363734\", \"usb\": \"0483:374e\"}, \"elf\": \"build/fw.bin\", \"flash\": {\"tool\": \"stm32cubeprogrammer\", \"address\": \"0x08000000\"}}"
flash stm32cube "$F;$end"
got=$(printf '%s' "$got" | tail -1)
check "STM32CubeProgrammer: a .bin at its address" "STM32_Programmer_CLI -c port=SWD sn=004D00283133510F37363734 -w build/fw.bin 0x08000000 -v -rst"

# iceprog and ecpprog: the FTDI chip by its ids and serial number, into
# SRAM (-S), or the flash.
printf 'ice' >"$P/build/top.bin"
rm -f "$P/build/top.bit"
board '{"probe": {"kind": "ftdi", "serial": "FT6SK8LR", "usb": "0403:6010"}, "bitstream": "build/top.bin", "flash": "iceprog"}'
flash iceprog-sram "command:Program the FPGA (SRAM);flashwait;progresswait:Load;progressdump;$end"
want <<'EOF'
button: ■ Load | row: Load
button: ▶ fw · Loaded into SRAM in N s | row: done · Loaded into SRAM in N s
iceprog -d s:0x0403:0x6010:FT6SK8LR -S build/top.bin
EOF
check "iceprog: the chip, into SRAM" "$expected"
flash iceprog "command:Program the FPGA's flash;flashwait;progresswait:Program 49%;progressdump;$end"
want <<'EOF'
button: ■ Program 49% | row: Erase ✓ · Program 49%
button: ▶ fw · Flashed 31.5 KB in N s, verified | row: done · Flashed 31.5 KB in N s, verified
iceprog -d s:0x0403:0x6010:FT6SK8LR build/top.bin
EOF
check "iceprog: the flash, read back" "$expected"
printf 'bits' >"$P/build/top.bit"
board '{"probe": {"kind": "ftdi", "usb": "0403:6010"}, "flash": "ecpprog"}'
flash ecpprog-sram "$F;$end"
got=$(printf '%s' "$got" | tail -2)
want <<'EOF'
button: ▶ fw · Loaded into SRAM in N s | row: done · Loaded into SRAM in N s
ecpprog -d i:0x0403:0x6010 -S build/top.bit
EOF
check "ecpprog: the chip by its ids alone, into SRAM" "$expected"

# pgrcmd: the board's chain file (else the newest .xcf), run every time
# (what it writes is the .xcf's to say); its return code in words.
mkdir -p "$P/impl"
printf '<ispXCF/>\n' >"$P/impl/top.xcf"
board '{"flash": "pgrcmd"}'
flash pgrcmd "$F;buildwait;progressdump;scandump;$F;buildwait"
want <<'EOF'
button: ▶ fw · 1 error | row: failed: exited with code 251
problems: 1
E pgrcmd:1 cable not detected (return code -5)
pgrcmd -infile impl/top.xcf
pgrcmd -infile impl/top.xcf
EOF
check "pgrcmd: the .xcf, its return code in words, run again" "$expected"
rm -rf "$P/impl"
flash pgrcmd "$F"
check "pgrcmd with no .xcf: said" "blit: No chain file for pgrcmd: the board's flash.xcf, or an .xcf under the folder"

# Each tool not installed: said, naming the program and this machine.
# (One that is installed here is left out: blit would run it, on whatever
# board is plugged in; the lab box has iceprog, and its FTDI chip is the
# Basys 3's.)
for t in "pyocd pyocd" "stm32cubeprogrammer STM32_Programmer_CLI" "iceprog iceprog" "ecpprog ecpprog" "pgrcmd pgrcmd"; do
    set -- $t
    for d in /usr/bin /bin /opt/homebrew/bin /usr/local/bin "$HOME/.local/bin"; do [ -x "$d/$2" ] && continue 2; done
    board "{\"chip\": \"STM32F411CEUx\", \"elf\": \"build/fw.elf\", \"bitstream\": \"build/top.bit\", \"flash\": {\"tool\": \"$1\", \"xcf\": \"build/top.bit\"}}"
    got=$(PATH="$dir/none:/usr/bin:/bin" "$blit" --offscreen 3000x700 --shot "$dir/shot.bmp" --script "workdir:$P;frame;$F" 2>&1 >/dev/null | tail -1)
    check "$2 not installed: said" "blit: $2 is not installed on $this"
done

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
printf '{"configurations": [{"name": "proj", "program": "build/proj", "board": {"flash": {"tool": "vivado", "address": "0x0"}}}]}\n' >"$P/.blit/run.json"
scan=basys3 detect=basys3
flash probe-rs "command:Choose the probe;probewait;text:Basys 3;key:Return;probewait;flashdump"
got=$(printf '%s' "$got" | tail -1)
check "a board loaded through Vivado, chosen again: Vivado kept" 'proj: tool vivado | board basys3 | cable  | interface  | target  | device  | address 0x0 | bitstream '
printf '{"configurations": [{"name": "proj", "program": "build/proj", "board": {"flash": "vivado"}}]}\n' >"$P/.blit/run.json"
scan=basys3 detect=basys3
flash probe-rs "command:Choose the probe;probewait;text:Basys 3;key:Return;probewait;flashdump"
got=$(printf '%s' "$got" | tail -1)
check "(Vivado named alone: kept, as an object)" 'proj: tool vivado | board basys3 | cable  | interface  | target  | device  | address  | bitstream '

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

# The tools not installed there (the login shell there keeps the PATH it
# is given, which has none of the stand-ins): the flash says which, and
# where; the Boards view lists the devices, and says the boards on the
# FPGA's cable go unnamed, and why.
printf '#!/bin/sh\nshift\nexec /bin/sh -c "$@"\n' >"$dir/plainsh"
chmod +x "$dir/plainsh"
bare="$dir/bare"
mkdir -p "$bare"
ln -s /usr/bin/* /bin/* "$bare/" 2>/dev/null
rm -f "$bare/probe-rs" "$bare/openFPGALoader" "$bare/picotool" "$bare/dfu-util"
case $(uname -s) in Darwin) this="this Mac" ;; *) this="this machine" ;; esac
kept=$PATH
PATH=$bare SHELL="$dir/plainsh"
export SHELL
mkelf "$T/build/fw.elf" C z
flash probe-rs "remotewait;$F;buildwait" "ssh://box$T"
got=$(printf '%s\n' "$got" | grep '^blit: ' | tail -1)
check "a tool not installed there: said, naming that machine" 'blit: probe-rs is not installed on box'
scan=basys3 detect=basys3
flash probe-rs "remotewait;command:Choose the probe;probedump" "ssh://box$T"
got=$(printf '%s\n' "$got" | grep -v '^blit: [^o]' | tail -2 | tr '\n' '/')
check "the Boards view there without openFPGALoader: said" 'blit: openFPGALoader is not installed on box: the boards on its cables are not named/fpga ftdi 0403:6010: Digilent FT2232 (FPGA)  210183A8F2C1  /dev/ttyUSB1  on box | probe stlink 0483:374e: ST-LINK V3E  004D00283133510F37363734  /dev/ttyACM0  on box/'
# A Pico on that machine with no picotool there: its .uf2 copied to the
# boot ROM's volume there; with none mounted there, said.
mkelf "$T/build/blinky.elf" P x
mkuf2 "$T/build/blinky.uf2" 16
printf '{"configurations": [{"name": "blinky", "program": "build/blinky", "board": {%s}}]}\n' "$BOOT" >"$T/.blit/run.json"
printf 'UF2 Bootloader v3.0\nModel: Raspberry Pi RP2\nBoard-ID: RPI-RP2\n' >"$V/RPI-RP2/INFO_UF2.TXT"
rm -f "$V/RPI-RP2/blinky.uf2"
restarts "$V/RPI-RP2"
flash picotool "remotewait;$F;buildwait;progressdump" "ssh://box$T"
got=$(printf '%s\n' "$got" | tail -1)
check "a Pico there with no picotool there: the UF2 copy there" 'button: ▶ blinky · Flashed 4 KB in N s | row: done · Flashed 4 KB in N s'
export BLIT_UF2_VOLUMES="$dir/none"
flash picotool "remotewait;$F;buildwait" "ssh://box$T"
export BLIT_UF2_VOLUMES="$dir/none:$V"
got=$(printf '%s\n' "$got" | grep '^blit: ' | tail -1)
check "no picotool there, and no volume: said" 'blit: picotool is not installed on box, and no RPI-RP2 volume is mounted: hold BOOTSEL while plugging the board in'
printf '{"configurations": [{"name": "fw", "program": "build/fw.elf", "board": {%s}}]}\n' "$DFU" >"$T/.blit/run.json"
printf 'raw' >"$T/build/fw.bin"
flash dfu-util "remotewait;$F;buildwait" "ssh://box$T"
got=$(printf '%s\n' "$got" | grep '^blit: ' | tail -1)
check "dfu-util not installed there: said" 'blit: dfu-util is not installed on box'
PATH=$kept

[ "$failed" = 0 ] && echo "flash: all passed"
exit "$failed"
