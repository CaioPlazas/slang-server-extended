`include "dual_purpose.v"

module top_dual_owner (
    input wire clk,
    output wire dout
);

dual_purpose u_dual (
    .clk(clk),
    .dout(dout)
);

endmodule
