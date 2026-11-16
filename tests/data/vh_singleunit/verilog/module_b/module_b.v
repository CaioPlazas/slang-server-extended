module module_b (
    input wire clk,
    input wire rst_n,
    input wire din,
    input wire [1:0] dout_cfg,
    output wire dout_b
);

wire dout_custom;

`include "custom_module_b.vh"

assign dout_b = dout_c;

endmodule
