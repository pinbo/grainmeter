#!/usr/bin/env bash
#
# package_windows_msys2_dist.sh -- package the GUI and CLI together into a
# plain dist/ folder, for the MSYS2/UCRT64 build described in
# compile_in_windows11.md (pacman-installed mingw-w64-ucrt-x86_64-* toolchain
# + OpenCV/Qt6, *not* vcpkg). Run this from an MSYS2 UCRT64 shell.
#
# Windows doesn't need rpath/install-name rewriting the way macOS does --
# it just searches the .exe's own folder for DLLs automatically, so
# "self-contained" here means "the right DLLs are sitting next to the
# .exe". This script:
#   1. Copies both .exe files into dist/.
#   2. Copies every non-system DLL `ldd` reports for either of them (i.e.
#      anything under /ucrt64, /mingw64, or $HOME -- not C:/Windows).
#   3. Runs windeployqt on grainmeter-gui.exe to pull in Qt's platform
#      plugin (platforms/qwindows.dll) and friends, which `ldd` doesn't
#      see since Qt loads those at runtime, not via the import table.
#
# Usage:
#   ./scripts/package_windows_msys2_dist.sh [build-dir]
#
# Requires (install once, matching compile_in_windows11.md):
#   pacman -S mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-tools
#
# Output: dist/ containing:
#   grainmeter.exe       -- CLI
#   grainmeter-gui.exe   -- GUI (looks for grainmeter.exe right next to it)
#   *.dll                -- OpenCV + Qt + their dependencies
#   platforms/, etc.     -- Qt plugins, from windeployqt

#set -euo pipefail

case "$(uname -s)" in
    MINGW*|MSYS*) ;;
    *)
        echo "Error: this script is meant to run inside an MSYS2 shell on Windows." >&2
        exit 1
        ;;
esac

BUILD_DIR="${1:-build}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "BUILD_DIR is $BUILD_DIR"
echo "PROJECT_ROOT is $PROJECT_ROOT"

cd "$PROJECT_ROOT"

DIST_DIR="dist"

echo "==> Locating built executables"

find_exe() {
    local name="$1" candidate
    for candidate in "$BUILD_DIR/$name" "$BUILD_DIR/gui/$name"; do
        if [[ -f "$candidate" ]]; then
            echo "$candidate"
            return 0
        fi
    done
    # Fallback: search recursively (covers other generator layouts).
    find "$BUILD_DIR" -type f -iname "$name" 2>/dev/null | head -n1
}

CLI_EXE="$(find_exe grainmeter.exe)"
GUI_EXE="$(find_exe grainmeter-gui.exe)"

if [[ -z "$CLI_EXE" ]]; then
    echo "Error: grainmeter.exe not found under $BUILD_DIR. Build first:" >&2
    echo "  cmake -B $BUILD_DIR -DCMAKE_PREFIX_PATH=\$HOME/opencv5" >&2
    echo "  cmake --build $BUILD_DIR" >&2
    exit 1
fi
if [[ -z "$GUI_EXE" ]]; then
    echo "Error: grainmeter-gui.exe not found under $BUILD_DIR." >&2
    echo "Make sure Qt6 was found at configure time -- check the cmake output" >&2
    echo "for \"Qt6 found\" vs \"Qt6 Widgets not found\". If it wasn't found," >&2
    echo "install it first:" >&2
    echo "  pacman -S mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-tools" >&2
    echo "then reconfigure (delete $BUILD_DIR and re-run cmake) so it's detected." >&2
    exit 1
fi
echo "    CLI: $CLI_EXE"
echo "    GUI: $GUI_EXE"

echo "==> Setting up $DIST_DIR"
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

echo "==> Copying executables"
cp "$CLI_EXE" "$GUI_EXE" "$DIST_DIR/"

echo "==> Copying non-system DLLs (via ldd)"
# Only DLLs under the MSYS2/mingw toolchain or the user's home (e.g. a
# manually-installed $HOME/opencv5) are ours to bundle -- anything under
# /c/Windows is a system DLL already present on every Windows machine.
copy_deps() {
    local exe="$1"
    ldd "$exe" 2>/dev/null | awk '{print $3}' | grep -E '^/(ucrt64|mingw64|clang64|home)/' | while IFS= read -r dll; do
        [[ -f "$dll" ]] && cp -n "$dll" "$DIST_DIR/" 2>/dev/null || true
    done
}
# DLLs depend on other bundled DLLs too (e.g. Qt -> ICU) -- keep running
# copy_deps over the top-level dist/*.dll set until nothing new shows up.
sweep_transitive_deps() {
    local prev_count=-1 count
    while true; do
        count=$(find "$DIST_DIR" -maxdepth 1 -iname '*.dll' | wc -l)
        [[ "$count" -eq "$prev_count" ]] && break
        prev_count="$count"
        for dll in "$DIST_DIR"/*.dll; do
            [[ -f "$dll" ]] && copy_deps "$dll"
        done
    done
}
copy_deps "$CLI_EXE"
copy_deps "$GUI_EXE"
sweep_transitive_deps
echo "    $(find "$DIST_DIR" -maxdepth 1 -iname '*.dll' | wc -l) DLLs copied"

echo "==> Locating windeployqt"
WINDEPLOYQT=""
for candidate in windeployqt6 windeployqt; do
    if command -v "$candidate" >/dev/null 2>&1; then
        WINDEPLOYQT="$(command -v "$candidate")"
        break
    fi
done
if [[ -z "$WINDEPLOYQT" ]]; then
    echo "Error: windeployqt not found in PATH. It ships with Qt's tools package:" >&2
    echo "  pacman -S mingw-w64-ucrt-x86_64-qt6-tools" >&2
    exit 1
fi
echo "    windeployqt: $WINDEPLOYQT"

echo "==> Running windeployqt (bundles Qt plugins, e.g. platforms/qwindows.dll)"
"$WINDEPLOYQT" "$DIST_DIR/grainmeter-gui.exe" --release --no-translations

# Belt-and-suspenders: on MSYS2, windeployqt's own plugin-directory
# detection doesn't always land right (its packaging splits plugins under
# /ucrt64/share/qt6/plugins rather than the layout windeployqt expects from
# an official Qt installer), and it can exit 0 without actually having
# copied platforms/qwindows.dll -- the app then fails at launch with
# "Could not find the Qt platform plugin \"windows\"". If that happened,
# fall back to locating Qt's plugin dir directly via qmake and copying the
# platform/imageformats/styles plugins ourselves.
if [[ ! -f "$DIST_DIR/platforms/qwindows.dll" ]]; then
    echo "==> windeployqt didn't produce platforms/qwindows.dll -- falling back to manual copy"
    QMAKE=""
    for candidate in qmake6 qmake; do
        if command -v "$candidate" >/dev/null 2>&1; then
            QMAKE="$candidate"
            break
        fi
    done
    if [[ -z "$QMAKE" ]]; then
        echo "Error: qmake not found, can't locate Qt's plugin directory for the fallback copy." >&2
        exit 1
    fi
    QT_PLUGINS_DIR="$("$QMAKE" -query QT_INSTALL_PLUGINS)"
    echo "    Qt plugins dir: $QT_PLUGINS_DIR"
    if [[ ! -f "$QT_PLUGINS_DIR/platforms/qwindows.dll" ]]; then
        echo "Error: $QT_PLUGINS_DIR/platforms/qwindows.dll not found either." >&2
        echo "Check that mingw-w64-ucrt-x86_64-qt6-base is actually installed." >&2
        exit 1
    fi
    for plugin_dir in platforms imageformats styles; do
        if [[ -d "$QT_PLUGINS_DIR/$plugin_dir" ]]; then
            mkdir -p "$DIST_DIR/$plugin_dir"
            cp "$QT_PLUGINS_DIR/$plugin_dir"/*.dll "$DIST_DIR/$plugin_dir/" 2>/dev/null || true
        fi
    done
    echo "    Copied platforms/, imageformats/, styles/ from $QT_PLUGINS_DIR"
    # These plugins pull in their own DLL dependencies (e.g. platforms/
    # needs Qt6Gui, styles/ needs Qt6Widgets) -- copy_deps always drops
    # dependencies into the top-level dist/ dir regardless of which file's
    # ldd output found them, which is where Windows' DLL search actually
    # looks (the main .exe's own directory, not a plugin subdirectory), so
    # sweeping transitively from here is enough.
    for dll in "$DIST_DIR"/platforms/*.dll "$DIST_DIR"/imageformats/*.dll "$DIST_DIR"/styles/*.dll; do
        [[ -f "$dll" ]] && copy_deps "$dll"
    done
    sweep_transitive_deps
fi

# I need to run this one more time to get all the dlls.
# windeployqt dist/grainmeter-gui.exe

echo ""
echo "Done: $PROJECT_ROOT/$DIST_DIR/"
echo "  grainmeter.exe       -- CLI"
echo "  grainmeter-gui.exe   -- GUI (looks for grainmeter.exe right next to it)"
echo "  *.dll                -- OpenCV + Qt + their dependencies"
echo "  platforms/, etc.     -- Qt plugins, from windeployqt"
echo ""
echo "Zip the whole dist/ folder to distribute both together -- Windows finds"
echo "DLLs sitting next to a .exe automatically, so nothing else is needed on"
echo "the other machine."
