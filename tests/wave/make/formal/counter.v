module counter (
    input            clk,
    input            rst,
    input            en,
    output reg [3:0] count
);
    always @(posedge clk)
        if (rst) count <= 0;
        else if (en) count <= count + 1'b1;
`ifdef FORMAL
    reg past = 0;
    always @(posedge clk) past <= 1;
    always @(*) if (!past) assume(rst);
    always @(posedge clk) if (past) assert(count != 4'd5);
    always @(posedge clk) cover(count == 4'd3);
`endif
endmodule
