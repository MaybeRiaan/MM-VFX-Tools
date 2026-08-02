# Read the Edge Repair AE perf log (written by the ER_PERF_LOG=1 build) and
# summarize per-pass timings. Run AFTER rendering/RAM-previewing in AE.
#   -Clear   truncate the log first (call before a fresh preview to isolate it)
#   -Tail N  show only the last N lines (default: full summary)
param([switch]$Clear, [int]$Tail = 0)

$log = Join-Path $env:TEMP "EdgeRepair_perf.log"

if ($Clear) {
    if (Test-Path $log) { Remove-Item $log -Force }
    Write-Host "Cleared $log -- now render/RAM-preview in AE, then re-run without -Clear." -ForegroundColor Yellow
    return
}

if (-not (Test-Path $log)) {
    Write-Host "No log at $log yet. Is this an ER_PERF_LOG=1 build, and have you rendered a Result-view frame in AE?" -ForegroundColor Red
    return
}

$lines = Get-Content $log
if ($Tail -gt 0) { $lines | Select-Object -Last $Tail; return }

Write-Host "=== Edge Repair AE perf log: $log ===" -ForegroundColor Cyan
$lines | Select-Object -Last 40 | Write-Host

# Parse TOTAL SmartRender lines for a quick min/median/max.
$totals = @()
foreach ($l in $lines) {
    if ($l -match 'TOTAL SmartRender\s+([\d.]+)\s+ms') { $totals += [double]$matches[1] }
}
if ($totals.Count -gt 0) {
    $sorted = $totals | Sort-Object
    $min = [math]::Round($sorted[0], 1)
    $max = [math]::Round($sorted[-1], 1)
    $med = [math]::Round($sorted[[int]([math]::Floor($sorted.Count / 2))], 1)
    Write-Host ""
    Write-Host ("Frames logged: " + $totals.Count + "   min " + $min + " ms   median " + $med + " ms   max " + $max + " ms") -ForegroundColor Green
}
