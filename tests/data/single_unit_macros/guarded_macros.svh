// Include-guarded macro header, the shape real UVM ships (uvm_macros.svh). The guard is what
// broke the original pool implementation: once GUARDED_MACROS_SVH was pooled, this file's own
// `ifndef went false and the body below stopped being seen.
`ifndef GUARDED_MACROS_SVH
`define GUARDED_MACROS_SVH

`define GUARDED_UTILS(T) \
  function string get_guarded_name(); \
    return "T"; \
  endfunction

`endif
