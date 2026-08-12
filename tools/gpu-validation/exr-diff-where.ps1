# Localise where two EXRs differ: by distance from each image edge, and by row.
#
# Discriminates the plausible causes of a GPU/CPU divergence:
#   errors confined to a border strip  -> edge/clamp policy in the kernel
#   errors clustered on a few rows     -> band seam / tile apron
#   errors spread evenly               -> accumulation order or weights
param(
    [Parameter(Mandatory=$true)][string]$A,
    [Parameter(Mandatory=$true)][string]$B,
    [double]$Threshold = 1e-5
)

Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Collections.Generic;

public class WhereResult {
    public double[] RowMax;
    public double[] EdgeDistMax;   // index = min distance to any edge, capped
    public long[]   EdgeDistCount; // pixels over threshold at that distance
    public double   Overall;
    public int W, H;
    public long OverCount;
    public double InteriorMax;     // max error at distance > Cap
    public int Cap;
}

public class ExrWhere {
    static string ReadStr(BinaryReader br) {
        var sb = new System.Text.StringBuilder();
        while (true) { byte b = br.ReadByte(); if (b == 0) break; sb.Append((char)b); }
        return sb.ToString();
    }
    static float H2F(ushort h) {
        int sign=(h>>15)&1, exp=(h>>10)&0x1F, mant=h&0x3FF, f;
        if (exp==0) { if (mant==0) f=sign<<31; else { exp=113; while((mant&0x400)==0){mant<<=1;exp--;} mant&=0x3FF; f=(sign<<31)|(exp<<23)|(mant<<13);} }
        else if (exp==31) f=(sign<<31)|(0xFF<<23)|(mant<<13);
        else f=(sign<<31)|((exp-15+127)<<23)|(mant<<13);
        return BitConverter.ToSingle(BitConverter.GetBytes(f),0);
    }
    class Hdr { public int W,H,Comp; public List<int> Types=new List<int>(); public long[] Offs; }
    static Hdr Read(BinaryReader br) {
        var e = new Hdr();
        if (br.ReadInt32()!=20000630) throw new Exception("not an OpenEXR file");
        br.ReadInt32();
        while (true) {
            string name=ReadStr(br); if (name.Length==0) break;
            string type=ReadStr(br); int size=br.ReadInt32(); byte[] data=br.ReadBytes(size);
            if (type=="chlist") {
                using (var ms=new MemoryStream(data)) using (var cr=new BinaryReader(ms))
                    while (ms.Position<ms.Length) {
                        string cn=ReadStr(cr); if (cn.Length==0) break;
                        e.Types.Add(cr.ReadInt32()); cr.ReadBytes(4); cr.ReadInt32(); cr.ReadInt32();
                    }
            } else if (type=="compression") e.Comp=data[0];
            else if (name=="dataWindow") {
                int x0=BitConverter.ToInt32(data,0), y0=BitConverter.ToInt32(data,4);
                int x1=BitConverter.ToInt32(data,8), y1=BitConverter.ToInt32(data,12);
                e.W=x1-x0+1; e.H=y1-y0+1;
            }
        }
        e.Offs=new long[e.H];
        for (int i=0;i<e.H;i++) e.Offs[i]=br.ReadInt64();
        return e;
    }

    public static WhereResult Run(string pa, string pb, double thr) {
        using (var fa=File.OpenRead(pa)) using (var ba=new BinaryReader(fa))
        using (var fb=File.OpenRead(pb)) using (var bb=new BinaryReader(fb)) {
            var ea=Read(ba); var eb=Read(bb);
            if (ea.Comp!=0||eb.Comp!=0) throw new Exception("only uncompressed EXR supported");
            int nc=ea.Types.Count, w=ea.W, h=ea.H;
            var r=new WhereResult(); r.W=w; r.H=h; r.Cap=64;
            r.RowMax=new double[h];
            r.EdgeDistMax=new double[r.Cap+1];
            r.EdgeDistCount=new long[r.Cap+1];

            int[] bpc=new int[nc]; int rowBytes=0;
            for (int c=0;c<nc;c++){ bpc[c]=(ea.Types[c]==1)?2:4; rowBytes+=bpc[c]*w; }
            byte[] rA=new byte[rowBytes], rB=new byte[rowBytes];

            for (int y=0;y<h;y++) {
                fa.Seek(ea.Offs[y],SeekOrigin.Begin); ba.ReadInt32(); ba.ReadInt32();
                fb.Seek(eb.Offs[y],SeekOrigin.Begin); bb.ReadInt32(); bb.ReadInt32();
                Fill(fa,rA); Fill(fb,rB);
                int off=0;
                for (int c=0;c<nc;c++) {
                    bool half=(ea.Types[c]==1);
                    for (int x=0;x<w;x++) {
                        int p=off+x*bpc[c];
                        double va = half?H2F(BitConverter.ToUInt16(rA,p)):BitConverter.ToSingle(rA,p);
                        double vb = half?H2F(BitConverter.ToUInt16(rB,p)):BitConverter.ToSingle(rB,p);
                        double d=Math.Abs(va-vb);
                        if (d==0) continue;
                        if (d>r.RowMax[y]) r.RowMax[y]=d;
                        if (d>r.Overall) r.Overall=d;
                        int dist=Math.Min(Math.Min(x,w-1-x),Math.Min(y,h-1-y));
                        int slot=Math.Min(dist,r.Cap);
                        if (d>r.EdgeDistMax[slot]) r.EdgeDistMax[slot]=d;
                        if (d>thr) { r.EdgeDistCount[slot]++; r.OverCount++;
                                     if (dist>r.Cap && d>r.InteriorMax) r.InteriorMax=d; }
                    }
                    off+=bpc[c]*w;
                }
            }
            return r;
        }
    }
    static void Fill(Stream s, byte[] b){ int g=0; while(g<b.Length){ int n=s.Read(b,g,b.Length-g); if(n<=0) throw new EndOfStreamException(); g+=n; } }
}
"@

$r = [ExrWhere]::Run($A, $B, $Threshold)

Write-Host ""
Write-Host ("{0}  vs  {1}   ({2}x{3})" -f (Split-Path $A -Leaf),(Split-Path $B -Leaf),$r.W,$r.H)
Write-Host ("overall max-abs {0:E3}   pixels over {1:E0}: {2:N0}" -f $r.Overall,$Threshold,$r.OverCount)

Write-Host ""
Write-Host "--- max error by distance from nearest image edge ---"
Write-Host ("{0,6} {1,14} {2,14}" -f "dist","max-abs","px over thr")
foreach ($d in 0,1,2,3,4,5,6,8,10,12,14,15,16,18,20,24,32,48,64) {
    if ($d -le $r.Cap) {
        Write-Host ("{0,6} {1,14:E3} {2,14:N0}" -f $d, $r.EdgeDistMax[$d], $r.EdgeDistCount[$d])
    }
}
Write-Host ("interior (dist > {0}) max-abs: {1:E3}" -f $r.Cap, $r.InteriorMax)

Write-Host ""
Write-Host "--- worst 15 rows ---"
$idx = 0..($r.H-1) | Sort-Object { -$r.RowMax[$_] } | Select-Object -First 15
foreach ($y in $idx) { Write-Host ("  row {0,5}   {1:E3}" -f $y, $r.RowMax[$y]) }

# A 2-band split puts its seam near the middle; report what happens there.
$mid = [int]($r.H/2)
Write-Host ""
Write-Host ("--- rows around the 2-band seam (~{0}) ---" -f $mid)
foreach ($y in ($mid-3)..($mid+3)) { if ($y -ge 0 -and $y -lt $r.H) { Write-Host ("  row {0,5}   {1:E3}" -f $y, $r.RowMax[$y]) } }
