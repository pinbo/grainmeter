#!/usr/bin/env bash
#
# package_macos_dist.sh -- package the GUI and CLI together into a plain
# dist/ folder, each fully self-contained with its own bundled
# dependencies. No .dmg, no hdiutil, no staging/symlink dance -- just a
# folder you can zip, copy to another Mac, or upload wherever, with
# nothing macOS-specific about the transport.
#
# Two separate bundling passes (matching what each binary actually needs
# to be relocatable, kept independent from each other on purpose --
# duplicating a few tens of MB of dylibs is a small price for both pieces
# being genuinely standalone rather than depending on each other's
# internal layout):
#   1. macdeployqt bundles Qt (frameworks + platform plugins) into
#      Grainmeter.app.
#   2. dylibbundler bundles OpenCV (and its own deps: libpng, libjpeg,
#      zlib, ...) twice -- once for the CLI copy living inside
#      Grainmeter.app (macdeployqt only touches Qt dependencies, so this
#      is a separate required step), and once for the flat `grainmeter`
#      copy at the top level of dist/, so it works as a plain standalone
#      binary too, without needing the .app bundle present at all.
#
# Usage:
#   ./scripts/package_macos_dist.sh [build-dir]
#
# Requires (install once):
#   brew install dylibbundler
# macdeployqt comes with your Qt install (via `brew install qt`).
#
# Output: dist/ containing:
#   grainmeter        (CLI, self-contained, with its own lib/ folder)
#   lib/*.dylib       (OpenCV + deps, for the standalone grainmeter above)
#   Grainmeter.app/    (GUI, self-contained, CLI copy bundled inside too)

set -euo pipefail

if [[ "$(uname)" != "Darwin" ]]; then
    echo "Error: this script only runs on macOS." >&2
    exit 1
fi

BUILD_DIR="${1:-build}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

DIST_DIR="dist"
APP_BUNDLE_SRC="${BUILD_DIR}/gui/grainmeter-gui.app"
CLI_BIN="${BUILD_DIR}/grainmeter"

# Several Homebrew-built libraries (OpenBLAS, OpenEXR's Iex/IlmThread/Imath,
# TBB) ship with more than one identical LC_RPATH load command already
# baked in from their own build -- dylibbundler rewrites each one's *value*
# but never deduplicates the *entries*, so the bundled copy ends up with
# the same rpath string listed 2-4 times. Recent dyld refuses to load a
# library at all when that happens ("Library not loaded ... duplicate
# LC_RPATH"), even though none of these files' own dependencies actually
# need rpath expansion in the first place (dylibbundler's -p bakes explicit
# @executable_path/... paths into every load command already). Rather than
# stripping rpaths unconditionally -- Qt's frameworks genuinely cross-
# reference each other via @rpath/ and would break -- this collapses each
# file down to at most one copy of each distinct rpath value, which is
# always safe and fixes the duplicate-load failure either way.
dedupe_rpaths() {
    local f="$1" path count extra i
    while read -r count path; do
        if [[ "$count" -gt 1 ]]; then
            extra=$((count - 1))
            for ((i = 0; i < extra; i++)); do
                install_name_tool -delete_rpath "$path" "$f" 2>/dev/null || true
            done
        fi
    done < <(otool -l "$f" 2>/dev/null | awk '/cmd LC_RPATH/{getline; getline; sub(/^ *path /,""); sub(/ \(offset.*/,""); print}' | sort | uniq -c)
}

dedupe_rpaths_in_dir() {
    local dir="$1" f
    find "$dir" -type f | while IFS= read -r f; do
        file "$f" 2>/dev/null | grep -q "Mach-O" && dedupe_rpaths "$f"; true
    done
}

echo "==> Checking prerequisites"

if [[ ! -d "$APP_BUNDLE_SRC" ]]; then
    echo "Error: $APP_BUNDLE_SRC not found. Build both targets first:" >&2
    echo "  cmake -B $BUILD_DIR -DCMAKE_BUILD_TYPE=Release" >&2
    echo "  cmake --build $BUILD_DIR" >&2
    echo "(If this still doesn't appear, Qt6 wasn't found at configure time --" >&2
    echo " check the cmake output for \"Qt6 found\" vs \"Qt6 Widgets not found\".)" >&2
    exit 1
fi
if [[ ! -x "$CLI_BIN" ]]; then
    echo "Error: $CLI_BIN not found or not executable. Build first (see above)." >&2
    exit 1
fi

find_macdeployqt() {
    if command -v macdeployqt >/dev/null 2>&1; then
        command -v macdeployqt
        return 0
    fi
    for prefix_cmd in "brew --prefix qt" "brew --prefix qt6"; do
        if prefix=$(eval "$prefix_cmd" 2>/dev/null); then
            if [[ -x "$prefix/bin/macdeployqt" ]]; then
                echo "$prefix/bin/macdeployqt"
                return 0
            fi
        fi
    done
    return 1
}

MACDEPLOYQT="$(find_macdeployqt || true)"
if [[ -z "$MACDEPLOYQT" ]]; then
    echo "Error: macdeployqt not found. It ships with Qt (brew install qt)." >&2
    echo "If Qt is installed, try: \$(brew --prefix qt)/bin/macdeployqt" >&2
    exit 1
fi
echo "    macdeployqt: $MACDEPLOYQT"

if ! command -v dylibbundler >/dev/null 2>&1; then
    echo "Error: dylibbundler not found. Install it with:" >&2
    echo "  brew install dylibbundler" >&2
    exit 1
fi
echo "    dylibbundler: $(command -v dylibbundler)"
echo "    Building for: $(uname -m) (the other machine must be the same"
echo "                  architecture -- arm64 for Apple Silicon, x86_64 for Intel)"

echo "==> Setting up $DIST_DIR"
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR/lib"

# ---------------------------------------------------------------------
# Part 1: the .app bundle (GUI, with the CLI copied inside it too)
# ---------------------------------------------------------------------
echo "==> Copying Grainmeter.app"
cp -R "$APP_BUNDLE_SRC" "$DIST_DIR/Grainmeter.app"

echo "==> Copying grainmeter CLI into Grainmeter.app (Contents/MacOS/)"
# The GUI looks for the CLI next to its own executable first, so once both
# live in Contents/MacOS/ together, no path configuration is needed
# inside the app -- the "CLI executable" field in the Options panel will
# auto-fill correctly for whoever you send this to.
cp "$CLI_BIN" "$DIST_DIR/Grainmeter.app/Contents/MacOS/grainmeter"

echo "==> Running dylibbundler on Grainmeter.app's internal CLI copy (bundles OpenCV)"
# Deliberately runs *before* macdeployqt, not after. dylibbundler's -od -b
# combination doesn't just follow the CLI's own OpenCV dependency chain --
# it sweeps everything already sitting in the -d output directory. Running
# it second (against a Frameworks/ that macdeployqt had already filled
# with Qt's .framework bundles) corrupts them: dylibbundler doesn't handle
# the framework Versions/A/ layout correctly, and a subsequent codesign
# --verify on the bundle fails on nested components (e.g.
# libbrotlicommon.1.dylib), leaving QtCore/QtGui/QtWidgets missing by the
# time the bundle is done even though they'd landed fine moments earlier.
# Going CLI-deps-first means dylibbundler only ever sees flat .dylib files
# it understands; Qt's frameworks don't exist yet for it to mangle.
mkdir -p "$DIST_DIR/Grainmeter.app/Contents/Frameworks"
dylibbundler -od -b \
    -x "$DIST_DIR/Grainmeter.app/Contents/MacOS/grainmeter" \
    -d "$DIST_DIR/Grainmeter.app/Contents/Frameworks" \
    -p "@executable_path/../Frameworks/" \
    -s /opt/homebrew/lib -s /usr/local/lib

echo "==> Running macdeployqt on Grainmeter.app (bundles Qt)"
# -libpath=/opt/homebrew/lib works around a Homebrew Qt packaging quirk:
# Homebrew splits Qt into separate formulas (qtbase, qtsvg, qtpdf, ...), and
# plugins built as part of qtbase (e.g. imageformats/libqjpeg.dylib) carry
# an @rpath list that only covers qtbase's own dependencies, not sibling
# formulas -- so without this, macdeployqt fails to resolve even
# QtCore/QtGui/QtWidgets themselves ("Cannot resolve rpath ...") and skips
# bundling them entirely, while still rewriting grainmeter-gui's own load
# commands to @executable_path/../Frameworks/... -- producing an app that
# "succeeds" here but fails to launch (dyld: Library not loaded).
#
# -no-codesign: newer macdeployqt (Qt 6.5+) ad-hoc signs everything it
# touches by default, which can log a scary-looking (but non-fatal)
# "Codesign signing error" for a dylib install_name_tool touched earlier.
# Harmless either way since this script re-signs the whole bundle itself
# below -- skipping macdeployqt's own redundant pass just avoids the
# misleading error output.
"$MACDEPLOYQT" "$DIST_DIR/Grainmeter.app" -verbose=1 -no-codesign -libpath=/opt/homebrew/lib -libpath=/usr/local/lib

echo "==> Removing duplicate LC_RPATH entries left by dylibbundler"
dedupe_rpaths_in_dir "$DIST_DIR/Grainmeter.app/Contents"

echo "==> Ad-hoc code-signing Grainmeter.app"
# Ad-hoc signing (no Developer ID) is enough for the app to *launch* on
# Apple Silicon, including on other people's Macs -- but macOS Gatekeeper
# will still show an "unidentified developer" warning for anything
# downloaded from the internet (quarantine flag), which the person you
# send it to has to click through once (System Settings > Privacy &
# Security > "Open Anyway", or right-click > Open). Avoiding that warning
# entirely requires a paid Apple Developer ID and notarization -- out of
# scope here.
codesign --force --deep --sign - "$DIST_DIR/Grainmeter.app"

# ---------------------------------------------------------------------
# Part 2: the flat, standalone CLI copy (independent of the .app)
# ---------------------------------------------------------------------
echo "==> Copying standalone grainmeter CLI"
cp "$CLI_BIN" "$DIST_DIR/grainmeter"

echo "==> Running dylibbundler on the standalone CLI (bundles OpenCV)"
dylibbundler -od -b \
    -x "$DIST_DIR/grainmeter" \
    -d "$DIST_DIR/lib" \
    -p "@executable_path/lib/" \
    -s /opt/homebrew/lib -s /usr/local/lib

echo "==> Removing duplicate LC_RPATH entries left by dylibbundler"
dedupe_rpaths "$DIST_DIR/grainmeter"
find "$DIST_DIR/lib" -type f | while IFS= read -r f; do
    file "$f" 2>/dev/null | grep -q "Mach-O" && dedupe_rpaths "$f"; true
done

echo "==> Ad-hoc code-signing the standalone CLI"
codesign --force --deep --sign - "$DIST_DIR/grainmeter"
find "$DIST_DIR/lib" -type f -exec codesign --force --sign - {} \; 2>&1 | grep -v "replacing existing signature" || true

# ---------------------------------------------------------------------
# Verify
# ---------------------------------------------------------------------
echo "==> Verifying both binaries are self-contained"
echo "    (should list no /opt/homebrew or /usr/local paths below)"
otool -L "$DIST_DIR/grainmeter" | grep -E "homebrew|/usr/local" || echo "    dist/grainmeter: OK, none found"
otool -L "$DIST_DIR/Grainmeter.app/Contents/MacOS/grainmeter" | grep -E "homebrew|/usr/local" || echo "    Grainmeter.app CLI: OK, none found"
otool -L "$DIST_DIR/Grainmeter.app/Contents/MacOS/grainmeter-gui" | grep -E "homebrew|/usr/local" || echo "    Grainmeter.app GUI:  OK, none found"

echo ""
echo "Done: $PROJECT_ROOT/$DIST_DIR/"
echo "  grainmeter        -- standalone CLI (needs lib/ alongside it)"
echo "  lib/               -- OpenCV + deps, for the standalone CLI above"
echo "  Grainmeter.app/     -- GUI + its own internal CLI copy, fully self-contained"
echo ""
echo "Zip or copy the whole dist/ folder to distribute both together; each"
echo "piece is independently relocatable within it (Grainmeter.app doesn't"
echo "need lib/, and grainmeter doesn't need Grainmeter.app -- only"
echo "grainmeter + lib/ need to travel together with each other)."
