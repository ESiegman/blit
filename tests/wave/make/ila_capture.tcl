# The captures in tests/wave, from a Basys 3 (xc7a35tcpg236-1) on this
# machine's USB: `vivado -mode batch -source ila_capture.tcl`, in the folder
# ila_build.tcl wrote ila_top.bit and ila_top.ltx in. It programs the FPGA's
# configuration memory (not the board's flash: a power cycle brings back what
# was there), arms the ILA twice and writes each capture in its VCD and CSV
# forms (and ila_one in the CSV form with no radix row, -legacy_csv_file):
#   ila_one:     one window of 1024 samples, the trigger (count = 0x40) at
#                sample 100; sum shown unsigned and sw_q in binary
#   ila_windows: four windows of 256 samples, each triggered where wrap
#                rises, at sample 32; count shown signed and phase in octal
# A hw_server it started itself it stops at the end (with its cs_server).
set ours [catch {close [socket localhost 3121]}]
open_hw_manager
connect_hw_server -allow_non_jtag
open_hw_target [lindex [get_hw_targets */xilinx_tcf/Digilent/*] 0]
set dev [lindex [get_hw_devices -filter {PART =~ xc7a35t*}] 0]
current_hw_device $dev
set_property PROGRAM.FILE ila_top.bit $dev
set_property PROBES.FILE ila_top.ltx $dev
set_property FULL_PROBES.FILE ila_top.ltx $dev
program_hw_devices $dev
refresh_hw_device $dev
set ila [lindex [get_hw_ilas -of_objects $dev] 0]
set_property DISPLAY_RADIX UNSIGNED [get_hw_probes sum -of_objects $ila]
set_property DISPLAY_RADIX BINARY [get_hw_probes sw_q -of_objects $ila]

proc capture {ila name} {
    run_hw_ila $ila
    wait_on_hw_ila -timeout 1 $ila
    set data [upload_hw_ila_data $ila]
    write_hw_ila_data -force -vcd_file $name.vcd $data
    write_hw_ila_data -force -csv_file $name.csv $data
    if {$name eq "ila_one"} { write_hw_ila_data -force -legacy_csv_file $name-legacy.csv $data }
}

set_property CONTROL.WINDOW_COUNT 1 $ila
set_property CONTROL.DATA_DEPTH 1024 $ila
set_property CONTROL.TRIGGER_POSITION 100 $ila
set_property TRIGGER_COMPARE_VALUE eq8'h40 [get_hw_probes count -of_objects $ila]
capture $ila ila_one

set_property CONTROL.WINDOW_COUNT 4 $ila
set_property CONTROL.DATA_DEPTH 256 $ila
set_property CONTROL.TRIGGER_POSITION 32 $ila
set_property TRIGGER_COMPARE_VALUE eq8'hxx [get_hw_probes count -of_objects $ila]
set_property TRIGGER_COMPARE_VALUE eq1'bR [get_hw_probes wrap -of_objects $ila]
set_property DISPLAY_RADIX SIGNED [get_hw_probes count -of_objects $ila]
set_property DISPLAY_RADIX OCTAL [get_hw_probes phase -of_objects $ila]
capture $ila ila_windows

close_hw_target
disconnect_hw_server
close_hw_manager
if {$ours} {
    catch {exec pkill -u $::env(USER) -x hw_server}
    catch {exec pkill -u $::env(USER) -x cs_server}
}
