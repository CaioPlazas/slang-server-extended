// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "utils/ServerHarness.h"

TEST_CASE("ClassInSvhStaysStandaloneByDefault") {
    // The class-in-package fragment binding is gated behind experimental.uvmVerificationLinting,
    // off by default. With default config, a class-only .svh included into a package behaves
    // exactly as before this feature existed: parsed standalone, with no visibility into the
    // owning package's `my_pkg_int_t` typedef.
    ServerHarness server("uvm_pkg_include");

    server.openFile("my_uvm_pkg.sv");

    auto body = server.openFile("my_monitor.svh");
    CHECK(!body.doc->isIncludeFragment());
    CHECK(!body.getDiagnostics().empty());
}

TEST_CASE("ClassInSvhBoundToPackageOwner") {
    // With the flag on, the class-only .svh is bound to the owning package's context instead,
    // so `my_pkg_int_t` (declared as a sibling member of the same package) resolves cleanly.
    ServerHarness server("uvm_pkg_include");
    server.loadConfig(Config{.experimental = Config::Experimental{.uvmVerificationLinting = true}});

    server.openFile("my_uvm_pkg.sv");

    auto body = server.openFile("my_monitor.svh");
    CHECK(body.doc->isIncludeFragment());
    CHECK(body.getDiagnostics().empty());
}

TEST_CASE("ClassFragmentDetectsRealBug") {
    // Confirms the fragment is bound to the OWNER's real context, not just silencing
    // diagnostics: introducing a genuine bug (typo the typedef name) should surface a real
    // diagnostic, same pattern as IncludeFragmentUsesOwnerContext in IncludeFragmentTests.cpp.
    ServerHarness server("uvm_pkg_include");
    server.loadConfig(Config{.experimental = Config::Experimental{.uvmVerificationLinting = true}});

    server.openFile("my_uvm_pkg.sv");
    auto body = server.openFile("my_monitor.svh");
    CHECK(body.getDiagnostics().empty());

    body.after("my_pkg_int_t").write("x");
    body.publishChanges();

    auto diags = body.getDiagnostics();
    REQUIRE(!diags.empty());
    bool foundUnknown = false;
    for (auto& d : diags) {
        if (d.message.find("my_pkg_int_tx") != std::string::npos) {
            foundUnknown = true;
        }
    }
    CHECK(foundUnknown);
}

TEST_CASE("ClassFragmentAdoptedWhenPackageOpensLater") {
    // Reverse order: the class-only .svh is opened before its package owner is known. It
    // should start out broken (standalone, my_pkg_int_t unresolved), then be retroactively
    // adopted once the package is opened.
    ServerHarness server("uvm_pkg_include");
    server.loadConfig(Config{.experimental = Config::Experimental{.uvmVerificationLinting = true}});

    auto body = server.openFile("my_monitor.svh");
    CHECK(!body.getDiagnostics().empty());

    server.openFile("my_uvm_pkg.sv");

    CHECK(body.getDiagnostics().empty());
}

TEST_CASE("ClassSvhIncludedAtUnitScopeStaysStandalone") {
    // Negative control: a class-only .svh `\`include`d directly at compilation-unit ($unit)
    // scope (not nested inside any package/module/etc.) is a different, unrelated usage and
    // must NOT be bound as a fragment even with the flag on.
    ServerHarness server("uvm_pkg_include");
    server.loadConfig(Config{.experimental = Config::Experimental{.uvmVerificationLinting = true}});

    server.openFile("top_unit_includer.sv");
    auto body = server.openFile("unit_class.svh");
    CHECK(!body.doc->isIncludeFragment());
}

TEST_CASE("ClassSvhWithNoOwnerStaysStandalone") {
    // No known owner at all -- must stay standalone regardless of the flag.
    ServerHarness server("uvm_pkg_include");
    server.loadConfig(Config{.experimental = Config::Experimental{.uvmVerificationLinting = true}});

    auto body = server.openFile("my_monitor.svh");
    CHECK(!body.doc->isIncludeFragment());
}
