# One-shot release builder: compiles the C++ backend, then packs everything
# into a Chinese Inno Setup installer at release\fsrec_Setup_1.0.13.exe.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\build_release.ps1          # build + package
#   powershell -ExecutionPolicy Bypass -File scripts\build_release.ps1 -NoBuild # package only (skip cmake)

param(
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root    = Split-Path -Parent $PSScriptRoot
$buildExe = Join-Path $root 'build\Release\recovery_server.exe'
$iss      = Join-Path $root 'packaging\fsrec.iss'
$langDir  = Join-Path $root 'packaging\languages'
$isl      = Join-Path $langDir 'ChineseSimplified.isl'
$iscc     = Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'

Write-Host "=== fsrec release builder ==="

# 1. Build backend (unless -NoBuild)
if (-not $NoBuild) {
    Write-Host "[1/4] Building backend (Release)..."
    Push-Location $root
    try {
        & cmake --build build --config Release
        if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }
    } finally { Pop-Location }
} else {
    Write-Host "[1/4] Skipping build (-NoBuild)"
}

if (-not (Test-Path $buildExe)) { throw "backend exe not found: $buildExe (build it first)" }

# 2. Ensure the icon exists
$ico = Join-Path $root 'resources\fsrec.ico'
if (-not (Test-Path $ico)) {
    Write-Host "[2/4] Generating icon..."
    & powershell -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\make_icon.ps1')
}

# 3. Ensure the Chinese language file exists (download once)
if (-not (Test-Path $isl)) {
    Write-Host "[3/4] Downloading ChineseSimplified.isl..."
    New-Item -ItemType Directory -Force -Path $langDir | Out-Null
    $url = 'https://raw.githubusercontent.com/jrsoftware/issrc/main/Files/Languages/Unofficial/ChineseSimplified.isl'
    try {
        Invoke-WebRequest -Uri $url -OutFile $isl -UseBasicParsing
    } catch {
        throw "Failed to download ChineseSimplified.isl ($url): $($_.Exception.Message)"
    }
} else {
    Write-Host "[3/4] ChineseSimplified.isl already present"
}

# 4. Compile the installer
if (-not (Test-Path $iscc)) { throw "Inno Setup 6 not found at $iscc" }
Write-Host "[4/4] Compiling installer (ISCC)..."
& $iscc $iss
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }

$out = Join-Path $root 'release\fsrec_Setup_1.0.13.exe'
if (-not (Test-Path $out)) { throw "installer output not found: $out" }
Write-Host ""
Write-Host ("DONE -> " + $out)
