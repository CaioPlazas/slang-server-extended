# Plan: cross-file macro inheritance for shallow/explore-mode UVM code

## Status

**Implemented (partially) since this doc was written.** A follow-up session re-traced the
code and found the archetypal UVM pattern (a class-only `.svh` `` `include``d inside a
package that itself `` `include``s `uvm_macros.svh` first) already worked via the existing
`resolveIncludeFragments`/`uvmVerificationLinting` owner-context mechanism, which borrows
the owner's already-preprocessed tree wholesale — this was previously untested, not
actually broken. The one confirmed, reproduced gap was narrower: a class-only `.svh`
`` `include``d at *$unit scope* (not nested in a design unit) is deliberately excluded from
full symbol-splice binding (see `ClassSvhIncludedAtUnitScopeStaysStandalone` in
`UvmClassFragmentTests.cpp`), and before this change that also meant zero macro carryover.
Fixed via `SlangDoc::addMacroOwner`/`collectInheritedMacros` (`SlangDoc.h`/`.cpp`) and three
new call sites in `ServerDriver::resolveDocument`/`openDocument`/`adoptOrphanFragments`,
using slang's existing `SyntaxTree::fromBuffer`'s `inheritedMacros` parameter and
`getDefinedMacros()` — macro state is inherited from *any* owner (regardless of
design-unit-nesting), decoupled from full symbol-splicing, since macros don't carry the
same scoping risk. See `tests/cpp/UvmMacroInheritanceTests.cpp`.

**Still out of scope** (unchanged from the original investigation): cross-file macro
inheritance across files that share no `` `include`` relationship at all — e.g. two
top-level flist entries relying purely on compile order, the way `--single-unit` would
provide. That's a materially bigger, riskier change (real single-unit-like semantics) and
wasn't attempted here. The `Indexer.cpp:91` `maxIncludeDepth = 0` gap (workspace-symbol /
`import pkg::*` fallback resolution) is also still open, tracked separately.

The rest of this document is kept as-written below for historical investigation context.

## Context

A user configured a workspace with `-I <uvm-dir>`, the UVM package/flist entries, and
`experimental.uvmVerificationLinting: true` (see `examples/uvm_apb_sram/.slang/server.json`
for a correctly-configured reference), and still got "unknown macro" / unresolved-symbol
diagnostics on files using `` `uvm_component_utils`` and similar UVM macros.

Root cause, confirmed by tracing the code (not guessed):

- In flags-only ("explore"/shallow) mode — i.e. no `build`/`builds` entry in
  `.slang/server.json` — each open document is analyzed independently via
  `ShallowAnalysis`. `ServerDriver::parseAndLoadSources` (`src/ServerDriver.cpp:68-153`)
  builds one shared `OptionBag` (include dirs, defines) from `flags` and threads it into
  every doc (`SlangDoc::SlangDoc`, `src/document/SlangDoc.cpp:36`; per-parse use at
  `:160`, `:165`). That bag is *preprocessor options*, not preprocessor *state* — it
  doesn't carry forward `` `define``s that one file's `` `include`` chain produced into a
  sibling file's independent parse.
- So a `` `define`` from `uvm_macros.svh` is only visible to a file that
  `` `include``s it itself (directly, or transitively through something it includes). A
  file that relies on a *different* file in the same package/testbench having already
  `` `include``d it (the common real-world UVM pattern) sees the macro as unknown.
- `experimental.uvmVerificationLinting` (`include/Config.h`, `Experimental` struct; logic
  in `src/ServerDriver.cpp:246-321` class-fragment/owner-context binding, `:354-377`,
  `:543-558`, `:590-649` more binding call sites, `:709-716` virtual-interface/`extends`
  cross-file type resolution) does **not** touch macro/preprocessor state at all — it only
  affects symbol/type resolution. This was true by design; see the "What this fix does not
  address" section of `UVM_CLASS_FRAGMENTS_PLAN.md`, which explicitly scoped macro
  inheritance out of that earlier change.
- A second, separate instance of the same class of gap: the static `Indexer` used for
  workspace-symbol lookups (e.g. `import uvm_pkg::*` fallback resolution) sets
  `PreprocessorOptions{ .maxIncludeDepth = 0 }` (`src/Indexer.cpp:91`), so it never
  descends into `` `include``d `.svh` files for symbols or macros either.
- Already documented in `docs/features/limitations.md` ("Single Unit Compilations" and
  "Some UVM Code" sections) and in `UVM_CLASS_FRAGMENTS_PLAN.md`, which cites the
  upstream issue this whole feature area traces back to.

## Current workaround (no code change, works today)

- Have every file that uses UVM macros `` `include "uvm_macros.svh"`` **directly**, not
  just rely on a sibling package file having already included it.
- Prefer a real `build`/`.f`-based config (`config.build`/`buildPattern`, gating
  elaborated single-compilation-unit-like mode — see `SlangServer.cpp:522-524`) over
  flags-only explore mode for testbenches. Elaborated/build mode compiles the whole flist
  together and doesn't have this per-file macro isolation problem.

## Candidate directions for a real fix (not committed to either)

1. **Reuse the owner-context graph.** `uvmVerificationLinting`'s class-fragment binding
   (`ServerDriver.cpp:246-321` etc.) already discovers, per fragment file, an "owner" tree
   that includes it. The same graph traversal could in principle be extended to seed a
   fragment's preprocessor macro state from its owner's accumulated `` `define``s before
   parsing — i.e. propagate macros along the same edges already being walked for symbol
   binding, rather than building a wholly separate mechanism. Needs a spike to see whether
   slang's preprocessor exposes a way to seed macro state without a full owner re-parse
   per fragment (perf risk, similar to concerns already raised in
   `UVM_CLASS_FRAGMENTS_PLAN.md`'s "Perf risk" section).
2. **Narrower opt-in single-unit-like pass.** Instead of general cross-file macro
   inheritance, scope a preprocessing pass to a file's own shallow dependency set (the
   same set already computed for `resolveIncludeFragments`/owner binding), effectively
   doing a mini single-unit compile just for that dependency closure. Bounded blast
   radius, but may reintroduce some of the perf concerns single-unit mode was avoided for
   in the first place (`docs/features/limitations.md`, "Single Unit Compilations").
3. **Indexer's `maxIncludeDepth = 0`** (`src/Indexer.cpp:91`) is a related but distinct
   gap (affects workspace-symbol/`import pkg::*` resolution, not per-file diagnostics) —
   worth deciding whether to fix alongside or treat as a separate, smaller follow-up.

## Suggested next step

Before designing further, reproduce against `examples/uvm_apb_sram` (already in the repo,
a real RTL+UVM workspace with a working baseline config): open a testbench file that uses
`` `uvm_component_utils`` without directly including `uvm_macros.svh` itself, confirm the
diagnostic, then evaluate direction 1 vs. 2 above with an actual perf measurement on that
workspace before committing to an approach — this area has a history
(`UVM_CLASS_FRAGMENTS_PLAN.md`'s cited `2ea88e9` regression) of naive fixes causing real
slowdowns, so a spike-and-measure step should come before a full design doc.

## Critical files (for orientation, not a claim these all need changes)

- `src/ServerDriver.cpp:68-153` — `OptionBag` construction, shared across all docs
- `src/ServerDriver.cpp:246-321, 354-377, 543-558, 590-649, 709-716` — existing UVM
  owner-context/cross-file resolution logic to potentially extend
- `src/document/SlangDoc.cpp:36, 160, 165` — per-file shallow parse using the shared
  `OptionBag` but no shared macro state
- `src/Indexer.cpp:91` — separate `maxIncludeDepth = 0` gap for workspace-symbol lookups
- `docs/features/limitations.md`, `UVM_CLASS_FRAGMENTS_PLAN.md` — prior scoping decisions
  and the upstream issue citation
