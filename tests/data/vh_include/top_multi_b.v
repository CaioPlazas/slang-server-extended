module top_multi_b (
    input wire clk,
    input wire different_input,
    output wire dout
);

wire dout_custom;
assign dout = dout_custom;

`include "multi_body.vh"

endmodule
