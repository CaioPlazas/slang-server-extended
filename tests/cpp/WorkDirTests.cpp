// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "utils/ServerHarness.h"

using namespace server;

TEST_CASE("workDir resolves relative flags paths as if launched from that directory") {
    // work/my_flist.f contains "../rtl/top.sv" -- only resolvable if the driver's
    // effective cwd while parsing `flags` is tests/data/workdir_test/work, not the
    // workspace root (tests/data/workdir_test).
    ServerHarness server("workdir_test");

    Config cfg;
    cfg.workDir = "work";
    cfg.flagsByFile.value().push_back({"test", "-F ./my_flist.f"});
    server.loadConfig(cfg);

    // Check the flags-driven parse itself picked up the file (not just that it happens to
    // also be independently parseable standalone via a later getDocument() fallback).
    REQUIRE(server.m_driver->docs.size() == 1);

    auto topPath = fs::current_path() / "rtl" / "top.sv";
    auto doc = server.m_driver->getDocument(URI::fromFile(topPath.string()));
    REQUIRE(doc != nullptr);
    CHECK(!doc->getSymbols().empty());
}

TEST_CASE("workDir + -F lets a flist's own -I .../ reach deeply nested incdirs") {
    // rtl/dig_top.flist (found via workDir="work" + "-F ../rtl/dig_top.flist") contains
    // "-I .../" -- only expanded relative to rtl/ (not work/) because -F chdirs into the
    // flist's own directory while parsing its content. That recursive incdir is what lets
    // top_with_include.sv's `include "header.svh" find the file several folders down, in
    // rtl/sub/deep/, without listing every subdirectory explicitly.
    ServerHarness server("workdir_test");

    Config cfg;
    cfg.workDir = "work";
    cfg.flagsByFile.value().push_back({"test", "-F ../rtl/dig_top.flist"});
    server.loadConfig(cfg);

    auto topPath = fs::current_path() / "rtl" / "top_with_include.sv";
    auto doc = server.m_driver->getDocument(URI::fromFile(topPath.string()));
    REQUIRE(doc != nullptr);

    auto analysis = doc->getAnalysis();
    auto& topInstances = analysis->getCompilation()->getRoot().topInstances;
    REQUIRE(!topInstances.empty());
    // FOUND_ME only exists if header.svh (in rtl/sub/deep/) was actually located and spliced
    // in -- proves the recursive incdir search, not just that the module parsed on its own.
    CHECK(topInstances[0]->body.find("FOUND_ME") != nullptr);
}

TEST_CASE("workDir + -f + a recursive -I in flags reaches nested incdirs without touching the flist") {
    // dig_top_no_incdir.flist has no -I of its own (some projects can't/won't edit their
    // flist to add one, and deliberately use lowercase -f so everything inside it -- if it
    // had any relative paths -- would resolve against workDir, not the flist's own
    // directory). Proves the recursive incdir can instead live directly in `flags`,
    // resolved relative to workDir like everything else in `flags`, with the flist
    // completely unmodified.
    ServerHarness server("workdir_test");

    Config cfg;
    cfg.workDir = "work";
    cfg.flagsByFile.value().push_back(
        {"test", "-I ../rtl/.../ -f ../rtl/dig_top_no_incdir.flist"});
    server.loadConfig(cfg);

    auto topPath = fs::current_path() / "rtl" / "top_with_include.sv";
    auto doc = server.m_driver->getDocument(URI::fromFile(topPath.string()));
    REQUIRE(doc != nullptr);

    auto analysis = doc->getAnalysis();
    auto& topInstances = analysis->getCompilation()->getRoot().topInstances;
    REQUIRE(!topInstances.empty());
    CHECK(topInstances[0]->body.find("FOUND_ME") != nullptr);
}
