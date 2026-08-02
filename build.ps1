# MM VFX Tools - suite build script.
#
# Builds every plugin in the suite from one tree and deploys them all to After
# Effects for testing.
#
#     .\build.ps1                      build + deploy everything
#     .\build.ps1 -Project MM_FreqSep  just one project
#     .\build.ps1 -Versions            print the version table and exit
#     .\build.ps1 -Publish             re-deploy what is built (no compile)
#     .\build.ps1 -Clean               wipe the suite build dir first
#
# This does NOT replace the per-project build.ps1 scripts. Those remain the
# tool for working on a single plugin: they own that plugin's version bump and
# its own conventions (ModernEdgeBlur/build.ps1 auto-increments and commits;
# build_edgerepair.ps1 bumps EdgeRepair only). This script never touches
# version numbers - it builds what is committed and installs it.
#
# ASCII only, deliberately: Windows PowerShell 5.1 reads a BOM-less script as
# ANSI, and a UTF-8 em-dash then decodes to a stray double-quote that breaks
# parsing several lines later. Keep it that way.

[CmdletBinding()]
param(
    [string]$Project,
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$Publish,
    [switch]$Versions
)

$ErrorActionPreference = "Stop"

$SuiteRoot = $PSScriptRoot
$BuildDir  = Join-Path $SuiteRoot "build"

$Projects = [ordered]@{
    "GrainTransfer"  = "MM_BUILD_GRAINTRANSFER"
    "MM_Qualify"     = "MM_BUILD_MM_QUALIFY"
    "MM_FreqSep"     = "MM_BUILD_MM_FREQSEP"
    "ModernEdgeBlur" = "MM_BUILD_MODERNEDGEBLUR"
    "EdgeRepair"     = "MM_BUILD_EDGEREPAIR"
}

Write-Host "=== MM VFX Tools - suite build ===" -ForegroundColor Cyan

# --- Versions: read straight from the sources, no configure needed ----------
if ($Versions) {
    $pairs = @(
        @{ Name = "GrainTransfer";  Header = "GrainTransfer\src\GrainTransfer.h";   Pipl = "GrainTransfer\resources\GrainTransferPiPL.r" },
        @{ Name = "MM_Qualify";     Header = "MM_Qualify\src\params.h";             Pipl = "MM_Qualify\resources\MM_QualifyPiPL.r" },
        @{ Name = "MM_FreqSep";     Header = "MM_FreqSep\src\params.h";             Pipl = "MM_FreqSep\resources\MM_FreqSepPiPL.r" },
        @{ Name = "ModernEdgeBlur"; Header = "ModernEdgeBlur\src\ModernEdgeBlur.h"; Pipl = "ModernEdgeBlur\resources\ModernEdgeBlurPiPL.r" },
        @{ Name = "EdgeRepair";     Header = "EdgeRepair\src\EdgeRepair.h";         Pipl = "EdgeRepair\resources\EdgeRepairPiPL.r" }
    )

    function Get-VersionQuad([string]$path) {
        $text = Get-Content (Join-Path $SuiteRoot $path) -Raw
        $parts = foreach ($p in "MAJOR", "MINOR", "BUG", "BUILD") {
            if ($text -match "#define\s+${p}_VERSION\s+(\w+)") { $matches[1] } else { "?" }
        }
        return ($parts -join ".")
    }

    Write-Host ""
    $bad = 0
    foreach ($p in $pairs) {
        $h = Get-VersionQuad $p.Header
        $r = Get-VersionQuad $p.Pipl
        if ($h -eq $r) {
            Write-Host ("  {0,-18} {1}" -f $p.Name, $h) -ForegroundColor Green
        } else {
            Write-Host ("  {0,-18} header {1} / PiPL {2}  MISMATCH - AE will refuse to load" -f $p.Name, $h, $r) -ForegroundColor Red
            $bad++
        }
    }
    Write-Host ""
    if ($bad -gt 0) {
        Write-Host "$bad plugin(s) have header/PiPL version drift. Fix before building." -ForegroundColor Red
        exit 1
    }
    Write-Host "All header/PiPL version pairs agree." -ForegroundColor Green
    exit 0
}

# --- AE lock check ----------------------------------------------------------
# A loaded .aex is locked; the copy fails and every project treats that as
# non-fatal, so the build "succeeds" while installing nothing.
if (Get-Process -Name "AfterFX" -ErrorAction SilentlyContinue) {
    Write-Host ""
    Write-Host "After Effects is running. It holds a file lock on any loaded .aex," -ForegroundColor Yellow
    Write-Host "so the deploy step will silently skip those plugins. Close AE and" -ForegroundColor Yellow
    Write-Host "re-run, or use -Publish afterwards to install without rebuilding." -ForegroundColor Yellow
    Write-Host ""
}

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning $BuildDir ..." -ForegroundColor Yellow
    Remove-Item $BuildDir -Recurse -Force
}

# --- Toolchain: Ninja out of the VS install, driven through vcvars ----------
# Same approach the per-project scripts use, and for the same reason: vcvars and
# the tool invocation must share ONE cmd.exe process. Transplanting vcvars'
# environment into PowerShell corrupts CMake's Ninja generator (CMAKE_BUILD_TYPE
# lands in rules.ninja as the literal text for the variable) - see the long note
# in ModernEdgeBlur/build.ps1.
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - is Visual Studio installed?" }
$vsPath = & $vswhere -latest -property installationPath
if (-not $vsPath) { throw "No Visual Studio installation found" }

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
$cmake  = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja  = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if (-not (Test-Path $cmake)) { $cmake = "cmake" }
if (-not (Test-Path $ninja)) { $ninja = "ninja" }
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

Write-Host "VS:    $vsPath"

# --- Which projects ---------------------------------------------------------
$cmakeOpts = ""
if ($Project) {
    if (-not $Projects.Contains($Project)) {
        throw "Unknown project '$Project'. One of: $($Projects.Keys -join ', ')"
    }
    foreach ($p in $Projects.Keys) {
        if ($p -eq $Project) { $state = "ON" } else { $state = "OFF" }
        $cmakeOpts += " -D$($Projects[$p])=$state"
    }
    Write-Host "Project: $Project (others disabled)" -ForegroundColor Cyan
} else {
    foreach ($p in $Projects.Keys) { $cmakeOpts += " -D$($Projects[$p])=ON" }
    Write-Host "Project: all" -ForegroundColor Cyan
}

if (-not (Test-Path $BuildDir)) { New-Item -ItemType Directory -Path $BuildDir | Out-Null }

$ErrorActionPreference = "Continue"   # native tools report via exit codes below

Write-Host ""
Write-Host "Configuring..." -ForegroundColor Cyan
$configureCmd = "call `"$vcvars`" 2>nul && `"$cmake`" -S `"$SuiteRoot`" -B `"$BuildDir`" -G Ninja -DCMAKE_BUILD_TYPE=$Config$cmakeOpts"
cmd /c $configureCmd
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed" }

if ($Publish) {
    Write-Host ""
    Write-Host "Publishing to After Effects..." -ForegroundColor Cyan
    $publishCmd = "call `"$vcvars`" 2>nul && `"$cmake`" --build `"$BuildDir`" --target mm_publish"
    cmd /c $publishCmd
    if ($LASTEXITCODE -ne 0) { throw "Publish failed" }
    Write-Host "Published." -ForegroundColor Green
    exit 0
}

Write-Host ""
Write-Host "Building..." -ForegroundColor Cyan
$buildCmd = "call `"$vcvars`" 2>nul && `"$ninja`" -C `"$BuildDir`""
cmd /c $buildCmd
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

Write-Host ""
Write-Host "=== Suite build complete ===" -ForegroundColor Green
Write-Host "Each plugin deployed to After Effects by its own post-build step." -ForegroundColor Green
Write-Host "If AE was open, re-run with -Publish once it is closed." -ForegroundColor DarkGray
