// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "utils/ServerHarness.h"

using namespace server;

// Reproduction scaffold for a user-reported regression: opening a `.vh` fragment in a real
// project (nested flists via `-f`, `--single-unit`) logs "Analyzed <path> with tops: " (empty)
// instead of resolving in the context of its owner. tests/data/vh_singleunit mirrors the
// reported project's shape:
//   verilog/slang-server.f          (top flist, opened via -F, has `+incdir+.` and `-f` into...)
//   verilog/module_b/slang-server-b.f   (nested flist, referenced via lowercase -f)
//   verilog/module_a.v `includes custom_module_a.vh, instantiates module_b
//   verilog/module_b/module_b.v `includes custom_module_b.vh
//
// This intentionally does NOT yet assert the "expected"/fixed behavior -- see the two TEST_CASEs
// below, which record what actually happens with and without `--single-unit`.

TEST_CASE("SingleUnit: fragment owned by the top-level flist file resolves correctly") {
    ServerHarness server("vh_singleunit");

    Config cfg;
    cfg.flagsByFile.value().push_back({"test", "--single-unit -F ./verilog/slang-server.f"});
    server.loadConfig(cfg);

    auto body = server.openFile("verilog/custom_module_a.vh");
    CHECK(body.doc->isIncludeFragment());

    auto analysis = body.doc->getAnalysis();
    auto& topInstances = analysis->getCompilation()->getRoot().topInstances;
    CHECK(!topInstances.empty());
}

TEST_CASE("SingleUnit: fragment owned by a file reached through a nested -f flist") {
    ServerHarness server("vh_singleunit");

    Config cfg;
    cfg.flagsByFile.value().push_back({"test", "--single-unit -F ./verilog/slang-server.f"});
    server.loadConfig(cfg);

    auto body = server.openFile("verilog/module_b/custom_module_b.vh");
    CHECK(body.doc->isIncludeFragment());

    auto analysis = body.doc->getAnalysis();
    auto& topInstances = analysis->getCompilation()->getRoot().topInstances;
    CHECK(!topInstances.empty());
}

TEST_CASE("SingleUnit: fragment owned by a nested-flist file resolves without --single-unit") {
    ServerHarness server("vh_singleunit");

    Config cfg;
    cfg.flagsByFile.value().push_back({"test", "-F ./verilog/slang-server.f"});
    server.loadConfig(cfg);

    auto body = server.openFile("verilog/module_b/custom_module_b.vh");
    CHECK(body.doc->isIncludeFragment());

    auto analysis = body.doc->getAnalysis();
    auto& topInstances = analysis->getCompilation()->getRoot().topInstances;
    CHECK(!topInstances.empty());
}
