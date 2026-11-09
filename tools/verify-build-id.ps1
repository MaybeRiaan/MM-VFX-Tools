# verify-build-id.ps1 - prove that an in-AE test result is real.
#
# WHY THIS EXISTS
# ---------------
# Two documented traps in this suite produce a PASS that is indistinguishable
# from a real result, and neither is visible from the log alone:
#
#   1. STALE BINARY. AE holds a file lock on a loaded plugin, so a build that
#      ran while AE was open compiles fine and silently does not install (see
#      the mm_publish note in CMakeLists.txt). The version quad in the log is
#      then correct and the binary is still the old one.
#
#   2. CACHE-SERVED RENDER. AE can answer a Save Frame As entirely from its
#      frame cache, so an A/B can pass with ZERO plugin renders and difference
#      to exactly 0.000e+00 (CLAUDE.md, "Verification traps").
#
# This script turns both into assertions, reading two files:
#
#   <temp>\MM_VFX_Tools_build.log   the [build] line every plugin writes at
#                                   GLOBAL_SETUP, naming the binary AE actually
#                                   LOADED. Shared by all five plugins.
#   <temp>\<Plugin>_diag.log        that plugin's per-render lines, each
#                                   carrying the id of the binary that rendered
#                                   it. Only plugins with a diag logger have
#                                   this; without it, render count cannot be
#                                   verified and the script says so rather than
#                                   passing.
#
# NOTE ON GPU LOGS: do not count renders in <Plugin>_gpu.log. Those sites use
# GT_GPU_LOG_FIRST, which caps SMART_RENDER lines at 8, so the count silently
# understates past 8 frames. The diag log's [render] line is uncapped.
#
# USAGE
# -----
#   .\tools\verify-build-id.ps1 -Reset                     # before the AE run
#   .\tools\verify-build-id.ps1 -MinRenders 3 `            # after the AE run
#       -ExpectAex "build\GrainTransfer\GrainTransfer.aex"
#
# Exit code 0 = every assertion held. Non-zero = do not trust the imagery.

[CmdletBinding()]
param(
    [string]$Plugin      = "GrainTransfer",
    [string]$ExpectAex   = "",
    [int]   $MinRenders  = 1,
    [string]$Json        = "",
    [switch]$Reset
)

$ErrorActionPreference = "Stop"

$buildLog = Join-Path $env:TEMP "MM_VFX_Tools_build.log"
$diagLog  = Join-Path $env:TEMP "$($Plugin)_diag.log"
$gpuLog   = Join-Path $env:TEMP "$($Plugin)_gpu.log"

if ($Reset) {
    foreach ($p in @($buildLog, $diagLog, $gpuLog)) {
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force; Write-Host "removed $p" }
        else { Write-Host "absent  $p" }
    }
    Write-Host "Logs reset. Run the AE case now, then re-run without -Reset." -ForegroundColor Cyan
    exit 0
}

$failures = @()
$warnings = @()
function Fail([string]$m) { $script:failures += $m }
function Warn([string]$m) { $script:warnings += $m }

# Field readers. Tolerant on purpose: the [build] line is name=value so fields
# can be added later without breaking this parser. `path` runs to end of line
# because AE plugin paths contain spaces.
function FieldToken([string]$line, [string]$name) {
    $re = '(?:^|\s)' + [regex]::Escape($name) + '=(\S+)'
    if ($line -match $re) { return $Matches[1] }
    return $null
}
function FieldRest([string]$line, [string]$name) {
    $re = '(?:^|\s)' + [regex]::Escape($name) + '=(.*)$'
    if ($line -match $re) { return $Matches[1] }
    return $null
}

# --- identity: the [build] line for THIS plugin --------------------------
if (-not (Test-Path -LiteralPath $buildLog)) {
    Write-Host "FAIL: no build log at $buildLog" -ForegroundColor Red
    Write-Host "      No plugin reached GLOBAL_SETUP - nothing loaded at all." -ForegroundColor Red
    exit 1
}

# @() is load-bearing throughout: Get-Content on a one-line file returns a bare
# string, and indexing a string walks CHARACTERS. A one-line log is exactly the
# cache-trap case (loaded, never rendered), so this must not degrade.
$buildLines = @(Get-Content -LiteralPath $buildLog)
$mine = @($buildLines | Where-Object {
    $_ -like '`[build`]*' -and (FieldToken $_ 'plugin') -eq $Plugin
})
if ($mine.Count -eq 0) {
    Write-Host "FAIL: no [build] line for plugin=$Plugin in $buildLog" -ForegroundColor Red
    Write-Host "      Either it never loaded, or the loaded binary predates" -ForegroundColor Red
    Write-Host "      provenance stamping - itself a stale-binary finding." -ForegroundColor Red
    exit 1
}

$buildLine  = $mine[$mine.Count - 1]          # most recent load
$loadedId   = FieldToken $buildLine 'id'
$loadedVer  = FieldToken $buildLine 'version'
$loadedWhen = FieldToken $buildLine 'time'
$loadedSize = [int64](FieldToken $buildLine 'size')
$loadedPath = FieldRest  $buildLine 'path'

# --- trap 2: did anything actually render? --------------------------------
$renderCount = -1
$renderIds   = @()
if (Test-Path -LiteralPath $diagLog) {
    $dl = @(Get-Content -LiteralPath $diagLog)

    # Scope to the last load recorded in THIS file, so renders from an earlier
    # run cannot be lent to this one.
    $lastBuild = -1
    for ($i = 0; $i -lt $dl.Count; $i++) { if ($dl[$i] -like '`[build`]*') { $lastBuild = $i } }
    if ($lastBuild -lt 0) {
        Warn "no [build] line in $diagLog - cannot scope renders to a load"
        $scoped = $dl
    } else {
        $diagId = FieldToken $dl[$lastBuild] 'id'
        if ($diagId -ne $loadedId) {
            Fail ("the diag log's last load (id=$diagId) is not the load in the " +
                  "build log (id=$loadedId) - the diag log is stale")
        }
        $scoped = @($dl[$lastBuild..($dl.Count - 1)])
    }

    $renderLines = @($scoped | Where-Object { $_ -like '`[render`]*' })
    $renderCount = $renderLines.Count
    $renderIds   = @($renderLines | ForEach-Object { FieldToken $_ 'id' } |
                     Where-Object { $_ } | Sort-Object -Unique)

    if ($renderCount -lt $MinRenders) {
        Fail ("only $renderCount render(s) logged, expected >= $MinRenders - " +
              "imagery was probably served from AE's frame cache, not rendered")
    }
    $foreign = @($renderIds | Where-Object { $_ -ne $loadedId })
    if ($foreign.Count -gt 0) {
        Fail ("renders came from more than one binary: loaded id=$loadedId but " +
              "render ids include " + ($foreign -join ", "))
    }
} elseif ($MinRenders -gt 0) {
    # Never pass an assertion that could not be made.
    Fail ("cannot verify render count: $Plugin has no diag log at $diagLog. " +
          "Either the plugin has no per-render [render] line wired, or it never " +
          "rendered. Pass -MinRenders 0 to check identity only.")
}

# --- the loaded file, as it sits on disk now ------------------------------
$loadedHash = "<unreadable>"
if ($loadedPath -and (Test-Path -LiteralPath $loadedPath)) {
    $fi = Get-Item -LiteralPath $loadedPath
    if ($fi.Length -ne $loadedSize) {
        Fail ("the file at the loaded path changed since AE loaded it " +
              "(logged $loadedSize bytes, now $($fi.Length)) - AE is running " +
              "code that no longer matches this file")
    }
    $loadedHash = (Get-FileHash -LiteralPath $loadedPath -Algorithm SHA256).Hash
} else {
    Warn "the loaded path no longer exists on disk: $loadedPath"
}

# --- trap 1: is the loaded binary the one just built? ---------------------
$expectHash = ""
if ($ExpectAex -ne "") {
    if (-not (Test-Path -LiteralPath $ExpectAex)) {
        Fail "-ExpectAex does not exist: $ExpectAex"
    } else {
        $expectHash = (Get-FileHash -LiteralPath $ExpectAex -Algorithm SHA256).Hash
        if ($loadedHash -eq "<unreadable>") {
            Fail "cannot compare against -ExpectAex: the loaded file is unreadable"
        } elseif ($expectHash -ne $loadedHash) {
            Fail ("AE LOADED A DIFFERENT BINARY than the one built. " +
                  "This is the file-lock trap: the build succeeded and the " +
                  "deploy silently did not. Close AE and re-publish.")
        }
    }
}

# --- report ---------------------------------------------------------------
Write-Host ""
Write-Host "=== build-ID verification: $Plugin ===" -ForegroundColor Cyan
Write-Host ("  loaded version : {0}" -f $loadedVer)
Write-Host ("  loaded id      : {0}" -f $loadedId)
Write-Host ("  loaded at      : {0}" -f $loadedWhen)
Write-Host ("  loaded path    : {0}" -f $loadedPath)
Write-Host ("  loaded sha256  : {0}" -f $loadedHash)
if ($expectHash -ne "") { Write-Host ("  expected sha256: {0}" -f $expectHash) }
if ($renderCount -ge 0) {
    Write-Host ("  renders logged : {0} (threshold {1})" -f $renderCount, $MinRenders)
    Write-Host ("  distinct ids   : {0}" -f ($renderIds -join ", "))
} else {
    Write-Host  "  renders logged : n/a (no diag log for this plugin)"
}
foreach ($w in $warnings) { Write-Host "  WARN: $w" -ForegroundColor Yellow }

if ($Json -ne "") {
    [pscustomobject]@{
        plugin         = $Plugin
        version        = $loadedVer
        buildId        = $loadedId
        loadedAt       = $loadedWhen
        loadedPath     = $loadedPath
        loadedSha256   = $loadedHash
        expectedSha256 = $expectHash
        renderCount    = $renderCount
        minRenders     = $MinRenders
        distinctIds    = $renderIds
        warnings       = $warnings
        failures       = $failures
        pass           = ($failures.Count -eq 0)
    } | ConvertTo-Json -Depth 4 | Out-File -FilePath $Json -Encoding utf8
    Write-Host "  json           : $Json"
}

Write-Host ""
if ($failures.Count -eq 0) {
    Write-Host "PASS - this result is attributable to the binary named above." -ForegroundColor Green
    exit 0
}
foreach ($f in $failures) { Write-Host "FAIL: $f" -ForegroundColor Red }
Write-Host ""
Write-Host "Do not trust any imagery from this run." -ForegroundColor Red
exit 1
