# Slang Server Extended: Verilog/SystemVerilog LSP

A community fork of [hudson-trading/slang-server](https://github.com/hudson-trading/slang-server)
(built on the [Slang](https://github.com/MikePopoloski/slang) library), extended to work well with
**unusual / ASIC-style project configurations** — shared work directories, deep flist-driven RTL
trees, and header/fragment files that are only ever `` `include``d rather than compiled on their own.

Everything upstream does, this does too. The additions below are the reason this fork exists; they
are additive and (unless noted) off by default, so nothing from upstream changes behavior unless you
opt in.

> **How this fork is built:** the changes here are written with a combination of a locally-run
> **Qwen3 (32B)** model and **Claude** prompts. It's a personal fork maintained to unblock real
> ASIC/FPGA workflows — but I'm happy to contribute any of these ideas or changes back upstream if
> the maintainers are interested. Feedback, issues, and suggestions are welcome.

## What this fork adds over upstream

| Feature | What it does |
| --- | --- |
| **`resolveIncludeFragments`** (on by default) | `.vh`/`.svh` files that are only ever `` `include``d elsewhere used to parse standalone and emit bogus "expected a module" errors, with no working goto/hover/references inside them. They're now analyzed in the context of the file(s) that include them, so diagnostics are real and position features (goto-definition, hover, find-references, completions) work inside headers. |
| **`workDir`** | Shared work-directory support for ASIC flows: relative paths in `flags` (e.g. `-F ./design.f`) resolve as if the server were launched from `workDir`, exactly like `cd <workDir> && <tool> <flags>`. |
| **Recursive incdirs for big RTL trees** | Guidance + tests for `-F` (chdir-into-flist) plus slang's `-I .../` recursive include-dir glob, so large multi-folder designs need no per-folder config and pick up new subfolders automatically. |
| **Bundled Linux binary** | The `linux-x64` VSIX ships the `slang-server` binary inside the extension — install and it just works, no separate server download or `PATH` setup. |
| **CentOS 7 / glibc 2.17 builds** | A fully-static build option so the bundled binary runs on old-but-common enterprise Linux, not just recent glibc. The packaging step refuses to bundle a non-optimized binary. |
| **Separate extension identity** | Published as **Slang Server Extended** under its own id and `slangCustom.*` settings namespace, so it can be installed side by side with the original without the two fighting over settings. |

See [FORK_FEATURES.md](https://github.com/CaioPlazas/slang-server-extended/blob/main/FORK_FEATURES.md)
for full configuration details and examples.

## Quick start

Install **Slang Server Extended** from the VS Code Marketplace (search the name, or
`code --install-extension CaioPlazas.slang-server-extended`). On Linux the server binary is bundled,
so there's nothing else to install. You can also grab the VSIX from the
[Releases page](https://github.com/CaioPlazas/slang-server-extended/releases).

To point the extension at your own `slang-server` build instead, set `slangCustom.path` in your
settings. Project-level flags live in `.slang/server.json`. See the
[upstream docs](https://hudson-trading.github.io/slang-server) for the shared feature and
configuration reference.

## Features (inherited from upstream)

Quick, high quality lint messages from [Slang](https://github.com/MikePopoloski/slang) on every
keystroke, with links to the [Slang warning reference](https://sv-lang.com/warning-ref.html).

![Lints](https://github.com/hudson-trading/slang-server/blob/main/docs/assets/images/lints.gif?raw=true)

Informative hovers and gotos on nearly every symbol across your workspace and libraries.

![Hovers](https://github.com/hudson-trading/slang-server/blob/main/docs/assets/images/hovers.gif?raw=true)

Find references across your entire workspace.

![Go to References](https://github.com/hudson-trading/slang-server/blob/main/docs/assets/images/gotorefs.gif?raw=true)

Configurable inlay hints that provide useful information.

![Inlays](https://github.com/hudson-trading/slang-server/blob/main/docs/assets/images/all_inlays.png?raw=true)

Intuitive completions for module instances and macros, as well as scope members of packages,
modules, structs, and more.

![Completions](https://github.com/hudson-trading/slang-server/blob/main/docs/assets/images/completions.gif?raw=true)

HDL-specific features that let you set a filelist or top level for a design, browse the elaborated
hierarchy, and interact with waveform viewers.

![HDL Features](https://github.com/hudson-trading/slang-server/blob/main/docs/assets/images/hdl.gif?raw=true)

For more detailed feature info, see [the upstream docs](https://hudson-trading.github.io/slang-server/features/features/).

## Credits & license

This is a fork of [hudson-trading/slang-server](https://github.com/hudson-trading/slang-server),
which is built on [Slang](https://github.com/MikePopoloski/slang) by Mike Popoloski. All original
work belongs to its respective authors; this fork keeps the upstream MIT license. Thank you to the
Hudson River Trading team and the Slang project for the foundation this builds on.
