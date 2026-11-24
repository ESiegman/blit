// A small design for the ILA captures in tests/wave whose probes Vivado
// shows as ASCII and by the names of their values (ENUM): a counter on the
// Basys 3's 100 MHz clock steps through sixteen bytes of text, some of them
// not printable, one a comma and one a double quote; `ch` is the byte, `pair`
// it and the next, `odd` the byte under four bits of the counter (twelve
// bits, not a whole number of bytes); `state` a SystemVerilog enum decoded
// from the counter, which also takes a value with no name (7). An ILA probes
// them (the ILA IP, made by ila_radix_build.tcl), and count beside them.
module ila_radix (
    input  wire        clk,
    input  wire        btnC,
    input  wire [15:0] sw,
    output wire [15:0] led
);
    typedef enum logic [2:0] {IDLE = 3'd0, LOAD = 3'd1, SHIFT = 3'd4, DONE = 3'd6} state_t;

    function automatic [7:0] msg(input [3:0] i);
        case (i)
            4'd0: msg = "H";   4'd1: msg = "i";   4'd2: msg = ",";   4'd3: msg = " ";
            4'd4: msg = "\"";  4'd5: msg = "I";   4'd6: msg = "L";   4'd7: msg = "A";
            4'd8: msg = "\"";  4'd9: msg = 8'h0a; 4'd10: msg = 8'h00; 4'd11: msg = "~";
            4'd12: msg = 8'h7f; 4'd13: msg = 8'hff; 4'd14: msg = 8'h09; default: msg = "z";
        endcase
    endfunction

    (* mark_debug = "true" *) reg [7:0]  count = 8'd0;
    (* mark_debug = "true" *) reg [7:0]  ch = 8'd0;
    (* mark_debug = "true" *) reg [15:0] pair = 16'd0;
    (* mark_debug = "true" *) reg [11:0] odd = 12'd0;
    (* mark_debug = "true" *) state_t    state = IDLE;

    always @(posedge clk) begin
        count <= btnC ? 8'd0 : count + 8'd1;
        ch <= msg(count[3:0]);
        pair <= {msg(count[3:0]), msg(count[3:0] + 4'd1)};
        odd <= {count[3:0], msg(count[3:0])};
        case (count[5:3])
            3'd0, 3'd1: state <= IDLE;
            3'd2: state <= LOAD;
            3'd3, 3'd4, 3'd5: state <= SHIFT;
            3'd6: state <= DONE;
            default: state <= state_t'(3'd7);
        endcase
    end

    ila_0 u_ila (.clk(clk), .probe0(count), .probe1(ch), .probe2(pair), .probe3(odd), .probe4(state));

    assign led = {state, sw[4:0], ch};
endmodule
