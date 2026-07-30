# Plan: `experimental.singleUnitMacros` — cross-file macro inheritance without an `` `include`` edge

## Context

`` `uvm_component_utils`` reports "unknown macro or compiler directive" on files that declare a
design unit (`tb_top`, an interface/program, or a `*_pkg.sv` that declares its package inline),
even with `experimental.uvmVerificationLinting: true` and a build/flist selected.

Root cause, traced:

1. `classifyStandaloneParse` (`src/ServerDriver.cpp:270-273`) returns `Standalone` on the first
   named module/interface/program/**package** in `nodeMeta`. All three sites that could attach
   macro context bail before the flag is read — `resolveDocument:549`, `openDocument:354-360`,
   `adoptOrphanFragments:649`. `uvmVerificationLinting` only ever engages for **class-only**
   files (`NeedsOwnerContext`, `:277-280`).
2. Both existing macro mechanisms travel `` `include`` edges only — `findIncludeOwners` requires
   `sm.getIncludedFrom(*bufId).valid()` (`:600`). A top-level flist entry is included by nobody.
3. Build mode doesn't help: `updateDoc` (`:166-180`) gets only *semantic* diags from `comp`;
   parse diags still come per-file from `SyntaxTree::fromBuffer` in `SlangDoc::getSyntaxTree()`
   (`src/document/SlangDoc.cpp:179-194`). The flist gives a file set and one `Compilation`; it
   never concatenates into a shared compilation unit.

Simulators are happy because they compile single-compilation-unit (`vlog -sv`, `-mfcu`), where
an earlier flist file's `` `include "uvm_macros.svh"`` stays in scope. This is the gap named in
`UVM_MACRO_INHERITANCE_PLAN.md:21-26`, `docs/features/limitations.md:18`, and
`docs/features/design/shallow.md:26-28` ("Support for this could be added in the future by
feeding the indexed macros to the preprocessor").

**Intended outcome:** a new experimental toggle that approximates `--single-unit` *preprocessor*
behavior — every document's parse predefines the union of `` `define``s found anywhere in the
build — so UVM macros resolve regardless of file shape or include topology. Symbol scoping is
deliberately unchanged; this is macro state only.

## Approach

Reuse the machinery the `m_macroOwners` feature already established: slang's
`SyntaxTree::fromBuffer(buffer, sm, options, inheritedMacros)` and `SyntaxTree::getDefinedMacros()`
(already used at `src/document/SlangDoc.cpp:142-152` and `:184-193`). The only new idea is a
**driver-level macro pool** feeding every parse, instead of a per-doc owner list.

Decisions taken: pool = every file in the build/flist (zero extra config); ordering =
order-insensitive union (reuses trees slang already parsed, no serial re-parse pass).

### 1. Config — `include/Config.h`

Add to `struct Experimental` (alongside `uvmVerificationLinting`, `:134-150`):

```cpp
rfl::Description<
    "Approximate `--single-unit` preprocessor behavior: predefine the union of all "
    "`define`s found in the build's files when parsing each file, so macros defined in "
    "one file (e.g. `uvm_macros.svh` pulled in by a package) resolve in files that don't "
    "`include` them. Symbol scoping is unchanged. Order-insensitive, so a macro defined "
    "in a later file is visible in an earlier one. Off by default.",
    bool>
    singleUnitMacros = false;
```

A config change already recreates the driver (`SlangServer::loadConfig` → `setBuildFile`/
`setExplore` → `ServerDriver::create`, `src/SlangServer.cpp:513-528`, `:277`, `:331`), so no
in-place reconfiguration path is needed.

### 2. Driver — `include/ServerDriver.h` / `src/ServerDriver.cpp`

New state:

```cpp
/// Build order (flist order) of docs, for deterministic last-wins resolution of the pool.
std::vector<URI> m_buildOrder;
/// Trees whose allocators own the DefineDirectiveSyntax pointers below -- pinned so the
/// pool never dangles. Only trees that actually won a slot are pinned.
std::vector<std::shared_ptr<slang::syntax::SyntaxTree>> m_macroPoolTrees;
std::vector<const slang::syntax::DefineDirectiveSyntax*> m_macroPool;
uint64_t m_macroPoolGen = 0;
bool m_rebuildingMacroPool = false;
```

Public: `const std::vector<const slang::syntax::DefineDirectiveSyntax*>& getMacroPool() const;`
and `uint64_t getMacroPoolGen() const;`

`rebuildMacroPool()`:
- No-op unless `m_config.experimental.value().singleUnitMacros.value()`.
- Re-entrancy guard on `m_rebuildingMacroPool` — during a rebuild, `collectInheritedMacros()`
  keeps returning the *previous* pool (still pinned, still valid), so parsing a contributor
  can't recurse into the rebuild.
- Walk `m_buildOrder` → `docs[uri]->getSyntaxTree()` → `getDefinedMacros()`, upserting into a
  `flat_hash_map<std::string_view, const DefineDirectiveSyntax*>` (last wins). Skip include
  fragments (they share their owner's tree).
- Materialize the map's values into a new vector, pin only the contributing trees, then swap
  both in one step.
- Bump `m_macroPoolGen` **only if the pool's content actually changed** (hash of each macro's
  name + raw source text). Otherwise every save mass-invalidates every doc.

Call sites:
- End of `parseAndLoadSources` (`src/ServerDriver.cpp:146-152`): populate `m_buildOrder` from
  `driver.syntaxTrees` order *before* the existing `std::move(tree)` into `SlangDoc::fromTree`,
  then `rebuildMacroPool()`.
- `updateDoc` on `FileUpdateType::SAVE` only (`:166`). Not on `CHANGE` — too thrashy.

Because the startup trees were parsed without the pool, every doc must drop its cached tree once
the pool exists so the next access reparses with inheritance. Add `SlangDoc::invalidateTree()`
(resets `m_tree`/`m_analysis`, keeps `m_buffer`) and call it for all docs after the first
rebuild. **This is the main cost of the feature** — see Perf below.

### 3. Document — `include/document/SlangDoc.h` / `src/document/SlangDoc.cpp`

- `collectInheritedMacros()` (`:142-152`): start from `m_driver.getMacroPool()`, then append the
  existing `m_macroOwners` macros so a specific include-owner still overrides the global pool.
  Verify slang's predefine loop is last-wins; if it's first-wins, reverse the concatenation.
- Filter out pool entries whose defining buffer is this doc's own buffer, if slang emits a
  redefinition warning for a predefine that the file then redefines. (Check first — slang
  generally treats predefines as silently overridable.)
- Add `uint64_t m_macroGen` recorded at parse time; `getSyntaxTree()` (`:179-194`) reparses when
  `m_driver.getMacroPoolGen() != m_macroGen`, alongside the existing `hasValidBuffers()` check.
- Add `invalidateTree()`.
- No change for include fragments (`m_includeOwners` non-empty, `:159-177`) — they borrow the
  owner's tree and inherit transitively.

### 4. Clients & docs

- Regenerate `clients/vscode/resources/config.schema.json` and
  `clients/vscode/src/config.gen.ts` with `scripts/genconfig.py` (do not hand-edit).
- `clients/vscode/src/sidebar/ServerConfigPanel.ts`: checkbox mirroring the UVM one
  (HTML `:292-305`, read-back `:870`, write-back `:938-941`). **The write-back currently does
  `config.experimental = { uvmVerificationLinting: true }` — it must build the object from both
  toggles so one no longer clobbers the other.**
- `docs/start/config.md:173-193` (experimental section), `docs/features/limitations.md:18,22`
  (point the Single Unit / Some UVM Code sections at the new toggle), `CHANGELOG.md`.

## Known divergences from a real `--single-unit` (document these)

- A macro defined in a *later* flist file is visible in an earlier one, so some genuinely-missing
  macros stop being reported.
- `` `undef`` is not modeled across files — `getDefinedMacros()` is per-tree end-of-parse state.
- Conflicting redefinitions resolve last-wins in flist order; a file's own `` `define`` always
  wins locally.
- Macros reachable only through an `` `include`` from a file that is *not* in the build never
  enter the pool. If that bites, the follow-up is an optional explicit header list (the third
  option considered) — not in this change.
- `src/Indexer.cpp:91` (`maxIncludeDepth = 0`) is untouched, so goto-definition on a macro
  resolved this way may still not land. Separate, pre-existing gap.

## Perf

This area has a regression history (`UVM_CLASS_FRAGMENTS_PLAN.md` cites `2ea88e9`), so measure
before committing. The cost is one extra parse of anything accessed after the initial pool
build — negligible in explore mode (only open files), but in build mode `createCompilation`
(`src/ServerDriver.cpp:777`) touches every tree, so expect roughly a second full parse at
startup. Measure startup and first-open latency on `examples/uvm_apb_sram` with the toggle off
vs. on; if it's unacceptable, the escape hatch is a preprocess-only harvest pass modeled on
`src/Indexer.cpp:100-110` (cheaper than parse, threadable) instead of reusing the parsed trees.

## Tests

New `tests/cpp/SingleUnitMacroTests.cpp` + fixture `tests/data/single_unit_macros/`, following
`tests/cpp/UvmMacroInheritanceTests.cpp` and its `tests/data/uvm_macro_include/` fixture:

- `macros.svh` — defines `MY_COMPONENT_UTILS`
- `macro_holder_pkg.sv` — a package that `` `include``s it (how the macro enters the build)
- `tb_top.sv` — `module tb_top;` invoking the macro, no include
- `inline_pkg.sv` — `package p; class c; <macro> endclass endpackage`, no include (the reported shape)
- `build.f` listing them

Cases:
1. Toggle **off** → `tb_top.sv` reports unknown macro (locks current behavior).
2. Toggle **on** → `tb_top.sv` clean.
3. Toggle **on** → `inline_pkg.sv` clean (the `Standalone`-classification path).
4. Toggle **on** → a macro defined nowhere still reports unknown (proves resolution, not
   suppression — same intent as `tests/cpp/UvmVirtualInterfaceTests.cpp:97-98`).
5. Toggle **on** → edit `macros.svh` and save; `tb_top.sv` reflects the change (generation
   invalidation works).
6. `UvmMacroInheritanceTests`, `UvmClassFragmentTests`, `IncludeFragmentTests` unchanged and
   passing with the toggle off.

## Verification

```bash
cmake -B build && cmake --build build -j8 --target server_unittests && build/bin/server_unittests
ctest --test-dir build --output-on-failure
uv run scripts/genconfig.py     # regenerate schema + config.gen.ts, confirm no manual drift
```

Manual, against the repo's real UVM workspace: temporarily remove the
`` `include "uvm_macros.svh"`` line from `examples/uvm_apb_sram/tb/tb_top/tb_top.sv:25` (do not
commit), open the file, and confirm it reports unknown macros with the toggle off and is clean
with it on. Time driver startup both ways on that workspace.

Work on branch `claude/uvm-component-utils-error-v6r265`.
