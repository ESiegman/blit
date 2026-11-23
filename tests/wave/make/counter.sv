`timescale 1ns/1ps
// A small design for the wave tests: a counter with a wrap flag, a
// nibble that starts x and goes through x and z, a bus that floats (z)
// while disabled, a real, a string, an integer, a 96-bit bus and an event.
module counter #(parameter int W = 8) (
    input  logic         clk,
    input  logic         rst,
    input  logic         en,
    output logic [W-1:0] count,
    output logic         wrap
);
    always_ff @(posedge clk)
        if (rst) count <= '0;
        else if (en) count <= count + 1'b1;
    assign wrap = &count;
endmodule

module tb;
    logic clk = 0;
    logic rst = 1;
    logic en = 0;
    logic [7:0] count;
    logic wrap;
    logic [3:0] nib;
    wire  [3:0] drv = en ? count[3:0] : 4'bz;
    logic [95:0] wide;
    real temp = 0.0;
    string word = "idle";
    integer steps = 0;
    event done;

    counter #(.W(8)) u_cnt (.clk(clk), .rst(rst), .en(en), .count(count), .wrap(wrap));

    always #5 clk = ~clk;

    initial begin
`ifdef DUMPVARS
        $dumpfile(`DUMPVARS);
        $dumpvars(0, tb);
`endif
        #12 rst = 0; en = 1; word = "run";
        #20 nib = 4'b1x0z; temp = 1.5; steps = 3; wide = {4'b10xz, 92'h0};
        #30 nib = 4'hc; temp = -2.25e-3; wide = 96'hdead_beef_0123_4567_89ab_cdef;
`ifdef DUMPOFF
        #10 $dumpoff;
        #25 $dumpon;
`endif
        #40 en = 0; word = "hold"; steps = -7;
        #20 en = 1; temp = 6.02e23; nib = 4'bzzzz;
        #2600 -> done; word = "done";
        #15 $finish;
    end
endmodule
