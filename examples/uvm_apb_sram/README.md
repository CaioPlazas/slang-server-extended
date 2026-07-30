# UVM/APB example workspace

A real, unmodified RTL + UVM testbench pairing for manually exercising the
`experimental.uvmVerificationLinting` feature end-to-end (cross-file resolution of
`virtual interface` member types and class `` `include``s inside a package).

## Sources

- **`rtl/`, `tb/`** — [courageheart/AMBA_APB_SRAM](https://github.com/courageheart/AMBA_APB_SRAM)
  (MIT License), vendored unmodified at commit `790f42549185dcdf5d4f303e7c438c98935d39ab`.
  See `LICENSE-AMBA_APB_SRAM`.
- **`uvm/src/`** — [accellera-official/uvm-core](https://github.com/accellera-official/uvm-core)
  (Apache License 2.0), `src/` only, vendored unmodified at commit
  `78c06547a2a0a29b3dc9dcafae62b75b2ff61544`. See `LICENSE-uvm-core` and `NOTICE-uvm-core`.

Neither project is modified from its upstream state; files are copied verbatim (no
submodule — this is a manual-testing fixture, not a build dependency).

## Why this pairing

`tb/agents/apb_mstr_agent/apb_agent_pkg.sv` is a `package` that `` `include``s
`apb_mstr_driver.sv`, and that file's `apb_master_driver` class declares
`virtual apb_interface apb_intf;` — a virtual-interface member referencing `apb_interface`,
which is declared in a wholly separate file, `tb/tb_top/apb_interface.sv`, never
`` `include``d by the driver or its owning package. This is exactly the cross-file
resolution scenario `uvmVerificationLinting` addresses, in real, unmodified UVM code.

## Trying it out

1. Open this folder (`examples/uvm_apb_sram`) in VS Code with the slang-server extension
   installed, or point a standalone `slang-server` at its `.slang/server.json`.
2. Open `tb/agents/apb_mstr_agent/apb_mstr_driver.sv` directly.
3. With `experimental.uvmVerificationLinting: true` (already set in `.slang/server.json`),
   confirm there's no bogus "unknown interface" diagnostic on `virtual apb_interface`, and
   that goto-definition on `apb_interface` resolves to `tb/tb_top/apb_interface.sv`.
4. Toggle the flag off (or use the VS Code config panel's checkbox) to see the diagnostic
   reappear — confirming this is real resolution, not something that always worked.
