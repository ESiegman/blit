#!/bin/sh
# Boards on the debug door (boarddebug.cpp, appdebug.cpp's board part): F5
# on a run configuration with a board starts its preset's gdb server and
# cross gdb, resets the board and runs to main, flashed first through
# "Flash the board" (only if changed); "Attach to the running board"
# connects with no reset and no flash; the server ends with the session;
# what is missing is said. Against QEMU's mps2-an385 (a Cortex-M3) running
# tests/boards/cm.c, built by Arm's GNU toolchain; the probes' presets'
# command lines from what each would start (no probe here). Skipped,
# saying why, where QEMU, the toolchain or a gdb that speaks DAP (14 or
# later, built with Python) is not installed.
#
#   tests/boards.sh path/to/blit            run the cases
#   tests/boards.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/boards.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd -P)
cc=$(command -v arm-none-eabi-gcc || ls -d "$HOME"/.local/opt/arm-gnu-toolchain-*/bin/arm-none-eabi-gcc 2>/dev/null | tail -1)
[ -n "$cc" ] || { echo "boards skipped: no arm-none-eabi-gcc (Arm's GNU toolchain)"; exit 0; }
command -v qemu-system-arm >/dev/null 2>&1 || { echo "boards skipped: no qemu-system-arm (QEMU)"; exit 0; }
gdb=
for g in arm-none-eabi-gdb gdb-multiarch; do
    command -v $g >/dev/null 2>&1 && [ "$($g -q -batch -ex 'python print(41 + 1)' 2>/dev/null)" = 42 ] && { gdb=$(command -v $g); break; }
done
[ -n "$gdb" ] || { echo "boards skipped: no arm-none-eabi-gdb or gdb-multiarch built with Python (gdb -i dap needs it)"; exit 0; }
dir=$(mktemp -d)
trap 'pkill -f "$dir/" 2>/dev/null; rm -rf "$dir"' EXIT
dir=$(cd "$dir" && pwd -P)
P="$dir/fw"
mkdir -p "$P/.blit" "$dir/bin"
cp "$here/boards/cm.c" "$here/boards/cm.ld" "$P/"
(cd "$P" && "$cc" -mcpu=cortex-m3 -mthumb -g -O0 -nostdlib -T cm.ld -o cm.elf cm.c) || exit 1
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua" BLIT_RECENT_FILE="$dir/recent" HOME="$dir/home" REPLAY_HOLD="$dir/hold"
mkdir -p "$HOME" "$REPLAY_HOLD"
LOG="$dir/log"
export LOG
case $(uname -s) in Darwin) this="this Mac" ;; *) this="this machine" ;; esac

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# board JSON: the run configuration "fw", its program cm.elf, that board.
board() { printf '{ "configurations": [ { "name": "fw", "program": "cm.elf", "board": %s } ] }\n' "$1" >"$P/.blit/run.json"; }
# run SCRIPT: the lines it printed, joined by " ;; ".
run() {
    got=$(cd "$P" && "$blit" --offscreen 1600x900 --shot "$dir/shot.bmp" --script "workdir:$P;$1" "$P/cm.c" 2>&1 >/dev/null | sed -e "s#$dir#@#g" |
        awk 'NR > 1 { printf " ;; " } { printf "%s", $0 }')
}
# What of QEMU runs still, from this test.
left() { pgrep -f "qemu-system-arm.*$dir/" >/dev/null 2>&1 && echo "QEMU left running" || echo "nothing left"; }
G="\"gdb\": \"$gdb\""

# QEMU: started by gdb through a pipe, halted at its first instruction,
# the ELF loaded; F5 runs to main; the program's breakpoints; a step; the
# registers; Shift+F5 ends QEMU with the session.
board "{ \"chip\": \"mps2-an385\", \"debug\": { \"preset\": \"qemu\", $G } }"
run "key:F5;debugwait:stopped;debugdump"
check "QEMU: F5 runs to main" 'stopped (breakpoint); stack: main@17; vars:'
run "break:12;key:F5;debugwait:stopped;key:F5;debugwait:stopped;debugdump;key:F10;debugwait:stopped;debugdump;debugpanel;debugpick:vars:5;debugwait:answers;debugrows:Variables:9;key:shift+F5;debugwait:idle;debugdump"
got="$got ;; $(left)"
check "QEMU: a breakpoint, a step, the registers; Shift+F5 ends it" \
    'stopped (breakpoint); stack: square@12 main@19; vars: x=1 y=0 ;; stopped (step); stack: square@13 main@19; vars: x=1 y=1 ;; Variables | ▾ Arguments | x = 1  (u32) | ▾ Locals | y = 1  (u32) | ▾ Registers | r0 = 1  (long) | r1 = 0  (long) | r2 = 0  (long) ;; idle ;; nothing left'
# (Restart: QEMU ended, and another started, to main again.)
run "key:F5;debugwait:stopped;key:F10;debugwait:stopped;key:cmd+shift+F5;debugwait:stopped;debugdump;key:shift+F5;debugwait:idle"
got="$got ;; $(left)"
check "QEMU: restart, to main again" 'stopped (breakpoint); stack: main@17; vars: ;; nothing left'

# Attaching to a QEMU already running (with its gdb port): stopped where it
# is, nothing reset; Shift+F5 lets it go on.
port=$((20000 + $$ % 20000))
qemu-system-arm -M mps2-an385 -nographic -monitor none -serial none -kernel "$P/cm.elf" -gdb tcp:127.0.0.1:$port 2>/dev/null &
qpid=$!
sleep 1
board "{ \"chip\": \"mps2-an385\", \"debug\": { \"preset\": \"qemu\", \"port\": $port, $G } }"
run "command:Attach to the running board;debugwait:stopped;debugdump;key:shift+F5;debugwait:idle;debugdump"
got=$(printf '%s' "$got" | sed -E 's/stack: (main@(18|19|20)|square@1[23] main@19); vars: .* ;; idle/stack: where it was ;; idle/')
kill -0 $qpid 2>/dev/null && got="$got ;; still running"
kill $qpid 2>/dev/null
check "attaching to the running board: stopped where it was; let go, it goes on" 'stopped (attach); stack: where it was ;; idle ;; still running'

# Flashed first: an ST-LINK's board, flashed by probe-rs (a stand-in that
# replays probe-rs 0.32's bars), then debugged through probe-rs's gdb
# server (the stand-in runs QEMU as one, on the port blit gives it): its
# build first, the flash's progress, reset and run to main; the server
# ended with the session. Again with the image unchanged: not flashed,
# debugged all the same. (QEMU answers the reset that probe-rs's server
# would take, `monitor reset halt`, with its own words.)
cat >"$dir/bin/probe-rs" <<EOF
#!/bin/sh
echo "probe-rs \$*" >>"\$LOG"
case \$1 in
    download) exec sh "$here/replay.sh" "\${SAMPLE:-$here/progress/probe-rs.txt}" ;;
    gdb) while [ \$# -gt 1 ]; do [ "\$1" = --gdb-connection-string ] && a=\$2; shift; done
         exec qemu-system-arm -M mps2-an385 -nographic -monitor none -serial none -kernel "$P/cm.elf" -S -gdb tcp:\$a ;;
esac
EOF
chmod +x "$dir/bin/probe-rs"
mkdir -p "$P/.blit"
printf '{ "tasks": [ { "label": "Build", "command": "echo built >>\\"$LOG\\"" } ] }\n' >"$P/.blit/tasks.json"
STLINK='"probe": {"kind": "stlink", "serial": "004D00283133510F37363734", "usb": "0483:374e"}, "chip": "STM32F411CEUx"'
printf '{ "configurations": [ { "name": "fw", "program": "cm.elf", "build": "Build", "board": { %s, "debug": { "port": %s, %s } } } ] }\n' "$STLINK" "$((port + 1))" "$G" >"$P/.blit/run.json"
: >"$LOG"
was=$PATH; PATH="$dir/bin:$PATH"; run "key:F5;debugwait:stopped;debugdump;progressdump;key:shift+F5;debugwait:idle;key:F5;debugwait:stopped;debugdump;progressdump;key:shift+F5;debugwait:idle"; PATH=$was
got="$got ;; $(left) ;; $(sed "s#$dir#@#g" "$LOG" | awk 'NR > 1 { printf " ;; " } { printf "%s", $0 }')"
check "F5 on a board: built, flashed, reset, run to main; unchanged, not flashed again" \
    "stopped (breakpoint); stack: main@17; vars:; output: unknown command: 'reset' | Register cache flushed. ;; button: ▶ fw · Flashed 40.2 KB, verified | row: done · Flashed 40.2 KB in 1.8 s, verified ;; stopped (breakpoint); stack: main@17; vars:; output: unknown command: 'reset' | Register cache flushed. ;; button: ▶ fw | row: Not flashed: unchanged since the last flash ;; nothing left ;; built ;; probe-rs download --chip STM32F411CEUx --probe 0483:374e:004D00283133510F37363734 --non-interactive --verify --reset cm.elf ;; probe-rs gdb --chip STM32F411CEUx --probe 0483:374e:004D00283133510F37363734 --non-interactive --gdb-connection-string 127.0.0.1:$((port + 1)) ;; built ;; probe-rs gdb --chip STM32F411CEUx --probe 0483:374e:004D00283133510F37363734 --non-interactive --gdb-connection-string 127.0.0.1:$((port + 1))"
# A flash that fails: no session.
: >"$LOG"
was=$PATH; PATH="$dir/bin:$PATH"; SAMPLE="$here/progress/probe-rs-fail.txt"; export SAMPLE; run "command:Flash anyway;flashwait;buildwait;key:F5;debugwait:idle;debugdump"; PATH=$was; unset SAMPLE
got="$got ;; $(grep -c '^probe-rs gdb' "$LOG") servers"
check "a flash that fails: nothing debugged" 'idle ;; 0 servers'
rm "$P/.blit/tasks.json"

# What each preset starts (no probe here: their command lines), F5's and
# attaching's.
plan() {
    board "$1"
    run "boardplan;boardplan:attach"
}
plan "{ $STLINK }"
check "probe-rs (an ST-LINK's): its gdb server" \
    'probe-rs | gdb: arm-none-eabi-gdb gdb-multiarch | server: {probe-rs} gdb --chip STM32F411CEUx --probe 0483:374e:004D00283133510F37363734 --non-interactive --gdb-connection-string 127.0.0.1:1337 | target: 127.0.0.1:1337 | after: monitor reset halt; maintenance flush register-cache | to main | flashed first | {"program":"@/fw/cm.elf","target":"127.0.0.1:1337"} ;; probe-rs | gdb: arm-none-eabi-gdb gdb-multiarch | server: {probe-rs} gdb --chip STM32F411CEUx --probe 0483:374e:004D00283133510F37363734 --non-interactive --gdb-connection-string 127.0.0.1:1337 | target: 127.0.0.1:1337 | after:  | {"program":"@/fw/cm.elf","target":"127.0.0.1:1337"}'
plan "{ $STLINK, \"debug\": \"openocd\" }"
check "openocd: started by gdb through a pipe" \
    "openocd | gdb: arm-none-eabi-gdb gdb-multiarch | target: | exec {openocd} -c 'gdb_port pipe' -c 'tcl_port disabled' -c 'telnet_port disabled' -f interface/stlink.cfg -c 'adapter serial 004D00283133510F37363734' -f target/stm32f4x.cfg -c '[target current] configure -event gdb-detach { resume }' | after: monitor reset halt; maintenance flush register-cache | to main | flashed first | {\"program\":\"@/fw/cm.elf\",\"target\":\"| exec \\\"\$BLIT_SERVER\\\" -c 'gdb_port pipe' -c 'tcl_port disabled' -c 'telnet_port disabled' -f interface/stlink.cfg -c 'adapter serial 004D00283133510F37363734' -f target/stm32f4x.cfg -c '[target current] configure -event gdb-detach { resume }'\"} ;; openocd | gdb: arm-none-eabi-gdb gdb-multiarch | target: | exec {openocd} -c 'gdb_port pipe' -c 'tcl_port disabled' -c 'telnet_port disabled' -f interface/stlink.cfg -c 'adapter serial 004D00283133510F37363734' -f target/stm32f4x.cfg -c '[target current] configure -event gdb-detach { resume }' | after:  | {\"program\":\"@/fw/cm.elf\",\"target\":\"| exec \\\"\$BLIT_SERVER\\\" -c 'gdb_port pipe' -c 'tcl_port disabled' -c 'telnet_port disabled' -f interface/stlink.cfg -c 'adapter serial 004D00283133510F37363734' -f target/stm32f4x.cfg -c '[target current] configure -event gdb-detach { resume }'\"}"
plan '{ "probe": {"kind": "jlink", "serial": "000801234567", "usb": "1366:0105"}, "chip": "STM32H743ZI" }'
got=${got%% ;; *}
check "jlink: SEGGER's GDB server" \
    'jlink | gdb: arm-none-eabi-gdb gdb-multiarch | server: {JLinkGDBServerCLExe|JLinkGDBServer} -device STM32H743ZI -if SWD -speed 4000 -port 2331 -swoport 2332 -telnetport 2333 -select '"'"'USB=000801234567'"'"' -singlerun -nogui | target: 127.0.0.1:2331 | after: monitor reset; maintenance flush register-cache | to main | flashed first | {"program":"@/fw/cm.elf","target":"127.0.0.1:2331"}'
plan '{ "probe": {"kind": "esp-usb-jtag", "serial": "AC:A7:04:2C:81:B4", "usb": "303a:1001"}, "chip": "ESP32-S3" }'
check "esp: Espressif's OpenOCD through a pipe, its gdb, to app_main; attaching, no reset (no flash size: its gdb-attach resets otherwise)" \
    "esp | gdb: xtensa-esp32s3-elf-gdb | target: | exec {openocd-esp32|openocd} -c 'gdb_port pipe' -c 'tcl_port disabled' -c 'telnet_port disabled' -f interface/esp_usb_jtag.cfg -c 'adapter serial AC:A7:04:2C:81:B4' -f target/esp32s3.cfg -c '[target current] configure -event gdb-detach { resume }' | after: set remote hardware-watchpoint-limit 2; monitor reset halt; maintenance flush register-cache | to app_main | flashed first | {\"program\":\"@/fw/cm.elf\",\"target\":\"| exec \\\"\$BLIT_SERVER\\\" -c 'gdb_port pipe' -c 'tcl_port disabled' -c 'telnet_port disabled' -f interface/esp_usb_jtag.cfg -c 'adapter serial AC:A7:04:2C:81:B4' -f target/esp32s3.cfg -c '[target current] configure -event gdb-detach { resume }'\"} ;; esp | gdb: xtensa-esp32s3-elf-gdb | target: | exec {openocd-esp32|openocd} -c 'gdb_port pipe' -c 'tcl_port disabled' -c 'telnet_port disabled' -c 'set ESP_FLASH_SIZE 0' -f interface/esp_usb_jtag.cfg -c 'adapter serial AC:A7:04:2C:81:B4' -f target/esp32s3.cfg -c '[target current] configure -event gdb-detach { resume }' | after: set remote hardware-watchpoint-limit 2 | {\"program\":\"@/fw/cm.elf\",\"target\":\"| exec \\\"\$BLIT_SERVER\\\" -c 'gdb_port pipe' -c 'tcl_port disabled' -c 'telnet_port disabled' -c 'set ESP_FLASH_SIZE 0' -f interface/esp_usb_jtag.cfg -c 'adapter serial AC:A7:04:2C:81:B4' -f target/esp32s3.cfg -c '[target current] configure -event gdb-detach { resume }'\"}"
plan '{ "probe": {"kind": "blackmagic", "serial": "7BB180B4", "usb": "1d50:6018", "gdb": "/dev/cu.usbmodem7BB180B41"}, "chip": "STM32F411CEUx" }'
got=${got%% ;; *}
check "blackmagic: its GDB port as extended-remote on gdb's command line, attach 1, gdb's load" \
    "blackmagic | gdb: arm-none-eabi-gdb gdb-multiarch | gdb: -iex 'target extended-remote /dev/cu.usbmodem7BB180B41' -iex 'monitor swd_scan' | after: load | to main | flashed first | {\"program\":\"@/fw/cm.elf\",\"pid\":1}"
plan "{ $STLINK, \"debug\": \"pyocd\" }"
got=${got%% ;; *}
check "pyocd: its gdb server" \
    'pyocd | gdb: arm-none-eabi-gdb gdb-multiarch | server: {pyocd} gdbserver -t stm32f411ceux -u 004D00283133510F37363734 -p 3333 -T 3334 | target: 127.0.0.1:3333 | after: monitor reset halt; maintenance flush register-cache | to main | flashed first | {"program":"@/fw/cm.elf","target":"127.0.0.1:3333"}'
plan '{ "chip": "stm32f4", "debug": { "preset": "renode", "machine": "platforms/cpus/stm32f4.repl", "port": 3340 } }'
check "renode: its GDB server, the platform and the ELF loaded; attaching, to its port" \
    "renode | gdb: arm-none-eabi-gdb gdb-multiarch | server: {renode} --disable-gui -e 'mach create' -e 'machine LoadPlatformDescription @platforms/cpus/stm32f4.repl' -e 'sysbus LoadELF @@/fw/cm.elf' -e 'machine StartGdbServer 3340' | target: 127.0.0.1:3340 | after: monitor start | to main | ended with it | {\"program\":\"@/fw/cm.elf\",\"target\":\"127.0.0.1:3340\"} ;; renode | gdb: arm-none-eabi-gdb gdb-multiarch | target: 127.0.0.1:3340 | after:  | {\"program\":\"@/fw/cm.elf\",\"target\":\"127.0.0.1:3340\"}"
plan '{ "chip": "STM32F411CEUx", "debug": "probe-rs" }'
got=${got%% ;; *}
check "a board with no flash tool: gdb loads it" \
    'probe-rs | gdb: arm-none-eabi-gdb gdb-multiarch | server: {probe-rs} gdb --chip STM32F411CEUx --non-interactive --gdb-connection-string 127.0.0.1:1337 | target: 127.0.0.1:1337 | after: monitor reset halt; maintenance flush register-cache; load | to main | flashed first | {"program":"@/fw/cm.elf","target":"127.0.0.1:1337"}'
plan '{ "debug": "probe-rs" }'
check "what a preset needs and the board lacks: said" 'probe-rs: probe-rs needs the board'"'"'s chip ;; probe-rs: probe-rs needs the board'"'"'s chip'
board '{ "chip": "x", "debug": "gdbserver" }'
run "runconfdump"
got=$(printf '%s' "$got" | grep -o 'board.debug is one of [^;]*' | head -1 | sed 's/ *$//')
[ -z "$got" ] && run "frame;boardplan" && got=$(printf '%s' "$got" | sed 's/^.*\(board.debug is one of\)/\1/')
check "a preset blit does not have: said" 'board.debug is one of "probe-rs", "openocd", "jlink", "esp", "blackmagic", "pyocd", "qemu", "renode"'

# What is missing, said, with the machine: the gdb (the first looked for,
# and the others), QEMU, a gdb too old, a gdb built without Python.
mkdir -p "$dir/nogdb"
ln -s "$(command -v qemu-system-arm)" "$dir/nogdb/"
board '{ "chip": "mps2-an385", "debug": "qemu" }'
was=$PATH; PATH="$dir/nogdb:/usr/bin:/bin"; run "key:F5;debugwait:idle;debugdump"; PATH=$was
check "no gdb for the ELF's machine: said" "idle error: arm-none-eabi-gdb is not installed on $this (nor gdb-multiarch)"
mkdir -p "$dir/noqemu"
ln -s "$gdb" "$dir/noqemu/arm-none-eabi-gdb"
was=$PATH; PATH="$dir/noqemu:/usr/bin:/bin"; run "key:F5;debugwait:idle;debugdump"; PATH=$was
check "no QEMU: said" "idle error: qemu-system-arm is not installed on $this"
printf '#!/bin/sh\ncase "$1" in --version) echo "GNU gdb (Arm GNU Toolchain 12.3.Rel1 (Build arm-12.35)) 12.3.90.20230627-git" ;; *) exit 1 ;; esac\n' >"$dir/bin/oldgdb"
printf '#!/bin/sh\ncase "$1" in --version) echo "GNU gdb (Arm GNU Toolchain 15.2.Rel1 (Build arm-15.86)) 16.3.90.20250906-git" ;; *) echo "Interpreter \\`dap'"'"' unrecognized" >&2; exit 1 ;; esac\n' >"$dir/bin/nopython"
chmod +x "$dir/bin/oldgdb" "$dir/bin/nopython"
board "{ \"chip\": \"mps2-an385\", \"debug\": { \"preset\": \"qemu\", \"gdb\": \"$dir/bin/oldgdb\" } }"
run "key:F5;debugwait:idle;debugdump"
check "a gdb older than 14: said" "idle error: @/bin/oldgdb 12 is too old to be a debugger blit can use: 14 or newer speaks its protocol"
board "{ \"chip\": \"mps2-an385\", \"debug\": { \"preset\": \"qemu\", \"gdb\": \"$dir/bin/nopython\" } }"
run "key:F5;debugwait:idle;debugdump"
check "a gdb built without Python: said" "idle error: @/bin/nopython on $this was built without Python, which gdb -i dap needs"
board '{ "chip": "mps2-an385", "debug": { "preset": "qemu", "gdb": "no-such-gdb" } }'
run "key:F5;debugwait:idle;debugdump"
check "the board's own gdb, not installed: said" "idle error: no-such-gdb is not installed on $this"

[ "$failed" = 0 ] && [ -z "$print" ] && echo "boards: all passed"
exit "$failed"
