# Edge Repair - Automated Build Script
# Auto-increments EdgeRepair's build version (never ModernEdgeBlur's),
# compiles ONLY the EdgeRepair target, and deploys to the AE plugins folder
# via the target's POST_BUILD step. No git auto-commit - stage commits
# capture version state.

param(
    [switch]$Clean,
    [switch]$NoBuildIncrement,
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

$ProjectRoot   = $PSScriptRoot
$VersionHeader = Join-Path $ProjectRoot "src\EdgeRepair.h"
$BuildDir      = Join-Path $ProjectRoot "build"

Write-Host "=== Edge Repair Build Script ===" -ForegroundColor Cyan
Write-Host ""

function Get-CurrentVersion {
    $content = Get-Content $VersionHeader -Raw

    if ($content -match '#define\s+MAJOR_VERSION\s+(\d+)') { $major = $matches[1] }
    if ($content -match '#define\s+MINOR_VERSION\s+(\d+)') { $minor = $matches[1] }
    if ($content -match '#define\s+BUG_VERSION\s+(\d+)')   { $bug   = $matches[1] }
    if ($content -match '#define\s+BUILD_VERSION\s+(\d+)') { $build = $matches[1] }

    return @{
        Major = [int]$major
        Minor = [int]$minor
        Bug   = [int]$bug
        Build = [int]$build
    }
}

function Update-BuildVersion {
    param([int]$NewBuild)

    $content = Get-Content $VersionHeader -Raw
    $content = $content -replace '(#define\s+BUILD_VERSION\s+)\d+', "`${1}$NewBuild"
    Set-Content $VersionHeader -Value $content -NoNewline

    Write-Host "Updated EdgeRepair BUILD_VERSION to $NewBuild" -ForegroundColor Green
}

$version = Get-CurrentVersion
$versionString = "$($version.Major).$($version.Minor).$($version.Bug).$($version.Build)"
Write-Host "Current Version: $versionString" -ForegroundColor Yellow

if (-not $NoBuildIncrement) {
    Update-BuildVersion -NewBuild ($version.Build + 1)
    $version = Get-CurrentVersion
    $versionString = "$($version.Major).$($version.Minor).$($version.Bug).$($version.Build)"
    Write-Host "New Version: $versionString" -ForegroundColor Green
}

Write-Host ""

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning build directory..." -ForegroundColor Yellow
    Remove-Item $BuildDir -Recurse -Force
}

if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

# Detect Visual Studio (same logic as build.ps1; VS 2026 → Ninja).
Write-Host "Detecting Visual Studio installation..." -ForegroundColor Cyan
$vsWherePath = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsGenerator = $null
$vsToolset = $null
$vsInstallPath = $null

if (Test-Path $vsWherePath) {
    try {
        $vsInstallPath = & "$vsWherePath" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $vsVersion = & "$vsWherePath" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion

        if ($vsVersion) {
            $vsMajor = [int]($vsVersion.Split('.')[0])
            Write-Host "Found Visual Studio version: $vsVersion" -ForegroundColor Green

            if ($vsMajor -ge 18) {
                $vsGenerator = "Ninja"
            }
            elseif ($vsMajor -ge 17) {
                $vsGenerator = "Visual Studio 17 2022"
                $vsToolset = "v143"
            }
            elseif ($vsMajor -ge 16) {
                $vsGenerator = "Visual Studio 16 2019"
                $vsToolset = "v142"
            }
        }
    }
    catch {
        Write-Host "vswhere failed: $($_.Exception.Message)" -ForegroundColor Yellow
    }
}

Push-Location $BuildDir
try {
    # See build.ps1 for why $ErrorActionPreference relaxes around native
    # tools and why vcvars + cmake/ninja must share one cmd.exe process.
    $ErrorActionPreference = "Continue"

    $vsCMake = "cmake"
    $vsNinjaExe = "ninja"
    $vcvarsPath = $null
    if ($vsGenerator -eq "Ninja" -and $vsInstallPath) {
        $vsCMakeCandidate = Join-Path $vsInstallPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        $vsNinjaCandidate = Join-Path $vsInstallPath "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
        if (Test-Path $vsCMakeCandidate) { $vsCMake = $vsCMakeCandidate }
        if (Test-Path $vsNinjaCandidate) { $vsNinjaExe = $vsNinjaCandidate }

        $vcvarsCandidate = Join-Path $vsInstallPath "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $vcvarsCandidate) { $vcvarsPath = $vcvarsCandidate }
    }

    # Configure only when the cache is missing or the generator changed -
    # unlike build.ps1 this script does NOT wipe the build dir every run,
    # so incremental EdgeRepair builds stay fast and never touch the
    # ModernEdgeBlur objects.
    $cmakeCache = Join-Path $BuildDir "CMakeCache.txt"
    $needConfigure = $true
    if (Test-Path $cmakeCache) {
        $cacheContent = Get-Content $cmakeCache -Raw
        if ($vsGenerator -and $cacheContent -match "CMAKE_GENERATOR:INTERNAL=$([regex]::Escape($vsGenerator))") {
            $needConfigure = $false
        }
    }

    if ($needConfigure) {
        if (Test-Path $cmakeCache) {
            Write-Host "Generator changed - cleaning build directory..." -ForegroundColor Yellow
            Pop-Location
            Remove-Item $BuildDir -Recurse -Force
            New-Item -ItemType Directory -Path $BuildDir | Out-Null
            Push-Location $BuildDir
        }
        Write-Host "Configuring ($vsGenerator)..." -ForegroundColor Cyan
        if ($vsGenerator -eq "Ninja") {
            if ($vcvarsPath) {
                $configureCmd = "call `"$vcvarsPath`" 2>nul && `"$vsCMake`" .. -G Ninja -DCMAKE_BUILD_TYPE=$Config"
                cmd /c $configureCmd
            } else {
                & $vsCMake .. -G Ninja -DCMAKE_BUILD_TYPE=$Config
            }
        }
        elseif ($vsGenerator) {
            if ($vsToolset) { cmake .. -G $vsGenerator -A x64 -T $vsToolset }
            else            { cmake .. -G $vsGenerator -A x64 }
        }
        else {
            cmake .. -A x64
        }
        if ($LASTEXITCODE -ne 0) {
            throw "CMake configuration failed"
        }
    }

    Write-Host ""
    Write-Host "Building EdgeRepair v$versionString..." -ForegroundColor Cyan

    if ($vsGenerator -eq "Ninja") {
        if ($vcvarsPath) {
            $buildCmd = "call `"$vcvarsPath`" 2>nul && `"$vsNinjaExe`" EdgeRepair.aex"
            cmd /c $buildCmd
        } else {
            & $vsNinjaExe EdgeRepair.aex
        }
    } else {
        cmake --build $BuildDir --config $Config --target EdgeRepair
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed"
    }

    Write-Host ""
    Write-Host "EdgeRepair v$versionString built and deployed." -ForegroundColor Green

} catch {
    Write-Host ""
    Write-Host "=== Build Failed ===" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
} finally {
    Pop-Location
    $ErrorActionPreference = "Stop"
}
