#!/bin/sh
# Makes the dumps in tests/wave again, in the folder it is run in, on a
# machine with the tools (they were made on Arch Linux, Nov 2026, with
# Icarus Verilog 13.0, Verilator 5.054, Yosys 0.71+77 and pyvcd 0.5.1 under
# Python 3.14). Vivado's xsim (xsim.tcl) is for when it runs there.
set -e
here=$(cd "$(dirname "$0")" && pwd)

iverilog -g2012 '-DDUMPVARS="icarus.vcd"' -DDUMPOFF -o tb.vvp "$here/counter.sv"
vvp -n tb.vvp

verilator --binary --trace -Wno-fatal '-DDUMPVARS="verilator.vcd"' -DDUMPOFF --top-module tb "$here/counter.sv"
./obj_dir/Vtb

sed -n '/^module counter/,/^endmodule/p' "$here/counter.v" >counter_rtl.v
yosys -q -p "read_verilog counter_rtl.v; prep -top counter; sim -clock clk -reset rst -n 24 -vcd yosys.vcd"

python3 -m venv venv
./venv/bin/pip -q install pyvcd==0.5.1
./venv/bin/python "$here/pyvcd_uart.py" pyvcd.vcd

if command -v xvlog >/dev/null; then
    xvlog "$here/counter.v"
    xelab tb -debug all -s tb
    xsim tb -tclbatch "$here/xsim.tcl"
fi
