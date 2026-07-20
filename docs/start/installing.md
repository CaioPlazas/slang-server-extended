# Installing

### Vscode

This fork is published on the VS Code Marketplace as **Slang Server Extended** (`CaioPlazas.slang-server-extended`) — search "Slang Server Extended" in the Extensions view, or install from the command line:

```
code --install-extension CaioPlazas.slang-server-extended
```

The Linux build bundles a CentOS 7 / glibc 2.17-compatible server binary, so no separate server install is needed on most systems. You can also grab the VSIX directly from this fork's [Releases page](https://github.com/CaioPlazas/slang-server-extended/releases) and install it with `code --install-extension slang-server-extended-linux-x64.vsix`. Alternatively, build slang-server yourself and set `slangCustom.path` to that binary. See [FORK_FEATURES.md](https://github.com/CaioPlazas/slang-server-extended/blob/main/FORK_FEATURES.md) for this fork's additions.

### Vscode Forks (Cursor, Antigravity, VSCodium, etc.)

Install the same VSIX from this fork's [Releases page](https://github.com/CaioPlazas/slang-server-extended/releases) using your editor's "Install from VSIX" command.

### Neovim

`slang-server` is available in [nvim-lspconfig](https://github.com/neovim/nvim-lspconfig) as `slang_server` (note the underscore) and in the [mason.nvim](https://github.com/mason-org/mason.nvim) package registry as `slang-server`, so no additional configuration is required in most cases. The default configuration shipped with nvim-lspconfig can be found [here](https://github.com/neovim/nvim-lspconfig/blob/master/lsp/slang_server.lua).

Install the binary via `:MasonInstall slang-server` (or otherwise place it on `PATH`), then enable the server with `vim.lsp.enable("slang_server")`, or follow your own Neovim configuration's convention for enabling servers.

Restart and run `:LspInfo` to make sure the LSP was correctly installed.

#### Enhanced features

Once the language server is installed, it is recommended to install the [slang-server.nvim](https://github.com/hudson-trading/slang-server.nvim) plugin; this provides enhanced HDL specific features such as design hierarchy view and waveform integration.

### Other editors

Most modern editors can at least point to a language server binary for specific file types. This will provide standard LSP features, but not HDL specific features.

If the editor also allows for executing LSP commands, HDL features like setting a compilation should be available.
