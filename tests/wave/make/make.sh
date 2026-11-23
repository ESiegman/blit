#!/bin/sh
# Makes the dumps in tests/wave again, in the folder it is run in, on a
# machine with the tools (they were made on Arch Linux, Nov 2026, with
# Icarus Verilog 13.0, Verilator 5.054, Yosys 0.71+77 and pyvcd 0.5.1 under
# Python 3.14). Each simulator's FST is the same run as its VCD.
set -e
here=$(cd "$(dirname "$0")" && pwd)

iverilog -g2012 '-DDUMPVARS="icarus.vcd"' -DDUMPOFF -o tb.vvp "$here/counter.sv"
vvp -n tb.vvp
iverilog -g2012 '-DDUMPVARS="icarus.fst"' -DDUMPOFF -o tb-fst.vvp "$here/counter.sv"
vvp -n tb-fst.vvp -fst

verilator --binary --trace -Wno-fatal '-DDUMPVARS="verilator.vcd"' -DDUMPOFF --top-module tb "$here/counter.sv"
./obj_dir/Vtb
verilator --binary --trace-fst -Wno-fatal '-DDUMPVARS="verilator.fst"' -DDUMPOFF --top-module tb --Mdir obj_fst "$here/counter.sv"
./obj_fst/Vtb

sed -n '/^module counter/,/^endmodule/p' "$here/counter.v" >counter_rtl.v
yosys -q -p "read_verilog counter_rtl.v; prep -top counter; sim -clock clk -reset rst -n 24 -vcd yosys.vcd"

python3 -m venv venv
./venv/bin/pip -q install pyvcd==0.5.1
./venv/bin/python "$here/pyvcd_uart.py" pyvcd.vcd

# Icarus's run again (icarus.txt): built without DUMPOFF, and with the
# testbench dumping its own scope only. (The FSTs still being written were
# caught by hand: icarus.txt and verilator.txt say how.)
iverilog -g2012 '-DDUMPVARS="icarus-rerun.vcd"' -o tb-rerun.vvp "$here/counter.sv"
vvp -n tb-rerun.vvp
sed 's/$dumpvars(0, tb)/$dumpvars(1, tb)/' "$here/counter.sv" >counter-top.sv
iverilog -g2012 '-DDUMPVARS="icarus-top.vcd"' -DDUMPOFF -o tb-top.vvp counter-top.sv
vvp -n tb-top.vvp

# SymbiYosys's traces (sby.txt), with sby and z3 on the PATH (it fails, as
# its bmc task finds the assertion's counterexample).
cp "$here/formal/counter.v" "$here/formal/counter.sby" .
sby -f counter.sby || true
cp counter_bmc/engine_0/trace.vcd sby-bmc.vcd
cp counter_cover/engine_0/trace0.vcd sby-cover.vcd
