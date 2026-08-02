<#
.SYNOPSIS
    Package the GUI and CLI together into a plain dist\ folder on Windows,
    with all required DLLs (OpenCV + Qt) copied alongside -- ready to zip
    and hand to someone without vcpkg/Qt/OpenCV installed on their machine.

.DESCRIPTION
    Windows is simpler here than macOS: there's no rpath/install-name
    rewriting to do -- Windows searches the .exe's own folder for DLLs
    automatically, so "self-contained" just means "the right DLLs are
    sitting next to the .exe". vcpkg's toolchain integration
    (VCPKG_APPLOCAL_DEPS, on by default) already copies OpenCV's DLLs
    next to grainmeter.exe and grainmeter-gui.exe at build time, so this
    script's main job is running windeployqt (Qt's official deployment
    tool, the Windows equivalent of macOS's macdeployqt) to pull in Qt's
    DLLs and plugins for the GUI, then consolidating both executables and
    every DLL into one flat dist\ folder.

.PARAMETER BuildDir
    The CMake build directory (default: build)

.PARAMETER Config
    The CMake build configuration that was used (default: Release)

.PARAMETER WindeployqtPath
    Explicit path to windeployqt.exe, if it isn't found automatically.
    Needed if Qt was installed somewhere this script doesn't already
    check -- see the search list in Find-Windeployqt below.

.EXAMPLE
    .\scripts\package_windows_dist.ps1
    .\scripts\package_windows_dist.ps1 -BuildDir build -Config Release
    .\scripts\package_windows_dist.ps1 -WindeployqtPath "C:\Qt\6.7.0\msvc2019_64\bin\windeployqt.exe"
#>

param(
    [string]$BuildDir = "build",
    [string]$Config = "Release",
    [string]$WindeployqtPath = ""
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $ProjectRoot

Write-Host "==> Locating built executables"

function Find-Exe {
    param([string]$Name)
    # Try the conventional multi-config (Visual Studio generator) paths
    # first...
    $candidates = @(
        (Join-Path $BuildDir "$Config\$Name"),
        (Join-Path $BuildDir "gui\$Config\$Name")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return (Resolve-Path $c).Path }
    }
    # ...otherwise search recursively under the build dir (covers
    # single-config generators like Ninja, and any other layout
    # differences). Newest match wins, in case stale old builds exist.
    $found = Get-ChildItem -Path $BuildDir -Recurse -Filter $Name -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($found) { return $found.FullName }
    return $null
}

$cliExe = Find-Exe -Name "grainmeter.exe"
$guiExe = Find-Exe -Name "grainmeter-gui.exe"

if (-not $cliExe) {
    Write-Error "grainmeter.exe not found under $BuildDir. Build first:`n  cmake -B $BuildDir -DCMAKE_TOOLCHAIN_FILE=...\vcpkg.cmake`n  cmake --build $BuildDir --config $Config"
    exit 1
}
if (-not $guiExe) {
    Write-Error "grainmeter-gui.exe not found under $BuildDir. Make sure Qt6 was found at configure time (check for 'Qt6 found' in the cmake output) and that the GUI target was built."
    exit 1
}
Write-Host "    CLI: $cliExe"
Write-Host "    GUI: $guiExe"

Write-Host "==> Locating windeployqt.exe"

function Find-Windeployqt {
    if ($WindeployqtPath -and (Test-Path $WindeployqtPath)) { return $WindeployqtPath }

    $cmd = Get-Command windeployqt.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    # Common vcpkg install layout -- triplet folder name can vary
    # (x64-windows, x64-windows-static, ...), so glob rather than
    # hardcoding one.
    if ($env:VCPKG_ROOT) {
        $vcpkgMatch = Get-ChildItem -Path (Join-Path $env:VCPKG_ROOT "installed") `
            -Recurse -Filter "windeployqt.exe" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($vcpkgMatch) { return $vcpkgMatch.FullName }
    }

    # Standalone Qt installer default location, as a last resort.
    if (Test-Path "C:\Qt") {
        $qtMatch = Get-ChildItem -Path "C:\Qt" -Recurse -Filter "windeployqt.exe" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($qtMatch) { return $qtMatch.FullName }
    }

    return $null
}

$windeployqt = Find-Windeployqt
if (-not $windeployqt) {
    Write-Error "windeployqt.exe not found. It ships with Qt. Pass its exact path with -WindeployqtPath, e.g.:`n  .\scripts\package_windows_dist.ps1 -WindeployqtPath 'C:\path\to\windeployqt.exe'"
    exit 1
}
Write-Host "    windeployqt: $windeployqt"

Write-Host "==> Setting up dist\"
$distDir = Join-Path $ProjectRoot "dist"
if (Test-Path $distDir) { Remove-Item $distDir -Recurse -Force }
New-Item -ItemType Directory -Path $distDir | Out-Null

Write-Host "==> Copying executables and their already-deployed DLLs"
# vcpkg's applocal deployment already copied OpenCV's DLLs next to both
# .exe files at build time -- copying each one's containing folder (just
# the .exe/.dll files, not build clutter like .pdb/CMakeFiles) picks
# those up automatically, no separate OpenCV-specific step needed here.
function Copy-BinaryAndDlls {
    param([string]$ExePath, [string]$DestDir)
    $srcDir = Split-Path $ExePath
    Get-ChildItem -Path $srcDir -File | Where-Object { $_.Extension -in ".exe", ".dll" } | ForEach-Object {
        Copy-Item $_.FullName -Destination $DestDir -Force
    }
}
Copy-BinaryAndDlls -ExePath $cliExe -DestDir $distDir
Copy-BinaryAndDlls -ExePath $guiExe -DestDir $distDir

Write-Host "==> Running windeployqt (bundles Qt DLLs + plugins for the GUI)"
& $windeployqt (Join-Path $distDir "grainmeter-gui.exe") --release --no-translations
if ($LASTEXITCODE -ne 0) {
    Write-Error "windeployqt failed with exit code $LASTEXITCODE"
    exit 1
}

Write-Host ""
Write-Host "Done: $distDir"
Write-Host "  grainmeter.exe       -- CLI"
Write-Host "  grainmeter-gui.exe   -- GUI (looks for grainmeter.exe right next to it)"
Write-Host "  *.dll                -- OpenCV (from vcpkg) + Qt (from windeployqt)"
Write-Host ""
Write-Host "Zip the whole dist\ folder to distribute both together -- Windows finds"
Write-Host "DLLs sitting next to a .exe automatically, so nothing else is needed on"
Write-Host "the other machine."
