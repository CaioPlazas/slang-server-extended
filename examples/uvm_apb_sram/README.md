# UVM/APB example workspace

A real, unmodified RTL + UVM testbench pairing, kept as a manual-testing workspace for
UVM-shaped code: classes living in their own files `` `include``d into packages, and
`virtual interface` members referencing interfaces declared in wholly separate files.

It was originally added to exercise `experimental.uvmVerificationLinting`, which was
**withdrawn in v0.5.5**. The workspace is still useful as a realistic testbench to point
the server at, so it stays -- but nothing here is a regression test, and the behaviour
described under "Why this pairing" is currently *not* resolved. See
`docs/features/limitations.md`.

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
`` `include``d by the driver or its owning package. That is the cross-file resolution
problem, in real, unmodified UVM code -- and it is currently unsolved.

## Trying it out

1. Open this folder (`examples/uvm_apb_sram`) in VS Code with the slang-server extension
   installed, or point a standalone `slang-server` at its `.slang/server.json`.
2. Open `tb/agents/apb_mstr_agent/apb_mstr_driver.sv` directly.

`virtual apb_interface` currently reports an unknown-interface diagnostic, and
goto-definition on `apb_interface` does not resolve. That is the known limitation above,
not a misconfiguration of this workspace.
