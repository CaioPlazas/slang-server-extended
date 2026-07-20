//------------------------------------------------------------------------------
// SlangDoc.cpp
// Implementation of SlangDoc class
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "document/SlangDoc.h"

#include "ServerDriver.h"
#include "document/ShallowAnalysis.h"
#include "lsp/URI.h"
#include "util/Logging.h"
#include "util/SlangExtensions.h"
#include <algorithm>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <stdexcept>
#include <string>
#include <string_view>

#include "slang/ast/Compilation.h"
#include "slang/diagnostics/DeclarationsDiags.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/diagnostics/ExpressionsDiags.h"
#include "slang/diagnostics/LookupDiags.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceLocation.h"
#include "slang/text/SourceManager.h"
namespace server {

using namespace slang;

SlangDoc::SlangDoc(ServerDriver& driver, URI uri, SourceBuffer buffer) :
    m_driver(driver), m_sourceManager(driver.sm), m_options(driver.options), m_uri(uri),
    m_buffer(buffer) {
}

std::shared_ptr<SlangDoc> SlangDoc::fromTree(ServerDriver& driver,
                                             std::shared_ptr<syntax::SyntaxTree> tree) {
    auto buffer = SourceBuffer{
        .data = driver.sm.getSourceText(tree->getSourceBufferIds()[0]),
        .id = tree->getSourceBufferIds()[0],
    };
    auto uri = URI::fromFile(driver.sm.getFullPath(tree->getSourceBufferIds()[0]));
    auto ret = std::make_shared<SlangDoc>(driver, uri, buffer);
    ret->m_tree = tree;
    return ret;
}

std::shared_ptr<SlangDoc> SlangDoc::fromText(ServerDriver& driver, const URI& uri,
                                             std::string_view text) {
    std::string_view path = uri.getPath();
    SourceBuffer buffer;

    // Check if this path was previously cached (e.g., from an include)
    // If so, we need to replace the old buffer with the editor's version
    if (driver.sm.isCached(path)) {
        auto existingBuffer = driver.sm.readSource(path, nullptr).value();
        SmallVector<char> newBuffer;
        newBuffer.insert(newBuffer.end(), text.begin(), text.end());
        if (newBuffer.empty() || newBuffer.back() != '\0')
            newBuffer.push_back('\0');
        buffer = driver.sm.replaceBuffer(existingBuffer.id, std::move(newBuffer));
    }
    else {
        buffer = driver.sm.assignText(path, text);
    }

    return std::make_shared<SlangDoc>(driver, uri, buffer);
}

std::shared_ptr<SlangDoc> SlangDoc::open(ServerDriver& driver, const URI& uri) {
    auto buffer = driver.sm.readSource(uri.getPath(), nullptr).value();
    return std::make_shared<SlangDoc>(driver, uri, buffer);
}

static SourceBuffer syncOwnerBuffer(SourceManager& sm, BufferID id, std::string_view text) {
    SmallVector<char> newBuffer;
    newBuffer.insert(newBuffer.end(), text.begin(), text.end());
    if (newBuffer.empty() || newBuffer.back() != '\0')
        newBuffer.push_back('\0');
    return sm.replaceBuffer(id, std::move(newBuffer));
}

std::shared_ptr<SlangDoc> SlangDoc::fromIncludeOwner(
    ServerDriver& driver, const URI& uri, std::optional<std::string_view> text,
    std::vector<std::shared_ptr<SlangDoc>> owners) {
    SLANG_ASSERT(!owners.empty());
    auto tree = owners[0]->getSyntaxTree();
    auto bufId = findBufferForPath(driver.sm, *tree, uri.getPath());
    SLANG_ASSERT(bufId.has_value());

    // With live editor text, sync it into the shared buffer (in case it differs from what's on
    // disk, e.g. unsaved edits) -- this also marks every owner's cached tree stale, so they'll
    // reparse with this content on next access (see hasValidBuffers()). Without text (a
    // disk-backed lookup, e.g. from getDocument()), just borrow the buffer already embedded in
    // the owner's tree instead -- syncing here would mint a fresh BufferID and needlessly
    // invalidate the owner's cached tree on every such lookup.
    SourceBuffer buffer = text ? syncOwnerBuffer(driver.sm, *bufId, *text)
                              : SourceBuffer{.data = driver.sm.getSourceText(*bufId), .id = *bufId};

    auto ret = std::make_shared<SlangDoc>(driver, uri, buffer);
    ret->m_includeOwners = std::move(owners);
    return ret;
}

void SlangDoc::addIncludeOwner(std::shared_ptr<SlangDoc> owner) {
    for (auto& existing : m_includeOwners) {
        if (existing == owner)
            return;
    }

    // Push our current (possibly live-edited) text into the new owner's buffer for this path,
    // mirroring fromIncludeOwner's initial sync -- otherwise the owner's tree would reflect
    // whatever was last on disk instead of what's live in the editor.
    std::string text{getText()};
    if (!text.empty() && text.back() == '\0')
        text.pop_back();

    auto tree = owner->getSyntaxTree();
    if (auto bufId = findBufferForPath(m_sourceManager, *tree, m_uri.getPath())) {
        syncOwnerBuffer(m_sourceManager, *bufId, text);
    }

    m_includeOwners.push_back(std::move(owner));
}

const std::string_view SlangDoc::getText() const {
    // null terminator is included in data
    return m_sourceManager.getSourceText(m_buffer.id);
}

std::shared_ptr<syntax::SyntaxTree> SlangDoc::getSyntaxTree() {
    if (!m_includeOwners.empty()) {
        // This fragment has no syntax tree of its own -- it's only valid spliced into an
        // owner. Borrow the first owner's (re-parsing it if stale).
        auto ownerTree = m_includeOwners[0]->getSyntaxTree();

        // Only re-resolve our buffer id when the owner actually produced a new tree (a reparse,
        // or the first call) -- the owner may get a fresh buffer id for this path on every
        // reparse, but getSyntaxTree() is called very frequently (every hover, completion,
        // semantic token request, etc.), and findBufferForPath scans every buffer in the tree,
        // so redoing that on every call when nothing changed is wasted work.
        if (ownerTree != m_tree) {
            m_tree = ownerTree;
            if (auto bufId = findBufferForPath(m_sourceManager, *m_tree, m_uri.getPath())) {
                m_buffer.id = *bufId;
            }
        }
        return m_tree;
    }

    if (!m_tree) {
        // Will read the cached file data if it exists
        if (!m_sourceManager.isLatestData(m_buffer.id)) {
            m_buffer = m_sourceManager.readSource(m_uri.getPath(), nullptr).value();
        }
        m_tree = syntax::SyntaxTree::fromBuffer(m_buffer, m_sourceManager, m_options);
    }
    else if (!hasValidBuffers(m_sourceManager, m_tree)) {
        // Tree has invalid buffers, need to reparse
        m_buffer = m_sourceManager.readSource(m_uri.getPath(), nullptr).value();
        m_tree = syntax::SyntaxTree::fromBuffer(m_buffer, m_sourceManager, m_options);
    }
    return m_tree;
}

std::shared_ptr<ShallowAnalysis> SlangDoc::getAnalysis(bool refreshDependencies) {
    if (!m_analysis || !m_analysis->hasValidBuffers() || refreshDependencies) {
        // Load dependent documents from driver if not already loaded
        if (m_dependentDocuments.empty() || refreshDependencies) {
            m_dependentDocuments = m_driver.getDependentDocs(getSyntaxTree());
        }

        std::vector<std::shared_ptr<syntax::SyntaxTree>> trees = {getSyntaxTree()};
        for (const auto& doc : m_dependentDocuments) {
            if (auto depTree = doc->getSyntaxTree()) {
                trees.push_back(depTree);
            }
        }
        m_analysis = std::make_shared<ShallowAnalysis>(m_sourceManager, m_buffer.id, m_tree,
                                                       m_options, trees);
        INFO("Analyzed {} with tops: {}", m_uri.getPath(),
             fmt::join(m_analysis->getCompilation()->getRoot().topInstances |
                           std::views::transform([](const auto& top) { return top->name; }),
                       ", "));
    }

    return m_analysis;
}

std::string SlangDoc::getPrevText(const lsp::Position& position) {
    return std::string(
        m_sourceManager.getLine(m_buffer.id, position.line + 1).substr(0, position.character));
}

bool SlangDoc::textMatches(std::string_view text) {
    // Just compute line offsets to validate UTF-8
    auto bufText = getText();
    if (bufText.size() != text.size() + 1) {
        ERROR("Text size mismatch: have {}, expected {}", bufText.size(), text.size() + 1);
        return false;
    }
    if (std::memcmp(bufText.data(), text.data(), bufText.size()) != 0) {
        ERROR("Text content mismatch");
        return false;
    }
    return true;
}

void SlangDoc::onChange(const std::vector<lsp::TextDocumentContentChangeEvent>& contentChanges) {
    // From the LSP spec:
    //
    // The actual content changes. The content changes describe single state changes to the
    // document. So if there are two content changes c1 (at array index 0) and c2 (at array
    // index 1) for a document in state S then c1 moves the document from S to S' and c2 from
    // S' to S''. So c1 is computed on the state S and c2 is computed on the state S'.
    //
    // To mirror the content of a document using change events use the following approach:
    // - start with the same initial content
    // - apply the 'textDocument/didChange' notifications in the order you receive them.
    // - apply the `TextDocumentContentChangeEvent`s in a single notification in the order you
    //   receive them.
    //
    // Partial is defined in the variant first, so it will match first iff range and text are both
    // present. WholeDocument will match if text is present, but only be tried second. Less specific
    // cases are tried later.
    SmallVector<char> buffer;
    std::string_view textView = getText();
    std::vector<size_t> lineOffsets;

    if (contentChanges.size() == 0) {
        ERROR("Empty onChange event");
        return;
    }

    auto getOffsets = [&](lsp::Range range) {
        // Only one thread is able to call onchange, so the offsets remain valid without locking
        SourceManager::computeLineOffsets(textView, lineOffsets);
        auto& start = range.start;
        auto& end = range.end;
        auto startOffset = lineOffsets[start.line] + start.character;
        auto endOffset = lineOffsets[end.line] + end.character;
        if (start.line >= lineOffsets.size() || end.line >= lineOffsets.size()) {
            throw std::runtime_error(fmt::format("Range out of bounds: {},{} / {}", start.line,
                                                 end.line, lineOffsets.size()));
        }
        return std::make_pair(startOffset, endOffset);
    };

    auto ensureNullTerminated = [&]() {
        if (buffer.empty() || buffer.back() != '\0')
            buffer.push_back('\0');
    };

    // Single change (most common)
    rfl::visit(
        [&](const auto& change) {
            using T = std::decay_t<decltype(change)>;
            if constexpr (std::is_same_v<T, lsp::TextDocumentContentChangePartial>) {
                auto offsets = getOffsets(change.range);
                buffer.append(textView.begin(), textView.begin() + offsets.first);
                buffer.append(change.text.begin(), change.text.end());
                buffer.append(textView.begin() + offsets.second, textView.end());
            }
            else {
                buffer.append(change.text.begin(), change.text.end());
            }
        },
        contentChanges[0]);
    ensureNullTerminated();

    // More than one change is rare- typically things like rename actions, or if there's some lag.
    for (size_t i = 1; i < contentChanges.size(); i++) {
        textView = std::string_view{buffer.data(), buffer.size()};
        lineOffsets.clear();
        rfl::visit(
            [&](const auto& change) {
                using T = std::decay_t<decltype(change)>;
                if constexpr (std::is_same_v<T, lsp::TextDocumentContentChangePartial>) {
                    auto offsets = getOffsets(change.range);
                    // handle deletes
                    if (offsets.second > offsets.first) {
                        buffer.erase(buffer.begin() + offsets.first,
                                     buffer.begin() + offsets.second);
                    }
                    // handle inserts
                    buffer.insert(buffer.begin() + offsets.first, change.text.begin(),
                                  change.text.end());
                }
                else {
                    // WholeDocument collapses all prior changes
                    buffer.clear();
                    buffer.append(change.text.begin(), change.text.end());
                    ensureNullTerminated();
                }
            },
            contentChanges[i]);
    }
    m_buffer = m_sourceManager.replaceBuffer(m_buffer.id, std::move(buffer));

    // Invalidate pointers to old buffer
    m_tree.reset();
    m_analysis.reset();
}
bool SlangDoc::reloadBuffer() {
    auto result = m_sourceManager.reloadBuffer(m_buffer.id);
    if (!result) {
        ERROR("Failed to re-read buffer for {}: {}", m_uri.getPath(), result.error().message());
        return false;
    }
    m_buffer = *result;
    m_tree.reset();
    m_analysis.reset();
    return true;
}

void SlangDoc::issueParseDiagnostics(DiagnosticEngine& diagEngine) {
    for (auto& diag : getSyntaxTree()->diagnostics()) {
        diagEngine.issue(diag);
    }
}

namespace {
/// Identifies "the same problem" across different owners of a shared fragment. We can't compare
/// raw source locations, because each owner gives the fragment its own internal buffer id even
/// though the text (and so the line/column of any diagnostic) is identical.
struct DedupeKey {
    DiagCode code;
    size_t line;
    size_t col;
};

bool isSameProblem(const DedupeKey& a, const DedupeKey& b) {
    return a.code == b.code && a.line == b.line && a.col == b.col;
}

/// Issues `diag` to `diagEngine`, but only if both are true:
///   1. It actually points at the fragment's own text (`fragmentBufferId`), not some other line
///      of the owner file.
///   2. An equivalent diagnostic hasn't already been issued for a different owner. `alreadyIssued`
///      is grown with the new key when we do issue something, so later calls see it too.
void issueFragmentDiagnostic(DiagnosticEngine& diagEngine, SourceManager& sourceManager,
                             const Diagnostic& diag, BufferID fragmentBufferId,
                             std::vector<DedupeKey>& alreadyIssued) {
    if (sourceManager.getFullyOriginalLoc(diag.location).buffer() != fragmentBufferId)
        return;

    DedupeKey key{diag.code, sourceManager.getLineNumber(diag.location),
                  sourceManager.getColumnNumber(diag.location)};
    for (auto& seenKey : alreadyIssued) {
        if (isSameProblem(seenKey, key))
            return;
    }

    alreadyIssued.push_back(key);
    diagEngine.issue(diag);
}
} // namespace

namespace {
/// A widely-shared fragment (e.g. a `defines.svh` included by hundreds of files) can have an
/// unbounded number of owners. Diagnosing against every single one has no real UX value past a
/// handful of distinct perspectives, and each one costs a full elaboration of that owner's
/// compilation, so cap how many are analyzed per refresh. See issueIncludeFragmentDiagnostics.
constexpr size_t kMaxFragmentOwnersAnalyzed = 8;
} // namespace

void SlangDoc::issueIncludeFragmentDiagnostics(DiagnosticEngine& diagEngine) {
    // A fragment is only meaningful spliced into its owner(s), and each owner may see it in a
    // different context (its own port/wire declarations, active macros, etc). A real problem
    // may only show up from one owner's perspective -- e.g. a signal that's declared in one
    // including module but not another -- so union diagnostics across every owner, dropping
    // exact duplicates so a universal typo doesn't show up once per owner.
    std::vector<DedupeKey> alreadyIssued;

    // Prefer owners that are actually open in the editor -- those are what the union is really
    // for -- before falling back to whichever others fit under the cap.
    std::vector<std::shared_ptr<SlangDoc>> ordered = m_includeOwners;
    std::stable_partition(ordered.begin(), ordered.end(), [this](const auto& owner) {
        return m_driver.isDocumentOpen(owner->getURI());
    });
    if (ordered.size() > kMaxFragmentOwnersAnalyzed) {
        ordered.resize(kMaxFragmentOwnersAnalyzed);
    }

    for (auto& owner : ordered) {
        auto tree = owner->getSyntaxTree();
        if (!tree)
            continue;

        auto bufId = findBufferForPath(m_sourceManager, *tree, m_uri.getPath());
        if (!bufId)
            continue;

        // Plain parse errors (e.g. a missing semicolon) come straight from the tree.
        for (auto& diag : tree->diagnostics()) {
            issueFragmentDiagnostic(diagEngine, m_sourceManager, diag, *bufId, alreadyIssued);
        }

        // Reuse the owner's own cached analysis instead of building a fresh one keyed on this
        // fragment's buffer id. ShallowAnalysis's semantic/analysis diagnostics come from its
        // compilation (built from all of the owner's dependent trees), which doesn't depend on
        // which buffer id was passed in as "focus" -- issueFragmentDiagnostic already filters by
        // buffer id below, so the result is identical either way. Reusing the cache means a
        // fragment-diagnostics refresh that finds nothing changed (e.g. the user is just
        // navigating around, or a *different* fragment shared by this owner just changed) skips
        // re-elaborating the owner's whole compilation from scratch. It's still invalidated
        // correctly when this fragment itself changes: editing it replaces its shared buffer,
        // which marks the owner's cached analysis stale via ShallowAnalysis::hasValidBuffers().
        auto analysis = owner->getAnalysis();
        for (auto& diag : analysis->getCompilation()->getSemanticDiagnostics()) {
            issueFragmentDiagnostic(diagEngine, m_sourceManager, diag, *bufId, alreadyIssued);
        }
        for (auto& diag : analysis->getAnalysisDiags()) {
            issueFragmentDiagnostic(diagEngine, m_sourceManager, diag, *bufId, alreadyIssued);
        }
    }
}

void SlangDoc::issueDiagnosticsTo(DiagnosticEngine& diagEngine) {
    if (!m_includeOwners.empty()) {
        issueIncludeFragmentDiagnostics(diagEngine);
        return;
    }

    // Issue compilation diagnostics
    auto analysis = getAnalysis(true);
    auto& shallowComp = *analysis->getCompilation();

    // Parse diags (just this tree, others will be handled by their SlangDoc objects
    for (auto& diag : getSyntaxTree()->diagnostics()) {
        diagEngine.issue(diag);
    }

    // Parse and shallow compilation diagnostics
    // There will be many diags outside the buffer, like unknown modules.
    for (auto& diag : shallowComp.getSemanticDiagnostics()) {
        if (m_sourceManager.getFullyOriginalLoc(diag.location).buffer() != m_buffer.id) {
            continue;
        }
        diagEngine.issue(diag);
    }
    // Analysis on the shallow compilation (unused, multidriven, etc)
    for (auto& diag : analysis->getAnalysisDiags()) {
        if (m_sourceManager.getFullyOriginalLoc(diag.location).buffer() != m_buffer.id) {
            continue;
        }
        diagEngine.issue(diag);
    }
}

std::vector<lsp::Range> SlangDoc::getInactiveRegions() {
    std::vector<lsp::Range> result;
    result.reserve(getAnalysis()->syntaxes.disabledRegions.size());

    for (const auto& region : getAnalysis()->syntaxes.disabledRegions) {
        result.push_back(toRange(region, m_sourceManager));
    }

    return result;
}

} // namespace server
