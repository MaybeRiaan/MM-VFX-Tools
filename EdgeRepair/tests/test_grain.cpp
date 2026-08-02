/*  test_grain.cpp — headless unit test for ER_Grain.h and the grain-based
    ComposeBandRGB (the 2026-07-22 Detail method that replaced frequency
    separation to stop the edge darkening).

    Properties:
      - deterministic: GrainAt(x,y) is a pure function of position (stable
        across frames / threads / runs).
      - zero-mean and bounded: mean ~0 over a field, values in [-1,1).
      - decorrelated: adjacent samples are not equal / not a visible lattice
        (a cheap autocorrelation-ish check).
      - end-to-end no-darkening: over a whole synthetic band, the MEAN of the
        grain composite equals the flat lerp(orig,ext,w) — i.e. grain adds no
        directional bias (this is the whole point vs. the old hi = orig-low).
      - seamless band edge: at w->0 the composite returns orig for any grain.
*/
#include "ER_Grain.h"
#include "ER_Composite.h"
#include <cstdio>
#include <cmath>
#include <vector>
using std::vector;
static int g_fail = 0;
static void Check(bool ok, const char *name, const char *d){
    printf("  [%-20s] %s%s%s\n", name, ok?"OK":"FAIL", d[0]?"  ":"", d);
    if(!ok)++g_fail;
}

int main()
{
    printf("ER_Grain + grain composite test\n");

    // 1. Deterministic + bounded + zero-mean over a field.
    {
        const int W=256,H=256; double sum=0,sumsq=0; double mn=9,mx=-9; bool det=true;
        for(int y=0;y<H;++y)for(int x=0;x<W;++x){
            float g=er::GrainAt(x,y);
            if(g!=er::GrainAt(x,y)) det=false;          // pure function
            if(g< -1.f || g>=1.f) det=false;             // bounded [-1,1)
            sum+=g; sumsq+=(double)g*g; if(g<mn)mn=g; if(g>mx)mx=g;
        }
        const double n=(double)W*H, mean=sum/n, var=sumsq/n-mean*mean;
        char d[96]; snprintf(d,sizeof d,"mean=%.4f std=%.3f range[%.2f,%.2f]",mean,std::sqrt(var),mn,mx);
        Check(det && std::fabs(mean)<0.01, "grain-field", d);
    }

    // 2. Decorrelation: fraction of equal horizontal neighbours must be tiny
    //    (a lattice or constant would spike this).
    {
        const int W=256,H=256; long eq=0,tot=0;
        for(int y=0;y<H;++y)for(int x=0;x<W-1;++x){
            if(er::GrainAt(x,y)==er::GrainAt(x+1,y))++eq; ++tot; }
        double frac=(double)eq/tot;
        char d[64]; snprintf(d,sizeof d,"equal-neighbour frac=%.5f",frac);
        Check(frac<1e-3, "grain-decorrelated", d);
    }

    // 3. Seamless band edge: at w=0, composite == orig for any grain/detail.
    {
        const float orig[3]={0.3f,0.5f,0.2f}, ext[3]={0.9f,0.1f,0.4f};
        bool ok=true; float o[3];
        for(float g : {-1.f,-0.3f,0.f,0.6f,0.99f}){
            er::ComposeBandRGB(orig,ext,g,0.f,1.f,o);
            for(int c=0;c<3;++c) ok &= (o[c]==orig[c]);
        }
        Check(ok,"band-edge-seamless","w=0 -> orig for any grain");
    }

    // 4. NO DARKENING: mean composite over a real grain field equals the flat
    //    lerp(orig,ext,w). This is the property the old method violated (the
    //    signed edge step biased the mean darker); grain must not.
    {
        const int W=200,H=200; const float orig[3]={0.5f,0.5f,0.5f}, ext[3]={0.7f,0.3f,0.6f};
        const float w=0.5f, det=1.f, amp=0.08f;
        double sum[3]={0,0,0};
        for(int y=0;y<H;++y)for(int x=0;x<W;++x){
            float o[3]; float g=er::GrainAt(x,y)*amp;
            er::ComposeBandRGB(orig,ext,g,w,det,o);
            for(int c=0;c<3;++c)sum[c]+=o[c];
        }
        double worst=0; const double n=(double)W*H;
        for(int c=0;c<3;++c){ double base=orig[c]+(ext[c]-orig[c])*w; worst=std::fmax(worst,std::fabs(sum[c]/n-base)); }
        char d[80]; snprintf(d,sizeof d,"mean bias=%.5f (0=no darkening)",worst);
        Check(worst<1e-3,"no-darkening", d);
    }

    printf(g_fail?"\nRESULT: %d FAILED\n":"\nRESULT: all passed\n",g_fail);
    return g_fail?1:0;
}
