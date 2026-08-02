# MM FreqSep - run the AE-free Phase 2 core tests.
# Compiles tests/identity_test.cpp + src/blur.cpp against the shared AE SDK
# headers (for A_long / PF types only) with cl, then runs the executable.
# Exit code mirrors the test binary (0 = all pass).

$ErrorActionPreference = "Stop"
$Root = Split-Path $PSScriptRoot -Parent

# Same shared SDK the CMake build uses (AE_SDK_ROOT); override via env var.
$SdkRoot = if ($env:AE_SDK_ROOT) { $env:AE_SDK_ROOT } else {
    "E:\OFX DEV\AE SDK\AfterEffectsSDK_25.6_61_win\ae25.6_61.64bit.AfterEffectsSDK\Examples"
}
if (-not (Test-Path (Join-Path $SdkRoot "Headers\AE_Effect.h"))) {
    throw "AE SDK not found at: $SdkRoot (set AE_SDK_ROOT)"
}

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
   /I "$SdkRoot\Headers" /I "$SdkRoot\Headers\SP" /I "$SdkRoot\Util" /I "$Root\src" ^
   "$PSScriptRoot\identity_test.cpp" "$Root\src\blur.cpp" "$Root\src\guided.cpp" "$Root\src\matte.cpp" ^
   /Fe:"$exe"
if errorlevel 1 exit /b 1
cl /nologo /EHsc /O2 /std:c++17 /DMSWindows /D_WINDOWS ^
   /I "$SdkRoot\Headers" /I "$SdkRoot\Headers\SP" /I "$SdkRoot\Util" /I "$Root\src" ^
   "$PSScriptRoot\gpu_parity_test.cpp" "$Root\src\blur.cpp" "$Root\src\guided.cpp" "$Root\src\matte.cpp" "$Root\src\fs_common.cpp" ^
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
