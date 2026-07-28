# Plan: UVM testbench linting — bind class-declaring `.svh` included into a package

> **Note:** implemented since this doc was written. The config flag described below as
> `experimental.uvmClassFragments` was renamed to `experimental.uvmVerificationLinting`
> when a follow-up change extended it to also cover cross-file virtual-interface/`extends`
> resolution. This document is kept as historical design context; the current behavior and
> flag name are documented in `docs/start/config.md`.

## Context

Real-world UVM testbenches keep hitting "unknown macro" / unresolved-type errors, even
after pointing `-I` at a vendor UVM tree and adding `uvm.sv` to the flist. This matches
upstream issue [hudson-trading/slang-server#135](https://github.com/hudson-trading/slang-server/issues/135)
exactly, and a live repro in that issue's comments pins down the mechanism:

```systemverilog
// my_uvm_pkg.sv
package my_uvm_pkg;
`include "my_monitor.svh"
endpackage

// my_monitor.svh
`include "uvm_macros.svh"
class my_monitor extends uvm_monitor;
  `uvm_component_utils(my_monitor)
  virtual my_bfm_if bfm;   // <- reported as "unknown interface 'my_bfm_if'"
endclass
```

This fork already has a feature built for exactly this shape of problem —
`resolveIncludeFragments` (on by default): when a `.vh`/`.svh` file that's only ever
`` `include``d elsewhere is opened, the server finds the including "owner" file(s) and
analyzes the fragment in the owner's context instead of parsing it standalone, so
sibling-declared symbols resolve and diagnostics are real.

**It just doesn't trigger for UVM's pattern.** The eligibility check,
`isEmptyStandaloneParse` in `src/ServerDriver.cpp:240-292`, requires the file's standalone
parse to declare *no* symbols at all — and `ParserMetadata::visitDeclaredSymbols` counts
class declarations. The moment a `.svh` declares a `class` (exactly `my_monitor.svh`'s
shape), it's classified as a legitimate standalone file and is analyzed in total
isolation, with zero visibility into sibling files like `my_bfm_if.sv`. This explains the
reported symptom precisely: the interface *type* still resolves via other lookup paths
(hence the correct hover), but the class body itself is bound with no knowledge that
`my_bfm_if` exists — "unknown interface."

There's a deliberate guardrail behind the current strictness: an earlier, broader
heuristic caused a real regression (`PERF_INVESTIGATION.md:24-34`, commit `2ea88e9`), now
locked in by the test `DualPurposeFileNotConvertedOnFreshOpen`
(`tests/cpp/IncludeFragmentTests.cpp:167`) — a `.svh` that declares something *and*
compiles standalone with zero errors must stay standalone. Any fix has to add the UVM case
without loosening that guarantee.

**What this fix does not address** (to avoid over-promising): cross-file macro inheritance
without `--single-unit` is a separate, documented, structural limitation
(`docs/features/limitations.md:16-18`); a vendor UVM tree living outside the workspace
still needs an explicit `index.dirs` entry to be indexed at all; and the static Indexer's
`maxIncludeDepth = 0` means `import pkg::*` resolution (`getFirstSymbolLoc`,
`src/ServerDriver.cpp:570-628`) still can't see classes/macros nested inside included
`.svh` files — that's a bigger indexer change, out of scope here.

**Immediate workaround for users hitting this today** (no code change): use an exact
`-I /cad/vendor/uvm/<ver>/src` (not the recursive `.../` glob, which risks matching a
duplicate/wrong copy of a header in a vendor tree and is a full recursive `stat` walk on
every config reload); add `uvm.sv`/`uvm_pkg.sv` to the flist so it's part of the driver's
document set; prefer a `build` entry in `.slang/server.json` over pure `flags`-only
"explore mode" — build mode elaborates all flist documents into one `ServerCompilation`,
giving real cross-file resolution that explore mode's per-doc shallow analysis doesn't;
and add an absolute `index.dirs` entry for the vendor UVM `src` directory so
workspace-symbol lookups (e.g. `import uvm_pkg::*`) have a chance to resolve it. This
won't fully fix the class-in-package case (that needs the code change below), but it
removes the `-I`/index-related noise around it.

## Approach

Split the current single boolean check into a classifier plus an owner-aware predicate, so
"declares only classes" becomes a *maybe* instead of an automatic "standalone" — resolved
by checking whether the include site (in the owner's tree) sits inside a design unit
(package/module/interface/program/class/checker) rather than at compilation-unit scope.

**This whole behavior is gated behind a new, off-by-default experimental config flag** —
given how central `resolveIncludeFragments` already is and how deliberately conservative
its current eligibility test is (the `2ea88e9` regression history), the class-in-package
extension should not change default behavior for any existing user until it's been
validated against real UVM codebases.

### 0. New config option: `experimental.uvmClassFragments`

- `include/Config.h`: add a nested struct, e.g.

  ```cpp
  struct Experimental {
      rfl::Description<
          "Extend resolveIncludeFragments to also bind class-declaring .svh files "
          "(e.g. UVM component/object headers) that are `include`d inside a package, "
          "module, or similar design unit, analyzing them in that owner's context "
          "instead of parsing them standalone. Off by default; behavior may change "
          "or be removed without notice while this is validated.",
          bool>
          uvmClassFragments = false;
  };
  rfl::Description<"Experimental / beta features. Off by default; interfaces here "
                   "may change or be removed without notice.",
                   Experimental>
      experimental = Experimental{};
  ```

- `classifyStandaloneParse` only returns `NeedsOwnerContext` for a class-only parse when
  `config.experimental.uvmClassFragments` is true; with the flag off (default), a
  class-only `.svh` falls through to exactly today's behavior (`Standalone`), so nothing
  changes for existing users who don't opt in.
- `docs/start/config.md`: new "Experimental" section documenting `experimental.*`,
  explicitly marked beta/subject to change, with the opt-in example:
  ```json
  { "experimental": { "uvmClassFragments": true } }
  ```
- `clients/vscode/resources/config.schema.json` regenerate via `scripts/genconfig.py` (or
  the `gen_config_main` build target) so the VS Code JSON editor picks up the new field
  with its description/default.
- Test `ClassFragmentBehaviorCanBeDisabled` becomes `ClassFragmentBehaviorRequiresOptIn`:
  default config (flag off) → class-in-svh stays standalone (today's behavior, unchanged);
  `experimental.uvmClassFragments = true` → fragment binding kicks in as designed below.

### 1. `src/ServerDriver.cpp` — replace `isEmptyStandaloneParse` (lines 237-292)

- `enum class StandaloneParse { Fragment, NeedsOwnerContext, Standalone }`
- `classifyStandaloneParse(doc)`:
  1. No doc/tree → `Fragment` (unchanged).
  2. Any `meta.nodeMeta` entry with a named decl (module/interface/program/package) →
     `Standalone` (unchanged — this is today's existing rule, minus classes).
  3. Else any named entry in `meta.classDecls` → `NeedsOwnerContext` (**the behavior
     change** — previously this fell through to `Standalone`).
  4. Else, existing error-diagnostic scan → `Fragment` if ≥1 error, else `Standalone`
     (unchanged).
- New `includedInsideDesignUnit(ownerTree, fragBufId)`: for each class decl in
  `ownerTree->getMetadata().classDecls` whose buffer is `fragBufId`, walk `node->parent`
  upward (through list nodes) looking for an ancestor kind in
  `ModuleDeclaration | PackageDeclaration | InterfaceDeclaration | ProgramDeclaration |
  ClassDeclaration | CheckerDeclaration`. Because `MetadataVisitor` recurses through
  `` `include``s, this works transitively for nested includes too.
- New `isFragmentEligible(doc, owners, uri)`: `Standalone` → false; `Fragment` → true;
  `NeedsOwnerContext` → true iff some owner satisfies `includedInsideDesignUnit`.

### 2. Call sites

- **`resolveDocument`** (`:471-488`) — owners are already found first; swap in
  `isFragmentEligible`. In the `NeedsOwnerContext` case, filter `includeOwners` down to
  only the design-unit-nested owners before `fromIncludeOwner`, so
  `m_includeOwners[0]` (which drives goto/hover/refs/completions —
  `SlangDoc::getSyntaxTree`, `src/document/SlangDoc.cpp:135-153`) is a real package owner,
  not an arbitrary hash-order `$unit`-level includer.
- **`openDocument` cached branch** (`:315`) — key the perf-motivated pre-check off the
  classifier: `Standalone` skips `findIncludeOwners` entirely (unchanged fast path for the
  vast majority of reopened files); `Fragment`/`NeedsOwnerContext` behave as above.
- **`adoptOrphanFragments`** (`:555`) — it already has the candidate owner's tree and
  buffer id in hand, so this is a straight swap to `classify(doc)` +
  `includedInsideDesignUnit(...)` with **no added scan cost**.

### 3. Tests

New fixture `tests/data/uvm_pkg_include/`, mirroring the issue #135 repro:
`my_bfm_if.sv` (interface), `my_monitor.svh` (class referencing it, `` `include``ing a
stand-in macros header), `my_uvm_pkg.sv` (package `` `include``ing the class), plus a
negative-control pair `unit_class.svh` + `top_unit_includer.sv` (class `.svh` included at
`$unit` scope — must stay standalone), and `pkg.f`.

New `tests/cpp/UvmClassFragmentTests.cpp` (test `CMakeLists.txt` globs `*.cpp`, no build
file edit needed):
- `ClassInSvhBoundToPackageOwner` — open pkg then svh, assert `isIncludeFragment()`.
- `ClassFragmentResolvesSiblingInterfaceType` — no bogus diagnostic on
  `virtual my_bfm_if bfm;`; then rename to `my_bfm_ifx` and confirm a real error appears
  (proves genuine owner binding, mirrors `IncludeFragmentUsesOwnerContext`).
- `ClassFragmentAdoptedWhenPackageOpensLater` — svh opened first, pkg opens after; bogus
  diagnostics clear (covers `adoptOrphanFragments`).
- `ClassSvhIncludedAtUnitScopeStaysStandalone` — negative control, `$unit`-scope include.
- `ClassSvhWithNoOwnerStaysStandalone` — svh opened alone, no owner ever appears.
- `ClassFragmentBehaviorCanBeDisabled` — `resolveIncludeFragments = false` restores old
  behavior.

Run existing `tests/cpp/IncludeFragmentTests.cpp`,
`tests/cpp/IncludeFragmentGotoTests.cpp`, and
`tests/cpp/SingleUnitFragmentRegressionTests.cpp` unmodified — the classifier is a strict
refactor for every case they already cover (`nodeMeta` rule untouched), so they must all
still pass, `DualPurposeFileNotConvertedOnFreshOpen` included.

### 4. Docs (small, low-risk cleanup alongside the fix)

- `docs/start/config.md:167` currently claims fragment diagnostics are "the union of every
  including context" — inaccurate since the union is capped at
  `kMaxFragmentOwnersAnalyzed = 8` (`src/document/SlangDoc.cpp:368`). Update the wording to
  mention the cap.
- `docs/features/limitations.md:20-22` — once this fix lands, narrow the "Some UVM Code"
  note to describe what's now supported (class-in-package via `` `include``) vs. what still
  isn't (cross-file macro inheritance without `--single-unit`; classes/macros inside
  included `.svh` not visible to the static Indexer for `import pkg::*` resolution).

### Perf risk (checked, bounded)

The `2ea88e9` regression class isn't reintroduced: the `nodeMeta` rule is untouched, and
the newly-eligible set is exactly "files whose only top-level declarations are classes."
The added `findIncludeOwners` scan only grows on `openDocument`'s cached-reopen path for
that narrow file set, once per `didOpen` — not per keystroke.
`resolveDocument`/`adoptOrphanFragments` gain zero additional scans. The deferred
reverse-include index (`PERF_INVESTIGATION.md` root cause 2) remains the real long-term
fix for scan cost at UVM scale; not needed for this change to be safe.

## Critical files

- `src/ServerDriver.cpp` — classifier + call sites (core of the change)
- `include/ServerDriver.h` — signature updates if any helpers become member functions
- `src/util/SlangExtensions.cpp` — `findBufferForPath`, referenced but not modified
- `tests/cpp/IncludeFragmentTests.cpp` — must remain green, unmodified
- `tests/cpp/UvmClassFragmentTests.cpp` — new
- `tests/data/uvm_pkg_include/` — new fixtures
- `docs/start/config.md`, `docs/features/limitations.md` — doc updates

## Verification (once implemented)

1. `cmake --build build -j8 --target server_unittests && build/bin/server_unittests` —
   confirm the new `UvmClassFragmentTests` pass (opt-in case with
   `experimental.uvmClassFragments = true`, and the default-off case behaving identically
   to today), and every existing
   `IncludeFragmentTests`/`IncludeFragmentGotoTests`/`SingleUnitFragmentRegressionTests`
   case is unchanged (byte-identical pass/fail) under the default config.
2. `ctest --test-dir build --output-on-failure` for the full suite (catch any interaction
   with `WorkDirTests`, `IndexerTests`, `GotoTests`, `HoverTests`).
3. Manual smoke test: build `slang_server`
   (`cmake --build build -j8 --target slang_server`), open a small reproduction workspace
   shaped like the fixture (package + class-in-svh + sibling interface) in the VS Code
   client with `experimental.uvmClassFragments: true` set, open the `.svh` directly, and
   confirm no bogus diagnostic and correct goto-definition on the sibling type. Repeat with
   the flag unset/false and confirm behavior matches current `main`.
4. If you can point it at a real vendor UVM tree, re-test actual TB files with the
   immediate-workaround config from the Context section applied plus the experimental flag
   on, to confirm the combination resolves the original "unknown macro"/unresolved-type
   reports.

## Status

This document is a **design plan only** — no implementation code has been written yet. It
was researched via upstream issue [#135](https://github.com/hudson-trading/slang-server/issues/135)
and traced against this fork's `resolveIncludeFragments` implementation, then committed to
`beta_changes` so the work (Sections 0-4 above) can be picked up later, gated behind the
`experimental.uvmClassFragments` flag so it ships without risk to default behavior once it
lands.
