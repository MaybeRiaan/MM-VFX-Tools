# Build + run perf_pipeline.cpp headlessly (no AE SDK). Same space-free-temp
# trick as build_tests.ps1 (cl chokes on the space in the project path).
$ErrorActionPreference = "Stop"
$TestsDir  = $PSScriptRoot
$CommonDir = Join-Path (Split-Path $TestsDir -Parent) "src\common"

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

$work = Join-Path $env:TEMP "edgerepair_perf"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Path $work | Out-Null

Copy-Item (Join-Path $CommonDir "*.h") $work -Force
Copy-Item (Join-Path $TestsDir "perf_pipeline.cpp") $work -Force

$cmd = "call `"$vcvars`" 2>nul && cd /d `"$work`" && cl /nologo /EHsc /O2 /std:c++17 perf_pipeline.cpp && .\perf_pipeline.exe"
cmd /c $cmd
exit $LASTEXITCODE
