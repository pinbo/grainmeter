#!/usr/bin/env bash
#
# package_macos_app.sh -- package the GUI and CLI together into one
# self-contained, distributable .dmg for macOS.
#
# The GUI already looks for the `grainmeter` CLI binary right next to
# itself first (Contents/MacOS/), so rather than building two separate
# distributables, this puts both binaries in the same .app bundle, backed
# by one shared set of bundled dependencies:
#   1. macdeployqt bundles Qt (frameworks + platform plugins) for the GUI.
#   2. dylibbundler bundles OpenCV (and its own deps: libpng, libjpeg,
#      zlib, ...) for the CLI binary now living inside the bundle --
#      macdeployqt only fixes up Qt dependencies of the app it's pointed
#      at, so this is a separate, required step (see
#      package_macos_standalone.sh, which does the same thing for a
#      CLI-only distribution, if you only need the CLI).
# The result: double-click Grainmeter.app for the GUI, or run the CLI
# directly at Grainmeter.app/Contents/MacOS/grainmeter from Terminal --
# both fully self-contained, sharing the same bundled Frameworks rather
# than duplicating Qt/OpenCV into two separate distributables. A symlink
# at the top level of the DMG exposes that CLI path under a plain
# `grainmeter` name for convenience, without copying it anywhere.
#
# Usage:
#   ./scripts/package_macos_app.sh [build-dir]
#
# Requires (install once):
#   brew install dylibbundler
# macdeployqt comes with your Qt install (via `brew install qt`).
#
# Output: grainmeter-<version>-macos.dmg in the project root, containing
#   Grainmeter.app       (GUI + CLI together, fully self-contained)
#   grainmeter            (symlink into the .app's CLI binary, for
#                          Terminal use straight off the mounted DMG, or
#                          drag it wherever you want a standalone copy of
#                          just the CLI entry point)
#   Applications          (symlink, for the standard drag-to-install GUI flow)

set -euo pipefail

if [[ "$(uname)" != "Darwin" ]]; then
    echo "Error: this script only runs on macOS." >&2
    exit 1
fi

BUILD_DIR="${1:-build}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

APP_NAME="Grainmeter"
DMG_NAME="grainmeter-macos.dmg"

APP_BUNDLE="${BUILD_DIR}/gui/grainmeter-gui.app"
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

if [[ ! -d "$APP_BUNDLE" ]]; then
    echo "Error: $APP_BUNDLE not found. Build both targets first:" >&2
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
echo "    Building for: $(uname -m) (the other Mac must be the same architecture --"
echo "                  arm64 for Apple Silicon, x86_64 for Intel)"

echo "==> Rebuilding grainmeter-gui.app from scratch (makes this script safe to re-run)"
# dylibbundler/macdeployqt permanently rewrite grainmeter-gui's own Mach-O
# load commands the first time this runs (absolute Homebrew paths ->
# @executable_path/../Frameworks/...). CMake's incremental build doesn't
# know that happened -- grainmeter-gui.cpp didn't change, so a later `cmake
# --build` (including via the `app` target's own DEPENDS) leaves the
# already-mutated binary alone. Re-running this script against that binary
# then breaks: macdeployqt sees only @executable_path Qt references, logs
# "Could not find any external Qt frameworks to deploy" and does nothing,
# and dylibbundler fails on Frameworks/PlugIns entries that turn out not to
# exist -- and since neither tool's per-file errors are fatal, the script
# still signs and ships a DMG containing a broken app (missing Qt
# frameworks, crashes on launch) while reporting success. Removing the
# bundle so CMake is forced to relink it from original absolute paths is
# what actually fixes that, not just clearing Contents/Frameworks.
rm -rf "$APP_BUNDLE"
cmake --build "$BUILD_DIR" --target grainmeter-gui

echo "==> Copying grainmeter CLI into the app bundle (Contents/MacOS/)"
# The GUI looks for the CLI next to its own executable first, so once both
# live in Contents/MacOS/ together, no path configuration is needed inside
# the shipped app -- and the "CLI executable" field in the Options panel
# will auto-fill correctly for whoever you send this to.
cp "$CLI_BIN" "$APP_BUNDLE/Contents/MacOS/grainmeter"

echo "==> Running dylibbundler (bundles OpenCV + its dependencies for the CLI binary)"
# Deliberately runs *before* macdeployqt, not after. dylibbundler's -od -b
# combination doesn't just follow the CLI's own OpenCV dependency chain --
# it sweeps everything already sitting in the -d output directory. Running
# it second (against a Frameworks/ that macdeployqt had already filled
# with Qt's .framework bundles) corrupted them: dylibbundler doesn't
# handle the framework Versions/A/ layout correctly and a subsequent
# codesign --verify on the bundle would fail on nested components (e.g.
# libbrotlicommon.1.dylib), leaving QtCore/QtGui/QtWidgets missing by the
# time the bundle was done even though they'd landed fine moments earlier.
# Going CLI-deps-first means dylibbundler only ever sees flat .dylib files
# it understands; Qt's frameworks don't exist yet for it to mangle.
mkdir -p "$APP_BUNDLE/Contents/Frameworks"
dylibbundler -od -b \
    -x "$APP_BUNDLE/Contents/MacOS/grainmeter" \
    -d "$APP_BUNDLE/Contents/Frameworks" \
    -p "@executable_path/../Frameworks/" \
    -s /opt/homebrew/lib -s /usr/local/lib

echo "==> Running macdeployqt (bundles Qt frameworks + plugins for the GUI)"
# -libpath=/opt/homebrew/lib works around a Homebrew Qt packaging quirk:
# Homebrew splits Qt into separate formulas (qtbase, qtsvg, qtpdf, ...),
# and plugins built as part of qtbase (e.g. imageformats/libqjpeg.dylib)
# carry an @rpath list that only covers qtbase's own dependencies, not
# sibling formulas -- so without this, macdeployqt fails to resolve even
# QtCore/QtGui/QtWidgets themselves ("Cannot resolve rpath ...") and skips
# bundling them entirely, well before it gets to the PDF/SVG/virtual-
# keyboard plugins this app doesn't use anyway (removed just below). Those
# "Cannot resolve rpath" errors for QtPdf/QtSvg/QtVirtualKeyboard* are
# expected and harmless -- they're for plugins this app never uses, and
# get deleted right after regardless of whether macdeployqt could resolve
# them.
#
# -no-codesign: newer macdeployqt (Qt 6.5+) ad-hoc signs everything it
# touches by default, and can log a scary-looking (but non-fatal) "Codesign
# signing error" if it trips over a dylib install_name_tool touched earlier
# (invalidating that dylib's original signature) -- e.g. libbrotlicommon,
# pulled in via Qt's own freetype/harfbuzz dependency. Harmless either way,
# since the script re-signs the entire bundle itself at the end (see
# "Ad-hoc code-signing the bundle" below) -- but skipping macdeployqt's own
# redundant signing pass avoids the misleading error output entirely.
"$MACDEPLOYQT" "$APP_BUNDLE" -verbose=1 -no-codesign -libpath=/opt/homebrew/lib -libpath=/usr/local/lib

echo "==> Removing Qt plugins this app doesn't use"
# grainmeter-gui only displays JPEG/PNG/TIFF images through Qt Widgets --
# no PDF rendering, SVG icon theming, or virtual-keyboard input. Those
# three plugins pull in QtPdf/QtSvg/QtVirtualKeyboard(Qml), which even
# with -libpath above macdeployqt still can't resolve (they live in
# separate, not-installed Homebrew formulas), so they'd otherwise sit in
# the bundle as plugins Qt can never actually load. Removing them keeps
# the bundle free of dangling references instead of just leaving Qt to
# silently fail loading them at runtime.
rm -f "$APP_BUNDLE/Contents/PlugIns/imageformats/libqpdf.dylib" \
      "$APP_BUNDLE/Contents/PlugIns/iconengines/libqsvgicon.dylib" \
      "$APP_BUNDLE/Contents/PlugIns/platforminputcontexts/libqtvirtualkeyboardplugin.dylib"

echo "==> Removing duplicate LC_RPATH entries left by dylibbundler"
dedupe_rpaths_in_dir "$APP_BUNDLE/Contents"

echo "==> Ad-hoc code-signing the bundle"
# Ad-hoc signing (no Developer ID) is enough for the app to *launch* on
# Apple Silicon, including on other people's Macs -- but once the app
# picks up the quarantine flag (from being on a mounted DMG, downloaded,
# AirDropped, etc.), Gatekeeper won't vouch for an ad-hoc/unnotarized
# signature and macOS shows "\"Grainmeter\" is damaged and can't be
# opened" -- not actual corruption, just Gatekeeper's wording for "no
# trusted signature". Fix on the receiving end:
#   xattr -cr /Applications/Grainmeter.app
# (right-click > Open sometimes offers a bypass instead, but for a purely
# ad-hoc signature -- no Developer ID at all -- it often doesn't, so the
# xattr command above is the reliable one.) Avoiding this warning entirely
# requires a paid Apple Developer ID and notarization -- out of scope here.
codesign --force --deep --sign - "$APP_BUNDLE"

echo "==> Verifying both binaries are self-contained"
echo "    (should list no /opt/homebrew or /usr/local paths below)"
otool -L "$APP_BUNDLE/Contents/MacOS/grainmeter" | grep -E "homebrew|/usr/local" || echo "    grainmeter (CLI): OK, none found"
otool -L "$APP_BUNDLE/Contents/MacOS/grainmeter-gui" | grep -E "homebrew|/usr/local" || echo "    grainmeter-gui:   OK, none found"

echo "==> Verifying every bundled dependency actually landed on disk"
# dylibbundler/macdeployqt errors above (e.g. "No such file or directory")
# aren't fatal to either tool's own exit code, so without this check a
# broken bundle -- a load command pointing at a Frameworks/ file that was
# never actually copied -- would sail through to a DMG that "succeeds" here
# but crashes on launch for whoever opens it.
missing=0
for bin in "$APP_BUNDLE/Contents/MacOS/grainmeter" "$APP_BUNDLE/Contents/MacOS/grainmeter-gui"; do
    while IFS= read -r dep; do
        resolved="${dep/@executable_path/$APP_BUNDLE/Contents/MacOS}"
        resolved="${resolved/@rpath/$APP_BUNDLE/Contents/Frameworks}"
        if [[ ! -e "$resolved" ]]; then
            echo "Error: $bin depends on $dep, but $resolved doesn't exist." >&2
            missing=1
        fi
    done < <(otool -L "$bin" | tail -n +2 | awk '{print $1}' | grep -E '^@(executable_path|rpath)/')
done
if [[ "$missing" -ne 0 ]]; then
    echo "Bundling produced a broken app -- see missing paths above. Try a" >&2
    echo "fully clean rebuild: rm -rf $BUILD_DIR && cmake -B $BUILD_DIR -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD_DIR && $0 $BUILD_DIR" >&2
    exit 1
fi

echo "==> Building $DMG_NAME"
STAGING="$(mktemp -d)"
trap 'rm -rf "$STAGING"' EXIT

cp -R "$APP_BUNDLE" "$STAGING/${APP_NAME}.app"
ln -s "${APP_NAME}.app/Contents/MacOS/grainmeter" "$STAGING/grainmeter"
ln -s /Applications "$STAGING/Applications"

rm -f "$DMG_NAME"
hdiutil create -volname "$APP_NAME" -srcfolder "$STAGING" -ov -format UDZO "$DMG_NAME"

echo ""
echo "Done: $PROJECT_ROOT/$DMG_NAME"
echo "Contains ${APP_NAME}.app (GUI + CLI, double-click for the GUI, or run"
echo "${APP_NAME}.app/Contents/MacOS/grainmeter directly for the CLI) and a"
echo "'grainmeter' symlink at the top level for convenient direct Terminal use."
