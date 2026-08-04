# Headless tests for Modern Edge Blur. Compiles and runs each test with plain
# cl.exe — no AE SDK, no GPU. Exit 0 = all passed.
#
# The parity test compiles BOTH sides of the CPU/GPU split as host C++:
#   src/core/MEB_CoreMath.h   the CPU render's real math
#   src/gpu/MEB_KernelMath.h  the GPU kernels' real math
#   src/gpu/MEB_KernelPasses.h  ... and their real pass bodies
# and diffs them with `==`. That is only meaningful because both are the
# shipping code — see the header of tests/test_gpuparity.cpp.
#
# The project path contains a space, which breaks cl's argv parsing, so we
# compile from a copy in a space-free temp dir (the same class of issue the
# CMake PiPL step works around).
#
# Run this after ANY change to a formula in either header. A formula changes in
# both, in the same commit, or not at all.

$ErrorActionPreference = "Stop"
$TestsDir = $PSScriptRoot
$Root     = Split-Path $TestsDir -Parent
$CoreDir  = Join-Path $Root "src\core"
$GpuDir   = Join-Path $Root "src\gpu"

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$work = Join-Path $env:TEMP "modernedgeblur_tests"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Path $work | Out-Null

Copy-Item (Join-Path $CoreDir "*.h") $work -Force
Copy-Item (Join-Path $GpuDir  "MEB_KernelMath.h")   $work -Force
Copy-Item (Join-Path $GpuDir  "MEB_KernelPasses.h") $work -Force

$tests = Get-ChildItem $TestsDir -Filter "test_*.cpp"
if (-not $tests) { throw "no test_*.cpp found in $TestsDir" }

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
