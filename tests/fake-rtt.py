#!/usr/bin/env python3
# Stand-ins for the tools that serve a board's RTT channels and semihosting
# output over TCP (tests/rtt.sh), shaped as each tool's own:
#
#   fake-rtt.py openocd ARGS...    OpenOCD 0.12 with its Tcl server (-c 'tcl_port N'):
#       commands end at 0x1a, each answer is the result then 0x1a
#       (src/server/tcl_server.c); `rtt setup ADDR SIZE ID`, `rtt start`
#       (the block is "found" on the START-th try, $FAKE_RTT_FOUND_AT, 3 by
#       default: the program has made it by then), `rtt channels` in
#       src/rtt/tcl.c's shape, `rtt server start PORT CH`: a TCP server for
#       the channel, whose input goes back out as "down CH: TEXT"
#       (OpenOCD writes it to the down-channel; the firmware here echoes).
#   fake-rtt.py jlink ARGS...      SEGGER's GDB server's RTT telnet port
#       (-RTTTelnetPort P): channel 0, or the one a
#       "$$SEGGER_TELNET_ConfigStr=RTTCh;N$$" sent in the first 100 ms asks for.
#   fake-rtt.py pyocd ARGS...      pyOCD's gdb server with an `rtt` config
#       (--config FILE): a TCP server per channel listed; -S: semihosting
#       output on the telnet port (-T).
#   fake-rtt.py tcp PORT           a channel's server alone.
#
# Each channel sends "CH NAME: line K" every 100 ms ($FAKE_RTT_LINES lines,
# 3 by default), then on channel 0 $FAKE_RTT_THEN if set (a crash dump's
# text, with \n), once to each client; what a client sends comes back as "down CH: ...".
# $FAKE_RTT_TAG, if set, goes before each line (which board's it is).
# A channel named "defmt" sends the bytes of $FAKE_RTT_FRAMES instead, if set
# (defmt's frames, as tests/rtt/defmt-qemu.elf wrote them on QEMU).
# $FAKE_RTT_LOG, if set, gets a line for each command, connection and
# config string, for the tests to read.
import os
import re
import select
import socket
import sys
import threading
import time

LOG = os.environ.get("FAKE_RTT_LOG")
NAMES = {0: "Terminal", 1: "defmt", 2: "Trace"}
LINES = int(os.environ.get("FAKE_RTT_LINES", "3"))
THEN = os.environ.get("FAKE_RTT_THEN", "").replace("\\n", "\n")
FOUND_AT = int(os.environ.get("FAKE_RTT_FOUND_AT", "3"))
UP = int(os.environ.get("FAKE_RTT_UP", "2"))
FRAMES = os.environ.get("FAKE_RTT_FRAMES")
TAG = os.environ.get("FAKE_RTT_TAG", "").encode()  # (before each line: which board's it is)


def log(text):
    if LOG:
        with open(LOG, "a") as f:
            f.write(text + "\n")


def listen(port):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", port))
    s.listen(4)
    return s


def channel_client(c, ch, choose=False):
    # (J-Link: 100 ms for a config string choosing another channel.)
    if choose:
        c.setblocking(False)
        got = b""
        end = time.time() + 0.1
        while time.time() < end:
            r, _, _ = select.select([c], [], [], 0.02)
            if r:
                d = c.recv(256)
                if not d:
                    break
                got += d
        m = re.search(rb"\$\$SEGGER_TELNET_ConfigStr=RTTCh;(\d+)\$\$", got)
        if m:
            ch = int(m.group(1))
            log("config RTTCh;%d" % ch)
        c.setblocking(True)
    name = NAMES.get(ch, "ch%d" % ch)

    def reader():  # (a line at a time: what was typed, up to its Return)
        got = b""
        while True:
            try:
                d = c.recv(4096)
            except OSError:
                return
            if not d:
                return
            got += d
            while b"\n" in got or b"\r" in got:
                cut = min(i for i in (got.find(b"\n"), got.find(b"\r")) if i >= 0)
                line, got = got[:cut + 1], got[cut + 1:]
                log("down %d: %r" % (ch, line))
                try:
                    c.sendall(b"down %d: %s\r\n" % (ch, line.strip()))
                except OSError:
                    return

    threading.Thread(target=reader, daemon=True).start()
    try:
        if FRAMES and name == "defmt":  # (a defmt channel's frames, as a firmware sent them)
            c.sendall(open(FRAMES, "rb").read())
            while True:
                time.sleep(1)
        for k in range(1, LINES + 1):
            c.sendall(b"%s%d %s: line %d\r\n" % (TAG + b" " if TAG else b"", ch, name.encode(), k))
            time.sleep(0.1)
        if THEN and ch == 0:
            c.sendall(THEN.encode())
        while True:
            time.sleep(1)
    except OSError:
        pass


def channel_server(port, ch, choose=False):
    s = listen(port)
    log("rtt server %d channel %d" % (port, ch))

    def loop():
        while True:
            c, _ = s.accept()
            log("connected %d" % port)
            threading.Thread(target=channel_client, args=(c, ch, choose), daemon=True).start()

    threading.Thread(target=loop, daemon=True).start()


def arg_after(argv, flag):
    for i, a in enumerate(argv[:-1]):
        if a == flag:
            return argv[i + 1]
    return None


def openocd(argv):
    port = None
    for i, a in enumerate(argv):
        m = re.match(r"tcl_port (\d+)", a)
        if m:
            port = int(m.group(1))
    if port is None:
        sys.exit("fake openocd: no tcl_port")
    log("openocd " + " ".join(argv))
    s = listen(port)
    state = {"tries": 0, "setup": None}

    def answer(cmd):
        log("tcl " + cmd)
        w = cmd.split()
        if w[:2] == ["rtt", "setup"]:
            state["setup"] = w[2]
            return ""
        if w[:2] == ["rtt", "start"]:
            state["tries"] += 1
            return "" if state["tries"] >= FOUND_AT else ""
        if w[:2] == ["rtt", "channels"]:
            if state["tries"] < FOUND_AT or not state["setup"]:
                return "rtt: Control block not available\n"
            out = "Channels: up=%d, down=1\nUp-channels:\n" % UP
            for k in range(UP):
                out += "%d: %s 1024 2\n" % (k, NAMES.get(k, "ch%d" % k))
            return out + "Down-channels:\n0: Terminal 16 0\n"
        if w[:3] == ["rtt", "server", "start"]:
            try:
                channel_server(int(w[3]), int(w[4]))
            except OSError:  # (another program has the port: OpenOCD's add_service fails, src/server/server.c)
                log("rtt server %s busy" % w[3])
                return "couldn't bind rtt to socket on port %s: Address already in use" % w[3]
            return ""
        return "invalid command name \"%s\"" % w[0]

    while True:
        c, _ = s.accept()
        log("tcl connected")
        buf = b""
        while True:
            d = c.recv(4096)
            if not d:
                break
            buf += d
            while b"\x1a" in buf:
                cmd, buf = buf.split(b"\x1a", 1)
                c.sendall(answer(cmd.decode()).encode() + b"\x1a")


def jlink(argv):
    port = int(arg_after(argv, "-RTTTelnetPort") or 19021)
    log("jlink " + " ".join(argv))
    channel_server(port, 0, choose=True)
    while True:
        time.sleep(1)


def pyocd(argv):
    log("pyocd " + " ".join(argv))
    cfg = arg_after(argv, "--config")
    if cfg:
        text = open(cfg).read()
        log("config " + " ".join(text.split()))
        for num, port in re.findall(r"number: (\d+)\s+mode: server\s+port: (\d+)", text):
            channel_server(int(port), int(num))
    if "-S" in argv:
        semi = int(arg_after(argv, "-T") or 4444)
        s = listen(semi)

        def loop():
            while True:
                c, _ = s.accept()
                log("semihosting connected")
                c.sendall(b"hello from semihosting\r\n")

        threading.Thread(target=loop, daemon=True).start()
    while True:
        time.sleep(1)


def probe_rs(argv):
    # probe-rs 0.32's DAP server on stdio (cmd/dap_server): Content-Length
    # framed JSON; on attach with rttEnabled, a probe-rs-rtt-channel-config
    # event per up-channel (channelNumber, channelName, dataFormat) and its
    # data as probe-rs-rtt-data events only once rttWindowOpened says the
    # client's window is open (server/debugger.rs, core_data.rs); a
    # semihosting stream as a channel numbered from 1024 named "stdout".
    log("probe-rs " + " ".join(argv))
    import json
    out = sys.stdout.buffer
    seq = [1]

    def send(obj):
        obj["seq"] = seq[0]
        seq[0] += 1
        body = json.dumps(obj).encode()
        out.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        out.flush()

    def event(name, body):
        send({"type": "event", "event": name, "body": body})

    inp = sys.stdin.buffer
    opened = set()
    while True:
        line = inp.readline()
        if not line:
            return
        if not line.lower().startswith(b"content-length:"):
            continue
        n = int(line.split(b":")[1])
        while inp.readline() not in (b"\r\n", b"\n", b""):
            pass
        msg = json.loads(inp.read(n))
        cmd = msg.get("command")
        log("dap " + cmd + " " + json.dumps(msg.get("arguments", {}), sort_keys=True))
        send({"type": "response", "request_seq": msg["seq"], "command": cmd, "success": True, "body": {}})
        if cmd == "initialize":
            event("initialized", {})
        elif cmd == "configurationDone":
            event("probe-rs-rtt-channel-config", {"channelNumber": 0, "channelName": "Terminal", "dataFormat": "String"})
            event("probe-rs-rtt-channel-config", {"channelNumber": 1024, "channelName": "stdout", "dataFormat": "String"})
        elif cmd == "rttWindowOpened":
            ch = msg["arguments"]["channelNumber"]
            if ch not in opened:
                opened.add(ch)
                if ch == 0:
                    for k in range(1, LINES + 1):
                        event("probe-rs-rtt-data", {"channelNumber": 0, "data": "0 Terminal: line %d\n" % k})
                else:
                    event("probe-rs-rtt-data", {"channelNumber": ch, "data": "semihosting through probe-rs\n"})


def main():
    kind, argv = sys.argv[1], sys.argv[2:]
    if kind == "probe-rs":
        probe_rs(argv)
        return
    if kind == "openocd":
        openocd(argv)
    elif kind == "jlink":
        jlink(argv)
    elif kind == "pyocd":
        pyocd(argv)
    elif kind == "tcp":
        channel_server(int(argv[0]), int(argv[1]) if len(argv) > 1 else 0)
        while True:
            time.sleep(1)


main()
