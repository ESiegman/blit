#!/bin/sh
# ESP-IDF's and PlatformIO's projects in the Build tab (buildsdk.cpp,
# appfirmware.cpp): found from their own files (a CMakeLists.txt that
# includes ESP-IDF's project.cmake, as idf.py v6.1.1 tests; platformio.ini's
# envs, default_envs, extends and ${...}), their SDK found in the home
# folder without its export script sourced (ESP-IDF's in ~/esp and its
# Python environment in ~/.espressif, the newest Python's; PlatformIO's pio
# in ~/.platformio/penv/bin), what is not installed said on the rows and
# when run, the commands run with the SDK's environment, a compiler's error
# in idf.py's output a problem, set-target's choice of ESP-IDF's own
# targets, menuconfig's tab given the keyboard, Flash through blit's own
# flash (esptool from ESP-IDF's environment with build/flasher_args.json;
# picotool for an RP2040, the board reset into BOOTSEL by a 1200 baud touch
# first) and Monitor in a serial tab at the project's speed, on the one port
# its chip's board could be on.
#
# The SDKs are stand-ins in a home folder made here: idf.py, idf_tools.py,
# ESP-IDF's Python, pio, esptool and picotool are scripts that say what
# they were given. The board is tests/fake-serial.py on a pseudo-terminal,
# its USB device a Pico in a stand-in sysfs (as tests/serial.sh has it).
#
#   tests/firmware.sh path/to/blit            run the cases
#   tests/firmware.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/firmware.sh path/to/blit [--print]}
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
command -v python3 >/dev/null || { echo "firmware.sh: python3 is not installed (the fake board is a Python script)"; exit 1; }
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\n' >"$dir/init.lua"
export BLIT_INIT="$dir/init.lua" BLIT_RECENT_FILE="$dir/recent" HOME="$dir/home"
export SHELL=/bin/sh  # (the tasks' commands run by the same shell everywhere)
unset IDF_PATH IDF_TOOLS_PATH PLATFORMIO_CORE_DIR
LOG="$dir/log"
export LOG
case $(uname -s) in Darwin) machine="this Mac" ;; *) machine="this machine" ;; esac
# Sealed off from the machine's own boards and tools: no USB device but
# the stand-ins (an empty sysfs until the Pico's), and the flash tools on
# the PATH stand-ins that say they were the PATH's (a real esptool would
# look for a board on every serial port).
mkdir -p "$dir/nousb"
export BLIT_USB_ROOT="$dir/nousb" BLIT_USB_DEV="$dir/nousb"
B="$dir/bin"
mkdir -p "$B"
printf '#!/bin/sh\necho "the PATH\x27s esptool $*" >>"$LOG"\n' >"$B/esptool"
printf '#!/bin/sh\necho "picotool $*" >>"$LOG"\n[ "$1" = load ] && echo "Loading into Flash:   [==============================]  100%%"\nexit 0\n' >"$B/picotool"
chmod +x "$B/esptool" "$B/picotool"
export PATH="$B:$PATH"

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        # (All the run printed, of which got is a part: what a machine
        # where it fails, as GitHub's runner, said.)
        [ -s "$dir/run.out" ] && { echo "    the run printed:"; sed -e 's/^/      | /' "$dir/run.out" | tail -60; }
        failed=1
    fi
}
# run FOLDER SCRIPT: blit's last line (or `lines` of them, joined by " ~ "),
# the folder made here written ~ and a run's time T; then the log of what
# the stand-ins were given, if any, after " ~ ".
run() {
    rm -f "$LOG"
    got=$(PATH="${path:-$PATH}" "$blit" --offscreen 1400x900 --shot "$dir/shot.bmp" --script "workdir:$1;frame;$2" 2>&1 >/dev/null |
        sed -e "s#$dir#~#g" -e 's/in [0-9.]* s\]/in T]/' | tee "$dir/run.out" | tail -n "${lines:-1}" | awk 'NR > 1 { printf " ~ " } { printf "%s", $0 }')
    [ -f "$LOG" ] && got="$got ~ $(sed -e "s#$dir#~#g" "$LOG" | awk 'NR > 1 { printf " ~ " } { printf "%s", $0 }')"
    path= lines=
}

# ---------------------------------------------------------------- ESP-IDF

# ESP-IDF v6.1 as install.sh leaves it: ~/esp/esp-idf, its Python
# environments in ~/.espressif/python_env (an older Python's that would
# not do, and 3.14's), idf_tools.py's export putting its tools/ on the PATH.
IDF="$HOME/esp/esp-idf"
mkdir -p "$IDF/tools/cmake" "$IDF/tools/idf_py_actions"
printf 'set(IDF_VERSION_MAJOR 6)\nset(IDF_VERSION_MINOR 1)\nset(IDF_VERSION_PATCH 1)\n' >"$IDF/tools/cmake/version.cmake"
printf "SUPPORTED_TARGETS = [\n    'esp32',\n    'esp32s3',\n]\nPREVIEW_TARGETS = ['linux', 'esp32h4']\n" >"$IDF/tools/idf_py_actions/constants.py"
# (Its export warns, as the lab box's does of the system's clangd; with
# TOOLS_MISSING it fails, as with a toolchain not installed.)
cat >"$IDF/tools/idf_tools.py" <<'EOF'
[ "$1" = export ] || exit 2
if [ -n "${TOOLS_MISSING:-}" ]; then
    echo "ERROR: tool xtensa-esp-elf has no installed versions. Please run 'install.sh' to install it." >&2
    exit 1
fi
echo "WARNING: Not using an unsupported version of tool esp-clangd found in PATH: unknown." >&2
printf 'export IDF_PYTHON_ENV_PATH="%s";export PATH="%s/tools:$PATH"\n' "$FAKE_ENV" "$IDF_PATH"
EOF
# (idf.py says what it was given, and with what; a build of a project
# with main/broken in it fails as GCC does.)
cat >"$IDF/tools/idf.py" <<'EOF'
#!/bin/sh
echo "idf.py $* (IDF_PATH=$IDF_PATH, env ${IDF_PYTHON_ENV_PATH##*/})"
if [ "$1" = build ] && [ -f main/broken ]; then
    echo "$PWD/main/hello_world_main.c:12:5: error: 'x' undeclared (first use in this function)"
    echo "ninja: build stopped: subcommand failed."
    exit 2
fi
EOF
chmod +x "$IDF/tools/idf.py"
for py in 3.9 3.14; do
    E="$HOME/.espressif/python_env/idf6.1_py${py}_env"
    mkdir -p "$E/bin"
    printf '#!/bin/sh\nFAKE_ENV=%s exec sh "$@"\n' "$E" >"$E/bin/python"
    chmod +x "$E/bin/python"
done
printf '#!/bin/sh\necho "the older Python was taken" >&2\nexit 1\n' >"$HOME/.espressif/python_env/idf6.1_py3.9_env/bin/python"
# (esptool in ESP-IDF's environment alone: not on the PATH.)
ESPTOOL="$HOME/.espressif/python_env/idf6.1_py3.14_env/bin/esptool"
printf '#!/bin/sh\necho "esptool $*" >>"$LOG"\nexec sh "%s/replay.sh" "%s/progress/esptool5.txt"\n' "$here" "$here" >"$ESPTOOL"
mkdir -p "$dir/hold" && : >"$dir/hold/a"  # (its sample's one wait, let go of at once)
export REPLAY_HOLD="$dir/hold"
chmod +x "$ESPTOOL"

# The project: hello_world as ESP-IDF's examples have it, set to the ESP32-S3.
P="$dir/hello"
mkdir -p "$P/main"
cat >"$P/CMakeLists.txt" <<'EOF'
# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include( $ENV{IDF_PATH}/tools/cmake/project.cmake )
project(hello_world)
EOF
printf 'CONFIG_IDF_TARGET="esp32s3"\nCONFIG_ESPTOOLPY_MONITOR_BAUD=74880\n' >"$P/sdkconfig"
printf 'void app_main(void) {}\n' >"$P/main/hello_world_main.c"

run "$P" "builddump"
check "an ESP-IDF project: its tasks, Build the Run button's; not CMake's" "ESP-IDF: >Build Flash Monitor Menuconfig Size Set target Reconfigure Clean Full clean"
run "$P" "buildpanel;builddump:rows"
check "each row's command; Flash and Monitor blit's own" "Build: idf.py build | Flash: flash · esptool, build/flasher_args.json | Monitor: serial port at 74880 baud | Menuconfig: idf.py menuconfig | Size: idf.py size | Set target: idf.py set-target … | Reconfigure: idf.py reconfigure | Clean: idf.py clean | Full clean: idf.py fullclean"
run "$P" "buildrun:Build;buildwait;builddump:text"
check "idf.py run with ESP-IDF's environment (export.sh never sourced): IDF_PATH, its newest Python's" "|idf.py build (IDF_PATH=~/home/esp/esp-idf, env idf6.1_py3.14_env)|[done in T]"
run "$P" "buildrun:Size;buildwait;builddump:text"
check "the other tasks with it too" "|idf.py size (IDF_PATH=~/home/esp/esp-idf, env idf6.1_py3.14_env)|[done in T]"
got=$(TOOLS_MISSING=1 "$blit" --offscreen 1400x900 --shot "$dir/shot.bmp" --script "workdir:$P;frame;buildrun:Build;buildwait;builddump:text" 2>&1 >/dev/null | sed -e "s#$dir#~#g" -e 's/after [0-9.]* s\]/after T]/' | tail -1)
check "ESP-IDF's environment that cannot be set up: its own words, and nothing run" "|ERROR: tool xtensa-esp-elf has no installed versions. Please run 'install.sh' to install it.|[failed with 1 after T]"
touch "$P/main/broken"
lines=2 run "$P" "buildrun:Build;buildwait;progressdump;scandump"
rm "$P/main/broken"
check "a compiler's error in idf.py's output is a problem" "problems: 1 ~ E main/hello_world_main.c:12:5 'x' undeclared (first use in this function)"
run "$P" "buildrun:Set target;finddump"
check "Set target: ESP-IDF's own targets to choose from, the preview ones as such" "pick [] 4 : >esp32 esp32s3 linux (preview) esp32h4 (preview)"
run "$P" "buildrun:Set target;text:h4;key:Return;buildwait;builddump:text"
check "the one chosen set, a preview one with --preview" "|idf.py --preview set-target esp32h4 (IDF_PATH=~/home/esp/esp-idf, env idf6.1_py3.14_env)|[done in T]"
run "$P" "buildrun:Menuconfig;wait:300;tabsdump"
check "menuconfig's tab has the keyboard" "Menuconfig"

# Flash: built first, then esptool (ESP-IDF's own, from its environment)
# with the files and addresses the build wrote, the chip from it; the
# progress on Flash's row.
mkdir -p "$P/build"
cat >"$P/build/flasher_args.json" <<'EOF'
{ "write_flash_args" : [ "--flash_mode", "dio", "--flash_size", "2MB", "--flash_freq", "80m" ],
  "flash_files" : { "0x0" : "bootloader/bootloader.bin", "0x10000" : "hello_world.bin", "0x8000" : "partition_table/partition-table.bin" },
  "extra_esptool_args" : { "after" : "hard_reset", "before" : "default_reset", "stub" : true, "chip" : "esp32s3" } }
EOF
mkdir -p "$P/build/bootloader" "$P/build/partition_table"
printf 'boot' >"$P/build/bootloader/bootloader.bin"
printf 'parts' >"$P/build/partition_table/partition-table.bin"
printf 'app' >"$P/build/hello_world.bin"
lines=2 run "$P" "buildpanel;buildrun:Flash;flashwait;buildwait;builddump;progressdump"
check "Flash: Build first, then esptool from ESP-IDF's environment with flasher_args.json's files" "ESP-IDF: Build >Flash Monitor Menuconfig Size Set target Reconfigure Clean Full clean (last: 0) (Flashed 170.5 KB in 8.8 s, verified) ~ button: ▶ Flash · Flashed 170.5 KB | row: done · Flashed 170.5 KB in 8.8 s, verified ~ esptool --chip esp32s3 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 2MB --flash-freq 80m 0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/hello_world.bin"
# ("Flash the board" with no run configuration: the project's own board.)
run "$P" "command:Flash the board;flashwait;buildwait;progressdump"
check "Flash the board: the project's own board when no run configuration has one" "button: ▶ Build · Flashed 170.5 KB | row: done · Flashed 170.5 KB in 8.8 s, verified ~ esptool --chip esp32s3 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 2MB --flash-freq 80m 0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/hello_world.bin"
# (A run configuration's board, the port chosen for it: its port, the rest the project's.)
mkdir -p "$P/.blit"
printf '{ "configurations": [ { "name": "hello", "program": "build/hello", "board": { "serial": { "port": "/dev/ttyUSB7" } } } ] }\n' >"$P/.blit/run.json"
run "$P" "buildpanel;buildrun:Flash;flashwait;buildwait;progressdump"
rm -r "$P/.blit"
check "a run configuration's board: its port, the image and chip the project's" "button: ▶ Flash · Flashed 170.5 KB | row: done · Flashed 170.5 KB in 8.8 s, verified ~ esptool --chip esp32s3 -p /dev/ttyUSB7 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 2MB --flash-freq 80m 0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/hello_world.bin"
rm -r "$P/build"

# What is not installed: said on each row, and when run, and nothing run.
mv "$HOME/.espressif/python_env/idf6.1_py3.14_env" "$dir/env-away"
mv "$HOME/.espressif/python_env/idf6.1_py3.9_env" "$dir/env-older"
run "$P" "buildpanel;builddump:rows"
check "no Python environment for this ESP-IDF: said once, under its heading, and where install.sh is" "! ESP-IDF's Python environment is not installed on $machine: ~/esp/esp-idf/install.sh sets it up | Build: idf.py build | Flash: flash · esptool, build/flasher_args.json | Monitor: serial port at 74880 baud | Menuconfig: idf.py menuconfig | Size: idf.py size | Set target: idf.py set-target … | Reconfigure: idf.py reconfigure | Clean: idf.py clean | Full clean: idf.py fullclean"
mv "$dir/env-away" "$HOME/.espressif/python_env/idf6.1_py3.14_env"
mv "$dir/env-older" "$HOME/.espressif/python_env/idf6.1_py3.9_env"
mv "$IDF" "$dir/idf-away"
run "$P" "buildrun:Build;buildwait"
check "no ESP-IDF: said when run, nothing run" "blit: ESP-IDF is not installed on $machine (not in IDF_PATH, ~/esp or ~/.espressif)"
run "$P" "buildpanel;buildrun:Size;frame;builddump:rows"
check "and on the row of the task that was run, the others' their commands" "! ESP-IDF is not installed on $machine (not in IDF_PATH, ~/esp or ~/.espressif) | Build: idf.py build | Flash: flash · esptool, build/flasher_args.json | Monitor: serial port at 74880 baud | Menuconfig: idf.py menuconfig | Size: ESP-IDF is not installed on $machine (not in IDF_PATH, ~/esp or ~/.espressif) | Set target: idf.py set-target … | Reconfigure: idf.py reconfigure | Clean: idf.py clean | Full clean: idf.py fullclean"
run "$P" "buildpanel;buildrun:Flash;flashwait"
check "nor flashed: its build cannot be run" "blit: ESP-IDF is not installed on $machine (not in IDF_PATH, ~/esp or ~/.espressif): flashing hello_world did not start"
# (IDF_PATH, in blit's environment, is looked in first.)
got=$(IDF_PATH="$dir/idf-away" "$blit" --offscreen 1400x900 --shot "$dir/shot.bmp" --script "workdir:$P;frame;buildrun:Build;buildwait;builddump:text" 2>&1 >/dev/null | sed -e "s#$dir#~#g" -e 's/in [0-9.]* s\]/in T]/' | tail -1)
check "IDF_PATH where it is elsewhere" "|idf.py build (IDF_PATH=~/idf-away, env idf6.1_py3.14_env)|[done in T]"
mv "$dir/idf-away" "$IDF"

# A CMakeLists.txt that only mentions ESP-IDF (in a comment) is CMake's.
C="$dir/plain"
mkdir -p "$C"
printf '# include($ENV{IDF_PATH}/tools/cmake/project.cmake)\ncmake_minimum_required(VERSION 3.22)\nproject(plain)\n' >"$C/CMakeLists.txt"
run "$C" "builddump"
check "not ESP-IDF's: CMake's" "CMake: >Build Configure Test"

# ---------------------------------------------------------------- PlatformIO

# pio where PlatformIO's installer puts it, not on the PATH; the
# raspberrypi platform installed, with the Pico's manifest (its firmware
# reset by a 1200 baud touch, as Arduino's core for it needs). (A machine
# with a pio of its own on the PATH, which blit rightly takes first: the
# stand-in goes before it on the PATH instead, and the cases of PlatformIO's
# own place, and of none at all, cannot be had there.)
PIO="$HOME/.platformio"
penv="$PIO/penv/bin"
real_pio=$(PATH=${PATH#"$B:"} command -v pio || PATH=${PATH#"$B:"} command -v platformio)
if [ -n "$real_pio" ]; then
    echo "SKIP: firmware.sh's cases of ~/.platformio/penv/bin, and of no pio, need no pio on the PATH ($real_pio is)"
    penv="$B"
fi
mkdir -p "$penv" "$PIO/platforms/raspberrypi/boards"
printf '#!/bin/sh\necho "pio $*"\ncase " $* " in *" -t upload "*) sleep 2 ;; esac\n' >"$penv/pio"
chmod +x "$penv/pio"
printf '{ "build": { "mcu": "rp2040" }, "upload": { "use_1200bps_touch": true, "wait_for_upload_port": true } }\n' >"$PIO/platforms/raspberrypi/boards/pico.json"
F="$dir/fw"
mkdir -p "$F/src" "$F/test"
cat >"$F/platformio.ini" <<'EOF'
; PlatformIO Project Configuration File
[platformio]
default_envs = pico2

[env]
monitor_speed = 115200

[common]
platform = raspberrypi

[env:pico]
extends = common
board = pico
framework = arduino

[env:pico2]
platform = ${common.platform}
board = rpipico2
upload_protocol = cmsis-dap   ; a probe: PlatformIO's own upload

[env:s3]
platform = espressif32
board = esp32-s3-devkitc-1
monitor_speed: 74880
build_flags =
    -DNAME=${this.__env__}
EOF
run "$F" "builddump"
check "PlatformIO: each env's tasks, default_envs first, its Build the Run button's" "PlatformIO: >Build (pico2) Upload (pico2) Monitor (pico2) Test (pico2) Clean (pico2) Compile database (pico2) Build (pico) Upload (pico) Monitor (pico) Test (pico) Clean (pico) Compile database (pico) Build (s3) Upload (s3) Monitor (s3) Test (s3) Clean (s3) Compile database (s3)"
run "$F" "buildpanel;builddump:rows"
check "rows: an RP2040's upload blit's picotool, others PlatformIO's; speeds through [env] and an env's own" "Build (pico2): pio run -e pico2 | Upload (pico2): flash · pio run -e pico2 -t upload | Monitor (pico2): serial port at 115200 baud | Test (pico2): pio test -e pico2 | Clean (pico2): pio run -e pico2 -t clean | Compile database (pico2): pio run -e pico2 -t compiledb | Build (pico): pio run -e pico | Upload (pico): flash · picotool .pio/build/pico/firmware.elf | Monitor (pico): serial port at 115200 baud | Test (pico): pio test -e pico | Clean (pico): pio run -e pico -t clean | Compile database (pico): pio run -e pico -t compiledb | Build (s3): pio run -e s3 | Upload (s3): flash · pio run -e s3 -t upload | Monitor (s3): serial port at 74880 baud | Test (s3): pio test -e s3 | Clean (s3): pio run -e s3 -t clean | Compile database (s3): pio run -e s3 -t compiledb"
run "$F" "buildrun:Build (pico);buildwait;builddump:text"
check "pio run, from ~/.platformio/penv/bin (or the stand-in on the PATH)" "|pio run -e pico|[done in T]"

# The Pico, plugged in (a stand-in), running Arduino's core: Upload builds,
# finds its port, resets it into BOOTSEL with a 1200 baud touch, then
# picotool loads the ELF.
DEV="$dir/dev" USB="$dir/usb" CTL="$dir/ctl" HEARD="$dir/heard"
export BLIT_USB_ROOT="$USB" BLIT_USB_DEV="$DEV"
IFACE="$USB/1-3:1.0"
start_fake() {
    stop_fake
    rm -rf "$DEV" "$USB" "$CTL" "$HEARD"
    mkdir -p "$DEV" "$USB/1-3" "$IFACE"
    mkfifo "$CTL"
    printf '2e8a\n' >"$USB/1-3/idVendor"
    printf '005e\n' >"$USB/1-3/idProduct"
    printf '00\n' >"$USB/1-3/bDeviceClass"
    printf 'Arduino\n' >"$USB/1-3/manufacturer"
    printf 'RaspberryPi Pico\n' >"$USB/1-3/product"
    printf 'FAKE0002\n' >"$USB/1-3/serial"
    printf '00\n' >"$IFACE/bInterfaceNumber"
    printf 'Board CDC\n' >"$IFACE/interface"
    python3 "$here/fake-serial.py" "$DEV" ttyACM9 "$HEARD" "$CTL" "$IFACE" &
    fake=$!
    for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do [ -e "$DEV/ttyACM9" ] && break; sleep 0.1; done
}
mkdir -p "$F/.pio/build/pico"
printf 'firmware' >"$F/.pio/build/pico/firmware.elf"
start_fake
run "$F" "buildpanel;buildrun:Upload (pico);flashwait;buildwait;progressdump;builddump:text"
check "Upload: built, the Pico's port touched at 1200 baud, then picotool" "|Resetting into BOOTSEL: ~/dev/ttyACM9 at 1200 baud|Loading into Flash:   [==============================]  100%|[done in T] ~ picotool info ~ picotool load -v -x .pio/build/pico/firmware.elf -f"

# Monitor: the one port an RP2040's board could be on, at the env's speed;
# its serial tab.
start_fake
run "$F" "buildpanel;buildrun:Monitor (pico);firmwarewait;serialstate:open;serialdump"
check "Monitor: the Pico's port found, at the env's speed" "ttyACM9 115200: open ttyACM9 115200 8N1 return CR dtr on rts on serial FAKE0002 kept 0 gaps 0"
# The project on another machine (tests/fake-ssh.sh standing in for ssh,
# the same home there): the port its board is on there, opened there by
# blit's agent (blit-agent --serial).
start_fake
mkdir -p "$dir/run" && chmod 700 "$dir/run"
export BLIT_SSH="$here/fake-ssh.sh" BLIT_FAKE_HOME="$HOME" BLIT_AGENT_RUN="$dir/run" BLIT_AGENT_IDLE=4
run "$F" "folder:ssh://box$F;remotewait;frame;buildpanel;buildrun:Monitor (pico);firmwarewait;serialstate:open;serialdump"
unset BLIT_SSH BLIT_FAKE_HOME BLIT_AGENT_RUN BLIT_AGENT_IDLE
pkill -9 -f "$HOME/.cache/blit/[b]lit-agent" 2>/dev/null
check "Monitor on another machine: the Pico's port there, opened there" "ttyACM9 on box 115200: open ttyACM9 on box 115200 8N1 return CR dtr on rts on serial FAKE0002 kept 0 gaps 0"
# Asked for as the folder there is opened, the connection to it taking 2 s
# to be made (an ssh that is slow to answer): its tasks read once it is,
# not an empty folder's while it is being made (GitHub's runner, slow,
# 2026-11-24: "no Monitor (pico)").
start_fake
printf '#!/bin/sh\nsleep 2\nexec "%s" "$@"\n' "$here/fake-ssh.sh" >"$dir/slow-ssh.sh"
chmod +x "$dir/slow-ssh.sh"
export BLIT_SSH="$dir/slow-ssh.sh" BLIT_FAKE_HOME="$HOME" BLIT_AGENT_RUN="$dir/run" BLIT_AGENT_IDLE=4
run "$F" "folder:ssh://box$F;frame;buildpanel;buildrun:Monitor (pico);firmwarewait;serialstate:open;serialdump"
unset BLIT_SSH BLIT_FAKE_HOME BLIT_AGENT_RUN BLIT_AGENT_IDLE
pkill -9 -f "$HOME/.cache/blit/[b]lit-agent" 2>/dev/null
check "and opened as the connection there is still being made: read once it is" "ttyACM9 on box 115200: open ttyACM9 on box 115200 8N1 return CR dtr on rts on serial FAKE0002 kept 0 gaps 0"
# (No such port: said.)
stop_fake
rm -rf "$USB" && mkdir -p "$USB"
run "$F" "buildpanel;buildrun:Monitor (pico);firmwarewait"
check "no port of its chip's: said" "blit: No serial port of an RP2040's is plugged in to $machine"
export BLIT_USB_ROOT="$dir/nousb" BLIT_USB_DEV="$dir/nousb"

# An ESP32's upload is PlatformIO's own (pio's stand-in takes 2 s over
# it): the serial tab on the env's monitor_port lets go of the port while
# it runs, and has it back after, as blit's own flash does.
E="$dir/esp"
mkdir -p "$E/src"
printf '[env:s3]\nplatform = espressif32\nboard = esp32-s3-devkitc-1\nmonitor_speed = 115200\nmonitor_port = %s/ttyACM9\n' "$DEV" >"$E/platformio.ini"
start_fake
run "$E" "buildpanel;buildrun:Monitor;serialstate:open;buildrun:Upload;serialstate:released;buildwait;serialstate:open;serialdump"
check "PlatformIO's own upload: the serial tab lets go of the port while it runs, and has it back" "ttyACM9 115200: open ttyACM9 115200 8N1 return CR dtr on rts on kept 0 gaps 1"
stop_fake

# No pio anywhere: said, on its rows and when run.
if [ -z "$real_pio" ]; then
    mv "$PIO/penv" "$dir/penv-away"
    run "$F" "buildrun:Build (pico);buildwait"
    check "no PlatformIO: said, and where it was looked for" "blit: PlatformIO is not installed on $machine (no pio on the PATH, nor in ~/.platformio/penv/bin)"
    mv "$dir/penv-away" "$PIO/penv"
fi

exit $failed
