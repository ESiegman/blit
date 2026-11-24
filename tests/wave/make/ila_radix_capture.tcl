# The ASCII and named-value captures in tests/wave, from a Basys 3
# (xc7a35tcpg236-1) on this machine's USB: `vivado -mode batch -source
# ila_radix_capture.tcl`, in the folder ila_radix_build.tcl wrote
# ila_radix.bit and ila_radix.ltx in. It programs the FPGA's configuration
# memory (not the board's flash), names state's values (add_hw_probe_enum:
# DISPLAY_RADIX has no ENUM, and a probe with names shows them while its
# DISPLAY_AS_ENUM is 1, as it is at first), arms the ILA twice and writes
# each capture in its VCD and CSV forms:
#   ila_radix:  one window of 1024 samples, the trigger (count = 0x40) at
#               sample 100; ch, pair and odd in ASCII, state named (in hex)
#   ila_radix2: two windows of 512, the trigger at sample 20 of each; count
#               in octal, ch in ASCII with a name for 'A' (AYE), pair
#               signed, odd unsigned with a name not shown (DISPLAY_AS_ENUM
#               0), state named in binary
# A hw_server it started itself it stops at the end (with its cs_server).
set ours [catch {close [socket localhost 3121]}]
open_hw_manager
connect_hw_server -allow_non_jtag
open_hw_target [lindex [get_hw_targets */xilinx_tcf/Digilent/*] 0]
set dev [lindex [get_hw_devices -filter {PART =~ xc7a35t*}] 0]
current_hw_device $dev
set_property PROGRAM.FILE ila_radix.bit $dev
set_property PROBES.FILE ila_radix.ltx $dev
set_property FULL_PROBES.FILE ila_radix.ltx $dev
program_hw_devices $dev
refresh_hw_device $dev
set ila [lindex [get_hw_ilas -of_objects $dev] 0]
proc probe {name} { return [get_hw_probes $name -of_objects $::ila] }

proc capture {ila name} {
    run_hw_ila $ila
    wait_on_hw_ila -timeout 1 $ila
    set data [upload_hw_ila_data $ila]
    write_hw_ila_data -force -vcd_file $name.vcd $data
    write_hw_ila_data -force -csv_file $name.csv $data
}

add_hw_probe_enum -dict {IDLE eq3'h0 LOAD eq3'h1 SHIFT eq3'h4 DONE eq3'h6} [probe state]
foreach p {ch pair odd} { set_property DISPLAY_RADIX ASCII [probe $p] }
set_property CONTROL.WINDOW_COUNT 1 $ila
set_property CONTROL.DATA_DEPTH 1024 $ila
set_property CONTROL.TRIGGER_POSITION 100 $ila
set_property TRIGGER_COMPARE_VALUE eq8'h40 [probe count]
capture $ila ila_radix

set_property DISPLAY_RADIX OCTAL [probe count]
add_hw_probe_enum AYE eq8'h41 [probe ch]
set_property DISPLAY_RADIX SIGNED [probe pair]
add_hw_probe_enum ZED eq12'hf7a [probe odd]
set_property DISPLAY_AS_ENUM 0 [probe odd]
set_property DISPLAY_RADIX UNSIGNED [probe odd]
set_property DISPLAY_RADIX BINARY [probe state]
set_property CONTROL.WINDOW_COUNT 2 $ila
set_property CONTROL.DATA_DEPTH 512 $ila
set_property CONTROL.TRIGGER_POSITION 20 $ila
capture $ila ila_radix2

close_hw_target
disconnect_hw_server
close_hw_manager
if {$ours} {
    catch {exec pkill -u $::env(USER) -x hw_server}
    catch {exec pkill -u $::env(USER) -x cs_server}
}
