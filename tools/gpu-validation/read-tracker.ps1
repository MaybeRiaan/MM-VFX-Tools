# Dump the GPU validation tracker xlsx as readable rows.
# Usage: .\read-tracker.ps1 [-Sheet "MM_Qualify"] [-Full]
param([string]$Sheet = "", [switch]$Full)

$xlsx = "E:\OFX DEV\MM VFX TOOLS\docs\gpu-validation-tracker.xlsx"
# Unpack under TEMP, not $PSScriptRoot: this script lives in the repo now, and
# an extracted xlsx tree beside it shows up as untracked junk in git status.
$work = Join-Path $env:TEMP "mm-gpu-validation\xlsx_read"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($xlsx, $work)

$shared = @()
if (Test-Path "$work\xl\sharedStrings.xml") {
    $ss = [xml](Get-Content "$work\xl\sharedStrings.xml" -Raw)
    foreach ($si in $ss.sst.si) { $shared += (($si.SelectNodes(".//*[local-name()='t']") | ForEach-Object { $_.InnerText }) -join "") }
}

$wb   = [xml](Get-Content "$work\xl\workbook.xml" -Raw)
$rels = [xml](Get-Content "$work\xl\_rels\workbook.xml.rels" -Raw)
$map  = @{}; foreach ($r in $rels.Relationships.Relationship) { $map[$r.Id] = $r.Target }

function Get-CellText($c) {
    if ($c.t -eq "inlineStr") { return (($c.SelectNodes(".//*[local-name()='t']") | ForEach-Object { $_.InnerText }) -join "") }
    if ($c.t -eq "s") { $i = [int]$c.v; if ($i -lt $shared.Count) { return $shared[$i] } else { return "" } }
    if ($c.t -eq "str") { return $c.v }
    return $c.v
}
function Col($ref) { ($ref -replace '\d','') }

foreach ($s in $wb.workbook.sheets.sheet) {
    if ($Sheet -and $s.name -ne $Sheet) { continue }
    $rid = $s.GetAttribute("id","http://schemas.openxmlformats.org/officeDocument/2006/relationships")
    $path = Join-Path $work ($map[$rid].TrimStart('/') -replace '/','\')
    # Explicit UTF-8: these parts carry no BOM and no XML declaration, so
    # Get-Content would fall back to the ANSI codepage and mangle the
    # ballot-box glyphs the Pass column is checked against.
    $doc = [xml]([System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8))

    Write-Host ""
    Write-Host ("=== " + $s.name + " ===") -ForegroundColor Cyan
    foreach ($row in $doc.SelectNodes("//*[local-name()='row']")) {
        $cells = @{}
        foreach ($c in $row.SelectNodes("./*[local-name()='c']")) { $cells[(Col $c.r)] = (Get-CellText $c) }
        if ($Full) {
            $vals = @('A','B','C','D','E','F','G','H','I') | ForEach-Object { $cells[$_] }
            ($vals -join " | ")
        } else {
            # A=ID B=Stage C=Reference D=Test E=Expected F=Pass G=Measured H=Notes
            $id = $cells['A']; $test = $cells['D']; $pass = $cells['F']; $meas = $cells['G']; $note = $cells['H']
            if ($id -eq "ID") { continue }
            if (-not $id) { continue }
            if ($test.Length -gt 62) { $test = $test.Substring(0,62) + "..." }
            "{0,-6} {1,-14} {2,-65} {3}" -f $id, $pass, $test, $meas
        }
    }
}
