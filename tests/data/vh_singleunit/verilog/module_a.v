module module_a (
    input wire clk,
    input wire rst_n,
    input wire din,
    input wire [1:0] dout_cfg,
    output wire dout
);

wire dout_custom;

assign dout = dout_custom;

`include "custom_module_a.vh"

module_b module_b_inst (
    .clk     (clk),
    .rst_n   (rst_n),
    .din     (din),
    .dout_cfg(dout_cfg),
    .dout_b  (dout_b)
);

endmodule
