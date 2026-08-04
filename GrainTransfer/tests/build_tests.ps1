# Headless unit tests for Grain Transfer's AE-free DSP core.
# Compiles and runs each test_*.cpp with plain cl.exe (no AE SDK). Exit 0 =
# all passed. The project path contains a space, which breaks cl's argv
# parsing, so everything is compiled from a copy in a space-free temp dir
# (same workaround as the rest of the suite).
#
# PNG/diagnostic outputs land in tests\out (GT_TEST_OUT) for human review.

$ErrorActionPreference = "Stop"
$TestsDir = $PSScriptRoot
$Root     = Split-Path $TestsDir -Parent
$SrcDir   = Join-Path $Root "src"
$CoreDir  = Join-Path $SrcDir "core"
$GpuDir   = Join-Path $SrcDir "gpu"
$FftDir   = Join-Path $Root "third_party\kissfft"
$TpDir    = Join-Path $TestsDir "third_party"
$OutDir   = Join-Path $TestsDir "out"

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# The work tree MIRRORS src/, because the shared kernel vocabulary is reached
# by relative path: core/GT_Colour.h includes ../gt_core.h, and
# gpu/cuda/gt_kernel_args.h includes ../kernels/gt_prologue_host.h. Flattening
# everything into one directory (as this script used to) breaks both. Test
# files and the vendored third-party bits are dropped into the core/ level so
# their existing flat includes still resolve, and that is where cl runs.
$work  = Join-Path $env:TEMP "graintransfer_tests"
$wCore = Join-Path $work "core"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Path $wCore | Out-Null
New-Item -ItemType Directory -Path (Join-Path $work "gpu\kernels") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $work "gpu\cuda")    | Out-Null
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

Copy-Item (Join-Path $SrcDir  "*.h")   $work  -Force
Copy-Item (Join-Path $CoreDir "*.h")   $wCore -Force
Copy-Item (Join-Path $CoreDir "*.cpp") $wCore -Force
Copy-Item (Join-Path $FftDir  "*.h")   $wCore -Force
Copy-Item (Join-Path $FftDir  "*.c")   $wCore -Force
Copy-Item (Join-Path $TpDir   "*.h")   $wCore -Force
Copy-Item (Join-Path $TpDir   "*.c")   $wCore -Force
Copy-Item (Join-Path $GpuDir "kernels\*") (Join-Path $work "gpu\kernels") -Force
if (Test-Path (Join-Path $GpuDir "cuda")) {
    Copy-Item (Join-Path $GpuDir "cuda\*") (Join-Path $work "gpu\cuda") -Force
}

$coreSrcs = "GT_FFT.cpp GT_Grain.cpp GT_Response.cpp GT_Stamp.cpp kiss_fft.c kiss_fftnd.c miniz.c"
$env:GT_TEST_OUT = $OutDir

$tests = Get-ChildItem $TestsDir -Filter "test_*.cpp"
$fail = 0
foreach ($t in $tests) {
    Copy-Item $t.FullName $wCore -Force
    $exe = [System.IO.Path]::GetFileNameWithoutExtension($t.Name)
    Write-Host "=== Building/running $($t.Name) ===" -ForegroundColor Cyan
    $cmd = "call `"$vcvars`" 2>nul && cd /d `"$wCore`" && cl /nologo /EHsc /O2 /std:c++17 /I. $($t.Name) $coreSrcs /Fe$exe.exe && .\$exe.exe"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { $fail++; Write-Host "$($t.Name) FAILED (exit $LASTEXITCODE)" -ForegroundColor Red }
}

if ($fail) { Write-Host "`n$fail test file(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "`nAll test files passed" -ForegroundColor Green
