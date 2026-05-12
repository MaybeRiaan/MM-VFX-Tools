# Modern Edge Blur - Automated Build Script
# Auto-increments build version, compiles, and copies to AE plugins folder

param(
    [switch]$Clean,
    [switch]$NoBuildIncrement,
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"

# Paths
$ProjectRoot = $PSScriptRoot
$VersionHeader = Join-Path $ProjectRoot "src\ModernEdgeBlur.h"
$BuildDir = Join-Path $ProjectRoot "build"
$GitRepo = Join-Path $ProjectRoot ".git"

Write-Host "=== Modern Edge Blur Build Script ===" -ForegroundColor Cyan
Write-Host ""

# Function to read version from ModernEdgeBlur.h
function Get-CurrentVersion {
    $content = Get-Content $VersionHeader -Raw
    
    if ($content -match '#define\s+MAJOR_VERSION\s+(\d+)') { $major = $matches[1] }
    if ($content -match '#define\s+MINOR_VERSION\s+(\d+)') { $minor = $matches[1] }
    if ($content -match '#define\s+BUG_VERSION\s+(\d+)') { $bug = $matches[1] }
    if ($content -match '#define\s+BUILD_VERSION\s+(\d+)') { $build = $matches[1] }
    
    return @{
        Major = [int]$major
        Minor = [int]$minor
        Bug = [int]$bug
        Build = [int]$build
    }
}

# Function to update build version in ModernEdgeBlur.h
function Update-BuildVersion {
    param([int]$NewBuild)
    
    $content = Get-Content $VersionHeader -Raw
    $content = $content -replace '(#define\s+BUILD_VERSION\s+)\d+', "`${1}$NewBuild"
    Set-Content $VersionHeader -Value $content -NoNewline
    
    Write-Host "Updated BUILD_VERSION to $NewBuild" -ForegroundColor Green
}

# Get current version
$version = Get-CurrentVersion
$versionString = "$($version.Major).$($version.Minor).$($version.Bug).$($version.Build)"
Write-Host "Current Version: $versionString" -ForegroundColor Yellow

# Increment build version unless disabled
if (-not $NoBuildIncrement) {
    $newBuild = $version.Build + 1
    Update-BuildVersion -NewBuild $newBuild
    $version = Get-CurrentVersion
    $versionString = "$($version.Major).$($version.Minor).$($version.Bug).$($version.Build)"
    Write-Host "New Version: $versionString" -ForegroundColor Green
}

Write-Host ""

# Clean build directory if requested OR if CMakeCache exists with wrong generator
$cmakeCache = Join-Path $BuildDir "CMakeCache.txt"
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning build directory..." -ForegroundColor Yellow
    Remove-Item $BuildDir -Recurse -Force
}
elseif (Test-Path $cmakeCache) {
    # Check if we need to clean due to generator mismatch
    $cacheContent = Get-Content $cmakeCache -Raw
    if ($cacheContent -match "CMAKE_GENERATOR:INTERNAL=(.+)") {
        $cachedGenerator = $matches[1]
        Write-Host "Found cached generator: $cachedGenerator" -ForegroundColor Yellow
        Write-Host "Cleaning build directory to regenerate..." -ForegroundColor Yellow
        Remove-Item $BuildDir -Recurse -Force
    }
}

# Create build directory
if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

# Detect available Visual Studio version
Write-Host "Detecting Visual Studio installation..." -ForegroundColor Cyan
$vsWherePath = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsGenerator = $null
$vsToolset = $null

if (Test-Path $vsWherePath) {
    try {
        $vsInstallPath = & "$vsWherePath" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $vsVersion = & "$vsWherePath" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
        
        if ($vsVersion) {
            $vsMajor = [int]($vsVersion.Split('.')[0])
            Write-Host "Found Visual Studio version: $vsVersion" -ForegroundColor Green
            Write-Host "Install path: $vsInstallPath" -ForegroundColor Gray
            
            if ($vsMajor -ge 18) {
                # Visual Studio 2026 (version 18.x) - use Ninja generator instead
                $vsGenerator = "Ninja"
                $vsToolset = $null
                Write-Host "Using Ninja generator for Visual Studio 2026" -ForegroundColor Green
            }
            elseif ($vsMajor -ge 17) {
                $vsGenerator = "Visual Studio 17 2022"
                $vsToolset = "v143"
                Write-Host "Using Visual Studio 2022 generator" -ForegroundColor Green
            }
            elseif ($vsMajor -ge 16) {
                $vsGenerator = "Visual Studio 16 2019"
                $vsToolset = "v142"
                Write-Host "Using Visual Studio 2019 generator" -ForegroundColor Green
            }
        }
    }
    catch {
        Write-Host "vswhere failed: $($_.Exception.Message)" -ForegroundColor Yellow
    }
}

# Fallback: Let CMake auto-detect
if (-not $vsGenerator) {
    Write-Host "Could not detect Visual Studio, letting CMake auto-detect..." -ForegroundColor Yellow
    $vsGenerator = $null  # Let CMake choose
}

# Configure CMake - Generate Visual Studio solution
if ($vsGenerator) {
    Write-Host "Generating Visual Studio solution with $vsGenerator..." -ForegroundColor Cyan
} else {
    Write-Host "Generating Visual Studio solution (auto-detect)..." -ForegroundColor Cyan
}

Push-Location $BuildDir
try {
    # Set up Visual Studio environment for Ninja if needed
    if ($vsGenerator -eq "Ninja" -and $vsInstallPath) {
        $vcvarsPath = Join-Path $vsInstallPath "VC\Auxiliary\Build\vcvars64.bat"
        if (Test-Path $vcvarsPath) {
            Write-Host "Setting up Visual Studio environment..." -ForegroundColor Cyan
            # Run vcvars and capture environment
            $tempFile = [System.IO.Path]::GetTempFileName()
            cmd /c "`"$vcvarsPath`" && set" > $tempFile
            Get-Content $tempFile | ForEach-Object {
                if ($_ -match "^([^=]+)=(.*)$") {
                    [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2])
                }
            }
            Remove-Item $tempFile
        }
    }
    
    if ($vsGenerator) {
        # Try with detected generator
        if ($vsGenerator -eq "Ninja") {
            cmake .. -G $vsGenerator -DCMAKE_BUILD_TYPE=$Config
        }
        elseif ($vsToolset) {
            cmake .. -G $vsGenerator -A x64 -T $vsToolset
        } else {
            cmake .. -G $vsGenerator -A x64
        }
        
        if ($LASTEXITCODE -ne 0) {
            # Try without toolset
            Write-Host "Retrying without toolset specification..." -ForegroundColor Yellow
            cmake .. -G $vsGenerator -A x64
        }
    } else {
        # Let CMake auto-detect
        cmake .. -A x64
    }
    
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed - please check your Visual Studio installation"
    }
    
    Write-Host ""
    Write-Host "=== Build System Generated ===" -ForegroundColor Green
    Write-Host "Version: $versionString" -ForegroundColor Green
    
    
    # Build the project
    Write-Host ""
    Write-Host "Building plugin..." -ForegroundColor Cyan
    Pop-Location
    
    if ($vsGenerator -eq "Ninja") {
        Push-Location $BuildDir
        ninja
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed"
        }
        Pop-Location
    } else {
        cmake --build $BuildDir --config $Config
        if ($LASTEXITCODE -ne 0) {
            throw "Build failed"
        }
    }
    
    Write-Host "Build completed successfully!" -ForegroundColor Green
    
    # Git commit version increment
    if ((Test-Path $GitRepo) -and (-not $NoBuildIncrement)) {
        Write-Host ""
        Write-Host "Committing version update to git..." -ForegroundColor Cyan
        git add $VersionHeader
        git commit -m "Build v$versionString - Auto-increment build version"
        Write-Host "Git commit created" -ForegroundColor Green
    }
    
    Push-Location $BuildDir
    
} catch {
    Write-Host ""
    Write-Host "=== Build Failed ===" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
} finally {
    Pop-Location
}

