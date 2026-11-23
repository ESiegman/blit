// A small design for the ILA captures in tests/wave: a counter on the
// Basys 3's 100 MHz clock, a flag the cycle after it is 0xfe, a two-bit
// phase that steps each time it wraps, a 12-bit sum of the counter and
// eight of the switches, and four switches as they are set. An ILA probes
// them: the ILA IP (Vivado's Basic licence has no create_debug_core), which
// ila_build.tcl makes.
module ila_top (
    input  wire        clk,
    input  wire        btnC,
    input  wire [15:0] sw,
    output wire [15:0] led
);
    (* mark_debug = "true" *) reg  [7:0]  count = 8'd0;
    (* mark_debug = "true" *) reg         wrap = 1'b0;
    (* mark_debug = "true" *) reg  [1:0]  phase = 2'd0;
    (* mark_debug = "true" *) reg  [11:0] sum = 12'd0;
    (* mark_debug = "true" *) reg  [3:0]  sw_q = 4'd0;

    always @(posedge clk) begin
        if (btnC) begin
            count <= 8'd0;
            phase <= 2'd0;
        end else begin
            count <= count + 8'd1;
            if (count == 8'hff) phase <= phase + 2'd1;
        end
        wrap <= count == 8'hfe;
        sum <= {4'd0, count} + {sw[15:8], 4'd0};
        sw_q <= sw[3:0];
    end

    ila_0 u_ila (.clk(clk), .probe0(count), .probe1(wrap), .probe2(phase), .probe3(sum), .probe4(sw_q));

    assign led = {phase, wrap, sw_q, 1'b0, count};
endmodule
