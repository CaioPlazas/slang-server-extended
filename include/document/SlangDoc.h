//------------------------------------------------------------------------------
// SlangDoc.h
// Document container for managing SystemVerilog syntax trees and analysis
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#pragma once

#include "document/ShallowAnalysis.h"
#include "lsp/LspTypes.h"
#include "lsp/URI.h"
#include <memory>
#include <optional>
#include <sys/types.h>
#include <vector>

#include "slang/ast/Compilation.h"
#include "slang/ast/Symbol.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceLocation.h"
#include "slang/text/SourceManager.h"
#include "slang/util/Bag.h"
namespace server {
/// Container around an open document, syntax tree, and shallow analysis. Isn't aware of any broader
/// compilation context at the moment. Creates a syntax tree at the minimum, and an analysis if
/// required.

using namespace slang;

class DocumentHandle;
class ServerDriver;

class SlangDoc {
private:
    /// Reference to the server driver, for grabbing other dependent documents
    ServerDriver& m_driver;

    /// Reference to the source manager
    slang::SourceManager& m_sourceManager;

    /// Options bag for compilation and analysis
    slang::Bag m_options;

    /// The URI of the document
    URI m_uri;

    /// The buffer of the actual source text (no expansions)
    slang::SourceBuffer m_buffer;

    /// The syntax tree for this document
    std::shared_ptr<slang::syntax::SyntaxTree> m_tree;

    /// List of weak pointers to documents that this one depends on. Owned by
    std::vector<std::shared_ptr<SlangDoc>> m_dependentDocuments;

    /// Document analysis for syntax and symbol analysis.
    /// Shared so that callers can hold the analysis alive even if getAnalysis() recreates it.
    std::shared_ptr<ShallowAnalysis> m_analysis;

    /// If non-empty, this document's text is only syntactically valid once spliced into these
    /// owner documents via `\`include` (e.g. a `.vh`/`.svh` fragment meant to be included inside
    /// a module body). Such a document never parses its own standalone syntax tree — instead
    /// getSyntaxTree()/getAnalysis() borrow the first owner's compiled tree, and diagnostics are
    /// unioned across every owner (see issueDiagnosticsTo).
    std::vector<std::shared_ptr<SlangDoc>> m_includeOwners;

    // For testing
    friend class DocumentHandle;

public:
    SlangDoc(ServerDriver& driver, URI uri, slang::SourceBuffer buffer);

    // Open a Document from a syntax tree (parsed from slang Driver)
    static std::shared_ptr<SlangDoc> fromTree(ServerDriver& driver,
                                              std::shared_ptr<slang::syntax::SyntaxTree> tree);

    // Open a Document from text (LSP open)
    static std::shared_ptr<SlangDoc> fromText(ServerDriver& driver, const URI& uri,
                                              std::string_view text);

    // Open a Document from file
    static std::shared_ptr<SlangDoc> open(ServerDriver& driver, const URI& uri);

    /// Open a Document whose content is only valid when `\`include`d into other files (e.g. a
    /// `.vh`/`.svh` fragment). `owners` are the documents already known to `\`include` this path;
    /// must be non-empty. `text` is live editor text to splice in (from openDocument); pass
    /// nullopt for a disk-backed lookup with no live text (e.g. ServerDriver::getDocument() on a
    /// cache miss) -- this just borrows the buffer already embedded in the owner's tree instead
    /// of re-syncing it, which would otherwise mint a fresh BufferID and needlessly invalidate
    /// the owner's cached tree on every call.
    static std::shared_ptr<SlangDoc> fromIncludeOwner(
        ServerDriver& driver, const URI& uri, std::optional<std::string_view> text,
        std::vector<std::shared_ptr<SlangDoc>> owners);

    /// True if this document only makes sense spliced into other files via `\`include` (see
    /// fromIncludeOwner).
    bool isIncludeFragment() const { return !m_includeOwners.empty(); }

    /// The documents that `\`include` this fragment. Empty unless isIncludeFragment().
    const std::vector<std::shared_ptr<SlangDoc>>& getIncludeOwners() const {
        return m_includeOwners;
    }

    /// Retroactively bind this document to a newly-discovered owner -- either adding it to an
    /// existing fragment's owner set, or (if this was previously parsed standalone because no
    /// owner was known yet) converting it into a fragment. No-op if `owner` is already known.
    /// See ServerDriver::adoptOrphanFragments.
    void addIncludeOwner(std::shared_ptr<SlangDoc> owner);

    SourceManager& getSourceManager() const { return m_sourceManager; }
    const slang::BufferID getBuffer() const { return m_buffer.id; }
    const std::string_view getText() const;
    const URI& getURI() { return m_uri; }
    std::string_view getPath() const { return m_uri.getPath(); }

    /// @brief Get the syntax tree, creating it if necessary
    std::shared_ptr<slang::syntax::SyntaxTree> getSyntaxTree();

    /// @brief Check if analysis exists without creating it
    bool hasAnalysis() const { return m_analysis != nullptr && m_analysis->hasValidBuffers(); }

    /// @brief Get the analysis, creating it if necessary.
    /// Returns a shared_ptr so callers can hold the analysis alive independently of this document.
    std::shared_ptr<ShallowAnalysis> getAnalysis(bool refreshDependencies = false);

    ////////////////////////////////////////////////
    /// Indexed Syntax Tree Methods
    ////////////////////////////////////////////////

    /// Methods dependent on the indexed syntax tree
    const slang::parsing::Token* getTokenAt(slang::SourceLocation loc) {
        return getAnalysis()->getTokenAt(loc);
    }

    const slang::parsing::Token* getWordTokenAt(slang::SourceLocation loc) {
        return getAnalysis()->getWordTokenAt(loc);
    }

    ////////////////////////////////////////////////
    /// Shallow Compilation Methods
    ////////////////////////////////////////////////

    const std::unique_ptr<slang::ast::Compilation>& getCompilation() {
        return getAnalysis()->getCompilation();
    }

    /// Return the scope at this location, if any. Does not return the root scope.
    const slang::ast::Scope* getScopeAt(slang::SourceLocation loc) {
        return getAnalysis()->getScopeAt(loc);
    }
    ////////////////////////////////////////////////
    /// File Lifecycle
    ////////////////////////////////////////////////
    /// @brief Set dependent documents for this document, updated by driver after document changes
    void setDependentDocuments(const std::vector<std::shared_ptr<SlangDoc>>& dependentDocs) {
        m_dependentDocuments = dependentDocs;
    }

    void onChange(const std::vector<lsp::TextDocumentContentChangeEvent>& contentChanges);

    /// @brief Re-read the buffer from disk (used for external file changes)
    /// @return true if successful, false if the read failed
    bool reloadBuffer();

    bool textMatches(std::string_view text);
    ////////////////////////////////////////////////
    /// Lsp Functions
    ////////////////////////////////////////////////

    void issueParseDiagnostics(DiagnosticEngine& diagEngine);

    /// @brief Issue all diagnostics from this document to the given diagnostic engine
    /// Issue diagnostics to the diagnostic engine
    /// @param diagEngine The diagnostic engine to issue to
    /// @param parseOnly If true, only issue parse diagnostics (for when ServerCompilation handles
    /// semantic diags)
    void issueDiagnosticsTo(slang::DiagnosticEngine& diagEngine);

    /// @brief For the document symbols request
    // TODO: should this use the shallow compilation instead of syntax tree?
    std::vector<lsp::DocumentSymbol> getSymbols() { return getAnalysis()->getDocSymbols(); }

    std::optional<slang::SourceLocation> getLocation(const lsp::Position& position) {
        return m_sourceManager.getSourceLocation(m_buffer.id, position.line, position.character);
    }

    // Previous text on and before a position
    std::string getPrevText(const lsp::Position& position);

    std::vector<lsp::DocumentLink> getDocLinks() { return getAnalysis()->getDocLinks(); }

    std::vector<lsp::Range> getInactiveRegions();

private:
    /// @brief Issues the union of diagnostics from every owner in m_includeOwners, filtered to
    /// this fragment's buffer within each owner and deduplicated. See m_includeOwners.
    void issueIncludeFragmentDiagnostics(slang::DiagnosticEngine& diagEngine);
};

} // namespace server
template<>
struct fmt::formatter<server::SlangDoc> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

    template<typename FormatContext>
    constexpr auto format(const server::SlangDoc& doc, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "{}", doc.getPath());
    }
};
