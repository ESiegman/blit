`timescale 1ns/1ps
// Times as testbenches print them, for the tests of Cmd+click on a time in
// a terminal: %0t after an @, in brackets, %t padded, $timeformat's units
// with and without a space, a time after "Time:", $error's and $finish's
// own lines. Run by Icarus and by Verilator (make.sh); what each printed is
// in tests/wave/times-*.log. With DUMPVARS it also writes a dump to click
// the times into.
module times;
    reg [7:0] count = 0;
    always #5 count = count + 1;

    initial begin
`ifdef DUMPVARS
        $dumpfile(`DUMPVARS);
        $dumpvars(0, times);
`endif
        #50 $display("@ %0t: count = %0d", $time, count);
        #12.5 $display("[%0t] count = %0d", $realtime, count);
        #10 $display("%t count = %0d", $time, count);
        $timeformat(-9, 2, " ns", 10);
        #20 $display("%t ERROR: count = %0d", $time, count);
        #1 $display("at %t the count is %0d", $time, count);
        $timeformat(-6, 4, "us", 0);
        #30 $display("%t ERROR", $time);
        #5 $display("Time: %0t", $time);
        $error("count is %0d", count);
        #3 $finish;
    end
endmodule
