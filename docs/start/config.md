# Configuration

The server uses a hierarchical configuration system with three config files:

1. `${workspaceFolder}/.slang/server.json` — workspace config (should be in source control)
2. `~/.slang/server.json` — user config (personal defaults across all projects)
3. `${workspaceFolder}/.slang/local/server.json` — local config (`.slang/local` should be ignored by source control)

Later files override earlier ones for scalar values. Lists (like `index` and `incdirs`) are appended across all files.

### Flags precedence

The `flags` field has special merging behavior. Workspace flags override user flags (only one is used as the base), and local flags are always appended on top. This means you can set shared flags in `.slang/server.json`, and add personal flags (like extra `-D` defines) in `.slang/local/server.json` without overriding the shared ones.

The server watches these config files, .f files that are passed in via flags and `.f` build files for changes, automatically reloading when they are saved.

## Auto-configure

Run **slang: Auto-configure** in VS Code to discover index and include directories and save them in `.slang/server.json`. Neovim and other LSP clients can invoke `slang.autoConfigure` through `workspace/executeCommand` with no arguments; no custom client handler is needed.

When indexing uses the default workspace scan, or a workspace `index` entry lists `.` as a directory, the command chooses directories covering all indexed `.sv`, `.svh`, `.v`, and `.vh` files within the workspace. Separate source trees under `a/b` and `c/d` produce `"index": [{"dirs": ["a/b", "c/d"]}]`. Dense branches stay grouped under a shared parent, while branches with substantial unrelated content split into narrower directories. Each branch can use a different depth. The command reuses its directory crawl, requiring each additional directory to save roughly 5% of the workspace's filesystem entries (at least 32 entries for small workspaces). This favors a short config when dozens of deeper paths would save little overall. Empty workspaces and source files directly in the workspace root prevent narrowing. With only external library roots configured, the command adds a separate workspace entry. Replacing `.` preserves that entry's exclusions and all other index roots, including external library directories. Index settings inherited from user or local configuration and legacy `indexGlobs` / `excludeDirs` settings are preserved.

The command scans the configured workspace index, uses existing quoted includes to identify missing search roots, and saves paths relative to the workspace. It preserves existing settings and include order, skips directories already in `incdirs`, and reports directories with conflicting header names instead of choosing an order for them. Suggestions are checked against each other and against existing configured directories, including user and local `incdirs` and headers outside the workspace index. The server reloads the configuration after saving it. Running the command again adds only newly discovered directories.

Workspace include-directory suggestions are computed, logged, and saved only when the command runs. Editor analysis can add paths to an individual syntax tree using the nearest indexed matches for its unresolved includes. Those paths do not change `incdirs`, other documents, or global include lookup. Equally close matches are resolved in canonical path order. Files supplied through build files continue to use configured include lookup.

When Git is available, auto-configure adds ignored folders to the matching workspace `index[].excludeDirs` entries as plain directory names, such as `"output_files"` and `".Xil"`. Repeated names are saved once. Git interprets nested ignore files, wildcards, and negations during discovery; the saved names use exact comparisons during indexing. Distinct names matched by a Git wildcard, such as `output_files` and `output_files_debug`, become separate entries. Ignored folders found during the crawl are considered even if they contain only non-HDL files. If a name would hide tracked files or re-included indexed sources elsewhere, safe specific paths are retained instead. Existing specific exclusions can be compacted into names, and obsolete wildcard exclusions in the workspace config are replaced on the next run. Ignored files are removed from discovery before choosing index and include directories. New exclusions are then limited to each entry's final index roots: narrowing `.` to `fpga` omits ignored folders found only outside `fpga`. Existing configured exclusions are preserved. File-only matches and folders covered by inherited or legacy index settings are reported for manual review. The server log records matching ignore files, lines, and patterns. Submodules and external index roots are outside this check.

## Config Options

All configuration options are optional and have sensible defaults. In VSCode, there are completions and hovers for `server.json` files. For other editors, you may be able to associate the [config schema](https://github.com/hudson-trading/slang-server/blob/main/clients/vscode/resources/config.schema.json) with these config files to get these features.

---

### `index`

:   **Type:** `list[IndexConfig]`

    ```typescript
    interface IndexConfig {
      /** Directories to index */
      dirs?: string[]
      /** Exact directory names at all path levels, or specific paths starting with './' */
      excludeDirs?: string[] | null
    }
    ```

    Which directories to index; by default it indexes the entire workspace. External library roots supplement this default: listing only directories outside the workspace still indexes the workspace. A configured directory inside the workspace, or an ancestor covering it, replaces the default scan. To index only external libraries, add a separate `{"dirs": []}` entry to disable the implicit workspace scan. Exclusions apply only to the directories in their own entry.

    It's **highly** recommended to configure workspace roots for your repo, especially if there are generated build directories and non-hardware directories that can be skipped.

    Bare `excludeDirs` entries match exact directory names at every level. For example, `"build"` skips folders named `build`, while retaining `build_extra` and source files named `build.sv`. Entries beginning with `./` identify an exact directory relative to the workspace; absolute paths are also supported. For example, `"./fpga/build"` excludes that folder and its descendants, while retaining `tools/build`. Wildcards are not supported.

---

### `flags`

:   **Type:** `string`

    Flags to pass to slang. It uses the underlying driver to parse the flags, however some flags may not be used by the server.

    Use this to configure things like [include paths](https://sv-lang.com/command-line-ref.html#include-paths), [LRM relaxations](https://sv-lang.com/command-line-ref.html#compat-option), configure [warning severity](https://sv-lang.com/command-line-ref.html#clr-warnings) and [specific warnings](https://sv-lang.com/warning-ref.html).

    It's recommended to keep your slang flags in a [flag file](https://sv-lang.com/user-manual.html#command-files), that way it can be shared by both CI and the language server. Another nice setup is having `slang.f` contain your CI flags, then have `slang-server.f` include that file (via `-f path/to/slang.f`), along with more warnings so that more pedantic checks will show as yellow underlines in your editor.

    For preprocessor defines (`-D`), you can also use the **"Add define"** code action: place your cursor on an undefined macro name in an `` `ifdef `` and use the quick fix to automatically add `-D<name>` to `.slang/local/server.json`.

    **Example:** `"-f path/to/slang_flags.f"`

---

### `workDir`

:   **Type:** `string`

    Directory to treat as the effective working directory when resolving relative paths in `flags` (e.g. `-F ./my_flist.f`), mirroring `cd <workDir> && <tool> <flags>`. Relative to the workspace root; absolute paths are also accepted.

    This is useful for ASIC flows where tools are conventionally run from a shared work directory (e.g. `design/work`) rather than the repo root, so flist paths in `flags` are written the same way you'd write them on the command line after `cd`-ing into that directory.

    **Example:** with `"workDir": "work"` and a flist at `work/my_flist.f`, use `"flags": "-F ./my_flist.f"`. If the flist instead lives in a sibling directory `rtl/`, use `"flags": "-F ../rtl/my_flist.f"`.

    **Reaching many RTL subfolders without listing each one:** use `-F` (capital), not `-f`, for the flist reference in `flags` -- `-F` changes directory into the flist's own location before parsing its contents, so relative paths written *inside* the flist (like `-I`) resolve against the flist's directory, not `workDir`. Then, inside the flist itself, use slang's recursive glob syntax to pick up every subdirectory automatically:

    ```
    # design/rtl/dig_top.flist
    -I .../
    dig_top.sv
    ```

    A plain `-I .` only covers the flist's own directory; `-I .../` recurses through every subfolder under it, so newly added subdirectories are picked up automatically with no config changes.

    **If you can't edit the flist** (shared/generated, or your project's convention is that everything in the flist -- including its own source-file entries -- is already written relative to `workDir`, which is exactly what `-f` assumes): put the recursive incdir directly in `flags` instead, ahead of the `-f`. It resolves relative to `workDir` like everything else in `flags`, so the flist itself needs no changes:

    ```json
    {
      "workDir": "design/work",
      "flags": "-I ../design/.../ -f ../design/rtl/dig_top.flist"
    }
    ```

    Note it's `...`, not `*`: a single `*` only matches names within one directory level, so `-I ../design/*` would only reach immediate subdirectories of `design/`, not their subfolders.

---

### `incdirs`

:   **Type:** `list[string]`

    Ordered include search directories. Relative paths resolve from the workspace root, including entries in user and local configuration. Absolute paths are also accepted.

    Directories from `flags` and build files are searched before this list. File-local lookup follows slang's normal rules, including `--incdir-first`. Lists from workspace, user, and local configuration are appended in that order. System includes (`<...>`) continue to use system include directories.

    **Example:** `"incdirs": ["rtl/include", "verification/include"]`

---

### `indexingThreads`

:   **Type:** `integer`

    **Default:** `0` (auto-detect)

    Thread count to use for indexing. When set to 0, automatically detects the optimal number of threads based on system capabilities.

---

### `build`

:   **Type:** `string`

    Build file to automatically open on start.

    **Example:** `"./build/compile.f"`

---

### `buildPattern`

:   **Type:** `string` (glob pattern)

    Build file pattern used to find a `.f` file given the name of a waveform file. For example, `/tmp/{}.fst` with `builds/{}.f` looks for `build/foo.f` to load the compilation.

    If omitted and no other build source is configured, it defaults to matching all `.f` files in the workspace.

    **Example:** `"builds/{}.f"`

---

### `builds`

:   **Type:** `list[Build]`

    ```typescript
    interface Build {
      /** Optional name used for generated build files and UI labels */
      name?: string
      /** Glob pattern to find build files, like .f files or makefiles */
      glob?: string
      /** Optional command that produces .f content on stdout when passed the selected file */
      command?: string
    }
    ```

    Provides additional build-file sources in the VSCode client. Each entry matches files with `glob`.

    If `command` is omitted, the matched files are treated as existing `.f` files and can be selected directly.

    If `command` is provided, VSCode parses it as an executable plus fixed arguments, without invoking a shell. The matched file path is appended as the final argument, and the command should write `.f` content to stdout. Quote paths or arguments with spaces as needed. Relative executables are resolved from the workspace root, and the command runs with the workspace root as its working directory. The generated `.f` file is written under `.slang/local/builds/` using a stable path-based filename that keeps as much of the source path as will fit. If `name` is set, it is used as the readable prefix for the generated filename and the selection UI.

    When a command-backed source file changes, the command is automatically re-run and the compilation is reloaded.

    **Example:**

    ```json
    "builds": [
      {
        "glob": "build/**/*.f"
      },
      {
        "name": "synth",
        "glob": "**/Makefile",
        "command": "scripts/make_dotf.py"
      }
    ]
    ```

---

### `resolveIncludeFragments`

:   **Type:** `boolean`

    **Default:** `true`

    When a file that's only ever `` `include``d elsewhere (e.g. a `.vh`/`.svh` fragment meant to be spliced into a module body, not a standalone compilation unit) is opened directly, analyze it in the context of the file(s) that include it instead of parsing it on its own. This avoids spurious "expected a module" style errors on such files, and gives real diagnostics based on the including module(s)' declarations. If the same fragment is included by more than one file, diagnostics are the union of up to 8 including contexts (capped for performance; features other than diagnostics, like goto-definition and hover, use only the first-found owner).

    Set to `false` to restore the previous behavior, where such files are always parsed standalone.

    **In the 0.7.x pre-releases this setting has no effect:** upstream's include-context support (see `incdirs` above) does this job instead, analysing a multiply-included header from one including file rather than the union.

---

### `wcpCommand`

:   **Type:** `string`

    Waveform viewer command where `{}` will be replaced with the WCP port.

    **Example:** `"surfer --wcp-initiate {}"`

---

### `hovers`

:   **Type:** `HoverConfig`

    ```typescript
    interface HoverConfig {
      /** How leading doc comments are rendered in hovers */
      docCommentFormat?: "plaintext" | "markdown" | "raw"  // default: "markdown"
    }
    ```

    Controls how hover popups present leading comments

    - **`docCommentFormat`**: How to render the leading doc comment of the hovered symbol.
        - `"markdown"` (default): strip `//` / `/* */` markers and render the contents as markdown, so things like `**bold**`, links, and lists render in the hover.
        - `"plaintext"`: strip markers but escape markdown characters so the comment text appears literally — useful when comments contain characters like `*`, `_`, or `<tag/>` that you don't want rendered.
        - `"raw"`: don't strip anything — show the comment text and the declaration together in a single SystemVerilog code block, exactly as they appear in source. Useful when comments contain code-like content (e.g. timing diagrams, ASCII tables) that should not be reflowed by a markdown renderer.

    **Example:**

    ```json
    "hovers": {
      "docCommentFormat": "plaintext"
    }
    ```

---

### `inlayHints`

:   **Type:** `InlayHints`

    ```typescript
    interface InlayHints {
      /** Hints for port types */
      portTypes?: boolean           // default: false
      /** Hints for inferred assignment pattern types */
      assignmentPatternTypes?: boolean // default: true
      /** Hints for active instance parameter and localparam values */
      activeParameterValues?: boolean  // default: true
      /** Hints for names of ordered ports and params */
      orderedInstanceNames?: boolean // default: true
      /** Hints for port names in wildcard (.*) ports */
      wildcardNames?: boolean       // default: true
      /** Function argument hints: 0=off, N=only calls with >=N args */
      funcArgNames?: integer        // default: 2
      /** Macro argument hints: 0=off, N=only calls with >=N args */
      macroArgNames?: integer       // default: 2
    }
    ```

    Controls inline hints displayed in the editor for things like ordered arguments, wildcard ports, and others.

    - **`portTypes`**: Show type hints on ports. Off by default.
    - **`assignmentPatternTypes`**: Show the inferred struct or union type before untyped assignment patterns.
    - **`activeParameterValues`**: Show parameter and localparam values from the active instance.
    - **`orderedInstanceNames`**: Show parameter/port name hints on ordered (positional) instance connections.
    - **`wildcardNames`**: Show port name hints on wildcard (`.*`) connections.
    - **`funcArgNames`**: Show argument name hints on function calls. Set to `0` to disable, or `N` to only show hints for calls with N or more arguments.
    - **`macroArgNames`**: Show argument name hints on macro invocations. Set to `0` to disable, or `N` to only show hints for calls with N or more arguments.

---

## Example Configuration

### Workspace config (`.slang/server.json`)

Shared across the team, checked into source control:

```json
{
  "flags": "-f tools/slang/slang-server.f",
  "index": [
    {
      "dirs": ["fpga/src", "fpga/tb"],
      "excludeDirs": ["build", "synth"]
    }
  ],
  "buildPattern": "builds/**/*.f",
  "builds": [
    {
      "glob": "build/generated/**/*.f"
    },
    {
      "name": "synth",
      "glob": "**/Makefile",
      "command": "scripts/makedotf.py"
    }
  ],
  "indexingThreads": 4
}
```

For more on direct and command-backed build sources, see [`builds`](#builds).

### User config (`~/.slang/server.json`)

Personal defaults that apply to all projects without a workspace config:

```json
{
  "flags": "-Wextra",
  "inlayHints": {
    "orderedInstanceNames": true,
    "funcArgNames": 3
  }
}
```

If the workspace config above has `flags`, it takes precedence over this one (they are not combined). If the workspace config has no `flags`, these user flags are used as the base.

### Local config (`.slang/local/server.json`)

Personal overrides for this workspace, not checked in (add `.slang/local` to `.gitignore`):

```json
{
  "flags": "-DSIM_MODE -DDEBUG_LEVEL=2",
  "build": "./builds/my_top.f"
}
```

These flags are **appended** to whichever base flags won (workspace or user), so the final flags in this example would be `-f tools/slang/slang-server.f -DSIM_MODE -DDEBUG_LEVEL=2`. The `build` field overrides any previous value since it's a scalar.
