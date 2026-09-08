// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "utils/ServerHarness.h"

namespace {
bool hasDiagMentioning(const std::vector<lsp::Diagnostic>& diags, std::string_view needle) {
    for (auto& d : diags) {
        if (d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}
} // namespace

TEST_CASE("VirtualInterfaceDepNotPulledByDefault") {
    // Negative control: `apb_driver.svh`'s class is spliced into tb_top.sv's tree by normal
    // `\`include` preprocessing regardless of any experimental flag, and tb_top.sv actually
    // instantiates the class (module-scope `apb_driver drv;` + `new()`/`drive()` in an
    // initial block) so its members are genuinely elaborated -- a class that's never
    // instantiated anywhere is never type-checked at all, so a real instantiation is needed
    // to prove this negative control means anything. The interface referenced via
    // `virtual apb_if vif;` lives in a wholly separate file (apb_if.sv) that's never
    // `\`include`d. Without the experimental flag, that cross-file dependency is never
    // pulled into the shallow compilation, so `apb_if` doesn't resolve.
    //
    // Diagnostics are checked on apb_driver.svh itself, not tb_top.sv: a diagnostic inside
    // `\`include`d text is attributed to the included file's own buffer (SlangDoc's
    // issueDiagnosticsTo filters a doc's semantic diagnostics down to its own buffer), so
    // checking tb_top.sv's diagnostics would always see none from the spliced-in class body.
    ServerHarness server("uvm_vif_dep");

    server.openFile("tb_top.sv");
    auto body = server.openFile("apb_driver.svh");
    auto diags = body.getDiagnostics();
    REQUIRE(!diags.empty());
    CHECK(hasDiagMentioning(diags, "apb_if"));
}

TEST_CASE("VirtualInterfaceDepPulledWhenEnabled") {
    // With the flag on, the virtual-interface member type is followed across files, so
    // apb_if.sv gets pulled into the shallow compilation and the reference resolves cleanly.
    // Diagnostics aren't asserted empty here: this fixture's class also has an unrelated,
    // genuinely-true "unused property"/"never assigned" pair of lints (last_addr is never
    // read, and vif itself -- as opposed to its members -- is never assigned a handle) that
    // have nothing to do with the interface-resolution feature under test. Only the
    // resolution-specific diagnostic ("unknown interface"/apb_if) is asserted absent.
    ServerHarness server("uvm_vif_dep");
    server.loadConfig(Config{.experimental = Config::Experimental{.uvmVerificationLinting = true}});

    server.openFile("tb_top.sv");
    auto body = server.openFile("apb_driver.svh");
    CHECK_FALSE(hasDiagMentioning(body.getDiagnostics(), "apb_if"));

    // Goto-definition on the interface *type name* itself (in "virtual apb_if vif;") proves
    // apb_if.sv was really pulled into the shallow compilation, not just silently tolerated.
    // Note: this deliberately targets the type-name token, not a `vif.psel`-style member
    // access -- general goto-def through a class-typed handle's member access
    // (MemberAccessExpressionSyntax) isn't implemented in ShallowAnalysis at all yet, which is
    // a separate, pre-existing gap outside this feature's scope.
    auto ifaceDefs = body.after("virtual apb_i").getDefinitions();
    REQUIRE(ifaceDefs.size() == 1);
    CHECK(ifaceDefs[0].targetUri.str().ends_with("apb_if.sv"));
}

TEST_CASE("VirtualInterfaceResolvesFromIncludedSvh") {
    // The literal UVM class-in-package-plus-cross-file-interface shape, end-to-end: open
    // the owning module first, then the class-only .svh directly (as a verification
    // engineer editing the class would). This exercises both halves of the unified flag
    // under one scenario: the already-shipped fragment binding (same-design-unit typedef
    // resolves) and the new cross-file resolution (the sibling interface resolves) -- and
    // tb_top.sv's real instantiation is what forces slang to actually elaborate (and thus
    // check) the class's members at all.
    ServerHarness server("uvm_vif_dep");
    server.loadConfig(Config{.experimental = Config::Experimental{.uvmVerificationLinting = true}});

    server.openFile("tb_top.sv");
    auto body = server.openFile("apb_driver.svh");

    CHECK(body.doc->isIncludeFragment());
    CHECK_FALSE(hasDiagMentioning(body.getDiagnostics(), "apb_if"));

    // Same-design-unit typedef (the already-shipped fragment-binding half). Cursor lands
    // mid-token within the *type* reference "apb_pkg_addr_t", not the "last_addr" variable
    // name that follows it.
    auto addrDefs = body.after("apb_pkg_add").getDefinitions();
    REQUIRE(addrDefs.size() == 1);
    CHECK(addrDefs[0].targetUri.str().ends_with("tb_top.sv"));

    // Cross-file interface member (the new half): goto-def on the interface type name.
    auto ifaceDefs = body.after("virtual apb_i").getDefinitions();
    REQUIRE(ifaceDefs.size() == 1);
    CHECK(ifaceDefs[0].targetUri.str().ends_with("apb_if.sv"));
}

TEST_CASE("VirtualInterfaceDepDetectsRealBug") {
    // Confirms the resolution is real binding, not silent suppression: introduce a genuine
    // bug (typo the signal name) and confirm a real diagnostic reappears, mirroring
    // IncludeFragmentUsesOwnerContext in IncludeFragmentTests.cpp. Edits happen on
    // apb_driver.svh directly -- that's where "vif.psel" literally appears in source text.
    ServerHarness server("uvm_vif_dep");
    server.loadConfig(Config{.experimental = Config::Experimental{.uvmVerificationLinting = true}});

    server.openFile("tb_top.sv");
    auto body = server.openFile("apb_driver.svh");
    CHECK_FALSE(hasDiagMentioning(body.getDiagnostics(), "apb_if"));

    body.after("vif.psel").write("x");
    body.publishChanges();

    auto diags = body.getDiagnostics();
    REQUIRE(!diags.empty());
    CHECK(hasDiagMentioning(diags, "pselx"));
}
