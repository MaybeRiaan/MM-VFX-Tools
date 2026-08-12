# Update cells in the GPU validation tracker xlsx, addressed by sheet name + row ID.
# Edits are applied via the XML DOM (so escaping is handled), then re-zipped.
#
# Usage:
#   $edits = @(
#     @{ Sheet="Session setup"; Id="SET-08"; Pass="* pass"; Measured="..."; Notes="..." }
#   )
#   .\write-tracker.ps1 -Edits $edits
#
# Pass must be one of the dropdown values (U+2610 not run / U+2611 pass /
# U+2612 fail / en-dash blocked / en-dash n/a) or the dropdown goes stale.
param([Parameter(Mandatory=$true)][array]$Edits, [switch]$WhatIf)

$xlsx = "E:\OFX DEV\MM VFX TOOLS\docs\gpu-validation-tracker.xlsx"
$work = Join-Path $env:TEMP "mm-gpu-validation\xlsx_write"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($xlsx, $work)

$wb   = [xml](Get-Content "$work\xl\workbook.xml" -Raw)
$rels = [xml](Get-Content "$work\xl\_rels\workbook.xml.rels" -Raw)
$map  = @{}; foreach ($r in $rels.Relationships.Relationship) { $map[$r.Id] = $r.Target }
$ns   = "http://schemas.openxmlformats.org/spreadsheetml/2006/main"

$sheetPath = @{}
foreach ($s in $wb.workbook.sheets.sheet) {
    $rid = $s.GetAttribute("id","http://schemas.openxmlformats.org/officeDocument/2006/relationships")
    $sheetPath[$s.name] = Join-Path $work ($map[$rid].TrimStart('/') -replace '/','\')
}

function Cell-Text($c) { ($c.SelectNodes(".//*[local-name()='t']") | ForEach-Object { $_.InnerText }) -join "" }
function Col-Of($ref) { $ref -replace '\d','' }
function Col-Index($col) { $n=0; foreach ($ch in $col.ToCharArray()) { $n = $n*26 + ([int][char]$ch - 64) }; $n }

# Group edits by sheet so each sheet XML is loaded and saved once.
$applied = 0
foreach ($grp in ($Edits | Group-Object { $_.Sheet })) {
    $name = $grp.Name
    if (-not $sheetPath.ContainsKey($name)) { Write-Warning "no such sheet: $name"; continue }
    $path = $sheetPath[$name]
    $doc = New-Object System.Xml.XmlDocument
    $doc.PreserveWhitespace = $true
    $doc.Load($path)

    foreach ($e in $grp.Group) {
        $row = $null
        foreach ($r in $doc.SelectNodes("//*[local-name()='row']")) {
            $first = $r.SelectSingleNode("./*[local-name()='c'][starts-with(@r,'A')]")
            if ($first -and (Cell-Text $first) -eq $e.Id) { $row = $r; break }
        }
        if (-not $row) { Write-Warning "$name : row id '$($e.Id)' not found"; continue }
        $rowNum = $row.GetAttribute("r")

        $targets = @{}
        if ($e.ContainsKey("Pass"))     { $targets["F"] = $e.Pass }
        if ($e.ContainsKey("Measured")) { $targets["G"] = $e.Measured }
        if ($e.ContainsKey("Notes"))    { $targets["H"] = $e.Notes }

        foreach ($col in $targets.Keys) {
            $ref = "$col$rowNum"
            $cell = $row.SelectSingleNode("./*[local-name()='c'][@r='$ref']")
            if (-not $cell) {
                # Create the cell, inheriting the style of the row's last cell,
                # and insert it in column order (Excel requires sorted cells).
                $cell = $doc.CreateElement("c", $ns)
                $cell.SetAttribute("r", $ref)
                $prev = $null
                foreach ($c in $row.SelectNodes("./*[local-name()='c']")) {
                    if ((Col-Index (Col-Of $c.GetAttribute("r"))) -lt (Col-Index $col)) { $prev = $c }
                }
                if ($prev) { $cell.SetAttribute("s", $prev.GetAttribute("s")); [void]$row.InsertAfter($cell, $prev) }
                else       { [void]$row.PrependChild($cell) }
            }
            $cell.SetAttribute("t","inlineStr")
            while ($cell.HasChildNodes) { [void]$cell.RemoveChild($cell.FirstChild) }
            $is = $doc.CreateElement("is", $ns)
            $t  = $doc.CreateElement("t", $ns)
            # No xml:space here. SetAttribute with the xml namespace URI emits a
            # generated prefix (d6p1:space) bound to that URI, which is illegal -
            # only the "xml" prefix may bind it - and the result is invalid XML
            # that Excel rejects. The original <t> elements carry no xml:space.
            $t.InnerText = [string]$targets[$col]
            [void]$is.AppendChild($t); [void]$cell.AppendChild($is)
            $applied++
        }
        Write-Host ("  {0,-14} {1,-7} updated: {2}" -f $name, $e.Id, (($targets.Keys | Sort-Object) -join ","))
    }
    if (-not $WhatIf) { $doc.Save($path) }
}

if ($WhatIf) { Write-Host "WhatIf - $applied cell(s) would change; xlsx not rewritten"; return }

# Re-zip over the original.
#
# NOT ZipFile::CreateFromDirectory - on .NET Framework it names entries with the
# platform separator, i.e. "xl\worksheets\sheet1.xml". That violates the ZIP/OPC
# spec (entry names must use '/') and Excel refuses the file. Entries are written
# by hand, in the original archive's order, with forward slashes.
$order = @()
$src = [System.IO.Compression.ZipFile]::OpenRead($xlsx)
foreach ($e in $src.Entries) { $order += ($e.FullName -replace '\\','/') }
$src.Dispose()

$tmp = "$work.zip"
if (Test-Path $tmp) { Remove-Item $tmp -Force }
$zip = [System.IO.Compression.ZipFile]::Open($tmp, [System.IO.Compression.ZipArchiveMode]::Create)
foreach ($name in $order) {
    $disk = Join-Path $work ($name -replace '/','\')
    # -LiteralPath: "[Content_Types].xml" is a wildcard character class to Test-Path.
    if (-not (Test-Path -LiteralPath $disk)) { Write-Warning "missing on disk, skipped: $name"; continue }
    $entry = $zip.CreateEntry($name, [System.IO.Compression.CompressionLevel]::Optimal)
    $out = $entry.Open()
    $bytes = [System.IO.File]::ReadAllBytes($disk)
    $out.Write($bytes, 0, $bytes.Length)
    $out.Dispose()
}
$zip.Dispose()

# Only replace the real file once the rebuilt archive reads back cleanly.
$check = [System.IO.Compression.ZipFile]::OpenRead($tmp)
$names = @($check.Entries | ForEach-Object { $_.FullName })
$check.Dispose()
if ($names.Count -ne $order.Count -or ($names | Where-Object { $_ -match '\\' })) {
    Remove-Item $tmp -Force
    throw "repack verification failed - $xlsx left untouched"
}
Copy-Item $tmp $xlsx -Force
Remove-Item $tmp -Force
Write-Host "$applied cell(s) written to $xlsx ($($names.Count) entries)" -ForegroundColor Green
