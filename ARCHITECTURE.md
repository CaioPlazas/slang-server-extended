# Slang Server Extended architecture

How this is built and why. `README.md` is the reference for someone *using* it,
and `FORK_FEATURES.md` for someone deciding whether the fork's additions are worth it.
If this file and the code disagree, **the code is right** — fix this file.

Most of what is described here is **upstream's**
([hudson-trading/slang-server](https://github.com/hudson-trading/slang-server)). It is
written down on this side because the fork needs a single architecture reference that
survives an upstream sync.

## 1. Shape

A SystemVerilog **Language Server** in C++20, plus editor clients. Everything analytical
is built on the [Slang](https://github.com/MikePopoloski/slang) library, vendored as a
submodule.

```
editor (clients/vscode, clients/nvim)
        │  LSP over stdio
        ▼
  SlangServer            one instance per workspace; LSP routes + HDL extensions
        ├── Indexer      workspace-wide symbol sweep, run at startup
        ├── ServerDriver wraps the slang driver; owns syntax trees and open documents
        │                and is RECREATED whenever flags have to be re-parsed
        └── SlangDoc     one per file: a file/SyntaxTree pair, plus that document's
                         token index and shallow compilation
                └── ServerCompilation   wraps a slang Compilation and knows how to
                                        update it rather than rebuild it
```

The split that matters: **`ServerDriver` is disposable, `SlangDoc` is not.** Flags change
(a new flist, a changed `workDir`) means a new driver; open documents and their indexes
outlive it.

## 2. Repository layout

| Path | What lives there |
|---|---|
| `src/SlangServer.cpp` | The server class. Its methods map one-to-one onto LSP routes and the HDL extensions; holds the indexer. |
| `src/ServerDriver.cpp` | Wrapper around the slang driver. Recreated on every flag re-parse. Manages syntax trees and open documents. |
| `src/Indexer.cpp` | Startup sweep that gathers every symbol in the workspace. |
| `src/ast/ServerCompilation.cpp` | Wrapper around a slang `Compilation`, with incremental update logic. |
| `src/document/SlangDoc.cpp` | A file/SyntaxTree pair and its per-document analysis (token index, shallow compilation). |
| `src/document/` | The core per-document LSP features — definitions, inlay hints, symbol and syntax indexing. |
| `src/completions/`, `src/codeactions/` | Feature areas, one directory each. |
| `src/lsp/`, `src/util/` | Protocol plumbing and shared helpers. |
| `clients/vscode/` | The VS Code extension (TypeScript, pnpm). **This is what ships to the Marketplace**, and `checks.version_from` points at its `package.json`. |
| `external/` | Vendored dependencies. `slang`, `reflect-cpp` and `ctre` are git submodules. |
| `tests/cpp/` | Catch2 tests. `tests/cpp/golden/` holds golden outputs. |
| `docs/` | mkdocs site. `docs/notes/` is archived design rationale — historical, **not** a backlog. |

## 3. Two version numbers, and only one of them is the release

- `VERSION` at the repo root is **upstream's server version**. Nothing in this fork's
  release path reads it. It used to be bumped by upstream's `release.yml`, which this
  fork no longer carries.
- `clients/vscode/package.json`'s `version` is **the released version**. It is what
  the release tooling reads, tags and publishes.

## 4. Decisions that constrain everything else

- **The `external/slang` submodule points at a private fork**
  (`CaioPlazas/slang`, branch `uvm-verification-refs-260726`), not at upstream slang. The
  fork's UVM work depends on it. Consequence, and it is not theoretical: **every GitHub
  Actions workflow on this repo fails at submodule checkout**, because the Actions token
  cannot read that private repo — `fatal: repository 'CaioPlazas/slang.git' not found`.
  That includes the weekly upstream sync. Verification happens locally; do not read a red
  check here as a signal about your change.
- **The `static-release` CMake preset targets glibc 2.17 / CentOS 7** via
  `SLANG_SERVER_FULLY_STATIC`. Old-but-common enterprise Linux is a real deployment
  target. Do not "modernize" this away.
- **The bundled binary ships only in the `linux-x64` VSIX.** Every other platform gets
  the universal package and resolves a server via `slang.path`, `PATH`, or the managed
  GitHub-release download. The universal package can never be omitted: without it, every
  platform with no targeted build resolves zero packages and reports the extension as
  unavailable.
- **Fork changes are additive and default-off** unless noted, so an upstream sync stays
  cheap and a user opting into nothing gets upstream behaviour.
- **Upstream files are kept as close to upstream as possible.** Restructuring one buys a
  conflict every week for as long as the fork lives.

## 5. Build and test

The build and test commands live with the build system rather than being duplicated
here. What they mean:

- **`server_unittests` is the primary suite.** It is Catch2, and much of it is golden-file
  based — `--update` regenerates `tests/cpp/golden/`, so a diff there is a real behaviour
  change and must be read, not rubber-stamped.
- **`ctest` runs the whole set**, including the pygls-driven Python tests, which need the
  `slang_server` binary built first.
- **The CMake target is `slang_server`; the binary it produces is `build/bin/slang-server`**
  (underscore vs hyphen). Both names are correct and neither is a typo.
- **Formatting is enforced by pre-commit hooks**, and the Python environment is managed
  with `uv`.
