#!/bin/sh
# The probe and port picker: USB devices named from probes.cpp's table (a
# probe and its serial port one row), "Choose the probe…" writing the one
# chosen into the run configuration's board in .blit/run.json (the rest of
# the file as it was written) and read back; on another machine through its
# agent (tests/fake-ssh.sh stands in for ssh); and this machine's own
# devices, whatever is plugged in, checked for their shape.
#
# The devices are folders laid out as Linux's /sys/bus/usb/devices is,
# which BLIT_USB_ROOT points blit (and its agent) at on either system. The
# ids are the table's; the strings are the firmware's where it says them
# (Black Magic: blackmagic v2.0.0 src/platforms/common/usb_descriptors.h
# lines 423-427; the Raspberry Pi Debug Probe: debugprobe-v2.3.1
# src/usb_descriptors.c lines 137-142; Digilent's product string: OpenOCD
# 0.12.0 tcl/interface/ftdi/digilent_jtag_hs3.cfg line 9), and otherwise
# stand-ins (serial numbers, the other products' names).
#
#   tests/probes.sh path/to/blit            run the cases
#   tests/probes.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/probes.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
agent=$(dirname "$blit")/blit-agent
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
T="$dir/there"
mkdir -p "$T"
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua" BLIT_RECENT_FILE="$dir/recent" BLIT_SSH="$here/fake-ssh.sh" BLIT_FAKE_HOME="$T"
# openFPGALoader, which the picker asks for the boards on an FTDI chip: a
# stand-in that finds none, so that a real one (and a real board plugged
# into this machine) is not asked.
mkdir -p "$dir/bin"
printf '#!/bin/sh
exit 0
' >"$dir/bin/openFPGALoader"
chmod +x "$dir/bin/openFPGALoader"
export PATH="$dir/bin:$PATH"

# dev ROOT NAME VID PID CLASS MANUFACTURER PRODUCT SERIAL: a device's folder.
dev() {
    mkdir -p "$1/$2"
    printf '%s\n' "$3" >"$1/$2/idVendor"
    printf '%s\n' "$4" >"$1/$2/idProduct"
    printf '%s\n' "$5" >"$1/$2/bDeviceClass"
    [ -n "$6" ] && printf '%s\n' "$6" >"$1/$2/manufacturer"
    [ -n "$7" ] && printf '%s\n' "$7" >"$1/$2/product"
    [ -n "$8" ] && printf '%s\n' "$8" >"$1/$2/serial"
    return 0
}
# iface ROOT NAME NUMBER INTERFACE TTY: an interface's folder, its tty as
# cdc-acm has it (tty/ttyACM0) or as a usb-serial driver does (ttyUSB0/).
iface() {
    mkdir -p "$1/$2"
    printf '%s\n' "$3" >"$1/$2/bInterfaceNumber"
    [ -n "$4" ] && printf '%s\n' "$4" >"$1/$2/interface"
    case "$5" in
        ttyACM*) mkdir -p "$1/$2/tty/$5" ;;
        ttyUSB*) mkdir -p "$1/$2/$5/tty/$5" ;;
    esac
    return 0
}

U="$dir/usb"
dev "$U" usb1 1d6b 0002 09 "Linux xhci-hcd" "xHCI Host Controller" "0000:0e:00.3"
dev "$U" 1-1 0483 374e 00 STMicroelectronics STLINK-V3 004D00283133510F37363734
iface "$U" 1-1:1.0 00 "" ""
iface "$U" 1-1:1.1 01 "ST-Link VCP Ctrl" ttyACM0
iface "$U" 1-1:1.2 02 "ST-Link VCP Data" ""
dev "$U" 1-2 0483 3748 00 STMicroelectronics "STM32 STLink" "$(printf 'U\377k\006IwVRR8\024\207')"
iface "$U" 1-2:1.0 00 "" ""
dev "$U" 1-3 1366 1015 00 SEGGER J-Link 000801234567
iface "$U" 1-3:1.0 00 "CDC" ttyACM1
iface "$U" 1-3:1.2 02 "BULK interface" ""
dev "$U" 1-4 2e8a 000c 00 "Raspberry Pi" "Debug Probe (CMSIS-DAP)" E6633861A3978538
iface "$U" 1-4:1.0 00 "CMSIS-DAP v2 Interface" ""
iface "$U" 1-4:1.1 01 "CDC-ACM UART Interface" ttyACM2
dev "$U" 1-5 303a 1001 ef Espressif "USB JTAG/serial debug unit" "F4:12:FA:00:00:01"
iface "$U" 1-5:1.0 00 "" ttyACM3
iface "$U" 1-5:1.2 02 "" ""
dev "$U" 1-6 1d50 6018 ef "Black Magic Debug" "Black Magic Probe v2.0.0" 7BB180B4
iface "$U" 1-6:1.0 00 "Black Magic GDB Server" ttyACM4
iface "$U" 1-6:1.2 02 "Black Magic UART Port" ttyACM5
iface "$U" 1-6:1.4 04 "Black Magic DFU" ""
dev "$U" 1-7 0403 6010 00 Digilent "Digilent USB Device" 210183A8F2C1
iface "$U" 1-7:1.0 00 "Digilent USB Device" ttyUSB0
iface "$U" 1-7:1.1 01 "Digilent USB Device" ttyUSB1
dev "$U" 1-8 10c4 ea60 00 "Silicon Labs" "CP2102N USB to UART Bridge Controller" 0001
iface "$U" 1-8:1.0 00 "" ttyUSB2
dev "$U" 1-9 1a86 7523 ff "" "USB Serial" ""
iface "$U" 1-9:1.0 00 "" ttyUSB3
dev "$U" 1-10 2e8a 0003 00 "Raspberry Pi" "RP2 Boot" E0C912D24340
dev "$U" 1-11 046d c52b 00 Logitech "USB Receiver" ""
dev "$U" 1-12 0d28 0204 00 ARM "DAPLink CMSIS-DAP" 0240000034544e45
iface "$U" 1-12:1.1 01 "mbed Serial Port" ttyACM6
dev "$U" 1-13 1366 1008 00 SEGGER J-Link 000801234568
iface "$U" 1-13:1.0 00 "CMSIS-DAP" ""
dev "$U" 1-14 05e3 0610 09 GenesysLogic "USB2.1 Hub" ""
# A Pico running the Pico SDK's USB serial, and an STM32 in its DFU boot
# ROM (the strings: the owner's Pico's, as IOKit had them; ST's, stand-ins).
dev "$U" 1-15 2e8a 000a ef "Raspberry Pi" Pico A58565170D696C53
iface "$U" 1-15:1.0 00 "Board CDC" ttyACM7
iface "$U" 1-15:1.2 02 "Reset" ""
dev "$U" 1-16 0483 df11 00 STMicroelectronics "STM32  BOOTLOADER" 205F34915741
# Nothing but a keyboard and a hub; and nothing at all.
O="$dir/others"
dev "$O" 1-1 046d c52b 00 Logitech "USB Receiver" ""
dev "$O" 1-2 05e3 0610 09 GenesysLogic "USB2.1 Hub" ""
mkdir -p "$dir/empty"

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run FOLDER SCRIPT: the last line the run printed (with the devices in
# $USB, the fake tree, unless it is empty).
USB=$U
run() {
    got=$(BLIT_USB_ROOT="$USB" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$1;frame;$2" 2>&1 >/dev/null | sed -e "s#$dir#~#g" | tail -1)
}

# The table and the rows.
P="$dir/proj"
mkdir -p "$P"
run "$P" "command:Choose the probe;probedump"
check "each probe, programmer, boot ROM and port named; a probe and its port one row; hubs, the keyboard and the rest left out; the RP2040 boot ROM's serial number (its version, the same on every RP2040) not shown" \
    'probe blackmagic 1d50:6018: Black Magic Probe  7BB180B4  /dev/ttyACM5  gdb /dev/ttyACM4 | probe cmsis-dap 0d28:0204: DAPLink CMSIS-DAP  0240000034544e45  /dev/ttyACM6 | fpga ftdi 0403:6010: Digilent FT2232 (FPGA)  210183A8F2C1  /dev/ttyUSB1 | probe esp-usb-jtag 303a:1001: Espressif USB-Serial/JTAG  F4:12:FA:00:00:01  /dev/ttyACM3 | probe jlink 1366:1015: J-Link  000801234567  /dev/ttyACM1 | probe cmsis-dap 1366:1008: J-Link  000801234568 | probe cmsis-dap 2e8a:000c: Raspberry Pi Debug Probe  E6633861A3978538  /dev/ttyACM2 | probe stlink 0483:3748: ST-LINK V2  55FF6B064977565252381487 | probe stlink 0483:374e: ST-LINK V3E  004D00283133510F37363734  /dev/ttyACM0 | boot rp-boot 2e8a:0003: RP2040 boot ROM | boot dfu 0483:df11: STM32 DFU boot ROM  205F34915741 | serial ch340 1a86:7523: CH340 serial bridge (USB Serial)  /dev/ttyUSB3 | serial cp210x 10c4:ea60: CP210x serial bridge (CP2102N USB to UART Bridge Controller)  0001  /dev/ttyUSB2 | serial pico-serial 2e8a:000a: Pico SDK USB serial (RP2040) (Pico)  A58565170D696C53  /dev/ttyACM7'
run "$P" "command:Choose the probe;probewait;finddump"
check "the box lists them, in that order (those it has room for)" \
    'pick [] 14 : >Black Magic Probe  7BB180B4  /dev/ttyACM5  gdb /dev/ttyACM4 DAPLink CMSIS-DAP  0240000034544e45  /dev/ttyACM6 Digilent FT2232 (FPGA)  210183A8F2C1  /dev/ttyUSB1 Espressif USB-Serial/JTAG  F4:12:FA:00:00:01  /dev/ttyACM3 J-Link  000801234567  /dev/ttyACM1 J-Link  000801234568'
USB=$O
run "$P" "command:Choose the probe;probedump"
check "nothing else there: the other devices (never a hub), with their ids" 'other usb 046d:c52b: USB Receiver  046d:c52b'
USB="$dir/empty"
run "$P" "command:Choose the probe;probewait"
check "nothing plugged in: said" 'blit: No USB devices are plugged in'
USB=$U

# The board written: a folder with no run.json gets one, its board the
# probe and port chosen.
run "$P" "command:Choose the probe;probewait;text:V3E;key:Return;probewait;boarddump"
check "chosen: read back as the run configuration's board" 'proj: probe stlink 004D00283133510F37363734 0483:374e gdb= | port /dev/ttyACM0 baud 0 | chip  elf  svd  flash  debug '
got=$(tr '\n' '|' <"$P/.blit/run.json")
check "the file made, as Edit the run configurations makes it, with the board" '{|  "configurations": [|    {|      "name": "proj",|      "program": "build/proj",|      "board": {|        "probe": {"kind": "stlink", "serial": "004D00283133510F37363734", "usb": "0483:374e"},|        "serial": {"port": "/dev/ttyACM0"}|      }|    }|  ]|}|'

# A run.json of the user's: the configuration the Run button runs gets the
# board; its other members, the other configuration, the order and the
# spacing stay as they were; a board's own members (chip, flash, the
# port's baud) too.
R="$dir/rc"
mkdir -p "$R/.blit"
cat >"$R/.blit/run.json" <<'EOF'
{ "configurations": [
    { "name": "app",   "program": "build/app" },
    {
        "name": "fw",
        "program": "build/fw.elf",
        "board": { "chip": "STM32F411CEUx", "probe": "OLD", "serial": { "baud": 115200, "port": "/dev/old" }, "flash": "probe-rs" },
        "debug": { "adapter": "gdb" }
    }
  ]
}
EOF
run "$R" "buildrun:fw;buildwait;command:Choose the probe;probewait;text:Black Magic;key:Return;probewait;boarddump"
check "the Run button's configuration's board: the probe with its GDB port, the console its UART port, the rest kept" \
    'fw: probe blackmagic 7BB180B4 1d50:6018 gdb=/dev/ttyACM4 | port /dev/ttyACM5 baud 115200 | chip STM32F411CEUx elf  svd  flash probe-rs debug '
got=$(tr '\n' '|' <"$R/.blit/run.json")
check "and the file otherwise as it was" \
    '{ "configurations": [|    { "name": "app",   "program": "build/app" },|    {|        "name": "fw",|        "program": "build/fw.elf",|        "board": { "chip": "STM32F411CEUx", "probe": {"kind": "blackmagic", "serial": "7BB180B4", "usb": "1d50:6018", "gdb": "/dev/ttyACM4"}, "serial": { "baud": 115200, "port": "/dev/ttyACM5" }, "flash": "probe-rs" },|        "debug": { "adapter": "gdb" }|    }|  ]|}|'
run "$R" "buildrun:fw;buildwait;command:Choose the probe;probewait;text:CP210x;key:Return;probewait;boarddump"
check "a serial bridge chosen: its port, and the probe gone (a port alone is no probe)" \
    'fw: probe    gdb= | port /dev/ttyUSB2 baud 115200 | chip STM32F411CEUx elf  svd  flash probe-rs debug '
got=$(tr '\n' '|' <"$R/.blit/run.json")
check "(with its comma)" \
    '{ "configurations": [|    { "name": "app",   "program": "build/app" },|    {|        "name": "fw",|        "program": "build/fw.elf",|        "board": { "chip": "STM32F411CEUx", "serial": { "baud": 115200, "port": "/dev/ttyUSB2" }, "flash": "probe-rs" },|        "debug": { "adapter": "gdb" }|    }|  ]|}|'
run "$R" "command:Choose the probe;probewait;text:000801234568;key:Return;probewait;runconfdump"
check "no configuration chosen in the Build tab: the first, the Run button's" 'app | debug: app in rc | profile: app in rc'
got=$(sed -n 2p "$R/.blit/run.json")
check "(its board made inline, as the configuration is written)" \
    '    { "name": "app",   "program": "build/app", "board": {"probe": {"kind": "cmsis-dap", "serial": "000801234568", "usb": "1366:1008"}} },'
run "$R" "buildrun:fw;buildwait;command:Choose the probe;probewait;text:RP2040;key:Return;probewait;boarddump"
check "a probe with no port: the port goes, its baud stays" \
    'fw: probe rp-boot  2e8a:0003 gdb= | port  baud 115200 | chip STM32F411CEUx elf  svd  flash probe-rs debug '

# A Pico: its SDK's USB serial port is the board itself, written with its
# serial number (picotool's --ser), and flashed with picotool; so is its
# boot ROM (BOOTSEL), which has no serial number of its own. An STM32's
# DFU boot ROM: dfu-util.
Q="$dir/pico"
mkdir -p "$Q"
run "$Q" "command:Choose the probe;probewait;text:Pico SDK;key:Return;probewait;boarddump;flashdump"
check "the Pico SDK's USB serial chosen: its probe and port, flashed with picotool" \
    'pico: tool picotool | board  | cable  | interface  | target  | device  | address  | bitstream '
got=$(sed -n 7,8p "$Q/.blit/run.json" | tr '\n' '|')
check "(the file: its serial number, and its port)" '        "probe": {"kind": "pico-serial", "serial": "A58565170D696C53", "usb": "2e8a:000a"},|        "serial": {"port": "/dev/ttyACM7"}|'
run "$Q" "command:Choose the probe;probewait;text:RP2040 boot;key:Return;probewait;flashdump"
check "its boot ROM chosen: picotool" 'pico: tool picotool | board  | cable  | interface  | target  | device  | address  | bitstream '
run "$Q" "command:Choose the probe;probewait;text:STM32 DFU;key:Return;probewait;boarddump;flashdump"
check "an STM32's DFU boot ROM chosen: dfu-util" 'pico: tool dfu-util | board  | cable  | interface  | target  | device  | address  | bitstream '

# The board read: every member, in either form; a flash tool it does not know said.
B="$dir/board"
mkdir -p "$B/.blit"
printf '{"configurations": [{"name": "esp", "program": "build/app.elf", "board": {"chip": "esp32s3", "probe": "F4:12:FA:00:00:01", "serial": "/dev/cu.usbmodem1101", "elf": "build/app.elf", "svd": "esp32s3.svd", "flash": {"tool": "esptool"}, "debug": {"preset": "esp"}}}]}\n' >"$B/.blit/run.json"
run "$B" "boarddump"
check "a board in the short forms: probe and serial as strings, flash and debug as objects" \
    'esp: probe  F4:12:FA:00:00:01  gdb= | port /dev/cu.usbmodem1101 baud 0 | chip esp32s3 elf build/app.elf svd esp32s3.svd flash esptool debug esp'
printf '{"configurations": [{"name": "esp", "program": "build/app.elf", "board": {"flash": "avrdude"}}]}\n' >"$B/.blit/run.json"
got=$(BLIT_USB_ROOT="$U" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$B;frame;builddump" 2>&1 >/dev/null | grep '^blit: ' | tail -1)
check "a flash tool it does not know: said" 'blit: .blit/run.json: esp'"'"'s board.flash is one of "esptool", "openocd", "probe-rs", "jlink", "picotool", "uf2", "dfu-util", "pyocd", "stm32cubeprogrammer", "openfpgaloader", "vivado", "iceprog", "ecpprog", "pgrcmd"'
printf '{"configurations": [{"name": "esp", "program": "build/app.elf", "board": "ttyACM0"}]}\n' >"$B/.blit/run.json"
got=$(BLIT_USB_ROOT="$U" "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$B;frame;builddump" 2>&1 >/dev/null | grep '^blit: ' | tail -1)
check "a board that is not an object: said" 'blit: .blit/run.json: esp'"'"'s board is an object'

# The Debug tab shows the board; Enter there chooses again.
run "$R" "buildrun:fw;buildwait;boarddump;debugpanel;debugrows:Board:2"
check "the Debug tab: the board's probe and port" 'Board | RP2040 boot ROM'
run "$R" "buildrun:fw;buildwait;debugpanel;debugpick:board;probewait;text:Espressif;key:Return;probewait;boarddump;debugrows:Board:2"
check "Enter on it: the picker, and the one chosen shown" 'Board | Espressif USB-Serial/JTAG F4:12:FA:00:00:01 /dev/ttyACM3'
run "$P" "boarddump;debugpanel;debugrows:Board:2"
check "(the board chosen before: shown when the folder is opened again)" 'Board | ST-LINK V3E 004D00283133510F37363734 /dev/ttyACM0'
rm -rf "$P/.blit"
run "$P" "boarddump;debugpanel;debugrows:Board:2"
check "(no run configuration: no board)" 'no Board'

# On another machine: the devices are its, listed by its agent (which sees
# BLIT_USB_ROOT as blit does), each row saying where; the board written
# into the run.json there.
mkdir -p "$T/fw"
run "ssh://box$T/fw" "remotewait;command:Choose the probe;probewait;finddump"
check "another machine's devices, from its agent, each row saying where" \
    'pick [] 14 : >Black Magic Probe  7BB180B4  /dev/ttyACM5  gdb /dev/ttyACM4  on box DAPLink CMSIS-DAP  0240000034544e45  /dev/ttyACM6  on box Digilent FT2232 (FPGA)  210183A8F2C1  /dev/ttyUSB1  on box Espressif USB-Serial/JTAG  F4:12:FA:00:00:01  /dev/ttyACM3  on box J-Link  000801234567  /dev/ttyACM1  on box J-Link  000801234568  on box'
run "ssh://box$T/fw" "remotewait;command:Choose the probe;probewait;text:Digilent;key:Return;probewait;boarddump"
check "chosen: written there, and read back" 'fw: probe ftdi 210183A8F2C1 0403:6010 gdb= | port /dev/ttyUSB1 baud 0 | chip  elf  svd  flash  debug '
got=$(tr '\n' '|' <"$T/fw/.blit/run.json")
check "(the file there)" '{|  "configurations": [|    {|      "name": "fw",|      "program": "build/fw",|      "board": {|        "probe": {"kind": "ftdi", "serial": "210183A8F2C1", "usb": "0403:6010"},|        "serial": {"port": "/dev/ttyUSB1"}|      }|    }|  ]|}|'

# This machine's own devices (whatever is plugged in: hubs, a keyboard, no
# probe as a rule): each row of the right shape, and the agent's listing too.
got=$(env -u BLIT_USB_ROOT "$blit" --offscreen 1000x700 --shot "$dir/shot.bmp" --script "workdir:$dir;frame;command:Choose the probe;probedump" 2>&1 >/dev/null | tail -1 |
    awk 'BEGIN { RS = " [|] "; ok = 1 } { sub(/\n$/, ""); if ($0 != "none" && $0 !~ /^(probe|fpga|boot|serial|other) [a-z0-9-]+ [0-9a-f][0-9a-f][0-9a-f][0-9a-f]:[0-9a-f][0-9a-f][0-9a-f][0-9a-f]: [^ ].*$/) { ok = 0; print "bad row: " $0 } } END { if (ok) print "ok" }')
check "this machine's devices: rows of the right shape" 'ok'
if [ -x "$agent" ]; then
    got=$(env -u BLIT_USB_ROOT "$agent" --usb | awk -F '\t' 'NR == 1 { ok = $0 == "blit-usb 1" } NR > 1 && !(($1 == "d" && NF == 8 && $2 ~ /^[0-9a-f]{4}$/ && $3 ~ /^[0-9a-f]{4}$/) || ($1 == "i" && NF == 2) || ($1 == "p" && NF == 4 && $3 ~ /^\/dev\//)) { ok = 0; print "bad line: " $0 } END { if (ok) print "ok" }')
    check "this machine's agent's listing: of the right shape" 'ok'
fi

[ "$failed" = 0 ] && echo "probes: all passed"
exit "$failed"
