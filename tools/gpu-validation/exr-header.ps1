# Print the header of an OpenEXR file: channels, pixel type, compression, size.
# Pixel type is the point of this - a HALF file cannot resolve a 1e-5 parity
# target, since half quantises at ~1e-3 near 1.0.
param([Parameter(Mandatory=$true)][string[]]$Path)

$PIXTYPE = @{ 0 = "UINT"; 1 = "HALF (16-bit)"; 2 = "FLOAT (32-bit)" }
$COMPRESSION = @{ 0="none"; 1="RLE"; 2="ZIPS"; 3="ZIP"; 4="PIZ"; 5="PXR24"; 6="B44"; 7="B44A"; 8="DWAA"; 9="DWAB" }

foreach ($p in $Path) {
    $fs = [System.IO.File]::OpenRead($p)
    $br = New-Object System.IO.BinaryReader($fs)
    try {
        $magic = $br.ReadInt32()
        if ($magic -ne 20000630) { Write-Host "$p : not an EXR (magic=$magic)" -ForegroundColor Red; continue }
        $version = $br.ReadInt32()

        Write-Host ""
        Write-Host (Split-Path $p -Leaf) -ForegroundColor Cyan
        Write-Host ("  version field : {0} (v{1})" -f $version, ($version -band 0xFF))

        function Read-NullString($br) {
            $sb = New-Object System.Text.StringBuilder
            while ($true) { $b = $br.ReadByte(); if ($b -eq 0) { break }; [void]$sb.Append([char]$b) }
            $sb.ToString()
        }

        while ($true) {
            $name = Read-NullString $br
            if ($name -eq "") { break }
            $type = Read-NullString $br
            $size = $br.ReadInt32()
            $data = $br.ReadBytes($size)

            switch ($type) {
                "chlist" {
                    $ms = New-Object System.IO.MemoryStream(,$data)
                    $cr = New-Object System.IO.BinaryReader($ms)
                    $chans = @()
                    while ($cr.BaseStream.Position -lt $cr.BaseStream.Length) {
                        $cn = Read-NullString $cr
                        if ($cn -eq "") { break }
                        $pt = $cr.ReadInt32()
                        [void]$cr.ReadBytes(4)          # pLinear + 3 reserved
                        [void]$cr.ReadInt32(); [void]$cr.ReadInt32()   # x/y sampling
                        $chans += ("{0}={1}" -f $cn, $PIXTYPE[$pt])
                    }
                    Write-Host ("  channels      : {0}" -f ($chans -join "  "))
                }
                "compression" {
                    Write-Host ("  compression   : {0}" -f $COMPRESSION[[int]$data[0]])
                }
                "box2i" {
                    if ($name -eq "dataWindow") {
                        $x0=[BitConverter]::ToInt32($data,0); $y0=[BitConverter]::ToInt32($data,4)
                        $x1=[BitConverter]::ToInt32($data,8); $y1=[BitConverter]::ToInt32($data,12)
                        Write-Host ("  dataWindow    : {0}x{1}" -f ($x1-$x0+1), ($y1-$y0+1))
                    }
                }
            }
        }
        Write-Host ("  file size     : {0:N0} bytes" -f (Get-Item $p).Length)
    }
    finally { $br.Close(); $fs.Close() }
}
