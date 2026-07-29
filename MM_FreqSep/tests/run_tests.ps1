# MM FreqSep - run the AE-free Phase 2 core tests.
# Compiles tests/identity_test.cpp + src/blur.cpp against the vendored SDK
# headers (for A_long / PF types only) with cl, then runs the executable.
# Exit code mirrors the test binary (0 = all pass).

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent

# Enter the VS dev environment for cl.exe.
$vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstall = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsInstall "VC\Auxiliary\Build\vcvars64.bat"

$outDir = Join-Path $PSScriptRoot "out"
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
$exe = Join-Path $outDir "identity_test.exe"
# GPU kernel parity: the .kernel bodies are platform-free by rule, so their
# arithmetic is transcribed into C and diffed against the CPU reference here —
# no DX12 device, no AE host, no Mac required.
$gpuExe = Join-Path $outDir "gpu_parity_test.exe"

$bat = @"
call "$vcvars"
if errorlevel 1 exit /b 1
cd /d "$outDir"
cl /nologo /EHsc /O2 /std:c++17 /DMSWindows /D_WINDOWS ^
   /I "$Root\sdk\Headers" /I "$Root\sdk\Headers\SP" /I "$Root\sdk\Util" /I "$Root\src" ^
   "$PSScriptRoot\identity_test.cpp" "$Root\src\blur.cpp" "$Root\src\guided.cpp" "$Root\src\matte.cpp" ^
   /Fe:"$exe"
if errorlevel 1 exit /b 1
cl /nologo /EHsc /O2 /std:c++17 /DMSWindows /D_WINDOWS ^
   /I "$Root\sdk\Headers" /I "$Root\sdk\Headers\SP" /I "$Root\sdk\Util" /I "$Root\src" ^
   "$PSScriptRoot\gpu_parity_test.cpp" "$Root\src\blur.cpp" "$Root\src\guided.cpp" "$Root\src\matte.cpp" "$Root\src\common\MM_Common.cpp" ^
   /Fe:"$gpuExe"
if errorlevel 1 exit /b 1
"@

$tmp = Join-Path $env:TEMP "mm_freqsep_tests.bat"
Set-Content -Path $tmp -Value $bat -Encoding ascii
$ErrorActionPreference = "Continue"
$installerDir = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
if (Test-Path $installerDir) { $env:PATH = "$installerDir;$env:PATH" }
try { & cmd /c $tmp; $code = $LASTEXITCODE } finally { Remove-Item $tmp -ErrorAction SilentlyContinue }
if ($code -ne 0) { throw "Test compile failed (exit $code)" }

Write-Host ""
& $exe
$coreCode = $LASTEXITCODE

& $gpuExe
$gpuCode = $LASTEXITCODE

if ($coreCode -ne 0) { exit $coreCode }
exit $gpuCode
