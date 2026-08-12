# Per-channel value distribution of an uncompressed EXR (HALF or FLOAT).
#
# Point of it: a parity A/B that lands at exactly 0.0 is only strong evidence if
# the frame actually carries intermediate values. An image clipped to 0.0/1.0
# everywhere matches bit-for-bit on any backend, because clipping erases the
# last-bit differences the test is looking for.
param([Parameter(Mandatory=$true)][string]$Path)

Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Collections.Generic;

public class ExrStatsResult {
    public string[] Channel;
    public double[] Min, Max, Mean;
    public long[] ZeroCount, OneCount, MidCount;
    public long Total;
}

public class ExrStats {
    static string ReadStr(BinaryReader br) {
        var sb = new System.Text.StringBuilder();
        while (true) { byte b = br.ReadByte(); if (b == 0) break; sb.Append((char)b); }
        return sb.ToString();
    }

    static float HalfToFloat(ushort h) {
        int sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF, f;
        if (exp == 0) {
            if (mant == 0) f = sign << 31;
            else { exp = 113; while ((mant & 0x400) == 0) { mant <<= 1; exp--; } mant &= 0x3FF;
                   f = (sign << 31) | (exp << 23) | (mant << 13); }
        } else if (exp == 31) f = (sign << 31) | (0xFF << 23) | (mant << 13);
        else f = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
        return BitConverter.ToSingle(BitConverter.GetBytes(f), 0);
    }

    public static ExrStatsResult Analyse(string path) {
        using (var fs = File.OpenRead(path)) using (var br = new BinaryReader(fs)) {
            if (br.ReadInt32() != 20000630) throw new Exception("not an OpenEXR file");
            br.ReadInt32();
            var chan = new List<string>(); var types = new List<int>();
            int comp = 0, w = 0, h = 0;
            while (true) {
                string name = ReadStr(br);
                if (name.Length == 0) break;
                string type = ReadStr(br);
                int size = br.ReadInt32();
                byte[] data = br.ReadBytes(size);
                if (type == "chlist") {
                    using (var ms = new MemoryStream(data)) using (var cr = new BinaryReader(ms)) {
                        while (ms.Position < ms.Length) {
                            string cn = ReadStr(cr); if (cn.Length == 0) break;
                            chan.Add(cn); types.Add(cr.ReadInt32());
                            cr.ReadBytes(4); cr.ReadInt32(); cr.ReadInt32();
                        }
                    }
                } else if (type == "compression") comp = data[0];
                else if (name == "dataWindow") {
                    int x0 = BitConverter.ToInt32(data,0), y0 = BitConverter.ToInt32(data,4);
                    int x1 = BitConverter.ToInt32(data,8), y1 = BitConverter.ToInt32(data,12);
                    w = x1-x0+1; h = y1-y0+1;
                }
            }
            if (comp != 0) throw new Exception("only uncompressed EXR supported (compression code " + comp + ")");
            var offs = new long[h];
            for (int i = 0; i < h; i++) offs[i] = br.ReadInt64();

            int nc = chan.Count;
            var r = new ExrStatsResult();
            r.Channel = chan.ToArray();
            r.Min = new double[nc]; r.Max = new double[nc]; r.Mean = new double[nc];
            r.ZeroCount = new long[nc]; r.OneCount = new long[nc]; r.MidCount = new long[nc];
            r.Total = (long)w * h;
            var sum = new double[nc];
            for (int c = 0; c < nc; c++) { r.Min[c] = double.MaxValue; r.Max[c] = double.MinValue; }

            int[] bpc = new int[nc]; int rowBytes = 0;
            for (int c = 0; c < nc; c++) { bpc[c] = (types[c] == 1) ? 2 : 4; rowBytes += bpc[c] * w; }
            byte[] row = new byte[rowBytes];

            for (int y = 0; y < h; y++) {
                fs.Seek(offs[y], SeekOrigin.Begin); br.ReadInt32(); br.ReadInt32();
                int got = 0; while (got < rowBytes) { int n = fs.Read(row, got, rowBytes-got); if (n <= 0) throw new EndOfStreamException(); got += n; }
                int off = 0;
                for (int c = 0; c < nc; c++) {
                    bool half = (types[c] == 1);
                    for (int x = 0; x < w; x++) {
                        int p = off + x * bpc[c];
                        double v = half ? HalfToFloat(BitConverter.ToUInt16(row, p)) : BitConverter.ToSingle(row, p);
                        if (v < r.Min[c]) r.Min[c] = v;
                        if (v > r.Max[c]) r.Max[c] = v;
                        sum[c] += v;
                        if (v == 0.0) r.ZeroCount[c]++;
                        else if (v == 1.0) r.OneCount[c]++;
                        else r.MidCount[c]++;
                    }
                    off += bpc[c] * w;
                }
            }
            for (int c = 0; c < nc; c++) r.Mean[c] = sum[c] / r.Total;
            return r;
        }
    }
}
"@

$res = [ExrStats]::Analyse($Path)

Write-Host ""
Write-Host (Split-Path $Path -Leaf) -ForegroundColor Cyan
Write-Host ("{0,-8} {1,13} {2,13} {3,13} {4,10} {5,10} {6,12}" -f "channel","min","max","mean","= 0.0","= 1.0","in between")
for ($i = 0; $i -lt $res.Channel.Length; $i++) {
    $tot = [double]$res.Total
    Write-Host ("{0,-8} {1,13:G6} {2,13:G6} {3,13:G6} {4,9:P1} {5,9:P1} {6,11:P1}" -f `
        $res.Channel[$i], $res.Min[$i], $res.Max[$i], $res.Mean[$i],
        ($res.ZeroCount[$i]/$tot), ($res.OneCount[$i]/$tot), ($res.MidCount[$i]/$tot))
}
Write-Host ""
Write-Host ("{0:N0} pixels" -f $res.Total)
