// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "utils/ServerHarness.h"

// Tests for UVM_MACRO_INHERITANCE_PLAN.md: cross-file macro inheritance for shallow/
// explore-mode UVM code, gated behind experimental.uvmVerificationLinting. Fixture:
// tests/data/uvm_macro_include.
//
// The archetypal UVM pattern -- a class-only .svh `\`include`d inside a package that itself
// `\`include`s uvm_macros.svh first -- already worked before any code change here: the
// existing fragment mechanism (ClassInSvhBoundToPackageOwner in UvmClassFragmentTests.cpp)
// borrows the owner's already-preprocessed tree wholesale, so macros the owner picked up via
// its own `\`include` chain are already expanded. See ClassFragmentSeesMacroDefinedEarlierInOwner
// and ClassFragmentOpenedBeforeOwnerSeesMacro below, which lock that in as a regression guard.
//
// The confirmed, actually-fixed gap is narrower: a class-only .svh `\`include`d at $unit scope
// (not nested in a design unit) is deliberately excluded from full symbol-splice binding (see
// ClassSvhIncludedAtUnitScopeStaysStandalone in UvmClassFragmentTests.cpp -- splicing symbols
// from a $unit-scope include is a different, unrelated usage). Before this change, that also
// meant zero macro carryover, even though the macro was genuinely defined earlier in the same
// owner's continuous parse. SlangDoc::addMacroOwner/collectInheritedMacros (see
// ServerDriver::resolveDocument/openDocument/adoptOrphanFragments) now inherits macro state --
// but not symbols -- from such owners, since macros don't carry the same scoping risk a full
// splice does. See ClassFragmentAtUnitScopeNowSeesMacro below.

TEST_CASE("ClassFragmentSeesMacroDefinedEarlierInOwner") {
    // my_agent_pkg.sv: `include "uvm_macros.svh" (defines MY_COMPONENT_UTILS) then
    // `include "my_component.svh" (a class-only file that invokes `MY_COMPONENT_UTILS
    // without defining or including it itself). If the fragment mechanism already borrows
    // the owner's fully-preprocessed tree, this macro invocation should already be expanded
    // and produce no "unknown macro" diagnostic.
    ServerHarness server("uvm_macro_include");
    server.loadConfig(Config{.experimental = Config::Experimental{.uvmVerificationLinting = true}});

    server.openFile("my_agent_pkg.sv");

    auto body = server.openFile("my_component.svh");
    CHECK(body.doc->isIncludeFragment());

    auto diags = body.getDiagnostics();
    bool foundUnknownMacro = false;
    for (auto& d : diags) {
        if (d.message.find("unknown macro") != std::string::npos) {
            foundUnknownMacro = true;
        }
    }
    CAPTURE(foundUnknownMacro);
    CHECK(!foundUnknownMacro);
}

TEST_CASE("ClassFragmentOpenedBeforeOwnerSeesMacro") {
    // Reverse order, mirroring ClassFragmentAdoptedWhenPackageOpensLater: the class-only
    // .svh is opened before its package owner is known, then the owner is opened
    // afterwards. Confirms retroactive adoption also carries macro visibility, not just
    // symbol visibility.
    ServerHarness server("uvm_macro_include");
    server.loadConfig(Config{.experimental = Config::Experimental{.uvmVerificationLinting = true}});

    auto body = server.openFile("my_component.svh");
    CHECK(!body.doc->isIncludeFragment());

    server.openFile("my_agent_pkg.sv");

    CHECK(body.doc->isIncludeFragment());
    auto diags = body.getDiagnostics();
    bool foundUnknownMacro = false;
    for (auto& d : diags) {
        if (d.message.find("unknown macro") != std::string::npos) {
            foundUnknownMacro = true;
        }
    }
    CAPTURE(foundUnknownMacro);
    CHECK(!foundUnknownMacro);
}

TEST_CASE("ClassFragmentAtUnitScopeNowSeesMacro") {
    // top_unit_macro_includer.sv `include`s uvm_macros.svh (defines MY_COMPONENT_UTILS) then
    // `include`s my_component.svh -- but both at $unit (compilation-unit) scope, not nested
    // inside a module/package/etc. This owner is correctly excluded from full symbol-splice
    // binding (a $unit-scope include is a different, unrelated usage -- see
    // ClassSvhIncludedAtUnitScopeStaysStandalone), so my_component.svh still parses its own,
    // independent syntax tree. But it should now inherit the owner's macros: the doc stays a
    // real standalone parse (not spliced into the owner's tree), while the previously-unknown
    // `MY_COMPONENT_UTILS invocation resolves cleanly.
    ServerHarness server("uvm_macro_include");
    server.loadConfig(Config{.experimental = Config::Experimental{.uvmVerificationLinting = true}});

    server.openFile("top_unit_macro_includer.sv");

    auto body = server.openFile("my_component.svh");
    CHECK(!body.doc->isIncludeFragment());

    auto diags = body.getDiagnostics();
    bool foundUnknownMacro = false;
    for (auto& d : diags) {
        if (d.message.find("unknown macro") != std::string::npos) {
            foundUnknownMacro = true;
        }
    }
    CAPTURE(foundUnknownMacro);
    CHECK(!foundUnknownMacro);
}

TEST_CASE("ClassFragmentAtUnitScopeAdoptsMacroWhenOwnerOpensLater") {
    // Reverse order of ClassFragmentAtUnitScopeNowSeesMacro: my_component.svh is opened
    // before its $unit-scope owner is known, then the owner opens afterwards. Covers the
    // adoptOrphanFragments macro-only path.
    ServerHarness server("uvm_macro_include");
    server.loadConfig(Config{.experimental = Config::Experimental{.uvmVerificationLinting = true}});

    auto body = server.openFile("my_component.svh");
    CHECK(!body.doc->isIncludeFragment());
    CHECK(!body.getDiagnostics().empty());

    server.openFile("top_unit_macro_includer.sv");

    CHECK(!body.doc->isIncludeFragment());
    auto diags = body.getDiagnostics();
    bool foundUnknownMacro = false;
    for (auto& d : diags) {
        if (d.message.find("unknown macro") != std::string::npos) {
            foundUnknownMacro = true;
        }
    }
    CAPTURE(foundUnknownMacro);
    CHECK(!foundUnknownMacro);
}

TEST_CASE("ClassFragmentWithoutMacroOwnerStillReportsUnknownMacro") {
    // Negative control: my_agent_pkg_no_macros.sv `include`s my_component_no_macros.svh
    // (same `MY_COMPONENT_UTILS invocation) WITHOUT ever including uvm_macros.svh. This
    // must legitimately still report an unknown-macro diagnostic -- otherwise the earlier
    // tests would be vacuously passing (e.g. because diagnostics aren't being collected at
    // all for fragments), not actually proving macro inheritance works.
    ServerHarness server("uvm_macro_include");
    server.loadConfig(Config{.experimental = Config::Experimental{.uvmVerificationLinting = true}});

    server.openFile("my_agent_pkg_no_macros.sv");

    auto body = server.openFile("my_component_no_macros.svh");
    CHECK(body.doc->isIncludeFragment());

    auto diags = body.getDiagnostics();
    bool foundUnknownMacro = false;
    for (auto& d : diags) {
        if (d.message.find("unknown macro") != std::string::npos) {
            foundUnknownMacro = true;
        }
    }
    CHECK(foundUnknownMacro);
}
