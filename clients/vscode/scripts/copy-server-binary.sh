#!/usr/bin/env bash
# Copies a locally-built slang-server release binary into clients/vscode/bin/ so
# `pnpm package:linux-x64` can bundle it directly into the VSIX. The extension looks
# for it there first (see PathConfigObject.resolveToolPath), before falling back to
# `slang.path` / PATH / a managed GitHub-release download.
#
# Refuses to copy a binary that wasn't configured with an optimized CMAKE_BUILD_TYPE
# (Release/RelWithDebInfo/MinSizeRel) -- bundling a Debug build makes the shipped
# server dramatically slower, since it disables optimizations and enables slang's
# runtime assertions.
set -euo pipefail

SRC="${1:?Usage: copy-server-binary.sh <path-to-slang-server-binary>}"
DEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/bin"

if [[ ! -f "$SRC" ]]; then
  echo "error: $SRC does not exist" >&2
  exit 1
fi

# The binary normally lives at <build-dir>/bin/slang-server, so the build dir
# (which holds CMakeCache.txt) is one level up from the binary's directory.
BUILD_DIR="$(cd "$(dirname "$SRC")/.." && pwd)"
CACHE_FILE="$BUILD_DIR/CMakeCache.txt"

if [[ ! -f "$CACHE_FILE" ]]; then
  echo "error: couldn't find CMakeCache.txt at $CACHE_FILE" >&2
  echo "       (expected \$SRC to be at <build-dir>/bin/<binary>)" >&2
  echo "       refusing to copy a binary of unknown build type" >&2
  exit 1
fi

BUILD_TYPE="$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$CACHE_FILE")"

case "$BUILD_TYPE" in
  Release | RelWithDebInfo | MinSizeRel) ;;
  *)
    echo "error: $SRC was configured with CMAKE_BUILD_TYPE='$BUILD_TYPE'" >&2
    echo "       refusing to bundle a non-optimized build into the VSIX." >&2
    echo "       Configure and build with an optimized preset first, e.g.:" >&2
    echo "         cmake --preset gcc-release && cmake --build build/gcc-release -j\$(nproc) --target slang_server" >&2
    exit 1
    ;;
esac

mkdir -p "$DEST_DIR"
cp "$SRC" "$DEST_DIR/slang-server"
chmod +x "$DEST_DIR/slang-server"
echo "Copied $SRC (CMAKE_BUILD_TYPE=$BUILD_TYPE) -> $DEST_DIR/slang-server"
