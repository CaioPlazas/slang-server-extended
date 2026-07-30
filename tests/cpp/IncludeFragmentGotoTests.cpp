// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "utils/ServerHarness.h"

using namespace slang;

// Regression: opening a `.vh`/`.svh` fragment (a body that's only ever `\`include`d into a module)
// made goto-definition, hover, references, etc. silently return nothing for every token in the
// file. The fragment is analyzed spliced into its owner's syntax tree, but the SyntaxIndexer keyed
// its token collection on the owner tree's *main* buffer, so none of the fragment's own tokens were
// ever indexed -- getWordTokenAt() returned null for any position inside the fragment. The indexer
// now keys on the fragment's included buffer, so position-based lookups resolve against the
// fragment's own text.

TEST_CASE("IncludeFragmentGotoModuleName") {
    // ctrl+click on the *type* of a module instantiated inside a `.vh` fragment should jump to that
    // module's own .v file -- this is the exact regression the user reported.
    ServerHarness server("vh_goto");

    // Owner opened first so body.vh has a known owner and is treated as a fragment.
    server.openFile("top.v");

    auto body = server.openFile("body.vh");
    REQUIRE(body.doc->isIncludeFragment());

    auto defs = body.before("sub u_sub").getDefinitions();
    REQUIRE(!defs.empty());
    CHECK(defs[0].targetUri.str().find("sub.v") != std::string::npos);
}

TEST_CASE("IncludeFragmentGotoLocalSignal") {
    // A signal declared and referenced inside the fragment should also resolve -- confirms the fix
    // restores position-based lookup generally, not just for module names.
    ServerHarness server("vh_goto");
    server.openFile("top.v");

    auto body = server.openFile("body.vh");
    REQUIRE(body.doc->isIncludeFragment());

    // The reference on the RHS of `assign dout_custom = dout_sub;`.
    auto defs = body.after("dout_custom = ").getDefinitions();
    REQUIRE(!defs.empty());
    // Resolves to the `wire dout_sub;` declaration at the top of the fragment.
    CHECK(defs[0].targetUri.str().find("body.vh") != std::string::npos);
    CHECK(defs[0].targetRange.start.line == 0);
}

TEST_CASE("IncludeFragmentGotoOwnerSignal") {
    // The most common real scenario: a wire/port declared in the *owner* module's header and used
    // inside the included body. Goto on such a reference should jump back into the owner .v file.
    ServerHarness server("vh_goto");
    server.openFile("top.v");

    auto body = server.openFile("body.vh");
    REQUIRE(body.doc->isIncludeFragment());

    // The LHS `dout_custom` of the assign -- declared as `wire dout_custom;` in top.v.
    auto defs = body.before("dout_custom = ").getDefinitions();
    REQUIRE(!defs.empty());
    CHECK(defs[0].targetUri.str().find("top.v") != std::string::npos);
}
