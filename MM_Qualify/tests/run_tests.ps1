# MM Qualify - run the AE-free core + pipeline-parity tests.
# Compiles the two test TUs against the shared AE SDK headers (for A_long /
# PF types only) with cl, then runs them. Exit code mirrors the tests
# (0 = all pass). No AE host, no GPU device required.

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
$coreExe = Join-Path $outDir "core_test.exe"
$pipeExe = Join-Path $outDir "pipeline_test.exe"

$bat = @"
call "$vcvars"
if errorlevel 1 exit /b 1
cd /d "$outDir"
cl /nologo /EHsc /O2 /std:c++17 /DMSWindows /D_WINDOWS ^
   /I "$SdkRoot\Headers" /I "$SdkRoot\Headers\SP" /I "$SdkRoot\Util" /I "$Root\src" ^
   "$PSScriptRoot\core_test.cpp" ^
   /Fe:"$coreExe"
if errorlevel 1 exit /b 1
cl /nologo /EHsc /O2 /std:c++17 /DMSWindows /D_WINDOWS ^
   /I "$SdkRoot\Headers" /I "$SdkRoot\Headers\SP" /I "$SdkRoot\Util" /I "$Root\src" ^
   "$PSScriptRoot\pipeline_test.cpp" "$Root\src\blur.cpp" "$Root\src\garbage.cpp" "$Root\src\common\MM_Common.cpp" ^
   /Fe:"$pipeExe"
if errorlevel 1 exit /b 1
"@

$tmp = Join-Path $env:TEMP "mm_qualify_tests.bat"
Set-Content -Path $tmp -Value $bat -Encoding ascii
$ErrorActionPreference = "Continue"
$installerDir = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
if (Test-Path $installerDir) { $env:PATH = "$installerDir;$env:PATH" }
try { & cmd /c $tmp; $code = $LASTEXITCODE } finally { Remove-Item $tmp -ErrorAction SilentlyContinue }
if ($code -ne 0) { throw "Test compile failed (exit $code)" }

Write-Host ""
& $coreExe
$coreCode = $LASTEXITCODE
Write-Host ""
& $pipeExe
$pipeCode = $LASTEXITCODE

if ($coreCode -ne 0) { exit $coreCode }
exit $pipeCode
