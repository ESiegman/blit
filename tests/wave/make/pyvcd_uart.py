# A UART transmitter's signals written with pyvcd: string state names, a
# real line voltage, an event per byte, a bus with x and z, scopes of
# several kinds, $dumpoff and $dumpon, comments.
import sys
from collections import defaultdict
from vcd import VCDWriter
from vcd.common import ScopeType

out = open(sys.argv[1], "w")
with VCDWriter(out, timescale="10 ns", date="today", version="pyvcd 0.5.1", comment="uart model") as w:
    clk = w.register_var("top", "clk", "wire", size=1, init=0)
    tx = w.register_var("top.uart", "tx", "reg", size=1, init=1)
    state = w.register_var("top.uart", "state", "string", init="IDLE")
    volts = w.register_var("top.uart", "volts", "real", init=3.3)
    sent = w.register_var("top.uart", "sent", "event")
    data = w.register_var("top.uart.fifo", "data", "reg", size=16, init="z")
    level = w.register_var("top.uart.fifo", "level", "integer", size=8, init=0)
    w.register_alias("top.uart.fifo", "bus_alias", data)
    w.set_scope_type("top.uart.fifo", ScopeType.fork)
    w.set_scope_type("top.uart", ScopeType.task)
    plan = defaultdict(list)
    t = 2
    off = None
    for byte in (0x55, 0xA3):
        plan[t] += [(data, byte), (level, 1), (state, "START"), (tx, 0), (volts, 0.12)]
        for i in range(8):
            t += 4
            bit = (byte >> i) & 1
            plan[t] += [(tx, bit), (volts, 3.29 if bit else 0.11), (state, "DATA%d" % i)]
        t += 4
        plan[t] += [(state, "STOP"), (tx, 1), (volts, 3.3), (level, 0), (data, "x"), (sent, True)]
        t += 4
        plan[t] += [(state, "IDLE")]
        if off is None:
            off = (t + 2, t + 9)
        t += 10
    for c in range(1, t + 2):
        plan[c].append((clk, c & 1))
    for when in sorted(plan):
        if when == off[0]:
            w.dump_off(when)
        if when == off[1]:
            w.dump_on(when)
        if off[0] <= when < off[1]:
            continue
        for var, value in plan[when]:
            w.change(var, when, value)
out.close()
