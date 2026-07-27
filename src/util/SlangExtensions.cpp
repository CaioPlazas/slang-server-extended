//------------------------------------------------------------------------------
// SlangExtensions.cpp
// Functions that only deal with slang objects and could potentially go upstream
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#include "util/SlangExtensions.h"

#include <filesystem>

namespace server {

using namespace slang;

bool hasValidBuffers(const SourceManager& sm, const std::shared_ptr<syntax::SyntaxTree>& tree) {
    if (!tree)
        return false;

    // Check the main buffer and included buffers
    auto buffers = tree->getSourceBufferIds();
    for (auto buf : buffers) {
        if (buf.valid() && !sm.isLatestData(buf)) {
            return false;
        }
    }

    return true;
}

std::optional<BufferID> findBufferForPath(const SourceManager& sm, const syntax::SyntaxTree& tree,
                                          std::string_view path) {
    // Both `path` (always a URI::getPath(), which is always absolute) and sm.getFullPath()
    // return already-absolute paths in normal operation, so a purely lexical comparison -- zero
    // syscalls -- resolves the overwhelming majority of lookups. This matters a lot: this
    // function is called in loops over every buffer of every document in the build (see
    // ServerDriver::findIncludeOwners/adoptOrphanFragments), so a per-call stat/readlink here
    // multiplies into a syscall storm on large projects, especially over NFS.
    //
    // Only fall back to weakly_canonical (which does resolve symlinks/`.`/`..` via the
    // filesystem) for the rare case where a lexical comparison misses -- e.g. the same file
    // reached through two different symlinked paths.
    std::filesystem::path targetLexical = std::filesystem::path(path).lexically_normal();

    // A tree's buffers include every file `included into it, not just its own root file, so
    // this also finds a fragment's buffer inside the tree of whatever file included it.
    for (auto bufId : tree.getSourceBufferIds()) {
        if (!bufId.valid())
            continue;

        if (std::filesystem::path(sm.getFullPath(bufId)).lexically_normal() == targetLexical)
            return bufId;
    }

    std::error_code ec;
    auto targetCanonical = std::filesystem::weakly_canonical(path, ec);
    if (ec)
        return std::nullopt;

    for (auto bufId : tree.getSourceBufferIds()) {
        if (!bufId.valid())
            continue;

        std::error_code bufEc;
        auto bufPath = std::filesystem::weakly_canonical(sm.getFullPath(bufId), bufEc);
        if (bufEc)
            continue;

        if (bufPath == targetCanonical)
            return bufId;
    }
    return std::nullopt;
}

} // namespace server
