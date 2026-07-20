# This fork's additions

This is [CaioPlazas/slang-server-extended](https://github.com/CaioPlazas/slang-server-extended), a fork of
[hudson-trading/slang-server](https://github.com/hudson-trading/slang-server) with a few
additions on top. Everything here is additive and off-by-default unless noted — nothing from
upstream changes behavior unless you opt in.

## Separate VS Code extension identity

This fork's VS Code extension is published under its own identity — **Slang Server Extended**,
`CaioPlazas.slang-server-extended` (package name `slang-server-extended`), not the original
`Hudson-River-Trading.vscode-slang`. This is deliberate: sharing the original's identity or its
`slang.*` settings/command namespace with the upstream extension caused real conflicts (VS Code
treats publisher+name as a single unique extension, and settings keys are a flat global namespace,
so both extensions installed side by side would fight over the same values). If you have the
original extension installed too, this fork will warn you and ask you to uninstall one.

**If you're migrating settings from the original extension**, every setting moved from `slang.*`
to `slangCustom.*` — e.g. `slang.path` → `slangCustom.path`, `slang.formatDirs` →
`slangCustom.formatDirs`. (The internal settings namespace stayed `slangCustom.*` across the
`custom` → `extended` rename so existing configs keep working.) The `.slang/server.json` /
`.slang/local/server.json` project config files (read by the `slang-server` binary itself, not
the VS Code extension) are unchanged.

## `resolveIncludeFragments` — standalone `.vh`/`.svh` fragments

`.vh`/`.svh` files that are only ever `` `include``d elsewhere (never opened as a standalone
top-level file) used to get parsed on their own and produce bogus "expected a module" errors,
with no real diagnostics. They're now analyzed in the context of whatever file(s) include them
instead — automatically, no config needed. This is **on by default**.

If you rely on the old standalone-parsing behavior for some reason, turn it off:

```json
// .slang/server.json
{
  "resolveIncludeFragments": false
}
```

Full reference: [docs/start/config.md#resolveincludefragments](docs/start/config.md#resolveincludefragments).

## `workDir` — shared work-directory support

In ASIC development it's common to run all tools from a shared "work" directory, so paths in
flists/build files are written relative to that directory rather than the repo root. Set
`workDir` in `.slang/server.json` and relative paths in `flags` (e.g. `-F ./my_flist.f`) resolve
as if the server had been launched from `workDir` — exactly like `cd <workDir> && <tool> <flags>`.

```json
// .slang/server.json
{
  "workDir": "design/work",
  "flags": "-F ./my_flist.f"
}
```

If your flist instead lives in a sibling directory (e.g. `design/rtl`), write the path the same
way you would after `cd`-ing into `workDir`:

```json
{
  "workDir": "design/work",
  "flags": "-F ../rtl/my_flist.f"
}
```

### Getting includes from many RTL subfolders (project-agnostic, no per-folder config)

Two things matter here, and both are slang's own native behavior — nothing fork-specific:

1. **Use `-F` (capital), not `-f`, for the top-level flist reference.** `-F` chdirs into the
   flist's own directory before parsing its contents, so relative paths *inside* the flist
   (`-I`, source files, etc.) resolve relative to where the flist lives — not relative to
   `workDir`. `-f` doesn't chdir, so anything relative inside the flist silently resolves
   against `workDir` instead, which is almost never what you want once the flist isn't sitting
   directly inside `workDir` itself.
2. **Inside the flist, use a recursive incdir glob** instead of listing every subfolder:
   ```
   -I .../
   ```
   `.../` recursively matches every subdirectory from that point down (this is slang's own glob
   syntax, also used for build-file patterns) — new subfolders anywhere under the tree get
   picked up automatically, with zero maintenance as the RTL tree grows.

Put together, a flist at `design/rtl/dig_top.flist` with a deep subfolder structure needs no
special handling beyond:

```json
// .slang/server.json
{
  "workDir": "design/work",
  "flags": "-F ../rtl/dig_top.flist"
}
```

```
# design/rtl/dig_top.flist
-I .../
dig_top.sv
```

**If you can't touch the flist** (shared/generated, owned by another team, or your project's
convention is that *everything* in the flist — including its own source-file entries — is
already written relative to `workDir`, not relative to the flist's own location, which is
exactly what `-f` assumes): put the recursive incdir directly in `flags` instead. It resolves
relative to `workDir` just like everything else in `flags`, so the flist needs zero changes:

```json
// .slang/server.json
{
  "workDir": "design/work",
  "flags": "-I ../design/.../ -f ../design/rtl/dig_top.flist"
}
```

Note it's `...`, not `*`: `-I ../design/*` only matches immediate subdirectories of `design/`
(one level); `-I ../design/.../` recurses through every level under it. This distinction and
both approaches above are verified by regression tests in `tests/cpp/WorkDirTests.cpp`.

Full reference: [docs/start/config.md#workdir](docs/start/config.md#workdir).

## Bundled Linux binary — no separate install

The `linux-x64` VSIX ships the `slang-server` binary directly inside the extension package.
Install it and it just works — no download prompt, no separate `slang-server` install on your
system.

- Grab `slang-server-extended-linux-x64.vsix` from the [Releases page](https://github.com/CaioPlazas/slang-server-extended/releases)
  and install with `code --install-extension slang-server-extended-linux-x64.vsix`.
- Need a specific/custom build instead? Set `slangCustom.path` in your VS Code settings — it
  always overrides the bundled binary.

## CentOS 7 / glibc 2.17 compatible builds

The bundled/released Linux binary is fully statically linked (no runtime glibc dependency at
all), so it runs on CentOS 7 and any newer x86_64 Linux distro — matching common ASIC-industry
baseline environments. Build it yourself with:

```bash
cmake --preset gcc-release -DSLANG_SERVER_FULLY_STATIC=ON
cmake --build build/gcc-release --target slang_server
```

---

Latest builds (including betas): [Releases](https://github.com/CaioPlazas/slang-server-extended/releases)
