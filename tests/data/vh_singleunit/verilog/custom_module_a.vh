assign dout_custom = (dout_cfg == 2'b00) ? din :
                     (dout_cfg == 2'b01) ? ~din :
                     (dout_cfg == 2'b10) ? din & clk :
                     (dout_cfg == 2'b11) ? din | clk : 1'b0;

wire dout_b;
