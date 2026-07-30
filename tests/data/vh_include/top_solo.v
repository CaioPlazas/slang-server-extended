module top_solo (
    input wire clk,
    input wire din,
    output wire dout
);

wire dout_custom;
assign dout = dout_custom;

`include "solo_body.vh"

endmodule
