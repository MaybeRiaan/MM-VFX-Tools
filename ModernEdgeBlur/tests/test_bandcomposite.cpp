/*  test_bandcomposite.cpp — end-to-end check that the band-box low-pass (with
    REUSED/stale scratch, exactly like the plugin) produces the same COMPOSITE
    output as the full-frame low-pass. This is stricter than test_bandblur:
    it runs the actual ComposeBandRGB math the viewer sees, and it deliberately
    pre-fills the reused `low` buffer with garbage from a "previous frame" to
    catch any used pixel the band box fails to overwrite (which would show as a
    fringe when Detail re-adds a stale hi = orig - low).
*/
#include "MM_Parallel.h"
#include "MM_DistanceField.h"
#include "MM_CoreMatte.h"
#include "MM_Gaussian.h"
#include "MM_Composite.h"
#include <cstdio>
#include <cstdint>
#include <vector>
#include <cmath>
#include <cstring>
using std::vector;
static int g_fail=0;

static void FullLow(const vector<float>&argb,int W,int H,const vector<float>&k,vector<float>&low){
    const size_t N=(size_t)W*H; low.assign(N*3,0.f); vector<float> pl(N),tm(N);
    for(int c=0;c<3;++c){ for(size_t i=0;i<N;++i)pl[i]=argb[i*4+1+c];
        mm::GaussianBlurH(pl.data(),tm.data(),W,H,k); mm::GaussianBlurV(tm.data(),pl.data(),W,H,k);
        for(size_t i=0;i<N;++i)low[i*3+c]=pl[i]; } }

// Mirrors EdgeRepair.cpp Stage 5 exactly, INTO a preexisting (stale) low.
static void BandLow(const vector<float>&argb,const vector<float>&dist,float ext,
                    int W,int H,const vector<float>&k,vector<float>&low){
    const int half=(int)k.size()/2; const size_t N=(size_t)W*H;
    static vector<float> plane,tmp; if(plane.size()<N)plane.resize(N); if(tmp.size()<N)tmp.resize(N);
    int bx0=W,by0=H,bx1=-1,by1=-1;
    for(int y=0;y<H;++y)for(int x=0;x<W;++x){float d=dist[(size_t)y*W+x];
        if(d>0.f&&d<=ext){if(x<bx0)bx0=x;if(x>bx1)bx1=x;if(y<by0)by0=y;if(y>by1)by1=y;}}
    if(bx1<bx0||by1<by0)return;
    const int box_x0=bx0,box_x1=bx1+1,box_y0=by0,box_y1=by1+1,h_y0=box_y0-half,h_y1=box_y1+half;
    int fx0=box_x0-half,fx1=box_x1+half,fy0=h_y0,fy1=h_y1;
    if(fx0<0)fx0=0;if(fy0<0)fy0=0;if(fx1>W)fx1=W;if(fy1>H)fy1=H;
    for(int c=0;c<3;++c){
        for(int y=fy0;y<fy1;++y)for(int x=fx0;x<fx1;++x)plane[(size_t)y*W+x]=argb[((size_t)y*W+x)*4+1+c];
        mm::GaussianBlurH_Rect(plane.data(),tmp.data(),W,H,k,box_x0,box_x1,h_y0,h_y1);
        mm::GaussianBlurV_Rect(tmp.data(),plane.data(),W,H,k,box_x0,box_x1,box_y0,box_y1);
        for(int y=box_y0;y<box_y1;++y)for(int x=box_x0;x<box_x1;++x)low[((size_t)y*W+x)*3+c]=plane[(size_t)y*W+x];
    }
}

// The composite RGB the viewer sees (straight space), from EdgeRepair.cpp.
static void Composite(const vector<float>&argb,const vector<float>&dist,const vector<float>&ext,
                      const vector<float>&low,float extend,float detail01,int W,int H,vector<float>&outRGB){
    const size_t N=(size_t)W*H; outRGB.assign(N*3,0.f);
    for(size_t i=0;i<N;++i){
        const float a=argb[i*4+0]; float d=dist[i]; float w=(d>0.f)?mm::BlendWeight(d,extend):0.f;
        if(w<=0.f){ outRGB[i*3+0]=argb[i*4+1]; outRGB[i*3+1]=argb[i*4+2]; outRGB[i*3+2]=argb[i*4+3]; continue; }
        float hi[3]; if(a>1e-6f){for(int c=0;c<3;++c)hi[c]=argb[i*4+1+c]-low[i*3+c];} else {hi[0]=hi[1]=hi[2]=0;}
        float o[3]; mm::ComposeBandRGB(&low[i*3],&ext[i*3],hi,w,detail01,o);
        outRGB[i*3+0]=o[0];outRGB[i*3+1]=o[1];outRGB[i*3+2]=o[2];
    }
}

int main(){
    printf("Band-box composite vs full-frame composite (Detail sweep, stale scratch)\n");
    const int W=320,H=240; const size_t N=(size_t)W*H;
    // disc scene
    vector<float> argb(N*4),alpha(N); double cx=170,cy=120,r=70;
    uint32_t st=7u; auto rnd=[&]{st=st*1664525u+1013904223u;return (st>>9)*(1.0/8388608.0);};
    for(int y=0;y<H;++y)for(int x=0;x<W;++x){double dx=x-cx,dy=y-cy,d=std::sqrt(dx*dx+dy*dy);
        float a=(float)(1-(d-r)/3);if(a<0)a=0;if(a>1)a=1;size_t i=(size_t)y*W+x;alpha[i]=a;argb[i*4]=a;
        for(int c=0;c<3;++c)argb[i*4+1+c]=0.3f+0.2f*c+(float)(rnd()-0.5)*0.06f;}
    vector<uint8_t> core(N); vector<float> dist(N),ext(N*3,0.5f);
    mm::BuildCoreMatte(alpha.data(),core.data(),W,H,0.5f,2.f);
    mm::CoreDistanceField(core.data(),dist.data(),W,H);
    const auto k=mm::BuildGaussianKernel(4.f); const float EXT=32.f;

    vector<float> lowFull; FullLow(argb,W,H,k,lowFull);
    // Reused low with GARBAGE from a "previous frame" — the plugin's stale scratch.
    vector<float> lowBox(N*3); for(size_t i=0;i<N*3;++i) lowBox[i]=(float)(rnd()*2.0-0.5); // includes >1 and <0
    BandLow(argb,dist,EXT,W,H,k,lowBox);

    double worstLow=0; for(size_t i=0;i<N;++i){float d=dist[i]; if(d>0&&d<EXT)
        for(int c=0;c<3;++c){double dd=std::fabs((double)lowFull[i*3+c]-(double)lowBox[i*3+c]); if(dd>worstLow)worstLow=dd;}}
    printf("  in-band low worst|diff| (with stale scratch) = %.4g  %s\n", worstLow, worstLow==0?"OK":"FAIL(stale leak)");
    if(worstLow!=0)++g_fail;

    for(float det : {0.f,0.5f,1.0f}){
        vector<float> oF,oB; Composite(argb,dist,ext,lowFull,EXT,det,W,H,oF);
        Composite(argb,dist,ext,lowBox,EXT,det,W,H,oB);
        double worst=0; for(size_t i=0;i<N*3;++i){double dd=std::fabs((double)oF[i]-(double)oB[i]); if(dd>worst)worst=dd;}
        bool ok=(worst==0.0);
        printf("  Detail=%3.0f%%  composite worst|diff| = %.4g  %s\n", det*100, worst, ok?"OK":"FAIL");
        if(!ok)++g_fail;
    }
    printf(g_fail?"\nRESULT: %d FAILED\n":"\nRESULT: all passed\n",g_fail);
    return g_fail?1:0;
}
