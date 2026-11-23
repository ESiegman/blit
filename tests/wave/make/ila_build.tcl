# ila_top.v built with the ILA IP on its signals (five probes of 1024
# samples, as Vivado's Basic licence allows), in Vivado's batch mode with
# no project: `vivado -mode batch -source ila_build.tcl`, in a folder of
# its own. Writes ila_top.bit and ila_top.ltx there.
set here [file dirname [file normalize [info script]]]
set part xc7a35tcpg236-1
set_part $part
file mkdir ip
create_ip -name ila -vendor xilinx.com -library ip -module_name ila_0 -dir ip
set_property -dict [list CONFIG.C_NUM_OF_PROBES 5 CONFIG.C_DATA_DEPTH 1024 \
    CONFIG.C_PROBE0_WIDTH 8 CONFIG.C_PROBE1_WIDTH 1 CONFIG.C_PROBE2_WIDTH 2 \
    CONFIG.C_PROBE3_WIDTH 12 CONFIG.C_PROBE4_WIDTH 4] [get_ips ila_0]
generate_target all [get_ips ila_0]
synth_ip [get_ips ila_0]
read_verilog $here/ila_top.v
read_xdc $here/basys3.xdc
synth_design -top ila_top -part $part
opt_design
place_design
route_design
write_bitstream -force ila_top.bit
write_debug_probes -force ila_top.ltx
