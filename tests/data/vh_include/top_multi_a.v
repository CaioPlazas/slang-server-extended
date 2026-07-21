module top_multi_a (
    input wire clk,
    input wire din,
    output wire dout
);

wire dout_custom;
assign dout = dout_custom;

`include "multi_body.vh"

endmodule
