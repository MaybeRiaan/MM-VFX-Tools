# Headless unit tests for Edge Repair's AE-free algorithm cores.
# Compiles and runs each test with plain cl.exe (no AE SDK). Exit 0 = all
# passed. The project path contains a space, which breaks cl's argv parsing,
# so we compile from a copy in a space-free temp dir (same class of issue the
# CMake PiPL step works around).

$ErrorActionPreference = "Stop"
$TestsDir = $PSScriptRoot
$CoreDir   = Join-Path (Split-Path $TestsDir -Parent) "src\core"
# The GPU kernel math (src\gpu\ER_KernelMath.h) also compiles as plain host
# C++ — that is what lets test_gpuparity.cpp call the EXACT functions the GPU
# runs and diff them against their MM_*.h twins with no GPU in the loop.
$GpuDir = Join-Path (Split-Path $TestsDir -Parent) "src\gpu"

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$work = Join-Path $env:TEMP "edgerepair_tests"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Path $work | Out-Null

Copy-Item (Join-Path $CoreDir "*.h") $work -Force
Copy-Item (Join-Path $GpuDir "ER_KernelMath.h") $work -Force
$tests = Get-ChildItem $TestsDir -Filter "test_*.cpp"

$fail = 0
foreach ($t in $tests) {
    Copy-Item $t.FullName $work -Force
    $exe = [System.IO.Path]::GetFileNameWithoutExtension($t.Name)
    Write-Host "=== Building/running $($t.Name) ===" -ForegroundColor Cyan
    $cmd = "call `"$vcvars`" 2>nul && cd /d `"$work`" && cl /nologo /EHsc /O2 /std:c++17 $($t.Name) && .\$exe.exe"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { $fail++; Write-Host "$($t.Name) FAILED (exit $LASTEXITCODE)" -ForegroundColor Red }
}

if ($fail) { Write-Host "`n$fail test file(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "`nAll test files passed" -ForegroundColor Green
