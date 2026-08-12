# Exact per-channel max-abs difference between two OpenEXR files.
#
# Built for the MM VFX Tools GPU parity target of <= 1e-5 per channel
# (docs/gpu-validation-checklist.md 3 Step 2). Reads uncompressed scanline EXRs
# of HALF or FLOAT type with no external dependencies.
#
# WARNING it will print loudly about: a HALF (16-bit) file cannot resolve 1e-5.
# Half quantises at ~9.8e-4 near 1.0 and ~6.1e-5 near 0.1, so a real difference
# below the quantisation step is rounded away in BOTH files and reads as zero.
# Render 32-bit float EXRs for a parity verdict.
param(
    [Parameter(Mandatory=$true)][string]$A,
    [Parameter(Mandatory=$true)][string]$B,
    [double]$Tolerance = 1e-5
)

Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Collections.Generic;

public class ExrImage {
    public int Width, Height;
    public List<string> Channels = new List<string>();
    public List<int> Types = new List<int>();   // 0=UINT 1=HALF 2=FLOAT
    public int Compression;
    public long[] Offsets;

    static string ReadStr(BinaryReader br) {
        var sb = new System.Text.StringBuilder();
        while (true) { byte b = br.ReadByte(); if (b == 0) break; sb.Append((char)b); }
        return sb.ToString();
    }

    public static ExrImage ReadHeader(BinaryReader br) {
        var e = new ExrImage();
        if (br.ReadInt32() != 20000630) throw new Exception("not an OpenEXR file");
        br.ReadInt32();
        while (true) {
            string name = ReadStr(br);
            if (name.Length == 0) break;
            string type = ReadStr(br);
            int size = br.ReadInt32();
            byte[] data = br.ReadBytes(size);
            if (type == "chlist") {
                using (var ms = new MemoryStream(data))
                using (var cr = new BinaryReader(ms)) {
                    while (ms.Position < ms.Length) {
                        string cn = ReadStr(cr);
                        if (cn.Length == 0) break;
                        e.Channels.Add(cn);
                        e.Types.Add(cr.ReadInt32());
                        cr.ReadBytes(4);
                        cr.ReadInt32(); cr.ReadInt32();
                    }
                }
            } else if (type == "compression") {
                e.Compression = data[0];
            } else if (name == "dataWindow") {
                int x0 = BitConverter.ToInt32(data,0), y0 = BitConverter.ToInt32(data,4);
                int x1 = BitConverter.ToInt32(data,8), y1 = BitConverter.ToInt32(data,12);
                e.Width = x1 - x0 + 1; e.Height = y1 - y0 + 1;
            }
        }
        e.Offsets = new long[e.Height];
        for (int i = 0; i < e.Height; i++) e.Offsets[i] = br.ReadInt64();
        return e;
    }

    public static float HalfToFloat(ushort h) {
        int sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF, f;
        if (exp == 0) {
            if (mant == 0) f = sign << 31;
            else {
                exp = 113;
                while ((mant & 0x400) == 0) { mant <<= 1; exp--; }
                mant &= 0x3FF;
                f = (sign << 31) | (exp << 23) | (mant << 13);
            }
        } else if (exp == 31) f = (sign << 31) | (0xFF << 23) | (mant << 13);
        else f = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
        return BitConverter.ToSingle(BitConverter.GetBytes(f), 0);
    }
}

public class ExrDiffResult {
    public string[] Channel;
    public double[] MaxAbs;
    public int[] WorstX, WorstY;
    public double[] ValA, ValB;
    public long[] DiffCount;
    public long TotalPixels;
}

public class ExrDiffer {
    public static ExrDiffResult Compare(string pathA, string pathB) {
        using (var fa = File.OpenRead(pathA)) using (var ba = new BinaryReader(fa))
        using (var fb = File.OpenRead(pathB)) using (var bb = new BinaryReader(fb)) {
            var ea = ExrImage.ReadHeader(ba);
            var eb = ExrImage.ReadHeader(bb);
            if (ea.Width != eb.Width || ea.Height != eb.Height)
                throw new Exception("dimensions differ: " + ea.Width + "x" + ea.Height + " vs " + eb.Width + "x" + eb.Height);
            if (ea.Compression != 0 || eb.Compression != 0)
                throw new Exception("only uncompressed EXR is supported (compression code " + ea.Compression + "/" + eb.Compression + "); re-render with compression = None");
            if (ea.Channels.Count != eb.Channels.Count)
                throw new Exception("channel count differs");

            int nc = ea.Channels.Count, w = ea.Width, h = ea.Height;
            var r = new ExrDiffResult();
            r.Channel = ea.Channels.ToArray();
            r.MaxAbs = new double[nc]; r.WorstX = new int[nc]; r.WorstY = new int[nc];
            r.ValA = new double[nc]; r.ValB = new double[nc]; r.DiffCount = new long[nc];
            r.TotalPixels = (long)w * h;

            int[] bpc = new int[nc];
            int rowBytes = 0;
            for (int c = 0; c < nc; c++) { bpc[c] = (ea.Types[c] == 1) ? 2 : 4; rowBytes += bpc[c] * w; }

            byte[] rowA = new byte[rowBytes], rowB = new byte[rowBytes];
            for (int y = 0; y < h; y++) {
                fa.Seek(ea.Offsets[y], SeekOrigin.Begin); ba.ReadInt32(); ba.ReadInt32();
                fb.Seek(eb.Offsets[y], SeekOrigin.Begin); bb.ReadInt32(); bb.ReadInt32();
                ReadFull(fa, rowA); ReadFull(fb, rowB);

                int off = 0;
                for (int c = 0; c < nc; c++) {
                    bool half = (ea.Types[c] == 1);
                    for (int x = 0; x < w; x++) {
                        int p = off + x * bpc[c];
                        double va, vb;
                        if (half) {
                            va = ExrImage.HalfToFloat(BitConverter.ToUInt16(rowA, p));
                            vb = ExrImage.HalfToFloat(BitConverter.ToUInt16(rowB, p));
                        } else {
                            va = BitConverter.ToSingle(rowA, p);
                            vb = BitConverter.ToSingle(rowB, p);
                        }
                        double d = Math.Abs(va - vb);
                        if (d > 0) r.DiffCount[c]++;
                        if (d > r.MaxAbs[c]) {
                            r.MaxAbs[c] = d; r.WorstX[c] = x; r.WorstY[c] = y;
                            r.ValA[c] = va; r.ValB[c] = vb;
                        }
                    }
                    off += bpc[c] * w;
                }
            }
            return r;
        }
    }

    static void ReadFull(Stream s, byte[] buf) {
        int got = 0;
        while (got < buf.Length) {
            int n = s.Read(buf, got, buf.Length - got);
            if (n <= 0) throw new EndOfStreamException();
            got += n;
        }
    }
}
"@

foreach ($p in @($A,$B)) {
    if (-not (Test-Path -LiteralPath $p)) { throw "not found: $p" }
}

# Report the pixel type first - it decides whether the verdict means anything.
$fsA = [System.IO.File]::OpenRead($A); $brA = New-Object System.IO.BinaryReader($fsA)
$hdr = [ExrImage]::ReadHeader($brA); $brA.Close(); $fsA.Close()
$isHalf = $hdr.Types -contains 1

Write-Host ""
Write-Host ("A: " + (Split-Path $A -Leaf) + "    B: " + (Split-Path $B -Leaf))
Write-Host ("{0}x{1}   channels: {2}   type: {3}" -f $hdr.Width, $hdr.Height,
            ($hdr.Channels -join ","), $(if ($isHalf) { "HALF (16-bit)" } else { "FLOAT (32-bit)" }))

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$res = [ExrDiffer]::Compare($A, $B)
$sw.Stop()

Write-Host ""
Write-Host ("{0,-8} {1,14} {2,12} {3,-22} {4}" -f "channel","max-abs","differing","worst pixel","values (A vs B)")
$overall = 0.0
for ($i = 0; $i -lt $res.Channel.Length; $i++) {
    if ($res.MaxAbs[$i] -gt $overall) { $overall = $res.MaxAbs[$i] }
    Write-Host ("{0,-8} {1,14:E3} {2,12:N0} {3,-22} {4:G9} vs {5:G9}" -f `
        $res.Channel[$i], $res.MaxAbs[$i], $res.DiffCount[$i],
        ("(" + $res.WorstX[$i] + "," + $res.WorstY[$i] + ")"),
        $res.ValA[$i], $res.ValB[$i])
}

Write-Host ""
Write-Host ("overall max-abs : {0:E3}   ({1:N0} pixels, {2:N1}s)" -f $overall, $res.TotalPixels, $sw.Elapsed.TotalSeconds)

if ($overall -le $Tolerance) {
    Write-Host ("VERDICT: within tolerance {0:E0}" -f $Tolerance) -ForegroundColor Green
} else {
    Write-Host ("VERDICT: EXCEEDS tolerance {0:E0}" -f $Tolerance) -ForegroundColor Red
}

if ($isHalf) {
    Write-Host ""
    Write-Host "CAVEAT: these are HALF (16-bit) files. Half quantises at ~9.8e-4 near 1.0" -ForegroundColor Yellow
    Write-Host "and ~6.1e-5 near 0.1, both coarser than the 1e-5 target, so a genuine" -ForegroundColor Yellow
    Write-Host "difference can be rounded away in BOTH files and read here as zero." -ForegroundColor Yellow
    Write-Host "A pass on half data is NOT a pass at 1e-5. Re-render as 32-bit float." -ForegroundColor Yellow
}
