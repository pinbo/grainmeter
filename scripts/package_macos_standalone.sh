#!/usr/bin/env bash
#
# package_macos_standalone.sh -- bundle OpenCV (and its own dependencies:
# libpng, libjpeg, zlib, ...) next to the grainmeter binary and rewrite its
# load commands to point at the bundled copies, so the result runs on a
# Mac that doesn't have OpenCV (or Homebrew) installed at all.
#
# Usage:
#   ./scripts/package_macos_standalone.sh [build-dir]
#
# Requires (install once):
#   brew install dylibbundler
#
# Output: dist/grainmeter-macos/ containing:
#   grainmeter       (the executable, load commands rewritten to @executable_path/lib/...)
#   lib/*.dylib      (OpenCV + its dependencies, copied and made relocatable)
#
# Copy the whole dist/grainmeter-macos/ folder to the other Mac -- both
# pieces are required, the binary alone is not self-contained -- and run
# ./grainmeter from inside it.

set -euo pipefail

if [[ "$(uname)" != "Darwin" ]]; then
    echo "Error: this script only runs on macOS." >&2
    exit 1
fi

BUILD_DIR="${1:-build}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

BIN="$BUILD_DIR/grainmeter"
DIST_DIR="dist/grainmeter-macos"

echo "==> Checking prerequisites"
if [[ ! -x "$BIN" ]]; then
    echo "Error: $BIN not found or not executable. Build first:" >&2
    echo "  cmake -B $BUILD_DIR -DCMAKE_BUILD_TYPE=Release" >&2
    echo "  cmake --build $BUILD_DIR" >&2
    exit 1
fi
if ! command -v dylibbundler >/dev/null 2>&1; then
    echo "Error: dylibbundler not found. Install it with:" >&2
    echo "  brew install dylibbundler" >&2
    exit 1
fi
echo "    dylibbundler: $(command -v dylibbundler)"
echo "    Building for: $(uname -m) (the other Mac must be the same architecture --"
echo "                  arm64 for Apple Silicon, x86_64 for Intel -- see note below"
echo "                  if you need to support both)"

echo "==> Setting up $DIST_DIR"
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR/lib"
cp "$BIN" "$DIST_DIR/grainmeter"

echo "==> Bundling OpenCV and its dependencies"
# -od : overwrite already-bundled dylibs if re-run
# -b  : bundle (copy dependencies + rewrite the executable's load commands)
# -x  : the executable to fix up
# -d  : where to copy the dependency dylibs to
# -p  : the path prefix baked into the executable's rewritten load commands,
#       relative to the executable itself via @executable_path
# -s  : extra places to look for a dependency if its recorded path doesn't
#       resolve directly (covers both Homebrew install locations)
dylibbundler -od -b \
    -x "$DIST_DIR/grainmeter" \
    -d "$DIST_DIR/lib" \
    -p "@executable_path/lib/" \
    -s /opt/homebrew/lib -s /usr/local/lib

echo "==> Ad-hoc code-signing (required on Apple Silicon after modifying the binary)"
codesign --force --sign - "$DIST_DIR/grainmeter"

echo "==> Verifying the binary is self-contained"
echo "    (should list no /opt/homebrew or /usr/local paths below)"
if otool -L "$DIST_DIR/grainmeter" | grep -E "homebrew|/usr/local"; then
    echo "    ^ WARNING: the above paths were not bundled -- the binary will"
    echo "      still fail to launch on a Mac without them installed."
else
    echo "    OK: none found"
fi

echo ""
echo "Done: $DIST_DIR/"
echo "Copy this whole folder to the other Mac and run ./grainmeter from inside it"
echo "(the lib/ folder must stay next to the executable -- it's not a single"
echo "self-contained file, it's a self-contained folder)."
