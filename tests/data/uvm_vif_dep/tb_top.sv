module tb_top;
    typedef logic [31:0] apb_pkg_addr_t;

`include "apb_driver.svh"

    apb_driver drv;

    initial begin
        drv = new();
        drv.drive();
    end
endmodule
