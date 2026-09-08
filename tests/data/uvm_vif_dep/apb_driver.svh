class apb_driver;
    apb_pkg_addr_t last_addr;
    virtual apb_if vif;

    function void drive();
        vif.psel = 1'b1;
    endfunction
endclass
