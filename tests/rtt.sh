#!/bin/sh
# A board's RTT channels, semihosting output and defmt logs as tabs
# (linktab.cpp, apprtt.cpp, rtt.cpp), and the loopback socket module under
# them (loopback.cpp). The tools are stand-ins shaped as each serves its
# ports (tests/fake-rtt.py: OpenOCD's Tcl and RTT servers, J-Link's RTT
# telnet port, pyOCD's servers from its config, probe-rs's DAP events),
# put on the PATH as the programs blit runs; no probe is needed. Where QEMU
# and Arm's toolchain are here, the real thing too: F5 on QEMU's
# mps2-an385 with semihosting on (tests/rtt/semi.c) and with defmt's
# frames over semihosting through defmt-print (tests/rtt/defmt-qemu.elf,
# built from tests/rtt/defmt-qemu).
#
#   tests/rtt.sh path/to/blit            run the cases
#   tests/rtt.sh path/to/blit --print    show what each case gives
set -u
blit=${1:?usage: tests/rtt.sh path/to/blit [--print]}
print=${2:-}
here=$(cd "$(dirname "$0")" && pwd -P)
. "$here/without.sh"
command -v python3 >/dev/null || { echo "rtt.sh: python3 is not installed (the stand-in tools are a Python script)"; exit 1; }
dir=$(mktemp -d)
dir=$(cd "$dir" && pwd -P)
fakes=
trap 'for f in $fakes; do kill $f 2>/dev/null; done; pkill -f "$dir/" 2>/dev/null; rm -rf "$dir"' EXIT
printf 'blit.opt.lsp.enabled = false\nblit.opt.git.enabled = false\nblit.opt.editor.modal = false\n' >"$dir/init.lua"
real_home=$HOME
# (A gdb for Arm that speaks DAP, looked for before HOME is the test's.)
# (Each on the PATH tried: Arm's own toolchain's gdb has no Python.)
gdb=
for d in $(printf '%s' "$PATH" | tr ':' ' '); do
    for g in arm-none-eabi-gdb gdb-multiarch gdb; do
        [ -z "$gdb" ] && [ -x "$d/$g" ] && [ "$("$d/$g" -q -batch -ex 'set architecture arm' -ex 'python print(41 + 1)' 2>/dev/null | tail -1)" = 42 ] && gdb="$d/$g"
    done
done
# (defmt-print: on the PATH, else where cargo installs it, ~/.cargo/bin.)
defmt=$(command -v defmt-print || { [ -x "$real_home/.cargo/bin/defmt-print" ] && echo "$real_home/.cargo/bin/defmt-print"; })
export BLIT_INIT="$dir/init.lua" BLIT_RECENT_FILE="$dir/recent" HOME="$dir/home"
LOG="$dir/log" P="$dir/fw" B="$dir/bin"
export FAKE_RTT_LOG="$LOG"
mkdir -p "$HOME" "$P/.blit" "$B"
# (Linked into the test's ~/.cargo/bin: where it is not on the PATH, as
# cargo leaves it, blit finds it there.)
if [ -n "$defmt" ]; then
    mkdir -p "$HOME/.cargo/bin"
    ln -s "$defmt" "$HOME/.cargo/bin/defmt-print"
fi
case $(uname -s) in Darwin) this="this Mac" ;; *) this="this machine" ;; esac
# (Ports of this run's own: two runs at once do not meet.)
base=$((30000 + $$ % 15000 * 2))

failed=0
check() {
    if [ -n "$print" ]; then
        printf '%s\n    %s\n' "$1" "$got"
    elif [ "$got" != "$2" ]; then
        printf 'FAIL: %s\n    expected %s\n    got      %s\n' "$1" "$2" "$got"
        failed=1
    fi
}
# run SCRIPT: blit in the project, its last `lines` lines joined by " ~ ",
# the folder written @, times as HH:MM:SS.mmm, spans as N s.
run() {
    : >"$LOG"
    got=$(cd "$P" && PATH="${path:-$PATH}" "$blit" --offscreen "${size:-1400x1400}" --shot "$dir/shot.bmp" --script "workdir:$P;frame;$1" 2>&1 >/dev/null |
        sed -E -e "s#$dir#@#g" -e 's/[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}/HH:MM:SS.mmm/g' -e 's/[0-9]+\.[0-9] s/N s/g' | tail -n "${lines:-1}" | awk 'NR > 1 { printf " ~ " } { printf "%s", $0 }')
    size= path= lines=
}
logged() { got=$(grep -E "$1" "$LOG" | sed "s#$dir#@#g" | awk 'NR > 1 { printf " ~ " } { printf "%s", $0 }'); }
fake() {  # fake MODE ARGS...: a stand-in in the background, until the run's end
    python3 "$here/fake-rtt.py" "$@" &
    fakes="$fakes $!"
    sleep 0.3
}
stop_fakes() {
    for f in $fakes; do kill $f 2>/dev/null; wait $f 2>/dev/null; done
    fakes=
}

# ---------------------------------------------------------------- the link: a channel's server

p=$base
fake tcp $p 0
run "link:rtt,$p;rttwait:kept 60;frame;termdump"
check "a channel's lines on the tab" "0 Terminal: line 1|0 Terminal: line 2|0 Terminal: line 3 @3,0 11x86"
run "link:rtt,$p;rttwait:kept 60;text:help;key:Return;termwait:down 0;frame;termdump"
check "typing goes down the channel, Return as LF" "0 Terminal: line 1|0 Terminal: line 2|0 Terminal: line 3|down 0: help @4,0 11x86"
logged "^down"
check "what the board was sent" "down 0: b'help\\n'"
lines=2 run "link:rtt,$p;rttwait:kept 60;tabsdump;rttdump"
check "the tab named for its channel; open, its port, what it kept" "RTT 0 ~ RTT 0: open rtt port $p kept 60 gaps 0 opens 1"
stop_fakes

# The server going (a session ended) and coming back (the next): the tab
# stays, says it is waiting, and takes up again, one line marking the gap.
p=$((base + 1))
start="python3 $here/fake-rtt.py tcp $p 0 >/dev/null 2>&1 & echo \$! >$dir/pid"
size=1400x1800 lines=3 run "lua:os.execute('$start');wait:300;link:rtt,$p;rttwait:kept 60;lua:os.execute('kill \$(cat $dir/pid)');rttwait:gone;wait:300;frame;termdump;tabsdump;lua:os.execute('$start');rttwait:kept 120;frame;termdump"
check "the server gone: waiting, said on its last line and in its name; back: a line marking the gap, and on" \
    "0 Terminal: line 1|0 Terminal: line 2|0 Terminal: line 3|[gone at HH:MM:SS.mmm (closed by the other end); trying 127.0.0.1:$p] @3,72 15x86 ~ RTT 0 (waiting) ~ 0 Terminal: line 1|0 Terminal: line 2|0 Terminal: line 3|[gone from HH:MM:SS.mmm to HH:MM:SS.mmm, N s (closed by the other end)]|0 Terminal: line 1|0 Terminal: line 2|0 Terminal: line 3 @7,0 15x86"
kill "$(cat "$dir/pid")" 2>/dev/null
run "link:rtt,$((base + 2));wait:400;frame;termdump"
check "nothing listening yet: waited for, and why" "[waiting for 127.0.0.1:$((base + 2)): connection refused, trying again] @0,63 11x86"

# Listening, for a tool that connects out (QEMU's chardev).
p=$((base + 3))
printf 'import socket, sys, time\ns = socket.create_connection(("127.0.0.1", int(sys.argv[1])))\ns.sendall(b"from the tool\\r\\n")\ntime.sleep(0.5)\n' >"$dir/send.py"
lines=2 run "link:semihosting,$p,listen;wait:200;frame;termdump;lua:os.execute('python3 $dir/send.py $p &');rttwait:kept 15;frame;termdump"
check "listening: waiting for the tool, then what it sends" "[waiting for a connection on $p] @0,35 11x86 ~ from the tool @1,0 11x86"

# J-Link's telnet port: channel 0, or another chosen by its config string in the first 100 ms.
p=$((base + 4))
fake jlink -RTTTelnetPort $p
run "link:rtt,$p,channel=1,jlink=1;rttwait:kept 51;rttwait:kept 60;frame;termdump"
check "J-Link: another channel chosen as it connects" "1 defmt: line 1|1 defmt: line 2|1 defmt: line 3 @3,0 11x86"
logged "^config"
check "its config string" "config RTTCh;1"
stop_fakes

# Times and hex, the log, Cmd+K: a serial tab's commands, any monitor tab's.
p=$((base + 5))
fake tcp $p 0
case $(uname -s) in Darwin) clear=cmd+k ;; *) clear=ctrl+shift+k ;; esac
run "link:rtt,$p;rttwait:kept 60;command:Show or hide the serial port's times;frame;termdump"
check "the host's times before each line" "HH:MM:SS.mmm 0 Terminal: line 1|HH:MM:SS.mmm 0 Terminal: line 2|HH:MM:SS.mmm 0 Terminal: line 3 @3,0 11x86"
size=1400x1400 run "link:rtt,$p;rttwait:kept 60;command:Show the serial port's bytes as hex, or as text;frame;termdump"
got=$(printf '%s' "$got" | cut -d'|' -f1)
check "hex of the same bytes" "00000000  30 20 54 65 72 6d 69 6e  61 6c 3a 20 6c 69 6e 65  0 Terminal: line"
run "link:rtt,$p;rttwait:kept 60;key:$clear;frame;termdump"
check "Cmd+K: what came goes" " @0,0 11x86"
# (The log from now on: its server held until it is started, else its
# first line may come before it, on a slow machine.)
stop_fakes
rm -f "$dir/go"
FAKE_RTT_HOLD="$dir/go" fake tcp $p 0
run "link:rtt,$p;command:Log the serial port to a file…;key:ctrl+u;text:$dir/rtt.log;key:Return;lua:io.open('$dir/go','w'):close();rttwait:kept 60;command:Stop logging the serial port"
got=$(sed -E 's/^[0-9-]+ [0-9:.]+ //' "$dir/rtt.log" 2>/dev/null | tr -d '\r' | tr '\n' '|')
check "logged, each line after its date and time" "0 Terminal: line 1|0 Terminal: line 2|0 Terminal: line 3|"
stop_fakes

# ---------------------------------------------------------------- a board's RTT on its own, by its tool

cc=$(command -v arm-none-eabi-gcc || ls -d "$real_home"/.local/opt/arm-gnu-toolchain-*/bin/arm-none-eabi-gcc 2>/dev/null | tail -1)
nm=
if [ -n "$cc" ]; then
    nm=$(dirname "$cc")/arm-none-eabi-nm
    (cd "$P" && cp "$here/rtt/rtt.c" "$here/boards/cm.ld" . && "$cc" -mcpu=cortex-m3 -mthumb -g -O0 -nostdlib -T cm.ld -e main -o fw.elf rtt.c) || cc=
elif [ "$(uname -s)" = Linux ] && command -v cc >/dev/null; then
    cc=cc nm=nm
    (cd "$P" && cp "$here/rtt/rtt.c" . && cc -g -O0 -o fw.elf rtt.c) || cc=
fi
if [ -z "$cc" ]; then
    echo "SKIP: rtt.sh's board cases need arm-none-eabi-gcc (or, on Linux, cc) to build a program with _SEGGER_RTT"
else
    rttaddr=$("$nm" -S "$P/fw.elf" | awk '$4 == "_SEGGER_RTT" { print "0x" $1; exit }' | sed 's/^0x0*/0x/')
    rttsize=$("$nm" -S "$P/fw.elf" | awk '$4 == "_SEGGER_RTT" { print $2; exit }')
    rttsize=$((0x$rttsize))
    for t in openocd pyocd JLinkGDBServerCLExe; do
        mode=$t
        [ $t = JLinkGDBServerCLExe ] && mode=jlink
        printf '#!/bin/sh\nexec python3 %s %s "$@"\n' "$here/fake-rtt.py" "$mode" >"$B/$t"
        chmod +x "$B/$t"
    done
    printf '#!/bin/sh\n[ "$1" = dap-server ] || exit 2\nexec python3 %s probe-rs "$@"\n' "$here/fake-rtt.py" >"$B/probe-rs"
    chmod +x "$B/probe-rs"
    PROBE='"probe": {"kind": "stlink", "serial": "004D00283133510F37363734", "usb": "0483:374e"}, "chip": "STM32F411CEUx"'
    STLINK="$PROBE"', "elf": "fw.elf"'
    board() { printf '{ "configurations": [ { "name": "fw", "program": "fw.elf", "board": { %s } } ] }\n' "$1" >"$P/.blit/run.json"; }
    p=$((base + 10))

    board "$STLINK, \"debug\": \"openocd\", \"rtt\": { \"port\": $p }"
    path="$B:$PATH" run "command:Open the board's RTT;rttwait:driver fw done;rttwait:RTT 1: defmt: open;rttwait:kept 51;rttwait:kept 60;frame;rttdump"
    got=$(printf '%s' "$got" | sed -E 's/tries [0-9]+/tries N/')
    check "OpenOCD: its Tcl port asked until the program has made the block, a tab for each up-channel, named from it" \
        "RTT 0: Terminal: open rtt 0 port $p of fw kept 60 gaps 0 opens 1 | RTT 1: defmt: open rtt 1 port $((p + 1)) of fw kept 51 gaps 0 opens 1 | driver fw done tries N opens 1"
    path="$B:$PATH" run "command:Open the board's RTT;rttwait:driver fw done;rttwait:RTT 1: defmt: open;wait:100"
    logged "^openocd|^tcl rtt"
    got=$(printf '%s' "$got" | sed -E 's/tcl_port [0-9]+/tcl_port T/' | awk -F' ~ ' '{ out = ""; for (i = 1; i <= NF; ++i) if ($i != prev) { out = out (out == "" ? "" : " ~ ") $i; prev = $i } print out }')
    check "what it was told: started with its Tcl port (no gdb port), set up at the ELF's _SEGGER_RTT, started until found, a server per channel" \
        "openocd -c gdb_port disabled -c telnet_port disabled -c tcl_port T -f interface/stlink.cfg -c adapter serial 004D00283133510F37363734 -f target/stm32f4x.cfg -c init ~ tcl rtt setup $rttaddr $rttsize {SEGGER RTT} ~ tcl rtt start ~ tcl rtt channels ~ tcl rtt start ~ tcl rtt channels ~ tcl rtt start ~ tcl rtt channels ~ tcl rtt server start $p 0 ~ tcl rtt server start $((p + 1)) 1"

    # An OpenOCD whose rtt has only its server (Espressif's for the ESP32
    # chips): said once, not asked again and again.
    export FAKE_RTT_NO_SETUP=1
    path="$B:$PATH" run "command:Open the board's RTT;rttwait:driver fw none;wait:700;rttdump"
    unset FAKE_RTT_NO_SETUP
    got=$(printf '%s' "$got" | sed -E 's/tries [0-9]+/tries N/')
    check "an OpenOCD with no RTT of its own (Espressif's): said, and not asked again" \
        "driver fw none tries N opens 1"
    logged "^tcl rtt"
    check "and asked once" "tcl rtt setup $rttaddr $rttsize {SEGGER RTT}"

    # A crash in what a channel sends: decoded with the board's ELF, a problem.
    pc=$("$nm" "$P/fw.elf" | awk '$3 == "fault" { print $1; exit }')
    pc=$(printf '0x%08x' $((0x$pc + 4)))
    FAKE_RTT_THEN="Guru Meditation Error: Core  0 panic'ed (LoadProhibited). Exception was unhandled.\\nBacktrace: $pc:0x3fc9a0a0\\nRebooting...\\n"
    export FAKE_RTT_THEN
    path="$B:$PATH" size=2400x900 run "command:Open the board's RTT;rttwait:RTT 0: Terminal: open;wait:800;problemsdump"
    got=$(printf '%s' "$got" | sed -E 's/ [0-9]+:[0-9]+ Crash/ L:C Crash/')
    check "a crash on an RTT channel decoded with the board's ELF, and a problem under the channel's tab" \
        "button: ▶ fw | RTT 0: Terminal 1 | file rtt.c | >▸E L:C Crash at rtt.c:19 in fault (LoadProhibited)"
    unset FAKE_RTT_THEN

    # defmt over RTT: a channel named "defmt", an ELF with .defmt (the
    # frames tests/rtt/defmt-qemu.elf wrote, its block's address given).
    if [ -n "$defmt" ]; then
        mkdir -p "$P/src"
        cp "$here/rtt/defmt-qemu.elf" "$P/defmt.elf"
        cp "$here/rtt/defmt-qemu/src/main.rs" "$P/src/"
        board "$PROBE, \"elf\": \"defmt.elf\", \"debug\": \"openocd\", \"rtt\": { \"port\": $p, \"address\": \"0x20000100\" }"
        export FAKE_RTT_FRAMES="$here/rtt/defmt-qemu.frames"
        path="$B:$PATH" size=2000x1400 lines=2 run "command:Open the board's RTT;rttwait:records 6;rttdump;key:ctrl+\`;text:\`;frame;termdump"
        got=$(printf '%s' "$got" | sed -E -e 's/ \| driver fw done tries [0-9]+ opens 1//' -e 's/^RTT 0: Terminal: [^|]*\| //')
        check "defmt over RTT: the defmt channel through defmt-print (on the PATH, or in ~/.cargo/bin), a record a line" \
            "RTT 1: defmt: open rtt 1 port $((p + 1)) of fw kept 423 gaps 0 opens 1 defmt from TRACE records 6 ~ 0.000000 TRACE clock tree set up  ./src/main.rs:22|0.000125 DEBUG adc offset -12  ./src/main.rs:23|0.000250 INFO  motor controller ready, 3 phases  ./src/main.rs:24|0.000375 WARN  supply at 3.05 V, below 3.2  ./src/main.rs:25|0.000500 ERROR over-current on phase 2: 1840 mA  ./src/main.rs:26|0.000625 done  ./src/main.rs:27 @6,0 11x124"
        unset FAKE_RTT_FRAMES
        logged "^tcl rtt setup"
        check "the block where the board says (its size unknown: OpenOCD reads the header there)" "tcl rtt setup 0x20000100 1024 {SEGGER RTT}"
    else
        echo "SKIP: rtt.sh's defmt over RTT needs defmt-print (cargo install defmt-print)"
    fi

    # Two boards' RTT at once (two blits, each its own board and its own
    # OpenOCD), neither naming a port: each channel's server on a free
    # port, so each tab has its own board's lines. (On 19021 + n for both,
    # the second's servers could not be had, and its tabs met the first's.)
    P2="$dir/fw2"
    mkdir -p "$P2/.blit"
    cp "$P/fw.elf" "$P2/"
    board "$STLINK, \"debug\": \"openocd\", \"rtt\": true"
    cp "$P/.blit/run.json" "$P2/.blit/"
    both="command:Open the board's RTT;rttwait:RTT 1: defmt: open;rttwait:kept 57;rttwait:kept 66;key:ctrl+\`;text:\`;frame;termdump"
    export FAKE_RTT_TAG=B
    (cd "$P2" && PATH="$B:$PATH" "$blit" --offscreen 1400x1400 --shot "$dir/shot2.bmp" --script "workdir:$P2;frame;$both" 2>"$dir/second" >/dev/null) &
    second=$!
    export FAKE_RTT_TAG=A
    path="$B:$PATH" run "$both"
    unset FAKE_RTT_TAG
    wait $second
    got="$got ~ $(tail -n 1 "$dir/second")"
    check "two boards' RTT at once, no port named: each tab its own board's lines" \
        "A 1 defmt: line 1|A 1 defmt: line 2|A 1 defmt: line 3 @3,0 11x86 ~ B 1 defmt: line 1|B 1 defmt: line 2|B 1 defmt: line 3 @3,0 11x86"

    board "$STLINK, \"debug\": \"pyocd\", \"rtt\": { \"port\": $p, \"channels\": [0, 1] }"
    path="$B:$PATH" run "command:Open the board's RTT;rttwait:RTT 1: open;rttwait:kept 51;rttwait:kept 60;rttdump"
    check "pyOCD: a server for each channel the board lists, from its config" \
        "RTT 0: open rtt 0 port $p of fw kept 60 gaps 0 opens 1 | RTT 1: open rtt 1 port $((p + 1)) of fw kept 51 gaps 0 opens 1"
    path="$B:$PATH" run "command:Open the board's RTT;rttwait:RTT 1: open;wait:100"
    logged "^pyocd|^config"
    got=$(printf '%s' "$got" | sed -E 's#@/home/[^ ]*/rtt/[0-9]+-fw.yaml#@/CACHE/fw.yaml#')
    check "attached as it runs, its config: the block at the ELF's _SEGGER_RTT, the channels' servers" \
        "pyocd gdbserver -t stm32f411ceux -u 004D00283133510F37363734 -M attach --persist --config @/CACHE/fw.yaml ~ config rtt: - control-block: address: $rttaddr size: $rttsize channel: - number: 0 mode: server port: $p - number: 1 mode: server port: $((p + 1))"

    board '"probe": {"kind": "jlink", "serial": "000801234567", "usb": "1366:0105"}, "chip": "STM32H743ZI", "elf": "fw.elf", "rtt": { "port": '$p', "channels": [0, 1] }'
    path="$B:$PATH" run "command:Open the board's RTT;rttwait:RTT 1: open;rttwait:kept 51;rttwait:kept 60;rttdump"
    check "J-Link: its RTT telnet port, a tab for each channel the board lists (the second chosen as it connects)" \
        "RTT 0: open rtt 0 port $p of fw kept 60 gaps 0 opens 1 | RTT 1: open rtt 1 port $p of fw kept 51 gaps 0 opens 1"
    path="$B:$PATH" run "command:Open the board's RTT;rttwait:RTT 1: open;wait:100"
    logged "^jlink|^config"
    check "attached without halting it, its RTT telnet port" \
        "jlink -device STM32H743ZI -if SWD -speed 4000 -nohalt -noir -nogui -select USB=000801234567 -RTTTelnetPort $p ~ config RTTCh;1"

    board "$STLINK, \"debug\": \"probe-rs\", \"rtt\": true"
    path="$B:$PATH" run "command:Open the board's RTT;rttwait:Board output: open;rttwait:kept 57;rttdump"
    check "probe-rs: its DAP server's RTT events, a tab for each channel, its semihosting as the board's output" \
        "RTT 0: Terminal: open rtt 0 events of fw kept 57 gaps 0 | Board output: open semihosting events of fw kept 29 gaps 0"
    path="$B:$PATH" run "command:Open the board's RTT;rttwait:Board output: open;wait:100"
    logged "^probe-rs|^dap (attach|rttWindowOpened)"
    got=$(printf '%s' "$got" | sed -E 's/"cwd": "[^"]*", //')
    check "attached with RTT on and no flashing, each window said open" \
        "probe-rs dap-server ~ dap attach {\"chip\": \"STM32F411CEUx\", \"coreConfigs\": [{\"coreIndex\": 0, \"programBinary\": \"@/fw/fw.elf\", \"rttEnabled\": true}], \"flashingConfig\": {\"flashingEnabled\": false}, \"probe\": \"0483:374e:004D00283133510F37363734\"} ~ dap rttWindowOpened {\"channelNumber\": 0, \"windowIsOpen\": true} ~ dap rttWindowOpened {\"channelNumber\": 1024, \"windowIsOpen\": true}"

    without "$dir/none" openocd openocd-esp32 probe-rs pyocd JLinkGDBServerCLExe JLinkGDBServer
    board "$STLINK, \"debug\": \"openocd\", \"rtt\": true"
    path="$dir/none" lines=20 run "command:Open the board's RTT;wait:1000;rttdump"
    got=$(printf '%s' "$got" | grep -o "blit: openocd is not installed on [^~]*" | head -1 | sed 's/ *$//')
    check "no OpenOCD: said, naming it and the machine" "blit: openocd is not installed on $this"
    board "$STLINK, \"debug\": \"probe-rs\", \"rtt\": true"
    path="$dir/none" lines=20 run "command:Open the board's RTT;wait:1000;rttdump"
    got=$(printf '%s' "$got" | grep -o "blit: probe-rs is not installed on [^~]*" | head -1 | sed 's/ *$//')
    check "no probe-rs: said" "blit: probe-rs is not installed on $this"

    # ---------------------------------------------------------------- a board on another machine
    # The project on another machine (tests/fake-ssh.sh standing in for ssh:
    # the "other machine" is this one, in a home of its own): the tool runs
    # there, and the Tcl port and each channel's server are reached through
    # blit's agent there (blit-agent --connect); the ELF is read here from a
    # copy fetched from there; pyOCD's config is written there.
    T="$dir/there"
    mkdir -p "$T" "$dir/run" && chmod 700 "$dir/run"
    # (A login there finds the stand-ins first: blit takes the PATH a login there has.)
    for f in .profile .zprofile .bash_profile; do printf 'PATH="%s:$PATH"\n' "$B" >"$T/$f"; done
    agent="$T/.cache/blit/[b]lit-agent"
    farrun() {
        export BLIT_SSH="$here/fake-ssh.sh" BLIT_FAKE_HOME="$T" BLIT_AGENT_RUN="$dir/run" BLIT_AGENT_IDLE=4
        run "folder:ssh://box$P;remotewait;frame;boarddump;$1"
        unset BLIT_SSH BLIT_FAKE_HOME BLIT_AGENT_RUN BLIT_AGENT_IDLE
    }
    board "$STLINK, \"debug\": \"openocd\", \"rtt\": { \"port\": $p }"
    path="$B:$PATH" farrun "command:Open the board's RTT;rttwait:driver fw done;rttwait:RTT 1: defmt: open;rttwait:kept 51;rttwait:kept 60;frame;rttdump"
    got=$(printf '%s' "$got" | sed -E 's/tries [0-9]+/tries N/')
    check "a board on another machine: OpenOCD there, its Tcl port and each channel reached through the agent" \
        "RTT 0: Terminal: open rtt 0 port $p of fw kept 60 gaps 0 opens 1 | RTT 1: defmt: open rtt 1 port $((p + 1)) of fw kept 51 gaps 0 opens 1 | driver fw done tries N opens 1"
    path="$B:$PATH" farrun "command:Open the board's RTT;rttwait:driver fw done;rttwait:RTT 1: defmt: open;wait:100"
    logged "^tcl rtt setup"
    check "the block's address from the ELF there, read here" "tcl rtt setup $rttaddr $rttsize {SEGGER RTT}"
    path="$B:$PATH" farrun "command:Open the board's RTT;rttwait:RTT 1: defmt: open;rttwait:kept 51;rttwait:kept 60;key:ctrl+\`;text:\`;frame;lua:os.execute('pgrep -f \"$agent.*--connect\" | wc -l | tr -d \" \" > $dir/pumps');termdump"
    got="$got | $(cat "$dir/pumps") pumps"
    check "its lines on the tab; the Tcl port and each channel a pump there" "1 defmt: line 1|1 defmt: line 2|1 defmt: line 3 @3,0 11x86 | 3 pumps"
    board "$STLINK, \"debug\": \"pyocd\", \"rtt\": { \"port\": $p, \"channels\": [0, 1] }"
    path="$B:$PATH" farrun "command:Open the board's RTT;rttwait:RTT 1: open;rttwait:kept 51;rttwait:kept 60;rttdump"
    check "pyOCD there: a server for each channel the board lists" \
        "RTT 0: open rtt 0 port $p of fw kept 60 gaps 0 opens 1 | RTT 1: open rtt 1 port $((p + 1)) of fw kept 51 gaps 0 opens 1"
    path="$B:$PATH" farrun "command:Open the board's RTT;rttwait:RTT 1: open;wait:100"
    logged "^pyocd"
    got=$(printf '%s' "$got" | sed -E "s#@/there/.cache/blit/rtt/[0-9]+-fw.yaml#~/.cache/blit/rtt/PID-fw.yaml#")
    check "its config written there, in the agent's cache" \
        "pyocd gdbserver -t stm32f411ceux -u 004D00283133510F37363734 -M attach --persist --config ~/.cache/blit/rtt/PID-fw.yaml"
    # (A machine without the tools: a login there whose PATH has none of them.)
    for f in .profile .zprofile .bash_profile; do printf 'PATH="%s"\n' "$dir/none" >"$T/$f"; done
    board "$STLINK, \"debug\": \"openocd\", \"rtt\": true"
    lines=20 farrun "command:Open the board's RTT;rttwait:driver fw;wait:1000;rttdump;wait:1000;rttdump;wait:1000;rttdump"  # (rttdump: what has come from there taken, as the window's loop would)
    got=$(printf '%s' "$got" | grep -o "blit: openocd is not installed on [^~]*" | head -1 | sed 's/ *$//')
    check "no OpenOCD there: said, naming that machine" "blit: openocd is not installed on box"
    board '"probe": {"kind": "jlink", "serial": "000801234567", "usb": "1366:0105"}, "chip": "STM32H743ZI", "elf": "fw.elf", "rtt": { "port": '$((base + 40))' }'
    farrun "command:Open the board's RTT;rttwait:RTT 0 (waiting);wait:500;key:ctrl+\`;text:\`;frame;termdump"
    got=${got% @*}
    check "a channel there that nothing serves: waited for, and why, through the agent" \
        "[waiting for 127.0.0.1:$((base + 40)): connection refused on box, trying again]"
    for f in .profile .zprofile .bash_profile; do printf 'PATH="%s:$PATH"\n' "$B" >"$T/$f"; done
    pkill -9 -f "$agent" 2>/dev/null

    # ---------------------------------------------------------------- with a debug session: what each preset is told
    plan() {
        board "$1"
        run "boardplan"
        got=$(printf '%s' "$got" | sed -E -e "s/tcl_port [0-9]+/tcl_port T/g" -e 's#@/home/[^ "]*/rtt/[0-9]+-fw#@/CACHE/fw#g' -e 's/ \| \{"program".*//' \
            -e 's/semihosting_redirect tcp [0-9]+/semihosting_redirect tcp FREE/' -e 's/-RTTTelnetPort [0-9]+/-RTTTelnetPort FREE/')
    }
    plan "$STLINK, \"debug\": \"openocd\", \"rtt\": true, \"semihosting\": true"
    check "openocd: its Tcl port open for RTT; semihosting on and to a port of its own, once attached" \
        "openocd | gdb: arm-none-eabi-gdb gdb-multiarch gdb:arm | target: | exec {openocd} -c 'gdb_port pipe' -c 'tcl_port T' -c 'telnet_port disabled' -f interface/stlink.cfg -c 'adapter serial 004D00283133510F37363734' -f target/stm32f4x.cfg -c '[target current] configure -event gdb-detach { resume }' | after: monitor reset halt; maintenance flush register-cache; monitor arm semihosting enable; monitor arm semihosting_redirect tcp FREE stdio | to main | flashed first"
    plan "$STLINK, \"debug\": \"pyocd\", \"rtt\": true, \"semihosting\": true"
    check "pyocd: its RTT config, semihosting on its telnet port" \
        "pyocd | gdb: arm-none-eabi-gdb gdb-multiarch gdb:arm | server: {pyocd} gdbserver -t stm32f411ceux -u 004D00283133510F37363734 -p 3333 -T 3334 --config @/CACHE/fw.yaml -S | target: 127.0.0.1:3333 | after: monitor reset halt; maintenance flush register-cache | to main | flashed first"
    plan '"probe": {"kind": "jlink", "serial": "000801234567", "usb": "1366:0105"}, "chip": "STM32H743ZI", "elf": "fw.elf", "rtt": true, "semihosting": true'
    check "jlink: its RTT telnet port, the block's address, semihosting to its telnet client" \
        "jlink | gdb: arm-none-eabi-gdb gdb-multiarch gdb:arm | server: {JLinkGDBServerCLExe|JLinkGDBServer} -device STM32H743ZI -if SWD -speed 4000 -port 2331 -swoport 2332 -telnetport 2333 -select 'USB=000801234567' -singlerun -nogui -RTTTelnetPort FREE | target: 127.0.0.1:2331 | after: monitor reset; maintenance flush register-cache; monitor exec SetRTTAddr $rttaddr; monitor semihosting enable; monitor semihosting IOClient 1 | to main | flashed first"
    plan "$STLINK"
    got=${got%% |*}
    check "an ST-LINK board names no preset: probe-rs" "probe-rs"
    plan "$STLINK, \"rtt\": true"
    got=$(printf '%s' "$got" | sed -E 's/ \| after:.*//')
    check "an ST-LINK board with RTT on, no preset named: openocd, which serves RTT beside gdb" \
        "openocd | gdb: arm-none-eabi-gdb gdb-multiarch gdb:arm | target: | exec {openocd} -c 'gdb_port pipe' -c 'tcl_port T' -c 'telnet_port disabled' -f interface/stlink.cfg -c 'adapter serial 004D00283133510F37363734' -f target/stm32f4x.cfg -c '[target current] configure -event gdb-detach { resume }'"
    plan "$STLINK, \"semihosting\": true"
    got=${got%% |*}
    check "and with semihosting on" "openocd"
    board "$STLINK, \"debug\": \"probe-rs\", \"rtt\": true"
    path="$B:$PATH" lines=20 run "command:Attach to the running board;wait:300"
    got=$(printf '%s' "$got" | grep -o "blit: probe-rs's gdb server has no RTT[^~]*" | head -1 | sed 's/ *$//')
    check "probe-rs named, RTT on: its gdb server has no RTT, and why, said as the session starts" \
        "blit: probe-rs's gdb server has no RTT, and one probe-rs at a time has the probe: RTT opens once the session ends (\"Open the board's RTT\"), or debug through the openocd preset"
fi

# ---------------------------------------------------------------- the real thing: QEMU, semihosting, defmt

if [ -z "$cc" ] || [ -z "${nm#nm}" ] || ! command -v qemu-system-arm >/dev/null 2>&1 || [ -z "$gdb" ]; then
    echo "SKIP: rtt.sh's QEMU cases need qemu-system-arm, arm-none-eabi-gcc and a gdb with Python for Arm (arm-none-eabi-gdb, gdb-multiarch or a gdb built for Arm)"
else
    Q="$dir/q"
    mkdir -p "$Q/.blit" "$Q/src"
    (cd "$Q" && cp "$here/rtt/semi.c" "$here/boards/cm.ld" . && "$cc" -mcpu=cortex-m3 -mthumb -g -O0 -nostdlib -T cm.ld -o semi.elf semi.c) || exit 1
    printf '{ "configurations": [ { "name": "fw", "program": "semi.elf", "board": { "chip": "mps2-an385", "elf": "semi.elf", "semihosting": true, "debug": { "preset": "qemu", "gdb": "%s" } } } ] }\n' "$gdb" >"$Q/.blit/run.json"
    qrun() {
        got=$(cd "$Q" && PATH="${path:-$PATH}" "$blit" --offscreen "${size:-1600x900}" --shot "$dir/shot.bmp" --script "workdir:$Q;$1" "$Q/${open:-semi.c}" 2>&1 >/dev/null |
            sed -E -e "s#$dir#@#g" -e 's/[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}/HH:MM:SS.mmm/g' | tail -n "${lines:-1}" | awk 'NR > 1 { printf " ~ " } { printf "%s", $0 }')
        size= path= lines= open=
    }
    size=3000x1400 qrun "key:F5;debugwait:stopped;key:F5;rttwait:kept 180;key:ctrl+\`;text:\`;frame;termdump"
    got=$(printf '%s' "${got% @*}" | sed -E 's/0x000000[0-9a-f]{2}/PC/g')
    check "QEMU: F5, the program's semihosting (SYS_WRITE0, SYS_WRITEC, SYS_WRITE to :tt) in the board's output tab, its crash decoded" \
        "semihosting: hello from the board|!|written to :tt|Guru Meditation Error: Core  0 panic'ed (LoadProhibited). Exception was unhandled.|Backtrace: PC:0x20000ff0|PC: length at @/q/semi.c:20|Rebooting..."
    size=3000x1200 qrun "key:F5;debugwait:stopped;key:F5;rttwait:kept 180;problemsdump"
    got=$(printf '%s' "$got" | sed -E 's/ [0-9]+:[0-9]+ Crash/ L:C Crash/')
    check "and its crash decoded, a problem" "button: ▶ fw | Board output 1 | file semi.c | >▸E L:C Crash at semi.c:20 in length (LoadProhibited)"
    qrun "key:F5;debugwait:stopped;key:F5;rttwait:kept 180;key:shift+F5;debugwait:idle;key:F5;debugwait:stopped;key:F5;rttwait:gaps 1;rttwait:kept 360;rttdump"
    check "F5 again: the same tab, the next run after a line marking the gap" "Board output: open semihosting file of fw kept 360 gaps 1"

    if [ -z "$defmt" ]; then
        echo "SKIP: rtt.sh's defmt cases need defmt-print (cargo install defmt-print)"
    else
        cp "$here/rtt/defmt-qemu.elf" "$Q/fw.elf"
        cp "$here/rtt/defmt-qemu/src/main.rs" "$Q/src/"
        printf '{ "configurations": [ { "name": "fw", "program": "fw.elf", "board": { "chip": "mps2-an385", "elf": "fw.elf", "semihosting": { "defmt": true }, "debug": { "preset": "qemu", "gdb": "%s" } } } ] }\n' "$gdb" >"$Q/.blit/run.json"
        go="key:F5;debugwait:stopped;key:F5;rttwait:records 6;key:ctrl+\`;text:\`;frame"
        size=2000x1400 open=src/main.rs qrun "$go;termdump"
        check "defmt over semihosting, through defmt-print: the target's time, the level, the message, its place" \
            "0.000000 TRACE clock tree set up  ./src/main.rs:22|0.000125 DEBUG adc offset -12  ./src/main.rs:23|0.000250 INFO  motor controller ready, 3 phases  ./src/main.rs:24|0.000375 WARN  supply at 3.05 V, below 3.2  ./src/main.rs:25|0.000500 ERROR over-current on phase 2: 1840 mA  ./src/main.rs:26|0.000625 done  ./src/main.rs:27 @6,0 11x89"
        lines=3 size=2000x1400 open=src/main.rs qrun "$go;text:4;frame;termdump;rttdump;text:1;frame;termdump"
        check "4: warn and up shown, none dropped; 1: all of them again" \
            "0.000375 WARN  supply at 3.05 V, below 3.2  ./src/main.rs:25|0.000500 ERROR over-current on phase 2: 1840 mA  ./src/main.rs:26|0.000625 done  ./src/main.rs:27 @3,0 11x89 ~ Board output: open semihosting file of fw kept 204 gaps 0 defmt from WARN records 6 ~ 0.000000 TRACE clock tree set up  ./src/main.rs:22|0.000125 DEBUG adc offset -12  ./src/main.rs:23|0.000250 INFO  motor controller ready, 3 phases  ./src/main.rs:24|0.000375 WARN  supply at 3.05 V, below 3.2  ./src/main.rs:25|0.000500 ERROR over-current on phase 2: 1840 mA  ./src/main.rs:26|0.000625 done  ./src/main.rs:27 @6,0 11x89"
        size=2000x1400 open=src/main.rs qrun "$go;termdimdump"
        got=$(printf '%s' "$got" | tr '|' '\n' | sed -E 's/.*  (\.\/src\/main\.rs:[0-9]+) \(part dim\)$/\1 dim/' | tr '\n' '|')
        check "each line part dim: its place" "./src/main.rs:22 dim|./src/main.rs:23 dim|./src/main.rs:24 dim|./src/main.rs:25 dim|./src/main.rs:26 dim|./src/main.rs:27 dim"
        size=2000x1400 open=src/main.rs qrun "$go;termclick:./src/main.rs:26;frame;statusdump"
        got=${got% LF *}
        check "Cmd+click on a place opens it" "src/main.rs | Ln 26, Col 1 Spaces: 4"
        without "$dir/nodefmt" defmt-print
        for t in qemu-system-arm "$gdb"; do ln -sf "$(command -v "$t" || echo "$t")" "$dir/nodefmt/"; done
        rm "$HOME/.cargo/bin/defmt-print"
        size=2400x1400 open=src/main.rs path="$dir/nodefmt" qrun "key:F5;debugwait:stopped;key:ctrl+\`;text:\`;frame;termdump"
        got=${got% @*}
        check "no defmt-print, on the PATH or in ~/.cargo/bin: said in the tab, and how to have it" "[defmt-print is not installed on $this (nor in ~/.cargo/bin: cargo install defmt-print)]"
    fi
fi

[ $failed = 0 ] && echo "rtt: all passed"
exit $failed
