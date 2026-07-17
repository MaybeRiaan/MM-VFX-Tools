# MM FreqSep - Windows build script.
# Enters the VS developer environment (so cl.exe / the PiPL custom commands
# resolve), configures with the Ninja generator, and builds. Deploys the
# .aex to the AE plugins folder via the CMake POST_BUILD step.
#
#   .\build.ps1              # incremental build
#   .\build.ps1 -Clean       # wipe build/ first
#   .\build.ps1 -Config Debug

param(
    [switch]$Clean,
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$ProjectRoot = $PSScriptRoot
$BuildDir = Join-Path $ProjectRoot "build"

# --- Locate Visual Studio + Ninja -----------------------------------------
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vsWhere)) { throw "vswhere.exe not found - is Visual Studio installed?" }
$vsInstall = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) { throw "No VS installation with the C++ toolset found." }

$vcvars = Join-Path $vsInstall "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$ninja = Join-Path $vsInstall "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if (-not (Test-Path $ninja)) {
    $ninjaCmd = Get-Command ninja -ErrorAction SilentlyContinue
    if ($ninjaCmd) { $ninja = $ninjaCmd.Source } else { throw "ninja.exe not found." }
}

Write-Host "VS:    $vsInstall" -ForegroundColor Gray
Write-Host "Ninja: $ninja"     -ForegroundColor Gray

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning build directory..." -ForegroundColor Yellow
    Remove-Item $BuildDir -Recurse -Force
}
if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir | Out-Null }

# --- Configure + build inside the VS dev environment ----------------------
# vcvars64 must run in the same cmd session as cmake/ninja so cl.exe, the
# Windows SDK, and rc.exe are all on PATH for the PiPL custom commands.
$cmakeArgs = "-G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_MAKE_PROGRAM=`"$ninja`" `"$ProjectRoot`""
$bat = @"
call "$vcvars"
if errorlevel 1 exit /b 1
cd /d "$BuildDir"
cmake $cmakeArgs
if errorlevel 1 exit /b 1
cmake --build .
if errorlevel 1 exit /b 1
"@

$tmp = Join-Path $env:TEMP "mm_freqsep_build.bat"
Set-Content -Path $tmp -Value $bat -Encoding ascii
# vcvars64 emits a benign "vswhere.exe is not recognized" line on stderr from
# an internal probe; putting the VS Installer dir on PATH silences it. The
# EAP flip stops that stderr line from being treated as a terminating error.
$ErrorActionPreference = "Continue"
$installerDir = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
if (Test-Path $installerDir) { $env:PATH = "$installerDir;$env:PATH" }
try {
    & cmd /c $tmp
    $code = $LASTEXITCODE
} finally {
    Remove-Item $tmp -ErrorAction SilentlyContinue
}

if ($code -ne 0) { throw "Build failed with exit code $code" }
Write-Host "=== Build succeeded ===" -ForegroundColor Green
