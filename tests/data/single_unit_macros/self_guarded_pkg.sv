// Defines its own include guard, so the guard lands in the pool. Without the self-file filter in
// collectInheritedMacros the guard gets predefined for THIS file's own parse, the `ifndef goes
// false, and the package below collapses into an inactive region.
`ifndef SELF_GUARDED_PKG_SV
`define SELF_GUARDED_PKG_SV

package self_guarded_pkg;
  class self_guarded_class;
    int x;
  endclass
endpackage

`endif
