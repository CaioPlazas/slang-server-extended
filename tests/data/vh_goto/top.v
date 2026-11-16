module top (
    input wire clk,
    output wire dout
);

wire dout_custom;
assign dout = dout_custom;

`include "body.vh"

endmodule
