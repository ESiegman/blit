#!/bin/sh
# Serial monitor tabs (appserial.cpp, serialtab.cpp, serial.cpp): a
# terminal tab whose program is the device on a port. The device is
# tests/fake-serial.py, a pseudo-terminal linked where a USB serial port's
# node would be, in a folder of its own (BLIT_USB_DEV), its USB device a
# Pico in a stand-in sysfs (BLIT_USB_ROOT, as tests/probes.sh has it): what
# it sends shown (LF alone a new line), typing and Return sent to it (CR,
# LF or CR LF), Cmd+F passing nothing through, host times per line, hex of
# the same bytes, the port gone and found again by its serial number at
# another path with a line marking the gap, let go of around a flash, the
# picker, the board's monitor in one step, speed and framing, a log, a file
# sent, DTR and RTS (a pseudo-terminal has neither: said), a port in use.
#
#   tests/serial.sh path/to/blit            run the cases
#   tests/serial.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/serial.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd -P)
dir=$(mktemp -d)
dir=$(cd "$dir" && pwd -P)
fake=
stop_fake() {
    [ -n "$fake" ] && kill "$fake" 2>/dev/null && wait "$fake" 2>/dev/null
    fake=
}
trap 'stop_fake; rm -rf "$dir"' EXIT
command -v python3 >/dev/null || { echo "serial.sh: python3 is not installed (the fake device is a Python script)"; exit 1; }
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua" BLIT_RECENT_FILE="$dir/recent" HOME="$dir/home"
mkdir -p "$HOME"
DEV="$dir/dev" USB="$dir/usb" CTL="$dir/ctl" HEARD="$dir/heard" P="$dir/proj"
export BLIT_USB_ROOT="$USB" BLIT_USB_DEV="$DEV"
IFACE="$USB/1-3:1.0"
# The editor's keys over a terminal: Cmd's on a Mac; elsewhere Ctrl alone is the
# program's (the board's), clearing is Ctrl+Shift+K, and Find is a command.
case $(uname -s) in
    Darwin) find=key:cmd+f clear=cmd+k ;;
    *) find="command:Find in this file" clear=ctrl+shift+k ;;
esac

# The device: a Pico running the SDK's USB serial (2e8a:000a), serial
# number FAKE0001, its port ttyACM9; started afresh for each case.
start_fake() {
    stop_fake
    rm -rf "$DEV" "$USB" "$CTL" "$HEARD" "$dir"/try*
    mkdir -p "$DEV" "$USB/1-3" "$IFACE" "$P"
    mkfifo "$CTL"
    printf '2e8a\n' >"$USB/1-3/idVendor"
    printf '000a\n' >"$USB/1-3/idProduct"
    printf '00\n' >"$USB/1-3/bDeviceClass"
    printf 'Raspberry Pi\n' >"$USB/1-3/manufacturer"
    printf 'Pico\n' >"$USB/1-3/product"
    printf 'FAKE0001\n' >"$USB/1-3/serial"
    printf '00\n' >"$IFACE/bInterfaceNumber"
    printf 'Board CDC\n' >"$IFACE/interface"
    python3 "$here/fake-serial.py" "$DEV" ttyACM9 "$HEARD" "$CTL" "$IFACE" &
    fake=$!
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do [ -e "$DEV/ttyACM9" ] && break; sleep 0.1; done
}

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run SCRIPT: blit with the device started afresh; the last line it printed,
# the folder written @, times as HH:MM:SS.mmm and spans as N s. In the
# script, "fake:WORDS" tells the device WORDS (a line to its fifo). (size,
# init and path, set before it: the window, init.lua and PATH of one run.)
port="$DEV/ttyACM9"
run() {
    start_fake
    script=$(printf '%s' "$1" | sed -e "s#fake:\([^;]*\)#lua:local f=io.open('$CTL','w') f:write('\1\\\\n') f:close()#g")
    got=$(BLIT_INIT="${init:-$dir/init.lua}" PATH="${path:-$PATH}" "$blit" --offscreen "${size:-1200x1400}" --shot "$dir/shot.bmp" --script "workdir:$P;frame;$script" 2>&1 >/dev/null |
        sed -E -e "s#$dir#@#g" -e 's/[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}/HH:MM:SS.mmm/g' -e 's/[0-9]+\.[0-9] s/N s/g' | tail -1)
    size= init= path=
}
# What the device has been sent, a byte a word (od -c's way: \r, 001).
heard() { got=$(od -An -c "$HEARD" 2>/dev/null | tr -s ' \n' ' ' | sed -e 's/^ //' -e 's/ $//'); }
open="serial:$port;frame;serialstate:open"

# ---------------------------------------------------------------- what comes, and what goes

run "$open;fake:ticks 3;termwait:tick 3;termdump"
check "the device's lines on the tab" "tick 1|tick 2|tick 3 @3,0 11x74"
run "$open;tabsdump"
check "the tab names the port and its speed" "ttyACM9 115200"
run "$open;fake:say boot\\\\nok\\\\r\\\\nend\\\\n;termwait:end;termdump"
check "LF alone is a new line, as CR LF is" "boot|ok|end @3,0 11x74"
run "$open;fake:say working 50%\\\\rworking 100%\\\\r\\\\n;termwait:100%;termdump"
check "CR alone goes back along the line (a count written over itself)" "working 100% @1,0 11x74"
run "$open;text:ab;key:Return;wait:200"
heard
check "typing goes to the board, Return as CR" 'a b \r'
run "$open;command:Change what Return sends to the port…;finddump;text:CR LF;key:Return;text:c;key:Return;command:Change what Return sends to the port…;text:LF;key:Return;key:Return;wait:200"
heard
check "Return as CR LF, then LF, chosen for the tab" 'c \r \n \n'
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.serial.line_end = "lf"\nblit.opt.serial.baud = 9600\n' >"$dir/init-lf.lua"
init="$dir/init-lf.lua"
run "$open;key:Return;wait:200;tabsdump"
title=$got
heard
got="$got $title"
check "serial.line_end and serial.baud in init.lua: what a new tab starts with" '\n ttyACM9 9600'
run "$open;$find;text:secret;key:Escape;text:x;wait:200"
heard
check "Cmd+F over the tab: what is typed in the box never reaches the board" 'x'
printf 'firmware\r\n\001\002' >"$P/image.bin"
run "$open;command:Send a file to the serial port…;text:image.bin;key:Return;wait:300"
heard
check "Send a file: its bytes, raw" 'f i r m w a r e \r \n 001 002'
run "$open;fake:say \\\\e[6n;wait:300"
heard
check "the Term's answers go back to the device (where the cursor is: ESP-IDF's console asks)" '033 [ 1 ; 1 R'

# ---------------------------------------------------------------- times and hex

run "$open;fake:ticks 2;termwait:tick 2;command:Show or hide the serial port's times;frame;termdump"
check "host times before each line, taken as the lines came" "HH:MM:SS.mmm tick 1|HH:MM:SS.mmm tick 2 @2,0 11x74"
run "$open;fake:ticks 2;termwait:tick 2;command:Show or hide the serial port's times;command:Show or hide the serial port's times;frame;termdump"
check "and hidden again" "tick 1|tick 2 @2,0 11x74"
run "$open;fake:ticks 2;termwait:tick 2;command:Show the serial port's bytes as hex, or as text;frame;termdump"
check "hex of the same bytes (8 a row in a tab too narrow for 16), each line's LF ending its row" \
    "00000000  74 69 63 6b 20 31 0d 0a  tick 1..|00000008  74 69 63 6b 20 32 0d 0a  tick 2.. @2,0 11x74"
size=1400x1400
run "$open;fake:say 0123456789abcdefXYZ\\\\001\\\\n;wait:300;command:Show the serial port's bytes as hex, or as text;frame;termdump"
check "16 a row where there is room, what is not printable a dot" "00000000  30 31 32 33 34 35 36 37  38 39 61 62 63 64 65 66  0123456789abcdef|00000010  58 59 5a 01 0a                                    XYZ.. @2,0 11x86"
run "$open;command:Show the serial port's bytes as hex, or as text;fake:ticks 1;wait:300;command:Show or hide the serial port's times;frame;termdump"
check "hex with times, as more comes" "HH:MM:SS.mmm 00000000  74 69 63 6b 20 31 0d 0a  tick 1.. @1,0 11x74"
run "$open;fake:ticks 2;termwait:tick 2;key:$clear;fake:say after\\\\n;termwait:after;termdump"
check "Cmd+K: what came goes (the board is sent nothing)" "after @1,0 11x74"

# ---------------------------------------------------------------- the port going and coming back

run "$open;fake:ticks 2;termwait:tick 2;fake:hangup;serialstate:gone;wait:300;termdump"
check "hung up: the tab stays, waiting for the same serial number" "tick 1|tick 2|[gone at HH:MM:SS.mmm (hung up); waiting for FAKE0001] @2,54 11x74"
run "$open;fake:ticks 2;termwait:tick 2;fake:hangup;serialstate:gone;tabsdump"
check "and says so in its name" "ttyACM9 115200 (gone)"
run "$open;fake:ticks 2;termwait:tick 2;fake:hangup;serialstate:gone;fake:back ttyACM10;serialstate:open;fake:ticks 1;wait:300;termdump"
check "back at another path with the same serial number: opened again, one line marking the gap" \
    "tick 1|tick 2|[gone from HH:MM:SS.mmm to HH:MM:SS.mmm, N s (hung up)]|tick 1 @4,0 11x74"
run "$open;fake:ticks 2;termwait:tick 2;fake:hangup;serialstate:gone;fake:back ttyACM10;serialstate:open;tabsdump"
check "its name follows it" "ttyACM10 115200"
run "$open;fake:ticks 1;termwait:tick 1;fake:hangup;serialstate:gone;fake:back ttyACM10;serialstate:open;command:Show or hide the serial port's times;frame;termdump"
check "drawn again from what is kept: the gap's line with it" \
    "HH:MM:SS.mmm tick 1|[gone from HH:MM:SS.mmm to HH:MM:SS.mmm, N s (hung up)] @2,0 11x74"
run "serial:$DEV/ttyACM5;frame;wait:300;termdump"
check "a port that is not there: waited for" "[waiting for ttyACM5] @0,21 11x74"
run "$open;serial:$port;frame;wait:400;termdump"
check "a port another program has: said, and tried again" "[ttyACM9 is in use by another program, trying again] @0,52 11x74"

# ---------------------------------------------------------------- around flashing

run "$open;fake:tryopen $dir/try1;wait:300;serialrelease:FAKE0001;serialstate:released;fake:tryopen $dir/try2;wait:300;serialreclaim;serialstate:open;termdump"
got="$(cat "$dir/try1" "$dir/try2" | tr '\n' ' ')$got"
check "let go of by serial number for a flash tool, and given back" "busy opened [released for flashing from HH:MM:SS.mmm to HH:MM:SS.mmm, N s] @1,0 11x74"
run "$open;serialrelease:$port;serialstate:released;tabsdump"
check "or by its port" "ttyACM9 115200 (released)"
run "$open;serialrelease:OTHER0002;wait:200;serialdump"
check "another board's flash leaves it alone" "ttyACM9 115200: open ttyACM9 115200 8N1 return CR dtr on rts on serial FAKE0001 kept 0 gaps 0"
# A real flash: the board's tool (probe-rs, a stand-in) opens the port as
# esptool would, while the tab is open.
mkdir -p "$P/.blit" "$P/build" "$dir/bin"
# (A 32-bit ARM ELF with one segment to load, as tests/flash.sh makes it.)
perl -e '
    my $eh = pack("a4CCCx9vvVVVVVvvvvvv", "\x7fELF", 1, 1, 1, 2, 40, 1, 0x08000000, 52, 0, 0, 52, 32, 1, 40, 0, 0);
    my $ph = pack("VVVVVVVV", 1, 84, 0x08000000, 0x08000000, 256, 256, 5, 4);
    open(my $f, ">", $ARGV[0]) or die; binmode $f; print $f $eh, $ph, "A" x 256;
' "$P/build/fw.elf"
cat >"$dir/bin/probe-rs" <<EOF
#!/bin/sh
printf 'tryopen $dir/try-flash\n' >"$CTL"
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do [ -s "$dir/try-flash" ] && break; sleep 0.1; done
echo flashed
EOF
chmod +x "$dir/bin/probe-rs"
printf '{"configurations": [{"name": "fw", "program": "build/fw.elf", "board": {"probe": {"kind": "stlink", "serial": "FAKE0001", "usb": "0483:374e"}, "chip": "STM32F411CEUx", "elf": "build/fw.elf", "serial": {"port": "%s", "baud": 230400}}}]}\n' "$port" >"$P/.blit/run.json"
path="$dir/bin:$PATH"
run "command:Open the board's serial monitor;serialstate:open;command:Flash the board;flashwait;buildwait;command:Open the board's serial monitor;serialstate:open;frame;termdump"
got="$(cat "$dir/try-flash" 2>/dev/null | tr '\n' ' ')$got"
check "Flash the board: the tool can open the port, the tab has it back after" "opened [released for flashing from HH:MM:SS.mmm to HH:MM:SS.mmm, N s] @1,0 11x74"
# F5 on the board flashes it the same way, and the tab has its port back
# before the debugger starts (here one that is not installed, so that no
# gdb server is needed: the session ends at once, saying so).
printf '{"configurations": [{"name": "fw", "program": "build/fw.elf", "board": {"probe": {"kind": "stlink", "serial": "FAKE0001", "usb": "0483:374e"}, "chip": "STM32F411CEUx", "elf": "build/fw.elf", "serial": {"port": "%s", "baud": 230400}, "debug": {"gdb": "no-such-gdb"}}}]}\n' "$port" >"$P/.blit/run.json"
# (The Debug tab takes the side panel: the window wide enough for the line.)
case $(uname -s) in Darwin) this="this Mac" ;; *) this="this machine" ;; esac
path="$dir/bin:$PATH" size=2400x1400
run "command:Open the board's serial monitor;serialstate:open;command:Start or continue debugging;debugwait:idle;serialstate:open;command:Open the board's serial monitor;serialstate:open;frame;termdump"
got="$(cat "$dir/try-flash" 2>/dev/null | tr '\n' ' ')${got% @*}"
check "F5 on the board: flashed with the port let go of, the tab has it back as the debugger starts" "opened [released for flashing from HH:MM:SS.mmm to HH:MM:SS.mmm, N s]"
path="$dir/bin:$PATH"
run "command:Open the board's serial monitor;serialstate:open;command:Start or continue debugging;debugwait:idle;debugdump"
check "(and the debugger's own word)" "idle error: no-such-gdb is not installed on $this"

# ---------------------------------------------------------------- opening one

run "command:Open a serial port…;serialwait;finddump"
check "the picker: each USB device's ports, named as the probe picker names them" "pick [] 1 : >ttyACM9  Pico SDK USB serial (RP2040) (Pico)  FAKE0001"
run "command:Open a serial port…;serialwait;key:Return;serialstate:open;fake:ticks 1;termwait:tick 1;serialdump"
check "chosen: its tab, found by its serial number" "ttyACM9 115200: open ttyACM9 115200 8N1 return CR dtr on rts on serial FAKE0001 kept 8 gaps 0 || ttyACM9  Pico SDK USB serial (RP2040) (Pico)  FAKE0001"
run "$open;command:Open a serial port…;serialwait;finddump"
check "a port open in a tab says so" "pick [] 1 : >ttyACM9  Pico SDK USB serial (RP2040) (Pico)  FAKE0001  (open)"
run "command:Open a serial port…;serialwait;text:$port;key:Return;serialstate:open;tabsdump"
check "a path typed" "ttyACM9 115200"
run "command:Open the board's serial monitor;serialstate:open;tabsdump"
check "the board's monitor in one step: its port, at its speed" "ttyACM9 230400"
run "command:Open the board's serial monitor;serialstate:open;command:Open the board's serial monitor;wait:200;serialdump"
check "and again: the same tab" "ttyACM9 230400: open ttyACM9 230400 8N1 return CR dtr on rts on serial FAKE0001 kept 0 gaps 0"
rm -f "$P/.blit/run.json"
run "command:Open the board's serial monitor"
check "no board: said" "blit: No run configuration has a board"

# ---------------------------------------------------------------- the port's settings and lines

run "$open;command:Change the port's speed and framing…;finddump"
check "speeds and framings to choose from, what the tab has now first" "pick [] 30 : >115200 8N1  (now) 300 1200 2400 4800 9600"
run "$open;command:Change the port's speed and framing…;text:74880 8E1;key:Return;wait:200;tabsdump"
check "a speed and framing typed (ESP8266's boot ROM's)" "ttyACM9 74880 8E1"
run "$open;command:Change the port's speed and framing…;text:fast;key:Return"
check "not a speed: said" "blit: Not a speed or a framing: fast"
run "$open;command:Reset the board through DTR/RTS (ESP32);wait:400;serialdump"
check "ESP32's reset: both off, RTS on with DTR off, then RTS off (a pseudo-terminal has no such lines: said)" \
    "ttyACM9 115200: open ttyACM9 115200 8N1 return CR dtr off rts off serial FAKE0001 kept 0 gaps 0 | dtr off rts off (failed) | rts on (failed) | rts off (failed)"
run "$open;command:Reset the board through DTR (Arduino);wait:500;serialdump"
check "Arduino's: both off, then on" \
    "ttyACM9 115200: open ttyACM9 115200 8N1 return CR dtr on rts on serial FAKE0001 kept 0 gaps 0 | dtr off rts off (failed) | dtr on rts on (failed)"
run "$open;command:Turn the port's DTR on or off;wait:200;serialstate:open"
check "a line that cannot be set: said" "blit: Cannot set DTR and RTS on ttyACM9: Inappropriate ioctl for device"

# ---------------------------------------------------------------- a log

run "$open;command:Log the serial port to a file…;key:ctrl+u;text:$dir/board.log;key:Return;fake:ticks 2;termwait:tick 2;fake:hangup;serialstate:gone;command:Stop logging the serial port"
got="$(sed -E -e 's/[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}/DATE TIME/' -e 's/[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}/HH:MM:SS.mmm/g' "$dir/board.log" | tr '\r\n' '<|')$got"
check "a log: each line after its date and time, the gap's line too" "DATE TIME tick 1<|DATE TIME tick 2<|DATE TIME [gone at HH:MM:SS.mmm (hung up)]|blit: Logged to @/board.log"

if [ "$failed" = 0 ] && [ -z "$print" ]; then echo "serial: all passed"; fi
exit "$failed"
