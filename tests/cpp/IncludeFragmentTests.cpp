// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "utils/ServerHarness.h"

TEST_CASE("IncludeFragmentUsesOwnerContext") {
    // Regression for: opening a `.vh` fragment that's only ever `\`include`d into a module body
    // used to parse it as a standalone top-level file, producing bogus "expected a module"
    // parse errors instead of real diagnostics.
    ServerHarness server("vh_include");

    // Establish top_solo.v first, so solo_body.vh has a known owner when opened directly.
    server.openFile("top_solo.v");

    // Opening the fragment directly should not produce bogus parse errors -- its content should
    // be understood in the context of its owner.
    auto body = server.openFile("solo_body.vh");
    CHECK(body.getDiagnostics().empty());

    // Introduce a real bug: reference an undeclared identifier ("din" -> "dinx").
    body.after("din").write("x");
    body.publishChanges();

    auto diags = body.getDiagnostics();
    REQUIRE(!diags.empty());

    bool foundUndeclared = false;
    for (auto& d : diags) {
        if (d.message.find("dinx") != std::string::npos) {
            foundUndeclared = true;
        }
    }
    CHECK(foundUndeclared);
}

TEST_CASE("IncludeFragmentAdoptedWhenOwnerOpensLater") {
    // Reverse of IncludeFragmentUsesOwnerContext: the fragment is opened BEFORE its owner has
    // ever been opened/parsed -- the realistic editor scenario (e.g. clicking the header first,
    // or a workspace restoring tabs in arbitrary order). Binding used to happen only once, at
    // fragment-open time, so this left it permanently standalone/broken. It should instead be
    // retroactively adopted once the owner is opened, clearing its bogus diagnostics.
    // (Uses the text-open overload since the disk-reading one asserts non-empty symbols, which
    // doesn't hold for the initial orphaned/standalone parse.)
    ServerHarness server("vh_include");

    auto body = server.openFile("solo_body.vh", "assign dout_custom = din & clk;\n");
    CHECK(!body.getDiagnostics().empty());

    server.openFile("top_solo.v");

    CHECK(body.getDiagnostics().empty());
}

TEST_CASE("IncludeFragmentUnionAcrossOwners") {
    // Both top_multi_a.v (which declares `din`) and top_multi_b.v (which doesn't) `\`include`
    // the same body. Diagnostics for the fragment should reflect the union of both contexts: an
    // undeclared-identifier error from top_multi_b's perspective should surface even though
    // top_multi_a alone is clean.
    ServerHarness server("vh_include");

    server.openFile("top_multi_a.v");
    server.openFile("top_multi_b.v");

    auto body = server.openFile("multi_body.vh");
    auto diags = body.getDiagnostics();

    bool foundUndeclared = false;
    for (auto& d : diags) {
        if (d.message.find("din") != std::string::npos) {
            foundUndeclared = true;
        }
    }
    CHECK(foundUndeclared);
}

TEST_CASE("IncludeFragmentDedupesIdenticalDiagnosticsAcrossOwners") {
    // If a fragment has a problem that's identical from every owner's perspective (e.g. a plain
    // typo unrelated to any one module's declarations), it should be reported once, not once per
    // owner.
    ServerHarness server("vh_include");

    server.openFile("top_multi_a.v");
    server.openFile("top_multi_b.v");

    auto body = server.openFile("multi_body.vh");
    body.replaceAll("assign dout_custom = totally_undeclared_signal;\n");
    body.publishChanges();

    auto diags = body.getDiagnostics();
    int count = 0;
    for (auto& d : diags) {
        if (d.message.find("totally_undeclared_signal") != std::string::npos) {
            count++;
        }
    }
    CHECK(count == 1);
}

TEST_CASE("IncludeFragmentBoundWhenPrecreatedByNavigation") {
    // ServerDriver::getDocument() is used by many navigation/analysis routes (goto-definition
    // targets, hover targets, reference scans, getDependentDocs) to resolve an arbitrary file
    // path to a SlangDoc. Unlike openDocument(), it does NOT consult findIncludeOwners() on a
    // cache miss -- it just parses the file standalone via SlangDoc::open() and inserts the
    // (broken, for a fragment) result into `docs`. This simulates that: a navigation-style hit
    // on the fragment's path happens BEFORE the editor's real didOpen for it arrives.
    //
    // Once such a standalone doc exists in `docs`, openDocument()'s reuse branch
    // (`docIter->second->textMatches(text)`) blindly reuses it without ever re-checking
    // findIncludeOwners(), so the bogus standalone "expected a module" diagnostics get published
    // for a file the editor legitimately opened.
    ServerHarness server("vh_include");
    server.openFile("top_solo.v");

    auto fragmentUri = URI::fromFile(fs::current_path() / "solo_body.vh");
    server.m_driver->getDocument(fragmentUri);

    // Editor now opens it for real, with text identical to what's on disk -- textMatches()
    // returns true, so this hits the reuse branch.
    auto body = server.openFile("solo_body.vh", "assign dout_custom = din & clk;\n");
    CHECK(body.getDiagnostics().empty());
}

TEST_CASE("IncludeFragmentOrphanedByNavigationStaysBrokenAfterUnrelatedSave") {
    // Hole (b): in build mode, ServerDriver::updateDoc's SAVE branch clears ALL published
    // diagnostics and re-issues parse diagnostics for every doc in `docs`, skipping only docs
    // already flagged as fragments. A standalone doc created via getDocument() (see
    // IncludeFragmentBoundWhenPrecreatedByNavigation above) is never flagged as a fragment, so
    // its bogus parse diagnostics get republished on every save of ANY file in the build -- even
    // though the user never opened that file, and even though its real owner is part of the same
    // build.
    //
    // NOTE: openDocument() unconditionally calls adoptOrphanFragments() for whatever doc it just
    // opened, even when reusing an already-loaded build doc via the textMatches() shortcut -- so
    // opening/saving the fragment's OWN owner (top_solo.v) would incidentally heal this orphan as
    // a side effect and mask the bug being tested here. To isolate the hole, this saves a
    // *different* top-level file that's part of the same build but doesn't `\`include`
    // solo_body.vh (top_multi_a.v, which only includes multi_body.vh), so adoptOrphanFragments
    // never gets a chance to run for it.
    ServerHarness server("vh_include");
    server.setBuildFile("solo.f");

    // Simulate a navigation/reference-scan hit on the fragment before it's ever opened, and
    // before its owner (top_solo.v) is ever opened by the editor either.
    auto fragmentUri = URI::fromFile(fs::current_path() / "solo_body.vh");
    server.m_driver->getDocument(fragmentUri);

    auto other = server.openFile("top_multi_a.v");
    other.save();

    CHECK(server.client.getDiagnostics(fragmentUri).empty());
}

TEST_CASE("IncludeFragmentBehaviorCanBeDisabled") {
    // The `resolveIncludeFragments` config toggle restores the previous behavior: fragments are
    // parsed standalone again, so opening one directly produces the old bogus parse errors.
    // (Uses the text-open overload since the disk-reading one asserts non-empty symbols, which
    // doesn't hold once the fragment is deliberately parsed standalone again.)
    ServerHarness server("vh_include");
    server.loadConfig(Config{.resolveIncludeFragments = false});

    server.openFile("top_solo.v");

    auto body = server.openFile("solo_body.vh", "assign dout_custom = din & clk;\n");
    CHECK(!body.getDiagnostics().empty());
}

TEST_CASE("DualPurposeFileNotConvertedOnFreshOpen") {
    // Regression: openDocument()'s fresh-open branch used to bind ANY file with a known owner to
    // that owner unconditionally, without first checking whether the file's standalone parse was
    // genuinely empty/garbage -- unlike getDocument(), the reuse branch, and adoptOrphanFragments,
    // which all already had that guard. A "dual-purpose" file -- one that's both `\`include`d
    // elsewhere AND perfectly valid as its own standalone top-level file (e.g. a full module
    // textually included into a wrapper) -- opened directly for the first time while its includer
    // is already open would get wrongly forced into fragment mode, silently breaking
    // find-references/goto-definition for its own top-level symbols.
    ServerHarness server("vh_include");

    // Establish the owner first, so dual_purpose.v has a known owner by the time it's opened.
    server.openFile("top_dual_owner.v");

    // Opening the dual-purpose file directly for the first time should NOT bind it as a
    // fragment -- it's a real standalone file with its own module.
    auto body = server.openFile("dual_purpose.v");
    CHECK(!body.doc->isIncludeFragment());

    auto syms = body.getSymbolTree();
    REQUIRE(!syms.empty());
    CHECK(syms[0].name == "dual_purpose");
}
