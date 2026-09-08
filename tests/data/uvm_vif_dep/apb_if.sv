interface apb_if (
    input logic clk
);
    logic        psel;
    logic [31:0] paddr;

    modport mstr(output psel, output paddr, input clk);
endinterface
