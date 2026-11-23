`timescale 1ns/1ps
// The same design as counter.sv in plain Verilog-2001 (no string), for
// simulators that take Verilog only.
module counter #(parameter W = 8) (
    input              clk,
    input              rst,
    input              en,
    output reg [W-1:0] count,
    output             wrap
);
    always @(posedge clk)
        if (rst) count <= 0;
        else if (en) count <= count + 1'b1;
    assign wrap = &count;
endmodule

module tb;
    reg clk = 0;
    reg rst = 1;
    reg en = 0;
    wire [7:0] count;
    wire wrap;
    reg [3:0] nib;
    wire [3:0] drv = en ? count[3:0] : 4'bz;
    reg [95:0] wide;
    real temp = 0.0;
    integer steps = 0;
    event done;

    counter #(.W(8)) u_cnt (.clk(clk), .rst(rst), .en(en), .count(count), .wrap(wrap));

    always #5 clk = ~clk;

    initial begin
        #12 rst = 0; en = 1;
        #20 nib = 4'b1x0z; temp = 1.5; steps = 3; wide = {4'b10xz, 92'h0};
        #30 nib = 4'hc; temp = -2.25e-3; wide = 96'hdead_beef_0123_4567_89ab_cdef;
        #40 en = 0; steps = -7;
        #20 en = 1; temp = 6.02e23; nib = 4'bzzzz;
        #2600 -> done;
        #15 $finish;
    end
endmodule
