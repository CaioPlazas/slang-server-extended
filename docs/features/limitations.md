
# Limitations

**Potential Shallow Compilation Issues**

In shallow compilations some nodes may be invalid due to unset parameters, missing defines, etc. Therefore some expressions may not be visited by slang and cause some false positives, especially with unused checking. These can often be fixed, so please raise an issue if you see one. Before raising an issue, please check whether it shows up via a normal slang command to determine which repo to raise the issue in.

**Untaken Ifdef Branches**

Features may be limited in the context of untaken ifdef branches. To minimize this, it's recommended to encode these ifdefs in package parameters, then use generate blocks in the hdl code. Or if these are single branch ifdefs, include the most permissible defines in your server config.

**Deep Hierarchical References**

Shallow compilations only load the directly referenced syntax trees, and only load more transitive trees through packages. This may therefore cause issues with deep hierarchical references.

**Single Unit Compilations**

Designs that use slang's [`--single-unit`](https://sv-lang.com/command-line-ref.html) are not be supported. The main effect of using this flag is that macros are not inherited, so with the server those macros will show as missing. The `--single-unit` flag means that all files are essentially concatenated before being sent to tools. Even if `slang-server` grabbed these dependencies, it would make file and compilation updates take much longer, so it's generally preferred to switch away from using this flag.

**Some UVM Code**

The common UVM practice where classes are `` `include``d in a package results in poor support for those classes: a class-only `.svh` is parsed on its own, so sibling declarations in the owning package do not resolve, and macros the owner defined before including it are not visible.

An experimental opt-in, `experimental.uvmVerificationLinting`, addressed part of this in v0.5.0 through v0.5.4. **It was withdrawn in v0.5.5** -- in real testbench use it misbehaved often enough not to be worth keeping, and it never handled the harder cases anyway (types declared in a file unreachable through the owner's own scope, or macro inheritance across files sharing no `` `include`` relationship). If you had it set in a `.slang/server.json`, it is now an unknown key and is ignored; remove it to silence the schema warning. See [upstream issue #135](https://github.com/hudson-trading/slang-server/issues/135) for ongoing discussion.

`resolveIncludeFragments`, which binds genuinely empty/`` `include``-only fragments to their owners, is unaffected and remains on by default.
