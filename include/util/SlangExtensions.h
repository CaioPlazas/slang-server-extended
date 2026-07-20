//------------------------------------------------------------------------------
// SlangExtensions.h
// Functions that only deal with slang objects and could potentially go upstream
//
// SPDX-FileCopyrightText: Hudson River Trading
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------

#pragma once

#include <memory>
#include <optional>
#include <string_view>

#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"

namespace server {

/// @brief Check if a syntax tree has valid (latest) buffers in the source manager
/// @param sm The source manager to check against
/// @param tree The syntax tree to validate
/// @return true if all buffers in the tree are up-to-date
bool hasValidBuffers(const slang::SourceManager& sm,
                     const std::shared_ptr<slang::syntax::SyntaxTree>& tree);

/// @brief Finds the buffer within `tree` whose file path matches `path`, if any. Used to locate
/// the buffer that a `\`include`d file (e.g. a `.vh`/`.svh` fragment) was expanded into within
/// an owning tree.
/// @param sm The source manager the tree's buffers belong to
/// @param tree The syntax tree to search
/// @param path The file path to look for, as returned by URI::getPath()
/// @return The matching buffer id, or nullopt if no buffer in the tree has that path
std::optional<slang::BufferID> findBufferForPath(const slang::SourceManager& sm,
                                                  const slang::syntax::SyntaxTree& tree,
                                                  std::string_view path);

} // namespace server
