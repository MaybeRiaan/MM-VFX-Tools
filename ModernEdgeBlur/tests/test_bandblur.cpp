/*  test_bandblur.cpp — proves the Stage 5 band-bounding-box-limited Detail
    low-pass is BYTE-IDENTICAL to the full-frame Gaussian for every pixel the
    composite actually reads (blend weight w>0, i.e. 0 < d < Extend).

    The plugin (EdgeRepair.cpp) blurs only the bounding box of {0<d<=Extend}
    plus kernel margin, using MM_Gaussian's _Rect passes (which clamp source
    reads against the full image). This test reimplements BOTH paths on
    synthetic mattes and asserts zero difference inside the used band. If the
    box or its margin is ever wrong, the used-pixel diff goes nonzero here —
    that is the guardrail that keeps this optimization from silently altering
    the Gate-4-confirmed Result. Pixels OUTSIDE the used band may differ (they
    are never read); we assert nothing about them.
*/

#include "MM_Parallel.h"
#include "MM_DistanceField.h"
#include "MM_CoreMatte.h"
#include "MM_Gaussian.h"

#include <cstdio>
#include <cstdint>
#include <vector>
#include <cmath>
#include <cstring>
#include <string>

using std::vector;
static int g_fail = 0;

// Full-frame path: low[i*3+ch] = gaussianBlur(src channel) over whole image.
static void FullFrameLow(const vector<float>&argb, int W,int H,
                         const vector<float>&kern, vector<float>&low)
{
    const size_t N=(size_t)W*H;
    low.assign(N*3,0.f);
    vector<float> plane(N), tmp(N);
    for(int ch=0;ch<3;++ch){
        for(size_t i=0;i<N;++i) plane[i]=argb[i*4+1+ch];
        mm::GaussianBlurH(plane.data(),tmp.data(),W,H,kern);
        mm::GaussianBlurV(tmp.data(),plane.data(),W,H,kern);
        for(size_t i=0;i<N;++i) low[i*3+ch]=plane[i];
    }
}

// Band-box path: mirrors EdgeRepair.cpp's Stage 5 ring-limited blur exactly.
static void BandBoxLow(const vector<float>&argb, const vector<float>&dist,
                       float extend, int W,int H,
                       const vector<float>&kern, vector<float>&low)
{
    const size_t N=(size_t)W*H;
    low.assign(N*3, -999.f);   // poison: any UNWRITTEN pixel we read → visible
    const int half=(int)kern.size()/2;
    vector<float> plane(N), tmp(N);

    int bx0=W,by0=H,bx1=-1,by1=-1;
    for(int y=0;y<H;++y)for(int x=0;x<W;++x){
        float d=dist[(size_t)y*W+x];
        if(d>0.f && d<=extend){ if(x<bx0)bx0=x; if(x>bx1)bx1=x; if(y<by0)by0=y; if(y>by1)by1=y; }
    }
    if(bx1<bx0||by1<by0) return;
    const int box_x0=bx0,box_x1=bx1+1,box_y0=by0,box_y1=by1+1;
    const int h_y0=box_y0-half,h_y1=box_y1+half;
    int fx0=box_x0-half,fx1=box_x1+half,fy0=h_y0,fy1=h_y1;
    if(fx0<0)fx0=0; if(fy0<0)fy0=0; if(fx1>W)fx1=W; if(fy1>H)fy1=H;
    for(int ch=0;ch<3;++ch){
        for(int y=fy0;y<fy1;++y)for(int x=fx0;x<fx1;++x)
            plane[(size_t)y*W+x]=argb[((size_t)y*W+x)*4+1+ch];
        mm::GaussianBlurH_Rect(plane.data(),tmp.data(),W,H,kern,box_x0,box_x1,h_y0,h_y1);
        mm::GaussianBlurV_Rect(tmp.data(),plane.data(),W,H,kern,box_x0,box_x1,box_y0,box_y1);
        for(int y=box_y0;y<box_y1;++y)for(int x=box_x0;x<box_x1;++x)
            low[((size_t)y*W+x)*3+ch]=plane[(size_t)y*W+x];
    }
}

static void CheckShape(const char*name, const vector<float>&alpha,
                       const vector<float>&argb, int W,int H,
                       float erode, float extend)
{
    const size_t N=(size_t)W*H;
    vector<uint8_t> core(N); vector<float> dist(N);
    mm::BuildCoreMatte(alpha.data(),core.data(),W,H,0.5f,erode);
    bool any=mm::CoreDistanceField(core.data(),dist.data(),W,H);

    const auto kern=mm::BuildGaussianKernel(4.0f);
    vector<float> lowFull, lowBox;
    FullFrameLow(argb,W,H,kern,lowFull);
    BandBoxLow(argb,dist,extend,W,H,kern,lowBox);

    // Compare ONLY where the composite reads low: w>0 ⟺ 0<d<extend.
    double worst=0; size_t nUsed=0, diffs=0;
    for(size_t i=0;i<N;++i){
        float d=any?dist[i]:1e20f;
        if(!(d>0.f && d<extend)) continue;   // matches BlendWeight's w>0
        ++nUsed;
        for(int c=0;c<3;++c){
            if(memcmp(&lowFull[i*3+c],&lowBox[i*3+c],sizeof(float))!=0){
                ++diffs;
                double dd=std::fabs((double)lowFull[i*3+c]-(double)lowBox[i*3+c]);
                if(dd>worst)worst=dd;
            }
        }
    }
    bool ok=(diffs==0);
    printf("  [%-16s] used=%6zu  differing=%zu  worst|diff|=%.3g  %s\n",
           name,nUsed,diffs,worst, ok?"OK (byte-identical in band)":"FAIL");
    if(!ok)++g_fail;
}

static void MakeDisc(vector<float>&alpha,vector<float>&argb,int W,int H,
                     double cx,double cy,double r,double soft)
{
    const size_t N=(size_t)W*H; alpha.assign(N,0.f); argb.assign(N*4,0.f);
    uint32_t st=999u; auto rnd=[&]{st=st*1664525u+1013904223u;return (st>>9)*(1.0/8388608.0);};
    for(int y=0;y<H;++y)for(int x=0;x<W;++x){
        double dx=x-cx,dy=y-cy,d=std::sqrt(dx*dx+dy*dy);
        float a=(float)(1.0-(d-r)/soft); if(a<0)a=0;if(a>1)a=1;
        size_t i=(size_t)y*W+x; alpha[i]=a;
        argb[i*4+0]=a;
        for(int c=0;c<3;++c) argb[i*4+1+c]=0.3f+0.2f*c+(float)(rnd()-0.5)*0.05f;
    }
}

int main()
{
    printf("Band-box low-pass vs full-frame low-pass (in-band byte-identity)\n");

    // Various shapes / positions / Extend values — the box + margin must be
    // right for all of them.
    { vector<float> a,g; MakeDisc(a,g,320,240,160,120,70,3);  CheckShape("centered disc",a,g,320,240,2.f,32.f); }
    { vector<float> a,g; MakeDisc(a,g,320,240,60,50,30,3);    CheckShape("corner small",a,g,320,240,2.f,32.f); }
    { vector<float> a,g; MakeDisc(a,g,320,240,160,120,70,3);  CheckShape("big extend",a,g,320,240,2.f,80.f); }
    { vector<float> a,g; MakeDisc(a,g,200,200,100,100,95,2);  CheckShape("touches edge",a,g,200,200,2.f,32.f); }
    { vector<float> a,g; MakeDisc(a,g,320,240,160,120,70,3);  CheckShape("erode 0",a,g,320,240,0.f,32.f); }
    { vector<float> a,g; MakeDisc(a,g,320,240,160,120,70,3);  CheckShape("small extend",a,g,320,240,2.f,6.f); }
    // Full-frame subject (band is a big fraction): box ~ whole frame, still must match.
    { vector<float> a,g; MakeDisc(a,g,256,256,128,128,110,2); CheckShape("fills frame",a,g,256,256,2.f,40.f); }

    printf(g_fail?"\nRESULT: %d FAILED\n":"\nRESULT: all passed\n",g_fail);
    return g_fail?1:0;
}
