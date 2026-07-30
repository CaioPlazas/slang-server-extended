// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT

#include "utils/ServerHarness.h"

// Tests for SINGLE_UNIT_MACROS_PLAN.md: experimental.singleUnitMacros, a driver-level pool of
// every `\`define` found anywhere in the build, fed into every doc's parse. Unlike
// experimental.uvmVerificationLinting / m_macroOwners (see UvmMacroInheritanceTests.cpp), this
// doesn't require any `\`include` relationship between the defining file and the file that uses
// the macro -- only that both are part of the same build (flist). Fixture:
// tests/data/single_unit_macros.
//
// macro_holder_pkg.sv `\`include`s macros.svh (defining MY_COMPONENT_UTILS) -- this is how the
// macro enters the build's pool. tb_top.sv and inline_pkg.sv both invoke the macro without
// `\`include`ing anything themselves, covering the two file shapes from the original bug report
// (a bare module, and an inline package+class). unresolved.sv invokes a macro that's genuinely
// undefined anywhere in the build, proving the feature resolves real macros rather than
// suppressing all "unknown macro" diagnostics outright.

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

TEST_CASE("SingleUnitMacrosOffTbTopReportsUnknownMacro") {
    // Locks current (pre-feature) behavior: with the toggle off, a module with no `\`include`
    // has no way to see a macro defined in a sibling flist file.
    ServerHarness server("single_unit_macros");
    server.setBuildFile("build.f");

    auto body = server.openFile("tb_top.sv");
    CHECK(hasDiagMentioning(body.getDiagnostics(), "MY_COMPONENT_UTILS"));
}

TEST_CASE("SingleUnitMacrosOnTbTopClean") {
    ServerHarness server("single_unit_macros");
    server.loadConfig(
        Config{.build = "build.f", .experimental = Config::Experimental{.singleUnitMacros = true}});

    auto body = server.openFile("tb_top.sv");
    CHECK_FALSE(hasDiagMentioning(body.getDiagnostics(), "MY_COMPONENT_UTILS"));
}

TEST_CASE("SingleUnitMacrosOnInlinePkgClean") {
    // inline_pkg.sv is the shape from the original bug report: a package/class file that
    // classifyStandaloneParse treats as Standalone (it declares a package), so
    // uvmVerificationLinting's NeedsOwnerContext path never engages for it -- singleUnitMacros
    // doesn't care about that classification at all, only build membership.
    ServerHarness server("single_unit_macros");
    server.loadConfig(
        Config{.build = "build.f", .experimental = Config::Experimental{.singleUnitMacros = true}});

    auto body = server.openFile("inline_pkg.sv");
    CHECK_FALSE(hasDiagMentioning(body.getDiagnostics(), "MY_COMPONENT_UTILS"));
}

TEST_CASE("SingleUnitMacrosOnUndefinedMacroStillReportsUnknown") {
    // Negative control: NEVER_DEFINED_ANYWHERE isn't defined by any file in the build, so it
    // must still report unknown even with the toggle on -- otherwise the passing cases above
    // could be vacuously passing because the feature just suppresses all such diagnostics.
    ServerHarness server("single_unit_macros");
    server.loadConfig(
        Config{.build = "build.f", .experimental = Config::Experimental{.singleUnitMacros = true}});

    auto body = server.openFile("unresolved.sv");
    CHECK(hasDiagMentioning(body.getDiagnostics(), "NEVER_DEFINED_ANYWHERE"));
}

TEST_CASE("SingleUnitMacrosSaveRefreshesPool") {
    // Confirms the generation-based invalidation actually propagates: editing and saving
    // macro_holder_pkg.sv (removing its `\`include` of macros.svh, so MY_COMPONENT_UTILS drops
    // out of the pool) must refresh diagnostics for tb_top.sv too, even though tb_top.sv itself
    // is never edited. setBuildFile() calls createCompilation() internally, so `comp` is set and
    // ServerDriver::updateDoc's SAVE branch republishes parse diagnostics for every doc in the
    // build -- that's what surfaces the now-stale-and-reparsed tb_top.sv diagnostics here.
    ServerHarness server("single_unit_macros");
    server.loadConfig(
        Config{.build = "build.f", .experimental = Config::Experimental{.singleUnitMacros = true}});

    auto tbTop = server.openFile("tb_top.sv");
    CHECK_FALSE(hasDiagMentioning(tbTop.getDiagnostics(), "MY_COMPONENT_UTILS"));

    auto pkg = server.openFile("macro_holder_pkg.sv");
    pkg.replaceAll("package macro_holder_pkg;\nendpackage\n");
    pkg.save();

    CHECK(hasDiagMentioning(tbTop.getDiagnostics(), "MY_COMPONENT_UTILS"));
}
