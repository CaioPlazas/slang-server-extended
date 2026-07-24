//------------------------------------------------------------------------------
// ServerDriver.cpp
// Implementation of server driver class for processing syntax trees
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "ServerDriver.h"

#include "Indexer.h"
#include "ServerDiagClient.h"
#include "SystemTaskDocs.h"
#include "ast/ServerCompilation.h"
#include "completions/CompletionDispatch.h"
#include "document/SlangDoc.h"
#include "lsp/LspTypes.h"
#include "lsp/URI.h"
#include "util/Converters.h"
#include "util/Formatting.h"
#include "util/Logging.h"
#include "util/Markdown.h"
#include "util/SlangExtensions.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <queue>
#include <string_view>

#include "slang/ast/Compilation.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/SystemSubroutine.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/types/Type.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/diagnostics/TextDiagnosticClient.h"
#include "slang/driver/Driver.h"
#include "slang/driver/SourceLoader.h"
#include "slang/parsing/ParserMetadata.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceLocation.h"
#include "slang/text/SourceManager.h"
#include "slang/util/ScopeGuard.h"

namespace server {
using namespace slang;

bool ServerDriver::s_debugHoversEnabled =
#ifdef SLANG_DEBUG
    true;
#else
    false;
#endif

ServerDriver::ServerDriver(Indexer& indexer, SlangLspClient& client, const Config& config,
                           std::vector<std::string> buildfiles) :
    sm(driver.sourceManager), diagEngine(driver.diagEngine), client(client),
    diagClient(std::make_shared<ServerDiagClient>(sm, client)),
    completions(*this, indexer, sm, options), codeActions(*this, sm), m_indexer(indexer),
    m_config(config) {
    parseAndLoadSources(buildfiles);
}

void ServerDriver::parseAndLoadSources(const std::vector<std::string>& buildfiles) {
    driver.addStandardArgs();
    diagEngine.removeClient(driver.textDiagClient);
    diagEngine.addClient(diagClient);

    // If `workDir` is configured (already resolved to an absolute path by SlangServer),
    // chdir there for the rest of this function so relative paths in `flags` and build
    // files (e.g. `-F ./my_flist.f`) resolve as if slang-server were launched from that
    // directory -- mirroring the common ASIC-flow convention of running tools from a
    // shared work directory rather than the repo root. Restored on scope exit.
    std::optional<slang::ScopeGuard<std::function<void()>>> workDirGuard;
    if (m_config.workDir.value()) {
        std::error_code ec;
        auto prevDir = std::filesystem::current_path(ec);
        std::filesystem::current_path(*m_config.workDir.value(), ec);
        if (ec) {
            client.showError(
                fmt::format("Failed to switch to workDir '{}': {}", *m_config.workDir.value(),
                            ec.message()));
        }
        else if (!prevDir.empty()) {
            workDirGuard.emplace([prevDir] {
                std::error_code restoreEc;
                std::filesystem::current_path(prevDir, restoreEc);
            });
        }
    }

    slang::CommandLine::ParseOptions parseOpts;
    parseOpts.expandEnvVars = true;
    parseOpts.ignoreProgramName = true;
    parseOpts.supportComments = true;
    parseOpts.ignoreDuplicates = true;

    // Parse each config file's flags separately so -D defines are attributed correctly
    bool ok = true;

    for (auto& src : m_config.flagsByFile.value()) {
        auto guard = driver.setCurrentCommandFile(src.filePath);
        ok &= driver.parseCommandLine(src.flags, parseOpts);
    }

    driver.options.errorLimit = 0;
    ok &= driver.processOptions(false);
    if (!ok) {
        client.showError("Failed to parse config flags");
    }

    for (auto& buildfile : buildfiles) {
        ok = driver.processCommandFiles(buildfile, m_config.buildRelativePaths.value(), false);
        if (ok) {
            INFO("Processed build file: {}", buildfile);
        }
        else {
            client.showError(fmt::format("Failed to process build file: {}", buildfile));
        }
    }

    // Build macro name -> source file map from the driver's per-file define lists
    for (auto& [path, meta] : driver.getCommandFileMetadata()) {
        for (auto& define : meta.defines) {
            auto eqPos = define.find('=');
            auto macroName = eqPos != std::string::npos ? define.substr(0, eqPos) : define;
            m_defineSources[macroName] = path;
        }
    }

    // Configure diagnostic engine. The LSP server reports warnings as editor
    // diagnostics by default; user-provided -Wno-* mappings still suppress
    // specific warnings via the severity table populated by processOptions().
    diagEngine.setIgnoreAllWarnings(false);
    diagEngine.setIgnoreAllNotes(false);

    options = driver.createOptionBag();
    options.set(driver.getAnalysisOptions());
    ok = driver.parseAllSources();
    diagEngine.setMappingsFromPragmas();

    // Create documents from syntax trees
    INFO("Creating ServerDriver with {} trees", driver.syntaxTrees.size());
    for (auto& tree : driver.syntaxTrees) {
        auto uri = URI::fromFile(sm.getFullPath(tree->getSourceBufferIds()[0]));
        auto doc = SlangDoc::fromTree(*this, std::move(tree));
        docs[uri] = doc;
    }
}

// Doc updates (open, change, save)
void ServerDriver::updateDoc(SlangDoc& doc, FileUpdateType type) {
    // Grab dependent documents
    doc.setDependentDocuments(getDependentDocs(doc.getSyntaxTree()));

    // Clear and re-issue diagnostics for this document
    diagClient->clear(doc.getURI());

    // Update pragma mappings for the changed buffer
    diagEngine.setMappingsFromPragmas(doc.getBuffer());

    bool usedFullCompilation = comp && type == FileUpdateType::SAVE;
    if (usedFullCompilation) {
        // Clear just the data structures; add all uris to dirty set
        diagClient->clear();

        // Re-issue parse diagnostics for all documents, since we cleared. Fragments share their
        // owner's tree, so skip them here or their owner's diagnostics get issued twice.
        for (const auto& [uri, d] : docs) {
            if (d->isIncludeFragment())
                continue;
            d->issueParseDiagnostics(diagEngine);
        }
        // Elaborate; Issue semantic diagnostics from full compilation
        comp->refresh();
        comp->issueDiagnosticsTo(diagEngine);
    }
    else {
        // In explore mode: issue normal shallow diags on changes
        doc.issueDiagnosticsTo(diagEngine);
    }
    diagClient->pushDiags(doc.getURI());
    INFO("Published diags for {}", doc.getURI().getPath());

    publishInactiveRegions(doc);

    // If this doc is an `include`-only fragment, editing it also changed its owner(s)' compiled
    // view (e.g. an "unused signal" diagnostic on the owner's own lines may now be stale).
    // Refresh diagnostics for any owner that's currently open. Owners are never fragments
    // themselves, so this doesn't recurse further.
    if (!usedFullCompilation && doc.isIncludeFragment()) {
        for (auto& owner : doc.getIncludeOwners()) {
            if (isDocumentOpen(owner->getURI())) {
                updateDoc(*owner, FileUpdateType::CHANGE);
            }
        }
    }
}

std::unique_ptr<ServerDriver> ServerDriver::create(Indexer& indexer, SlangLspClient& client,
                                                   const Config& config,
                                                   std::vector<std::string> buildfiles,
                                                   const ServerDriver* oldDriver) {
    auto newDriver = std::make_unique<ServerDriver>(indexer, client, config, buildfiles);

    // Copy only open documents from old driver if provided
    if (oldDriver) {
        oldDriver->diagClient->clearAndPush();
        for (const auto& uri : oldDriver->m_openDocs) {
            auto docIt = oldDriver->docs.find(uri);
            if (docIt == oldDriver->docs.end()) {
                ERROR("Open Doc {} not found in old driver", uri.getPath());
                continue;
            }
            // Only copy if the URI isn't already in the new driver's docs
            auto newDocit = newDriver->docs.find(uri);
            if (newDocit == newDriver->docs.end()) {
                // Open the document in the new driver using the text from the old document
                newDriver->openDocument(uri, docIt->second->getText());
                // Trigger diagnostics for the newly opened document
            }
            else {
                // Publish diags for the existing document
                // Add to open doc set
                newDriver->m_openDocs.insert(uri);
                newDriver->updateDoc(*newDocit->second, FileUpdateType::OPEN);
            }
        }
    }

    return newDriver;
}

namespace {
/// True if `doc`'s standalone parse is NOT a viable independent compilation unit -- the signal used
/// throughout this file to tell a genuine `\`include`-only fragment apart from a "dual-purpose" file
/// that's also valid parsed on its own. Only the former should ever be bound to its owner(s) instead
/// of parsed standalone. See resolveDocument and adoptOrphanFragments.
///
/// A file is treated as a viable standalone unit (returns false) when EITHER:
///   (a) it declares a module/interface/program/package/class -- the set of things that make a file
///       a legitimate independent compilation target, which is exactly what
///       ParserMetadata::visitDeclaredSymbols reports; or
///   (b) it has no such declaration but still parses cleanly, i.e. its top-level members are all
///       valid compilation-unit ($unit) items (localparams, typedefs, nets, functions, ...). Such a
///       file is a perfectly good standalone unit even without a module, so it must NOT be forced
///       into fragment mode.
/// Only when BOTH fail -- no top-level design unit AND the standalone parse doesn't even hold
/// together (a syntax error) -- is it a genuine `\`include`-only body fragment.
///
/// Why not "declares no module/etc." alone: bare net/variable/parameter declarations must not count
/// as a design unit (a real `.vh` body fragment ending in `wire dout_c;` was wrongly rejected when
/// any top-level symbol counted). But going purely by "declares no design unit" over-corrects: a
/// `.svh` of only `localparam`s (e.g. macros expanding to `localparam int X = pkg::num;`) declares
/// no module yet parses cleanly and is a valid standalone unit whose references must resolve against
/// its own buffer -- forcing it into a fragment breaks that. The discriminator between the two is
/// exactly whether the standalone parse errors: a body fragment's `assign`/statement content is a
/// syntax error at file scope, whereas bare `$unit`-legal declarations are not.
///
/// Deliberately parse-only and cheap: both getMetadata() and diagnostics() are populated as a side
/// effect of parsing, so this needs no tree walk and no compilation. doc->getSymbols() would go
/// through getAnalysis(), which resolves dependent documents and builds a full ShallowAnalysis --
/// this runs on the doc-open/getDocument hot path for every file in the build, so it must stay
/// parse-only.
bool isEmptyStandaloneParse(const std::shared_ptr<SlangDoc>& doc) {
    if (!doc)
        return true;
    auto tree = doc->getSyntaxTree();
    if (!tree)
        return true;

    // (a) A declared module/interface/program/package/class makes this a real compilation unit --
    // even if its body has errors, it's the file's own tree, not a fragment to splice elsewhere.
    bool hasDeclaration = false;
    tree->getMetadata().visitDeclaredSymbols([&](std::string_view) { hasDeclaration = true; });
    if (hasDeclaration)
        return false;

    // (b) No design unit, but if it parses cleanly its top-level members are all valid $unit-scope
    // declarations -- a viable standalone unit, not a fragment. Only a standalone parse that fails
    // to hold together (a syntax error, e.g. bare `assign`/statement content that's illegal outside
    // a module) marks a genuine `\`include`-only body fragment.
    for (auto& diag : tree->diagnostics()) {
        if (diag.isError())
            return true;
    }
    return false;
}
} // namespace

void ServerDriver::openDocument(const URI& uri, const std::string_view text) {
    auto docIter = docs.find(uri);
    std::shared_ptr<SlangDoc> doc;
    bool alreadyInBuild = false;
    if (docIter != docs.end() && docIter->second->textMatches(text)) {
        doc = docIter->second;
        alreadyInBuild = true;

        // The cached doc may have been parsed standalone at a time its owner wasn't known yet
        // (e.g. via ServerDriver::getDocument(), or opened before any includer was seen, then
        // left untouched across a close+reopen). If an owner is known now, and the standalone
        // parse is genuinely empty/garbage (not a dual-purpose file -- see
        // adoptOrphanFragments), rebind it instead of continuing to treat it as a broken
        // top-level file. Force a diagnostics refresh, since it may have previously published
        // bogus standalone diagnostics under its own URI.
        //
        // Check emptiness first: `doc` is already cached here, so it's a free read of its
        // existing tree, whereas findIncludeOwners is an O(number of docs in the build) scan.
        // The overwhelming majority of reopened files are normal, non-empty top-level files, so
        // this ordering skips the expensive scan entirely for the common case.
        if (!doc->isIncludeFragment() && isEmptyStandaloneParse(doc)) {
            auto includeOwners = findIncludeOwners(uri);
            if (!includeOwners.empty()) {
                for (auto& owner : includeOwners) {
                    doc->addIncludeOwner(owner);
                }
                alreadyInBuild = false;
            }
        }
    }
    else {
        if (docIter != docs.end())
            WARN("Document {} text does not match, updating", uri.getPath());

        // If this path is only ever `\`include`d by other files (e.g. a `.vh`/`.svh` fragment),
        // bind it to its owner(s) instead of parsing it as a standalone compilation unit, which
        // would produce bogus "not a module" errors. See resolveDocument for the guard that
        // protects dual-purpose files (also `\`include`d, but independently valid).
        doc = resolveDocument(uri, text);
        docs[uri] = doc;
    }

    if (comp && alreadyInBuild) {
        // File is already part of the compilation — compilation diags were
        // already published, so skip shallow diags. Still publish inactive regions.
        publishInactiveRegions(*doc);
    }
    else {
        updateDoc(*doc, FileUpdateType::OPEN);
    }

    // Track this as an open document
    m_openDocs.insert(uri);

    // This doc may `\`include` some other doc that was previously opened as an orphaned
    // fragment (parsed standalone because no owner was known at the time, e.g. it was opened
    // before this file). Now that this doc is known, retroactively bind and refresh any such
    // fragments. Fragments can't include anything themselves, so skip the scan for those.
    //
    // Only worth scanning when `doc` is genuinely new to the build (freshly created above, or
    // just transitioned from standalone to fragment, in which case isIncludeFragment() is now
    // true and the check above already skips it): if it was already an unchanged, already-known
    // build member (alreadyInBuild, still true here), its set of included files hasn't changed
    // since the last time this scan had a chance to run for it, so there's nothing new to find.
    // This avoids another O(number of docs) scan on every no-op reopen of an already-open file.
    if (!doc->isIncludeFragment() && !alreadyInBuild) {
        adoptOrphanFragments(doc);
    }
}

std::shared_ptr<SlangDoc> ServerDriver::getDocument(const URI& uri) {
    auto it = docs.find(uri);
    if (it != docs.end())
        return it->second;

    // This is used by many navigation/analysis routes (goto-definition targets, hover targets,
    // reference scans, getDependentDocs) to resolve an arbitrary file path, not just symbol
    // names -- so a `.vh`/`.svh` fragment path can land here before it's ever been opened by the
    // editor. Mirror openDocument()'s handling: bind it to its owner(s) instead of caching a
    // standalone parse that would produce bogus "not a module" errors later (e.g. when reused by
    // a subsequent didOpen, or republished by a docs-wide diagnostics loop).
    auto doc = resolveDocument(uri, std::nullopt);
    if (doc) {
        docs[uri] = doc;
    }
    return doc;
}

bool ServerDriver::isDocumentOpen(const URI& uri) {
    return m_openDocs.find(uri) != m_openDocs.end();
}

void ServerDriver::onDocDidChange(const lsp::DidChangeTextDocumentParams& params) {
    std::string_view path = params.textDocument.uri.getPath();
    auto doc = getDocument(params.textDocument.uri);
    if (!doc) {
        ERROR("Document {} not found", path);
        return;
    }

    doc->onChange(params.contentChanges);
    // Update Tree and Compilation
    updateDoc(*doc, FileUpdateType::CHANGE);
}

void ServerDriver::closeDocument(const URI& uri) {
    // Remove from open docs set
    m_openDocs.erase(uri);
    if (!comp) {
        diagClient->clear(uri);
    }
}

void ServerDriver::reloadDocument(const URI& uri) {
    // Only reload if this is an open document
    if (m_openDocs.find(uri) == m_openDocs.end()) {
        return;
    }

    auto doc = getDocument(uri);
    if (!doc) {
        WARN("Document {} not found for reload", uri.getPath());
        return;
    }

    if (!doc->reloadBuffer()) {
        return;
    }

    INFO("Reloaded document {} from disk", uri.getPath());

    // Update the document (reparse and issue diagnostics)
    updateDoc(*doc, FileUpdateType::CHANGE);
}

void ServerDriver::onWorkspaceDidChangeWatchedFiles(
    const lsp::DidChangeWatchedFilesParams& params) {
    // Collect docs that need updating after all buffers are reloaded
    std::vector<std::shared_ptr<SlangDoc>> updatedDocs;

    for (const auto& change : params.changes) {
        switch (change.type) {
            case lsp::FileChangeType::Changed: {
                // Only reload if this is an open document
                if (m_openDocs.find(change.uri) == m_openDocs.end()) {
                    continue;
                }

                auto doc = getDocument(change.uri);
                if (!doc) {
                    WARN("Document {} not found for reload", change.uri.getPath());
                    continue;
                }

                if (!doc->reloadBuffer()) {
                    continue;
                }

                INFO("Reloaded document {} from disk", change.uri.getPath());
                updatedDocs.push_back(doc);
                break;
            }
            case lsp::FileChangeType::Deleted:
                closeDocument(change.uri);
                break;
            case lsp::FileChangeType::Created:
                break;
        }
    }

    // Update all open docs after all buffers have been reloaded
    for (auto& doc : updatedDocs) {
        updateDoc(*doc, FileUpdateType::CHANGE);
    }
}

std::shared_ptr<SlangDoc> ServerDriver::resolveDocument(const URI& uri,
                                                        std::optional<std::string_view> text) {
    auto includeOwners = findIncludeOwners(uri);
    if (includeOwners.empty())
        return text ? SlangDoc::fromText(*this, uri, *text) : SlangDoc::open(*this, uri);

    // Probe with the on-disk parse even when called from openDocument (which has live editor
    // text) -- syncing live text into the shared owner buffer just to make this check would
    // gratuitously invalidate every owner's cached tree, even for the common, unambiguous case.
    // A file can be `\`include`d elsewhere while still being perfectly valid on its own (e.g. a
    // full module textually included into a wrapper/testbench), so only treat it as a fragment
    // if this standalone parse is genuinely empty/garbage. See adoptOrphanFragments for the
    // matching guard.
    auto standalone = SlangDoc::open(*this, uri);
    if (!isEmptyStandaloneParse(standalone))
        return text ? SlangDoc::fromText(*this, uri, *text) : standalone;

    return SlangDoc::fromIncludeOwner(*this, uri, text, std::move(includeOwners));
}

std::vector<std::shared_ptr<SlangDoc>> ServerDriver::findIncludeOwners(const URI& uri) {
    std::vector<std::shared_ptr<SlangDoc>> result;

    if (!m_config.resolveIncludeFragments.value())
        return result;

    for (auto& [ownerUri, ownerDoc] : docs) {
        // Fragments don't have a tree of their own to search, and can't include anything.
        if (ownerUri == uri || ownerDoc->isIncludeFragment())
            continue;

        auto tree = ownerDoc->getSyntaxTree();
        if (!tree)
            continue;

        auto bufId = findBufferForPath(sm, *tree, uri.getPath());
        if (!bufId)
            continue;

        // Only count it if it's actually `\`include`d, not e.g. the owner's own root buffer.
        if (!sm.getIncludedFrom(*bufId).valid())
            continue;

        result.push_back(ownerDoc);
    }

    return result;
}

void ServerDriver::adoptOrphanFragments(const std::shared_ptr<SlangDoc>& newOwner) {
    if (!m_config.resolveIncludeFragments.value())
        return;

    auto tree = newOwner->getSyntaxTree();
    if (!tree)
        return;

    // Collect matches first, then mutate/updateDoc afterwards -- updateDoc can transitively call
    // getDocument(), which inserts into `docs` and would invalidate iterators mid-scan.
    std::vector<std::shared_ptr<SlangDoc>> toAdopt;
    for (auto& [uri, doc] : docs) {
        if (doc == newOwner)
            continue;

        auto bufId = findBufferForPath(sm, *tree, uri.getPath());
        if (!bufId || !sm.getIncludedFrom(*bufId).valid())
            continue;

        bool alreadyOwned = false;
        for (auto& owner : doc->getIncludeOwners()) {
            if (owner == newOwner) {
                alreadyOwned = true;
                break;
            }
        }
        if (alreadyOwned)
            continue;

        // A file can be `\`include`d elsewhere while still being perfectly valid on its own
        // (e.g. a full module textually included into a wrapper/testbench) -- that's not the
        // orphaned-fragment case this is meant to fix, so leave it alone if it already parses
        // standalone with real top-level content. Only adopt genuinely broken/empty parses,
        // matching what findIncludeOwners would have produced had this doc been opened after
        // newOwner in the first place.
        if (!doc->isIncludeFragment() && !isEmptyStandaloneParse(doc))
            continue;

        toAdopt.push_back(doc);
    }

    for (auto& doc : toAdopt) {
        doc->addIncludeOwner(newOwner);

        if (isDocumentOpen(doc->getURI())) {
            updateDoc(*doc, FileUpdateType::CHANGE);
        }
    }
}

std::vector<std::shared_ptr<SlangDoc>> ServerDriver::getDependentDocs(
    std::shared_ptr<SyntaxTree> tree) {
    std::vector<std::shared_ptr<SlangDoc>> result;
    std::queue<std::shared_ptr<SyntaxTree>> treesToProcess;
    flat_hash_set<std::string_view> knownNames;
    flat_hash_set<std::string> processedFiles;

    treesToProcess.push(tree);

    while (!treesToProcess.empty()) {
        auto currentTree = treesToProcess.front();
        treesToProcess.pop();

        auto& meta = currentTree->getMetadata();

        // Collect declared symbols from current tree
        meta.visitDeclaredSymbols([&](std::string_view name) { knownNames.emplace(name); });

        meta.visitReferencedSymbols([&](std::string_view name) {
            if (knownNames.find(name) != knownNames.end())
                return; // already added

            // Don't try multiple times
            knownNames.emplace(name);
            auto symbolLoc = m_indexer.getFirstSymbolLoc(name);
            if (!symbolLoc)
                return;

            std::string filePath = symbolLoc->uri->string();

            // Check if we've already processed this file to avoid cycles
            if (processedFiles.find(filePath) != processedFiles.end())
                return;

            processedFiles.insert(filePath);

            auto newdoc = getDocument(URI::fromFile(filePath));
            if (newdoc) {
                result.push_back(newdoc);
                docs[newdoc->getURI()] = newdoc;

                // Recurse into packages and interfaces, since they may contain types from other
                // packages that are referenced by the analyzed module.
                for (auto& [decl, _] : newdoc->getSyntaxTree()->getMetadata().nodeMeta) {
                    if (decl->kind == syntax::SyntaxKind::PackageDeclaration ||
                        decl->kind == syntax::SyntaxKind::InterfaceDeclaration) {
                        treesToProcess.push(newdoc->getSyntaxTree());
                        break;
                    }
                }
            }
            else {
                ERROR("No doc found for {}", filePath);
            }
        });
    }

    return result;
}

std::vector<std::string> ServerDriver::getModulesInFile(const std::string& path) {
    // Find the document
    auto uri = URI::fromFile(path);
    auto it = docs.find(uri);
    if (it == docs.end()) {
        WARN("Document {} not found", path);
        return {};
    }

    auto& doc = it->second;

    // Get the module-like things from the document and collect into a vector
    std::vector<std::string> moduleNames;
    for (auto& name : doc->getSyntaxTree()->getMetadata().getDeclaredSymbols()) {
        moduleNames.push_back(std::string{name});
    }
    if (moduleNames.empty()) {
        WARN("No modules found in file {}", path);
    }
    INFO("Found {} modules in file {}", moduleNames.size(), path);
    return moduleNames;
}

bool ServerDriver::createCompilation(std::shared_ptr<SlangDoc> doc, std::string_view top) {
    // Collect documents starting with the target document
    std::vector<std::shared_ptr<syntax::SyntaxTree>> syntaxTrees{doc->getSyntaxTree()};
    driver::SourceLoader::loadTrees(
        syntaxTrees,
        [this](std::string_view name) {
            auto paths = m_indexer.getFilesForSymbol(name);
            if (!paths.empty()) {
                auto maybeBuf = sm.readSource(paths[0], /* library */ nullptr);
                if (maybeBuf) {
                    return *maybeBuf;
                }
                else {
                    ERROR("Failed to read source for {}: {}", paths[0].string(),
                          maybeBuf.error().message());
                }
            }
            return SourceBuffer{};
        },
        sm, this->options);

    std::vector<std::shared_ptr<SlangDoc>> documents;
    documents.reserve(syntaxTrees.size());
    for (const auto& tree : syntaxTrees) {
        documents.push_back(SlangDoc::fromTree(*this, tree));
    }
    // insert the documents into the driver
    for (const auto& doc : documents) {
        docs[doc->getURI()] = doc;
    }

    comp = std::make_unique<ServerCompilation>(documents, this->options, sm, std::string(top));

    // Apply pragma mappings for all buffers (including newly loaded ones)
    diagEngine.setMappingsFromPragmas();

    // Publish initial diags
    for (const auto& doc : documents) {
        doc->issueParseDiagnostics(diagEngine);
    }
    comp->issueDiagnosticsTo(diagEngine);
    diagClient->pushDiags();

    return true;
}

bool ServerDriver::createCompilation() {
    // Collect all documents. Fragments (e.g. `.vh` files only ever `\`include`d elsewhere) share
    // their owner's tree rather than having one of their own, so skip them here -- otherwise the
    // same tree would be added to the compilation twice, once via its owner and once via the
    // fragment.
    std::vector<std::shared_ptr<SlangDoc>> documents;

    for (const auto& [uri, doc] : docs) {
        if (doc->isIncludeFragment())
            continue;
        if (doc->getSyntaxTree()) {
            documents.push_back(doc);
        }
        else {
            ERROR("Document {} has no syntax tree", uri.getPath());
        }
    }

    if (documents.empty()) {
        ERROR("No documents available for compilation");
        return false;
    }

    comp = std::make_unique<ServerCompilation>(std::move(documents), this->options, sm);

    // Apply pragma mappings for all buffers
    diagEngine.setMappingsFromPragmas();

    // Issue parse diagnostics for all documents + semantic diagnostics from compilation
    // This ensures that when a user opens a document later, the diagnostics don't disappear
    diagClient->clear();
    for (const auto& [uri, doc] : docs) {
        if (doc->isIncludeFragment())
            continue;
        doc->issueParseDiagnostics(diagEngine);
    }

    // Issue semantic diagnostics from the compilation
    comp->issueDiagnosticsTo(diagEngine);
    diagClient->pushDiags();
    return true;
}

std::optional<DefinitionInfo> ServerDriver::getDefinitionInfoAt(const URI& uri,
                                                                const lsp::Position& position) {
    auto doc = getDocument(uri);
    if (!doc) {
        return {};
    }
    auto analysis = doc->getAnalysis();

    // Get location, token, and syntax node at position
    auto loc = sm.getSourceLocation(doc->getBuffer(), position.line, position.character);
    if (!loc) {
        return {};
    }
    const parsing::Token* declTok = analysis->syntaxes.getWordTokenAt(loc.value());
    if (!declTok) {
        return {};
    }
    const syntax::SyntaxNode* declSyntax = analysis->syntaxes.getTokenParent(declTok);
    if (!declSyntax) {
        return {};
    }

    std::optional<parsing::Token> nameToken;
    const syntax::SyntaxNode* symSyntax = nullptr;
    const ast::Symbol* symbol = nullptr;

    auto isMacroRef = [&]() {
        // Normal macro usage (`FOO) or usage inside a `define body
        return declTok->kind == parsing::TokenKind::Directive &&
               (declSyntax->kind == syntax::SyntaxKind::MacroUsage ||
                declSyntax->kind == syntax::SyntaxKind::DefineDirective);
    };
    auto isUndefRef = [&]() {
        // Identifier in `undef FOO
        return declTok->kind == parsing::TokenKind::Identifier &&
               declSyntax->kind == syntax::SyntaxKind::UndefDirective;
    };
    auto isIfdefRef = [&]() {
        // Identifier in `ifdef FOO / `ifndef FOO / `elsif FOO
        return declTok->kind == parsing::TokenKind::Identifier &&
               declSyntax->kind == syntax::SyntaxKind::NamedConditionalDirectiveExpression;
    };

    if (isMacroRef() || isUndefRef() || isIfdefRef()) {

        // For macro usages and undefs, look up the definition that was
        // active at the time (handles undef/redefine correctly)
        const syntax::DefineDirectiveSyntax* macroDef = nullptr;
        if (declSyntax->kind == syntax::SyntaxKind::MacroUsage ||
            declSyntax->kind == syntax::SyntaxKind::UndefDirective) {
            auto it = analysis->macroUsageDefinitions.find(declSyntax);
            if (it != analysis->macroUsageDefinitions.end())
                macroDef = it->second;
        }

        // Fall back to the macros map
        // if we're looking at a macro usage in a define body, or macro isn't found but indexed
        if (!macroDef) {
            // For directive tokens (`FOO), valueText strips the backtick.
            // For identifier tokens (FOO in `undef), valueText is the name.
            auto macroName = declTok->kind == parsing::TokenKind::Directive
                                 ? declTok->rawText().substr(1)
                                 : declTok->valueText();
            auto macro = analysis->macros.find(macroName);
            if (macro == analysis->macros.end()) {
                auto files = m_indexer.getFilesForMacro(macroName);
                if (files.empty())
                    return {};
                auto macroDoc = getDocument(URI::fromFile(files[0].string()));
                if (!macroDoc)
                    return {};
                auto macroAnalysis = macroDoc->getAnalysis();
                macro = macroAnalysis->macros.find(macroName);
                if (macro == macroAnalysis->macros.end())
                    return {};
            }
            macroDef = macro->second;
        }

        symSyntax = macroDef;
        nameToken = macroDef->name;
    }
    else if (declTok->kind == parsing::TokenKind::SystemIdentifier) {
        auto knownName = declTok->systemName();
        if (knownName == parsing::KnownSystemName::Unknown)
            return {};

        auto* sub = analysis->getCompilation()->getSystemSubroutine(knownName);
        auto* sysDoc = getSystemTaskDoc(knownName);
        if (!sub || !sysDoc)
            return {};

        return DefinitionInfo{DefinitionInfo::SystemSubroutineTarget{
            *declTok, sysDoc, sub->kind == ast::SubroutineKind::Task}};
    }
    else {
        symbol = analysis->getSymbolAtToken(declTok);
        if (!symbol) {
            // check the index
            auto symbols = m_indexer.getFilesForSymbol(declTok->rawText());
            if (symbols.empty()) {
                return {};
            }
            auto symDoc = getDocument(URI::fromFile(symbols[0].string()));
            if (!symDoc) {
                return {};
            }
            auto symAnalysis = symDoc->getAnalysis();
            auto result = symAnalysis->getCompilation()->tryGetDefinition(
                declTok->rawText(), symAnalysis->getCompilation()->getRoot());
            if (!result.definition) {
                return {};
            }
            symbol = result.definition;
        }
        symSyntax = symbol->getSyntax();

        if (!symSyntax) {
            ERROR("Failed to get syntax for symbol {} of kind {}", symbol->name,
                  toString(symbol->kind));
            return {};
        }

        // For some symbols we want to return the parent to get the data type
        if (symbol->kind == ast::SymbolKind::Modport ||
            symbol->kind == ast::SymbolKind::ModportPort) {
            symSyntax = symSyntax->parent;
        }
        nameToken = findNameToken(symSyntax, symbol->name);
        if (!nameToken) {
            ERROR("Failed to find name token for symbol '{}' of kind {} = {}", symbol->name,
                  toString(symbol->kind), symSyntax->toString());

            // TODO: figure out why this fails sometimes with all generates
            nameToken = symSyntax->getFirstToken();
        }
    }

    auto macroUsageRange = SourceRange::NoLocation;
    if (nameToken && sm.isMacroLoc(nameToken->location())) {
        auto locs = sm.getMacroExpansions(nameToken->location());
        // TODO: maybe include more expansion infos?
        auto macroInfo = sm.getMacroInfo(locs.back());
        auto text = macroInfo ? sm.getText(macroInfo->expansionRange) : "";
        if (text.empty()) {
            ERROR("Couldn't get original range for symbol {}", nameToken->valueText());
        }
        else {
            macroUsageRange = macroInfo->expansionRange;
        }
    }

    std::string macroExpansionText;
    if (declSyntax->kind == syntax::SyntaxKind::MacroUsage) {
        auto it = analysis->syntaxes.macroExpansions.find(declSyntax);
        if (it != analysis->syntaxes.macroExpansions.end())
            macroExpansionText = it->second.getText();
    }

    auto makeSyntaxTarget = [&]() {
        return DefinitionInfo::SyntaxTarget{symSyntax, *nameToken, macroUsageRange};
    };

    auto makeTarget = [&]() -> DefinitionInfo::Target {
        if (symbol)
            return DefinitionInfo::SymbolTarget{makeSyntaxTarget(), symbol, analysis};

        DefinitionInfo::MacroTarget::Definition macroDefinition = makeSyntaxTarget();

        const auto defPath = sm.getFullPath(nameToken->location().buffer());
        const auto defPathStr = defPath.filename().string();
        if (defPathStr.empty() || defPathStr[0] == '<') {
            std::string defineSourceFile;
            const auto srcIt = m_defineSources.find(std::string(nameToken->valueText()));
            if (srcIt != m_defineSources.end())
                defineSourceFile = srcIt->second.string();
            macroDefinition = DefinitionInfo::CommandLineDefineTarget{*nameToken, defineSourceFile};
        }

        return DefinitionInfo::MacroTarget{macroDefinition, macroExpansionText};
    };
    return DefinitionInfo{makeTarget()};
}

std::optional<lsp::Hover> ServerDriver::getDocHover(const URI& uri, const lsp::Position& position) {
    const auto doc = getDocument(uri);
    if (!doc) {
        return {};
    }
    auto loc = sm.getSourceLocation(doc->getBuffer(), position.line, position.character);
    if (!loc) {
        return {};
    }
    auto maybeInfo = getDefinitionInfoAt(uri, position);
    if (!maybeInfo) {
        if (s_debugHoversEnabled) {
            // Shows debug info for the token under cursor when debugging.
            auto analysis = doc->getAnalysis();
            markup::Document markup;
            markup.addParagraph(analysis->getDebugHover(loc.value()));
            return lsp::Hover{.contents = markup.build()};
        }
        return {};
    }
    const auto& info = *maybeInfo;
    return lsp::Hover{.contents = info.getHover(sm, doc->getBuffer(), m_config.hovers.value())};
}

std::vector<lsp::LocationLink> ServerDriver::getDocDefinition(const URI& uri,
                                                              const lsp::Position& position) {
    auto maybeInfo = getDefinitionInfoAt(uri, position);
    if (!maybeInfo)
        return {};
    return maybeInfo->getDefinition(sm);
}

std::optional<std::vector<lsp::DocumentHighlight>> ServerDriver::getDocDocumentHighlight(
    const URI& uri, const lsp::Position& position) {
    auto doc = getDocument(uri);
    if (!doc) {
        return std::nullopt;
    }
    auto analysis = doc->getAnalysis();

    // Get the symbol at the position
    auto loc = sm.getSourceLocation(doc->getBuffer(), position.line, position.character);
    if (!loc) {
        return std::nullopt;
    }
    auto declTok = analysis->syntaxes.getWordTokenAt(loc.value());
    if (!declTok) {
        return std::nullopt;
    }
    auto symbol = analysis->getSymbolAtToken(declTok);
    if (!symbol) {
        return std::nullopt;
    }

    // Find all references to the symbol in the current document
    std::vector<lsp::Location> references;
    analysis->addLocalReferences(references, symbol->location, symbol->name);
    if (references.empty()) {
        return std::nullopt;
    }

    std::vector<lsp::DocumentHighlight> highlights;
    highlights.reserve(references.size());
    for (auto& ref : references) {
        highlights.push_back(lsp::DocumentHighlight{
            .range = ref.range,
        });
    }

    return highlights;
}

void ServerDriver::addMemberReferences(std::vector<lsp::Location>& references,
                                       const ast::Symbol& parentSymbol,
                                       const ast::Symbol& targetSymbol, bool isTypeMember) {

    auto targetBuffer = sm.getFullyOriginalLoc(targetSymbol.location).buffer();
    auto targetDoc = getDocument(URI::fromFile(sm.getFullPath(targetBuffer)));
    auto targetName = targetSymbol.name;

    auto referencingFiles = m_indexer.getFilesReferencingSymbol(parentSymbol.name);
    for (auto& filePath : referencingFiles) {
        URI fileUri = URI::fromFile(filePath.string());

        // Skip the file where targetSymbol is defined to avoid duplicates
        if (fileUri == targetDoc->getURI()) {
            continue;
        }

        auto fileDoc = getDocument(fileUri);
        if (!fileDoc) {
            continue;
        }

        // if a package, check if we can just use the package ref syntaxes to save on
        // making analysis
        if (!isTypeMember && parentSymbol.kind == ast::SymbolKind::Package) {
            auto& meta = fileDoc->getSyntaxTree()->getMetadata();
            bool hasWildcard = [&] {
                for (auto ref : meta.packageImports) {
                    for (auto item : ref->items) {
                        if (item->package.valueText() == parentSymbol.name &&
                            item->item.kind == parsing::TokenKind::Star) {
                            return true;
                        }
                    }
                }
                return false;
            }();
            if (!hasWildcard) {
                // no wildcard, just check cases of pkg::<targetName>
                for (auto ref : meta.classPackageNames) {
                    if (ref->identifier.valueText() != parentSymbol.name) {
                        continue;
                    }
                    auto tok = ref->parent->as<ScopedNameSyntax>().right->getFirstToken();
                    if (tok.valueText() == targetName) {
                        references.push_back(toOriginalLocation(tok.range(), sm));
                    }
                }
                continue;
            }
        }

        auto fileAnalysis = fileDoc->getAnalysis();
        fileAnalysis->addLocalReferences(references, targetSymbol.location, targetName);
    }
}

std::optional<std::vector<lsp::Location>> ServerDriver::getDocReferences(
    const URI& srcUri, const lsp::Position& position, bool includeDeclaration) {
    auto doc = getDocument(srcUri);
    if (!doc) {
        return std::nullopt;
    }

    // Get the symbol at the position. Hold the analysis via shared_ptr so that
    // targetSymbol remains valid even if getAnalysis() is called on this doc again.
    auto analysis = doc->getAnalysis();
    auto loc = sm.getSourceLocation(doc->getBuffer(), position.line, position.character);
    if (!loc) {
        return std::nullopt;
    }

    const parsing::Token* declTok = analysis->syntaxes.getWordTokenAt(loc.value());
    if (!declTok) {
        return std::nullopt;
    }

    const ast::Symbol* targetSymbol = analysis->getSymbolAtToken(declTok);
    if (!targetSymbol) {
        return std::nullopt;
    }

    // A top level of a shallow compilation is an instance body; get the definition instead
    if (targetSymbol->kind == ast::SymbolKind::InstanceBody) {
        targetSymbol = &targetSymbol->as<ast::InstanceBodySymbol>().getDefinition();
    }

    std::vector<lsp::Location> references;

    auto targetName = declTok->rawText();

    auto findPkgReferencesInDocument = [&](const parsing::ParserMetadata& meta, const URI&) {
        for (auto ref : meta.packageImports) {
            for (auto item : ref->items) {
                if (item->package.valueText() == targetName) {
                    references.push_back(toOriginalLocation(item->package.range(), sm));
                }
            }
        }
        for (auto ref : meta.classPackageNames) {
            if (ref->identifier.valueText() == targetName) {
                references.push_back(toOriginalLocation(ref->identifier.range(), sm));
            }
        }
    };

    auto findModuleReferencesInDocument = [&](const parsing::ParserMetadata& meta, const URI&) {
        for (auto inst : meta.globalInstances) {
            if (inst->type.valueText() == targetName) {
                references.push_back(toOriginalLocation(inst->type.range(), sm));
            }
        }
    };

    auto findInterfaceReferencesInDocument = [&](const parsing::ParserMetadata& meta, const URI&) {
        for (auto inst : meta.globalInstances) {
            if (inst->type.valueText() == targetName) {
                references.push_back(toOriginalLocation(inst->type.range(), sm));
            }
        }
        for (auto intf : meta.interfacePorts) {
            if (intf->nameOrKeyword.valueText() == targetName) {
                references.push_back(toOriginalLocation(intf->nameOrKeyword.range(), sm));
            }
        }
    };

    auto targetLoc = sm.getFullyOriginalLoc(targetSymbol->location);
    auto targetDoc = getDocument(URI::fromFile(sm.getFullPath(targetLoc.buffer())));

    // Helper to process referencing files with a given finder function
    auto processReferencingFiles = [&](std::string_view name, auto&& finder) {
        for (const auto& filePath : m_indexer.getFilesReferencingSymbol(name)) {
            if (filePath == targetDoc->getURI().getPath()) {
                continue;
            }
            URI fileUri = URI::fromFile(filePath.string());
            auto fileDoc = getDocument(fileUri);
            if (fileDoc) {
                finder(fileDoc->getSyntaxTree()->getMetadata(), fileUri);
            }
            else {
                ERROR("No doc found for {}", filePath.string());
            }
        }
    };

    // Add refs in declaration file, and remove declaration if requested
    if (targetDoc) {
        auto targetAnalysis = targetDoc->getAnalysis();
        targetAnalysis->addLocalReferences(references, targetSymbol->location, targetName);
        if (!includeDeclaration) {
            auto targetLspLoc = lsp::Location{
                .uri = URI::fromFile(sm.getFullPath(targetLoc.buffer())),
                .range = toRange(SourceRange(targetLoc, targetLoc + targetSymbol->name.size()), sm),
            };
            references.erase(std::remove_if(references.begin(), references.end(),
                                            [&](const lsp::Location& loc) {
                                                return loc.uri == targetLspLoc.uri &&
                                                       loc.range == targetLspLoc.range;
                                            }),
                             references.end());
        }
    }

    // Add global references
    switch (targetSymbol->kind) {
        case ast::SymbolKind::Instance: {
            processReferencingFiles(targetSymbol->as<ast::InstanceSymbol>().getDefinition().name,
                                    findModuleReferencesInDocument);
        } break;
        case ast::SymbolKind::InstanceBody: {
            processReferencingFiles(
                targetSymbol->as<ast::InstanceBodySymbol>().getDefinition().name,
                findModuleReferencesInDocument);
        } break;
        case ast::SymbolKind::Definition: {
            const auto& definition = targetSymbol->as<ast::DefinitionSymbol>();
            if (definition.definitionKind == ast::DefinitionKind::Interface) {
                processReferencingFiles(definition.name, findInterfaceReferencesInDocument);
            }
            else {
                processReferencingFiles(definition.name, findModuleReferencesInDocument);
            }
        } break;
        case ast::SymbolKind::Package: {
            processReferencingFiles(targetName, findPkgReferencesInDocument);
        } break;
        default: {
            if (targetSymbol->getParentScope() == nullptr ||
                targetSymbol->getParentScope()->asSymbol().getParentScope() == nullptr) {
                ERROR("Target symbol {}: {} has no parent scope, missed kind case for global "
                      "symbol",
                      targetName, toString(targetSymbol->kind));
                break;
            }
            auto& parentSymbol = targetSymbol->getParentScope()->asSymbol();
            auto& gParentSymbol = parentSymbol.getParentScope()->asSymbol();
            if (gParentSymbol.kind == ast::SymbolKind::CompilationUnit) {
                // Package and module members
                addMemberReferences(references, parentSymbol, *targetSymbol);
            }
            else if (gParentSymbol.kind == ast::SymbolKind::Package &&
                     ast::Type::isKind(parentSymbol.kind)) {
                // submembers in the case of structs and enums
                addMemberReferences(references, gParentSymbol, *targetSymbol, true);
            }
            else {
                if (targetLoc.buffer() != doc->getBuffer()) {
                    analysis->addLocalReferences(references, targetSymbol->location, targetName);
                }
            }
        }
    }

    return references.empty() ? std::nullopt : std::make_optional(std::move(references));
}

std::optional<lsp::WorkspaceEdit> ServerDriver::getDocRename(const URI& uri,
                                                             const lsp::Position& position,
                                                             std::string_view newName) {
    // Reuse getDocReferences to find all locations (including declaration)
    auto references = getDocReferences(uri, position, /* includeDeclaration */ true);
    if (!references || references->empty()) {
        return std::nullopt;
    }

    // Group edits by URI
    std::unordered_map<std::string, std::vector<lsp::TextEdit>> changes;

    for (const auto& loc : *references) {
        lsp::TextEdit edit{
            .range = loc.range,
            .newText = std::string(newName),
        };
        changes[loc.uri.str()].push_back(edit);
    }

    return lsp::WorkspaceEdit{.changes = changes};
}

void ServerDriver::publishInactiveRegions(SlangDoc& doc) {
    if (!client.experimentalCapabilities.inactiveRegionsSupported)
        return;

    auto regions = doc.getInactiveRegions();

    client.onTextDocumentInactiveRegions(lsp::InactiveRegionsParams{
        .uri = doc.getURI(),
        .regions = std::move(regions),
    });
}

} // namespace server
