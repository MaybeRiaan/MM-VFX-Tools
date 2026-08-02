/*  gpu_parity_test.cpp — GPU kernel math validated WITHOUT a GPU.

    The .kernel bodies in src/gpu/kernels/ are platform-free by rule
    (../docs/gpu-kernel-rules.md §0), which has a useful consequence: their
    arithmetic can be transcribed into plain C and diffed against the CPU
    reference on this machine, with no DX12 device, no AE host, and no Mac.

    That is what this file does. It catches ALGORITHM error now, so that when
    the DX12 host lands, any remaining mismatch is unambiguously PLUMBING —
    and when Metal is written later, the math is already known-good.

    IMPORTANT: the emulations below must stay a faithful transcription of the
    .kernel bodies. If you change a kernel, change it here. The whole value of
    this file is that it is the same arithmetic, not a re-derivation. It links
    the REAL src/blur.cpp and the REAL src/fs_core.h as the reference side.
*/

#define NOMINMAX
#include "blur.h"
#include "fs_core.h"
#include "guided.h"
#include "matte.h"
#include "gpu/mm_gpu_host.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int g_pass = 0, g_fail = 0;

static void check(bool ok, const char *name, double detail)
{
    if (ok) { ++g_pass; std::printf("  PASS  %s (max-abs %.3e)\n", name, detail); }
    else    { ++g_fail; std::printf("  FAIL  %s (max-abs %.3e)\n", name, detail); }
}

// Deterministic pseudo-random fill — same data every run so a failure is
// reproducible. Alpha deliberately includes exact 0 and near-0 values to
// exercise the coverage-epsilon branches.
static void FillRandom(std::vector<float> &buf, unsigned seed, bool someZeroAlpha)
{
    unsigned s = seed;
    auto next = [&s]() { s = s * 1664525u + 1013904223u; return (float)((s >> 8) & 0xFFFF) / 65535.0f; };
    for (size_t i = 0; i < buf.size(); i += 4) {
        float a = next();
        if (someZeroAlpha && (i / 4) % 7 == 0) a = 0.0f;
        buf[i + 0] = a;
        // Premultiplied: colour never exceeds alpha.
        buf[i + 1] = next() * a;
        buf[i + 2] = next() * a;
        buf[i + 3] = next() * a;
    }
}

// ---------------------------------------------------------------------------
// Transcription of fs_gauss.kernel. One invocation == one GPU thread.
// Note the tap-range narrowing (i0/i1): SKIP out-of-range, never clamp.
// ---------------------------------------------------------------------------
static void EmulateGaussPass(const float *src, const float *kern, float *dst,
                             int W, int H, int R, int horizontal)
{
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const int oo = (y * W + x) * 4;
            if (R == 0) {
                dst[oo+0]=src[oo+0]; dst[oo+1]=src[oo+1];
                dst[oo+2]=src[oo+2]; dst[oo+3]=src[oo+3];
                continue;
            }
            int i0, i1;
            if (horizontal) {
                i0 = (x - R < 0)     ? -x         : -R;
                i1 = (x + R > W - 1) ? (W - 1 - x) :  R;
            } else {
                i0 = (y - R < 0)     ? -y         : -R;
                i1 = (y + R > H - 1) ? (H - 1 - y) :  R;
            }
            float a=0.f, r=0.f, g=0.f, b=0.f;
            for (int i = i0; i <= i1; ++i) {
                const float w = kern[i + R];
                const int o = horizontal ? ((y * W + (x + i)) * 4)
                                         : (((y + i) * W + x) * 4);
                a += w * src[o+0]; r += w * src[o+1];
                g += w * src[o+2]; b += w * src[o+3];
            }
            dst[oo+0]=a; dst[oo+1]=r; dst[oo+2]=g; dst[oo+3]=b;
        }
    }
}

// Rebuilds the weights the way blur.cpp does — in DOUBLE, then cast to float
// (rules §3.1). This is what the host will upload; the GPU never rebuilds them.
static void BuildKernelLikeCPU(double sigma, int R, std::vector<float> &k)
{
    k.resize((size_t)(2 * R + 1));
    double sum = 0.0;
    const double inv2s2 = 1.0 / (2.0 * sigma * sigma);
    for (int i = -R; i <= R; ++i) {
        const double w = std::exp(-(double)(i * i) * inv2s2);
        k[(size_t)(i + R)] = (float)w;
        sum += w;
    }
    const float inv = (float)(1.0 / sum);
    for (float &w : k) w *= inv;
}

// ---------------------------------------------------------------------------
// Transcription of fs_compose.kernel (Result view, no offset, matte == 1).
// ---------------------------------------------------------------------------
static void EmulateComposeResult(const float *src, const float *lo, float *dst,
                                 int W, int H, float texGain, float mix)
{
    const float EPS = 1e-8f;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const int o = (y * W + x) * 4;
            const float a = src[o+0], la = lo[o+0];
            float s0,s1,s2, l0,l1,l2;
            if (a > EPS) { const float inva=1.f/a;
                s0=src[o+1]*inva; s1=src[o+2]*inva; s2=src[o+3]*inva; }
            else { s0=s1=s2=0.f; }
            if (la > EPS) { const float invla=1.f/la;
                l0=lo[o+1]*invla; l1=lo[o+2]*invla; l2=lo[o+3]*invla; }
            else { l0=s0; l1=s1; l2=s2; }

            if (a > EPS) {
                const float m = 1.0f * mix;
                const float h0=s0-l0, h1=s1-l1, h2=s2-l2;
                const float p0=l0+texGain*h0, p1=l1+texGain*h1, p2=l2+texGain*h2;
                const float o0=s0+(p0-s0)*m, o1=s1+(p1-s1)*m, o2=s2+(p2-s2)*m;
                dst[o+0]=a; dst[o+1]=o0*a; dst[o+2]=o1*a; dst[o+3]=o2*a;
            } else {
                dst[o+0]=a; dst[o+1]=src[o+1]; dst[o+2]=src[o+2]; dst[o+3]=src[o+3];
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Transcription of fs_guided.kernel. Five stages, dispatched in guided.cpp's
// order. NOTE the deliberate divergence: the box sum here is a DIRECT per-pixel
// window sum (GPU-shaped, fully parallel), not the CPU's serial sliding window
// — accumulated in double, which measurement shows is bit-exact against it
// while a float accumulator is not. See the header comment in fs_guided.kernel.
// ---------------------------------------------------------------------------
static void EmulateGuidedBox(const float *src, float *dst,
                             int W, int H, int R, int horizontal)
{
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            double a0=0, a1=0, a2=0, a3=0;      // ACCUM_T
            int i0, i1;
            if (horizontal) {
                i0 = (x - R < 0)     ? -x         : -R;
                i1 = (x + R > W - 1) ? (W - 1 - x) :  R;
            } else {
                i0 = (y - R < 0)     ? -y         : -R;
                i1 = (y + R > H - 1) ? (H - 1 - y) :  R;
            }
            for (int i = i0; i <= i1; ++i) {
                const int o = horizontal ? ((y * W + (x + i)) * 4)
                                         : (((y + i) * W + x) * 4);
                a0 += src[o+0]; a1 += src[o+1]; a2 += src[o+2]; a3 += src[o+3];
            }
            const int o = (y * W + x) * 4;
            dst[o+0]=(float)a0; dst[o+1]=(float)a1;
            dst[o+2]=(float)a2; dst[o+3]=(float)a3;
        }
    }
}

static void EmulateGuidedBoxSeparable(const float *src, float *tmp, float *dst,
                                      int W, int H, int rx, int ry)
{
    EmulateGuidedBox(src, tmp, W, H, rx, 1);
    EmulateGuidedBox(tmp, dst, W, H, ry, 0);
}

static void EmulateGuided(const float *src, float *dst, int W, int H,
                          int rx, int ry, float eps)
{
    const size_t N4 = (size_t)W * H * 4;
    const float WEPS = 1e-6f, AEPS = 1e-8f;

    if (rx == 0 && ry == 0) {
        for (size_t i = 0; i < N4; ++i) dst[i] = src[i];
        return;
    }
    if (eps < 1e-6f) eps = 1e-6f;

    std::vector<float> s1(N4), s2(N4), sa(N4), sb(N4), tmp(N4);

    // stage 1: box(src) -> s1
    EmulateGuidedBoxSeparable(src, tmp.data(), s1.data(), W, H, rx, ry);

    // stage 2: squares -> s2
    for (int i = 0; i < W * H; ++i) {
        const int o = i * 4;
        const float a = src[o+0];
        s2[o+0] = a;
        if (a > AEPS) { const float inva = 1.f/a;
            s2[o+1]=src[o+1]*src[o+1]*inva;
            s2[o+2]=src[o+2]*src[o+2]*inva;
            s2[o+3]=src[o+3]*src[o+3]*inva;
        } else { s2[o+1]=s2[o+2]=s2[o+3]=0.f; }
    }
    EmulateGuidedBoxSeparable(s2.data(), tmp.data(), s2.data(), W, H, rx, ry);

    // stage 3: coefficients -> sa/sb
    for (int i = 0; i < W * H; ++i) {
        const int o = i * 4;
        const float a = src[o+0], N = s1[o+0];
        if (N > WEPS) {
            const float invN = 1.f/N;
            sa[o+0]=a; sb[o+0]=a;
            for (int c = 1; c < 4; ++c) {
                const float mean = s1[o+c]*invN;
                float var = s2[o+c]*invN - mean*mean;
                if (var < 0.f) var = 0.f;
                const float A = var/(var+eps);
                sa[o+c] = a*A;
                sb[o+c] = a*(mean*(1.f-A));
            }
        } else {
            for (int c = 0; c < 4; ++c) { sa[o+c]=0.f; sb[o+c]=0.f; }
        }
    }
    EmulateGuidedBoxSeparable(sa.data(), tmp.data(), sa.data(), W, H, rx, ry);
    EmulateGuidedBoxSeparable(sb.data(), tmp.data(), sb.data(), W, H, rx, ry);

    // stage 4: final
    const float invCount = 1.f / (float)((2*rx+1)*(2*ry+1));
    for (int i = 0; i < W * H; ++i) {
        const int o = i * 4;
        const float N2 = sa[o+0];
        if (N2 > WEPS) {
            const float invN2 = 1.f/N2;
            const float abar = N2*invCount;
            const float a = src[o+0];
            const float inva = (a > AEPS) ? 1.f/a : 0.f;
            dst[o+0] = abar;
            for (int c = 1; c < 4; ++c) {
                const float p = src[o+c]*inva;
                const float q = sa[o+c]*invN2*p + sb[o+c]*invN2;
                dst[o+c] = q*abar;
            }
        } else {
            for (int c = 0; c < 4; ++c) dst[o+c] = 0.f;
        }
    }
}

static double MaxAbsDiff(const std::vector<float> &a, const std::vector<float> &b)
{
    double m = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double d = std::fabs((double)a[i] - (double)b[i]);
        if (d > m) m = d;
    }
    return m;
}

// ===========================================================================
// Phase 5 host-side additions: fs_tile / fs_matte / full fs_compose
// transcriptions, plus the tiled pipeline driven by the REAL tile planner
// (src/gpu/mm_gpu_host.h — code under test, not a transcription).
// ===========================================================================

// ---------------------------------------------------------------------------
// Transcription of fs_tile.kernel. AE GPU worlds are BGRA float with a row
// pitch in floats; tiles are packed ARGB. The swizzle is index reversal.
// ---------------------------------------------------------------------------
static void EmulateLoadTile(const float *world, int pitchFl, int worldY0,
                            float *tile, int W, int H)
{
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const int t = (y * W + x) * 4;
        const int w = (worldY0 + y) * pitchFl + x * 4;
        tile[t + 0] = world[w + 3];
        tile[t + 1] = world[w + 2];
        tile[t + 2] = world[w + 1];
        tile[t + 3] = world[w + 0];
    }
}

static void EmulateStoreTile(const float *tile, int W, int H,
                             float *world, int pitchFl, int worldY0)
{
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const int t = (y * W + x) * 4;
        const int w = (worldY0 + y) * pitchFl + x * 4;
        world[w + 0] = tile[t + 3];
        world[w + 1] = tile[t + 2];
        world[w + 2] = tile[t + 1];
        world[w + 3] = tile[t + 0];
    }
}

static void EmulateFillOnes(float *plane, int W, int H)
{
    for (int i = 0; i < W * H; ++i) plane[i] = 1.0f;
}

// Reverse of EmulateLoadTile, used to BUILD a fake AE world from an ARGB
// plane. Slack floats beyond each row's W*4 get a sentinel so a pitch bug
// reads garbage loudly instead of zeros quietly.
static void MakeBGRAWorld(const std::vector<float> &argb, int W, int H,
                          int pitchFl, std::vector<float> &world)
{
    world.assign((size_t)H * pitchFl, 777.0f);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const int t = (y * W + x) * 4;
        const int w = y * pitchFl + x * 4;
        world[w + 0] = argb[t + 3];
        world[w + 1] = argb[t + 2];
        world[w + 2] = argb[t + 1];
        world[w + 3] = argb[t + 0];
    }
}

// ---------------------------------------------------------------------------
// FULL transcription of fs_compose.kernel: origin offsets, all four View
// modes, matte plane. (EmulateComposeResult above predates this and covers
// the offset-free Result case; kept as-is.)
// ---------------------------------------------------------------------------
static void EmulateComposeKernel(const float *src, const float *lo, const float *matte,
                                 float *dst, int outW, int outH,
                                 int srcW, int srcOX, int srcOY,
                                 int view, float texGain, float mix)
{
    const float EPS = 1e-8f;
    for (int oy = 0; oy < outH; ++oy) for (int ox = 0; ox < outW; ++ox) {
        const int so = ((oy + srcOY) * srcW + (ox + srcOX)) * 4;
        const int oo = (oy * outW + ox) * 4;

        const float a  = src[so + 0];
        const float la = lo [so + 0];

        float s0, s1, s2;
        if (a > EPS) { const float inva = 1.0f / a;
            s0 = src[so+1]*inva; s1 = src[so+2]*inva; s2 = src[so+3]*inva; }
        else { s0 = 0.f; s1 = 0.f; s2 = 0.f; }

        float l0, l1, l2;
        if (la > EPS) { const float invla = 1.0f / la;
            l0 = lo[so+1]*invla; l1 = lo[so+2]*invla; l2 = lo[so+3]*invla; }
        else { l0 = s0; l1 = s1; l2 = s2; }

        const float matteW = matte[oy * outW + ox];

        if (view == 1) {
            dst[oo+0] = a;
            dst[oo+1] = l0 * a; dst[oo+2] = l1 * a; dst[oo+3] = l2 * a;
        } else if (view == 2) {
            dst[oo+0] = 1.0f;
            dst[oo+1] = texGain * (s0 - l0) + 0.5f;
            dst[oo+2] = texGain * (s1 - l1) + 0.5f;
            dst[oo+3] = texGain * (s2 - l2) + 0.5f;
        } else if (view == 3) {
            dst[oo+0] = 1.0f;
            dst[oo+1] = matteW; dst[oo+2] = matteW; dst[oo+3] = matteW;
        } else {
            if (a > EPS) {
                const float m = matteW * mix;
                const float h0=s0-l0, h1=s1-l1, h2=s2-l2;
                const float p0=l0+texGain*h0, p1=l1+texGain*h1, p2=l2+texGain*h2;
                const float o0=s0+(p0-s0)*m, o1=s1+(p1-s1)*m, o2=s2+(p2-s2)*m;
                dst[oo+0]=a; dst[oo+1]=o0*a; dst[oo+2]=o1*a; dst[oo+3]=o2*a;
            } else {
                dst[oo+0]=a; dst[oo+1]=src[so+1]; dst[oo+2]=src[so+2]; dst[oo+3]=src[so+3];
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Reference compose — the CPU side, mirroring smart_render.cpp's
// FS_ComposeRow exactly (inva formulation, fs::RecombineChannel /
// fs::HighBandDisplay, fs::SampleMatte). viewL is the 1-BASED popup value.
// ---------------------------------------------------------------------------
static void RefCompose(const float *src, const float *lo,
                       int srcW, int srcOX, int srcOY,
                       const fs::FS_MatteField &mf, int viewL,
                       float g, float mix,
                       float *dst, int outW, int outH)
{
    const float EPS = 1e-8f;
    for (int oy = 0; oy < outH; ++oy) {
        const size_t off = ((size_t)(oy + srcOY) * srcW + srcOX) * 4;
        const float *s = src + off;
        const float *l = lo  + off;
        float *d = dst + (size_t)oy * outW * 4;
        for (int ox = 0; ox < outW; ++ox, s += 4, l += 4, d += 4) {
            const float a  = s[0];
            const float la = l[0];
            float ss[3], ll[3];
            if (a > EPS) { const float inva = 1.f/a;
                ss[0]=s[1]*inva; ss[1]=s[2]*inva; ss[2]=s[3]*inva; }
            else { ss[0]=ss[1]=ss[2]=0.f; }
            if (la > EPS) { const float invla = 1.f/la;
                ll[0]=l[1]*invla; ll[1]=l[2]*invla; ll[2]=l[3]*invla; }
            else { ll[0]=ss[0]; ll[1]=ss[1]; ll[2]=ss[2]; }

            const float matteW = fs::SampleMatte(mf, ox, oy);

            switch (viewL) {
                case 2:   // Low Band
                    d[0]=a; d[1]=ll[0]*a; d[2]=ll[1]*a; d[3]=ll[2]*a;
                    break;
                case 3:   // High Band
                    d[0]=1.f;
                    d[1]=fs::HighBandDisplay(ss[0], ll[0], g);
                    d[2]=fs::HighBandDisplay(ss[1], ll[1], g);
                    d[3]=fs::HighBandDisplay(ss[2], ll[2], g);
                    break;
                case 4:   // Matte
                    d[0]=1.f; d[1]=matteW; d[2]=matteW; d[3]=matteW;
                    break;
                default:  // Result
                    if (a > EPS) {
                        const float m = matteW * mix;
                        d[0]=a;
                        d[1]=fs::RecombineChannel(ss[0], ll[0], g, m)*a;
                        d[2]=fs::RecombineChannel(ss[1], ll[1], g, m)*a;
                        d[3]=fs::RecombineChannel(ss[2], ll[2], g, m)*a;
                    } else {
                        d[0]=a; d[1]=s[1]; d[2]=s[2]; d[3]=s[3];
                    }
                    break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Transcription of fs_matte.kernel — reads the matte's BGRA pitched world,
// writes one band of the single-float weight plane.
// ---------------------------------------------------------------------------
struct MtParams {
    int   mtW, mtH, mtPitch;
    int   mOriginX, mOriginY, mRefW, mRefH;
    int   outOriginX, outOriginY, srcRefW, srcRefH;
    float dsx, dsy;
    int   channel, fit;
};

static void EmulateMatteBand(const float *world, const MtParams &mp,
                             int outW, int bandH, int outY0, float *dst)
{
    for (int by = 0; by < bandH; ++by) for (int ox = 0; ox < outW; ++ox) {
        const int oy = by + outY0;

        const float invdsx = (mp.dsx > 0.0f) ? 1.0f / mp.dsx : 1.0f;
        const float invdsy = (mp.dsy > 0.0f) ? 1.0f / mp.dsy : 1.0f;
        const float lx = ((float)ox + 0.5f + (float)mp.outOriginX) * invdsx;
        const float ly = ((float)oy + 0.5f + (float)mp.outOriginY) * invdsy;

        float mlx, mly;
        if (mp.fit == 2) {          // Center
            const float offx = ((float)mp.srcRefW - (float)mp.mRefW) * 0.5f;
            const float offy = ((float)mp.srcRefH - (float)mp.mRefH) * 0.5f;
            mlx = lx - offx; mly = ly - offy;
        } else if (mp.fit == 3) {   // Top-Left
            mlx = lx; mly = ly;
        } else {                    // Fit to Layer
            const float u = lx / (float)(mp.srcRefW > 0 ? mp.srcRefW : 1);
            const float v = ly / (float)(mp.srcRefH > 0 ? mp.srcRefH : 1);
            mlx = u * (float)mp.mRefW;
            mly = v * (float)mp.mRefH;
        }

        const float mx = mlx * mp.dsx - (float)mp.mOriginX;
        const float my = mly * mp.dsy - (float)mp.mOriginY;

        const float fx = mx - 0.5f;
        const float fy = my - 0.5f;
        const int   x0 = (int)std::floor(fx);
        const int   y0 = (int)std::floor(fy);
        const float tx = fx - (float)x0;
        const float ty = fy - (float)y0;

        const int clampEdge = (mp.fit == 1) ? 1 : 0;

        float a = 0.f, r = 0.f, g = 0.f, b = 0.f;
        for (int j = 0; j < 2; ++j) {
            int yy = y0 + j;
            const float wyj = (j == 0) ? (1.0f - ty) : ty;
            if (clampEdge) { yy = yy < 0 ? 0 : (yy >= mp.mtH ? mp.mtH - 1 : yy); }
            else if (yy < 0 || yy >= mp.mtH) continue;
            for (int i = 0; i < 2; ++i) {
                int xx = x0 + i;
                const float wxi = (i == 0) ? (1.0f - tx) : tx;
                if (clampEdge) { xx = xx < 0 ? 0 : (xx >= mp.mtW ? mp.mtW - 1 : xx); }
                else if (xx < 0 || xx >= mp.mtW) continue;
                const float w = wxi * wyj;
                const int   p = yy * mp.mtPitch + xx * 4;
                a += w * world[p + 3];
                r += w * world[p + 2];
                g += w * world[p + 1];
                b += w * world[p + 0];
            }
        }

        const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        float wgt;
        if      (mp.channel == 2) wgt = 1.0f - a;
        else if (mp.channel == 3) wgt = luma * a;
        else if (mp.channel == 4) wgt = (1.0f - luma) * a;
        else                      wgt = a;

        dst[by * outW + ox] = wgt;
    }
}

// ---------------------------------------------------------------------------
// The TILED pipeline, exactly as mm_gpu_dx.cpp schedules it, driven by the
// REAL tile planner: per band — load (swizzle) → separation → smooth →
// matte → compose → store. This is the end-to-end proof that band margins,
// clipping and the compose offset are right: its output must equal the
// UNTILED reference below on every float.
// ---------------------------------------------------------------------------
static void RunTiledPipeline(
    const std::vector<float> &srcWorld, int srcPitch,
    int srcW, int outW, int srcOX,
    bool guided, double sepSigma, int grx, int gry, float eps,
    double smSigma,
    int viewKernel, float g, float mix,
    const float *matteWorld, const MtParams *mp,
    const fsgpu::FS_TilePlan &plan,
    std::vector<float> &outWorld, int outPitch)
{
    const int R  = (!guided) ? (int)fs::GaussianSupport(sepSigma) : 0;
    const int Rs = (int)fs::GaussianSupport(smSigma);
    std::vector<float> kSep, kSm;
    if (R > 0)  fs::BuildGaussianKernel(sepSigma, R, kSep);   // the CPU's exact weights
    if (Rs > 0) fs::BuildGaussianKernel(smSigma, Rs, kSm);

    const size_t tileN = (size_t)plan.tileRowsMax * srcW * 4;
    std::vector<float> A(tileN), T(tileN), L(tileN);
    std::vector<float> outTile((size_t)plan.bandHMax * outW * 4);
    std::vector<float> matteP((size_t)plan.bandHMax * outW);
    if (!mp) EmulateFillOnes(matteP.data(), outW, plan.bandHMax);

    for (const fsgpu::FS_TileBand &band : plan.bands) {
        const int rows = band.srcRows;
        EmulateLoadTile(srcWorld.data(), srcPitch, band.srcY0, A.data(), srcW, rows);

        const float *lo = A.data();
        if (guided && (grx > 0 || gry > 0)) {
            EmulateGuided(A.data(), L.data(), srcW, rows, grx, gry, eps);
            lo = L.data();
        } else if (R > 0) {
            EmulateGaussPass(A.data(), kSep.data(), T.data(), srcW, rows, R, 1);
            EmulateGaussPass(T.data(), kSep.data(), L.data(), srcW, rows, R, 0);
            lo = L.data();
        }
        if (Rs > 0) {
            EmulateGaussPass(lo, kSm.data(), T.data(), srcW, rows, Rs, 1);
            EmulateGaussPass(T.data(), kSm.data(), L.data(), srcW, rows, Rs, 0);
            lo = L.data();
        }

        if (mp)
            EmulateMatteBand(matteWorld, *mp, outW, band.outH, band.outY0, matteP.data());

        EmulateComposeKernel(A.data(), lo, matteP.data(), outTile.data(),
                             outW, band.outH, srcW, srcOX, band.loOY,
                             viewKernel, g, mix);
        EmulateStoreTile(outTile.data(), outW, band.outH,
                         outWorld.data(), outPitch, band.outY0);
    }
}

// The UNTILED reference: the REAL CPU implementations (blur.cpp /
// guided.cpp) on the full inflated buffer + the FS_ComposeRow-equivalent
// loop, stored to a BGRA world for a like-for-like diff. viewL is 1-based.
static void RunUntiledReference(
    const std::vector<float> &srcARGB, int srcW, int srcH,
    int outW, int outH, int srcOX, int srcOY,
    bool guided, double sepSigma, int grx, int gry, float eps,
    double smSigma,
    const fs::FS_MatteField &mf, int viewL, float g, float mix,
    std::vector<float> &outWorld, int outPitch)
{
    std::vector<float> tmp(srcARGB.size()), lo(srcARGB.size());
    if (guided && (grx > 0 || gry > 0)) {
        fs::GuidedFilterARGB(nullptr, srcARGB.data(), lo.data(), srcW, srcH, grx, gry, eps);
    } else if (!guided && fs::GaussianSupport(sepSigma) > 0) {
        fs::GaussianBlurARGB(nullptr, srcARGB.data(), tmp.data(), lo.data(),
                             srcW, srcH, sepSigma, sepSigma);
    } else {
        lo = srcARGB;
    }
    if (fs::GaussianSupport(smSigma) > 0) {
        fs::GaussianBlurARGB(nullptr, lo.data(), tmp.data(), lo.data(),
                             srcW, srcH, smSigma, smSigma);
    }
    std::vector<float> outARGB((size_t)outW * outH * 4);
    RefCompose(srcARGB.data(), lo.data(), srcW, srcOX, srcOY,
               mf, viewL, g, mix, outARGB.data(), outW, outH);
    EmulateStoreTile(outARGB.data(), outW, outH, outWorld.data(), outPitch, 0);
}

int main()
{
    std::printf("\nMM FreqSep - GPU kernel parity (CPU-emulated, no device)\n\n");

    const int W = 64, H = 48;
    const size_t N = (size_t)W * H * 4;

    std::vector<float> src(N), ref(N), tmp(N), gpu(N), gtmp(N);
    FillRandom(src, 12345u, /*someZeroAlpha*/ true);

    // --- Gaussian: emulated two-pass vs the REAL blur.cpp ------------------
    // Several sigmas, including one small enough that the buffer edge clamps
    // the tap range on most pixels (that is where skip-vs-clamp shows up).
    const double sigmas[] = { 0.8, 2.5, 7.0, 15.0 };
    for (double sigma : sigmas) {
        fs::GaussianBlurARGB(nullptr, src.data(), tmp.data(), ref.data(), W, H, sigma, sigma);

        const int R = (int)fs::GaussianSupport(sigma);
        std::vector<float> k;
        BuildKernelLikeCPU(sigma, R, k);
        EmulateGaussPass(src.data(),  k.data(), gtmp.data(), W, H, R, 1);
        EmulateGaussPass(gtmp.data(), k.data(), gpu.data(),  W, H, R, 0);

        char name[96];
        std::snprintf(name, sizeof name, "fs_gauss.kernel vs blur.cpp, sigma=%.1f", sigma);
        const double d = MaxAbsDiff(ref, gpu);
        check(d <= 1e-5, name, d);
    }

    // --- Anisotropic sigmas (non-square pixel aspect / downsample) ----------
    {
        fs::GaussianBlurARGB(nullptr, src.data(), tmp.data(), ref.data(), W, H, 6.0, 2.0);
        const int Rx = (int)fs::GaussianSupport(6.0);
        const int Ry = (int)fs::GaussianSupport(2.0);
        std::vector<float> kx, ky;
        BuildKernelLikeCPU(6.0, Rx, kx);
        BuildKernelLikeCPU(2.0, Ry, ky);
        EmulateGaussPass(src.data(),  kx.data(), gtmp.data(), W, H, Rx, 1);
        EmulateGaussPass(gtmp.data(), ky.data(), gpu.data(),  W, H, Ry, 0);
        const double d = MaxAbsDiff(ref, gpu);
        check(d <= 1e-5, "fs_gauss.kernel anisotropic sigma 6.0/2.0", d);
    }

    // --- Skip-don't-clamp: prove the edge behaviour actually differs --------
    // A control showing that clamp-sampling (the reflexive GPU choice) would
    // NOT match. If this "fails", the two behaviours have become identical and
    // the invariant is no longer being tested by the cases above.
    {
        const double sigma = 7.0;
        fs::GaussianBlurARGB(nullptr, src.data(), tmp.data(), ref.data(), W, H, sigma, sigma);
        const int R = (int)fs::GaussianSupport(sigma);
        std::vector<float> k; BuildKernelLikeCPU(sigma, R, k);

        // Deliberately WRONG variant: clamp the tap index to the edge.
        std::vector<float> bad(N), badtmp(N);
        for (int pass = 0; pass < 2; ++pass) {
            const float *in = (pass == 0) ? src.data() : badtmp.data();
            float *out      = (pass == 0) ? badtmp.data() : bad.data();
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                float a=0,r=0,g=0,b=0;
                for (int i = -R; i <= R; ++i) {
                    const float w = k[i + R];
                    int sx = x, sy = y;
                    if (pass == 0) { sx = x + i; sx = sx < 0 ? 0 : (sx > W-1 ? W-1 : sx); }
                    else           { sy = y + i; sy = sy < 0 ? 0 : (sy > H-1 ? H-1 : sy); }
                    const int o = (sy * W + sx) * 4;
                    a += w*in[o+0]; r += w*in[o+1]; g += w*in[o+2]; b += w*in[o+3];
                }
                const int oo = (y * W + x) * 4;
                out[oo+0]=a; out[oo+1]=r; out[oo+2]=g; out[oo+3]=b;
            }
        }
        const double d = MaxAbsDiff(ref, bad);
        check(d > 1e-4, "control: clamp-sampling edges DOES diverge (invariant is live)", d);
    }

    // --- Compose: emulated kernel vs fs_core.h -----------------------------
    {
        const double sigma = 5.0;
        fs::GaussianBlurARGB(nullptr, src.data(), tmp.data(), ref.data(), W, H, sigma, sigma);

        struct { float gain, mix; const char *name; } cases[] = {
            { 1.0f, 1.0f, "compose identity gain=1 mix=1 -> out==src" },
            { 1.6f, 1.0f, "compose texture gain=1.6" },
            { 0.0f, 1.0f, "compose texture gain=0 (flatten)" },
            { 2.0f, 0.35f,"compose gain=2.0 mix=0.35" },
        };

        for (auto &c : cases) {
            EmulateComposeResult(src.data(), ref.data(), gpu.data(), W, H, c.gain, c.mix);

            // Reference side: drive the REAL fs::RecombineChannel.
            std::vector<float> cref(N);
            const float EPS = 1e-8f;
            for (int i = 0; i < W * H; ++i) {
                const int o = i * 4;
                const float a = src[o+0], la = ref[o+0];
                float s[3], l[3];
                if (a > EPS) { for (int c2=0;c2<3;++c2) s[c2]=src[o+1+c2]/a; }
                else { s[0]=s[1]=s[2]=0.f; }
                if (la > EPS) { for (int c2=0;c2<3;++c2) l[c2]=ref[o+1+c2]/la; }
                else { l[0]=s[0]; l[1]=s[1]; l[2]=s[2]; }
                if (a > EPS) {
                    cref[o+0]=a;
                    for (int c2=0;c2<3;++c2)
                        cref[o+1+c2] = fs::RecombineChannel(s[c2], l[c2], c.gain, c.mix) * a;
                } else {
                    cref[o+0]=a; cref[o+1]=src[o+1]; cref[o+2]=src[o+2]; cref[o+3]=src[o+3];
                }
            }
            const double d = MaxAbsDiff(cref, gpu);
            check(d <= 1e-6, c.name, d);
        }

        // Identity, end to end on PREMULTIPLIED values. The associativity of
        // RecombineChannel makes the STRAIGHT-colour result exact at gain=1,
        // mix=1 — but compose then round-trips through straight and back
        // (s = src/a ... out = s*a), and that divide/multiply pair costs about
        // one ulp. So the bound here is ~1 ulp, NOT zero.
        //
        // This is not GPU-specific: the CPU compose loop does the identical
        // round-trip, which is why tests/identity_test.cpp asserts <= 1e-6 on
        // RecombineChannel (straight colour, pre-premultiply) rather than
        // exactness. Both paths agree; the residue is float, not algorithm.
        EmulateComposeResult(src.data(), ref.data(), gpu.data(), W, H, 1.0f, 1.0f);
        const double d = MaxAbsDiff(src, gpu);
        check(d <= 2e-7, "compose identity: out==src to ~1ulp (premult round-trip)", d);
    }

    // --- Guided filter: emulated 5-stage pipeline vs the REAL guided.cpp ----
    // Radii from GuidedRadius() at realistic sigmas, plus a large one where the
    // float-accumulator hazard would bite hardest. eps spans the slider range.
    {
        struct { double sigma; float eps; } gcases[] = {
            { 2.0,  1e-4f }, { 8.0,  1e-4f }, { 8.0, 1e-2f },
            { 8.0,  1e-6f },                        // eps floor
            { 20.0, 1e-3f },                        // large radius
        };
        for (auto &c : gcases) {
            const int r = (int)fs::GuidedRadius(c.sigma);
            std::vector<float> gref(N), ggpu(N);
            const PF_Err e = fs::GuidedFilterARGB(nullptr, src.data(), gref.data(),
                                                  W, H, r, r, c.eps);
            if (e != PF_Err_NONE) { std::printf("  FAIL  guided alloc\n"); ++g_fail; continue; }
            EmulateGuided(src.data(), ggpu.data(), W, H, r, r, c.eps);

            char name[128];
            std::snprintf(name, sizeof name,
                          "fs_guided.kernel vs guided.cpp, sigma=%.1f r=%d eps=%.0e",
                          c.sigma, r, (double)c.eps);
            const double d = MaxAbsDiff(gref, ggpu);
            check(d <= 1e-5, name, d);
        }

        // Anisotropic radii (non-square downsample).
        {
            const int rx = (int)fs::GuidedRadius(9.0), ry = (int)fs::GuidedRadius(3.0);
            std::vector<float> gref(N), ggpu(N);
            fs::GuidedFilterARGB(nullptr, src.data(), gref.data(), W, H, rx, ry, 1e-4f);
            EmulateGuided(src.data(), ggpu.data(), W, H, rx, ry, 1e-4f);
            const double d = MaxAbsDiff(gref, ggpu);
            check(d <= 1e-5, "fs_guided.kernel anisotropic radii", d);
        }

        // r == 0 must be an exact copy (identity chain, guided.cpp:215-219).
        {
            std::vector<float> gref(N), ggpu(N);
            fs::GuidedFilterARGB(nullptr, src.data(), gref.data(), W, H, 0, 0, 1e-4f);
            EmulateGuided(src.data(), ggpu.data(), W, H, 0, 0, 1e-4f);
            const double d = MaxAbsDiff(gref, ggpu);
            check(d == 0.0, "fs_guided.kernel r=0: exact copy", d);
        }

        // CONTROL: the GPU box sum diverges from the CPU's serial sliding
        // window if accumulated in FLOAT instead of ACCUM_T/double. This is why
        // the prologue routes accumulation through ACCUM_T (and why the Metal
        // prologue needs a Kahan sum — Apple GPUs have no double). If this
        // stops failing-to-match, the hazard is no longer being tested.
        {
            const int R = 25;
            std::vector<float> boxRefD(N), boxF(N), tmpb(N);
            EmulateGuidedBoxSeparable(src.data(), tmpb.data(), boxRefD.data(), W, H, R, R);

            // float-accumulator variant of the same direct sum
            auto boxFloat = [&](const float *in, float *out, int horiz) {
                for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                    float a0=0,a1=0,a2=0,a3=0;
                    int i0,i1;
                    if (horiz) { i0=(x-R<0)?-x:-R; i1=(x+R>W-1)?(W-1-x):R; }
                    else       { i0=(y-R<0)?-y:-R; i1=(y+R>H-1)?(H-1-y):R; }
                    for (int i=i0;i<=i1;++i) {
                        const int o = horiz ? ((y*W+(x+i))*4) : (((y+i)*W+x)*4);
                        a0+=in[o+0]; a1+=in[o+1]; a2+=in[o+2]; a3+=in[o+3];
                    }
                    const int o=(y*W+x)*4;
                    out[o+0]=a0; out[o+1]=a1; out[o+2]=a2; out[o+3]=a3;
                }
            };
            std::vector<float> t2(N);
            boxFloat(src.data(), t2.data(), 1);
            boxFloat(t2.data(), boxF.data(), 0);
            const double d = MaxAbsDiff(boxRefD, boxF);
            check(d > 0.0, "control: float box accumulator DOES drift (ACCUM_T earns its keep)", d);
        }
    }

    // =======================================================================
    // Phase 5 host: tile plumbing, tile planner, and the tiled pipeline
    // against the untiled CPU reference. Anything wrong in band margins,
    // clipping, the compose offset, or the BGRA swizzle fails HERE — with no
    // GPU and no AE host — so a mismatch in AE is D3D12 plumbing by
    // elimination.
    // =======================================================================

    // ---- fs_tile.kernel: swizzle + pitch + band offsets -------------------
    {
        const int TW = 64, TH = 48;
        std::vector<float> argb((size_t)TW * TH * 4);
        FillRandom(argb, 999u, true);
        std::vector<float> world;
        MakeBGRAWorld(argb, TW, TH, TW * 4 + 12, world);

        std::vector<float> tile((size_t)TW * TH * 4, -1.f);
        EmulateLoadTile(world.data(), TW * 4 + 12, 0, tile.data(), TW, TH);
        double d = MaxAbsDiff(tile, argb);

        // Partial band at a row offset.
        const int bH = 10, bY = 7;
        std::vector<float> bandBuf((size_t)TW * bH * 4, -1.f);
        EmulateLoadTile(world.data(), TW * 4 + 12, bY, bandBuf.data(), TW, bH);
        for (int y = 0; y < bH; ++y) for (int i = 0; i < TW * 4; ++i) {
            const double dd = std::fabs((double)bandBuf[(size_t)y * TW * 4 + i]
                                      - (double)argb[(size_t)(y + bY) * TW * 4 + i]);
            if (dd > d) d = dd;
        }

        // Store into a differently-pitched world, read back.
        std::vector<float> world2((size_t)TH * (TW * 4 + 20), 777.f);
        EmulateStoreTile(argb.data(), TW, TH, world2.data(), TW * 4 + 20, 0);
        std::vector<float> back((size_t)TW * TH * 4);
        EmulateLoadTile(world2.data(), TW * 4 + 20, 0, back.data(), TW, TH);
        const double dd = MaxAbsDiff(back, argb);
        if (dd > d) d = dd;

        check(d == 0.0, "fs_tile.kernel: BGRA<->ARGB round-trip, pitched worlds, band offset", d);
    }

    // ---- FS_PlanTiles: coverage/adjacency/margin/clip invariants ----------
    {
        bool ok = true;
        const struct { int outH, srcH, srcOY, margin, band; } cfgs[] = {
            { 48,  84, 18, 18, 17 },   // interior margins both sides
            { 48,  66,  0, 18, 16 },   // top margin fully clipped
            { 200, 260, 30, 30, 33 },  // tall output
            { 17,  53, 18, 18, 16 },   // single short band
            { 1,   37, 18, 18, 16 },   // one-row output
            { 500, 500, 0,  0,  90 },  // no margins at all
        };
        for (const auto &c : cfgs) {
            const size_t bt = fsgpu::FS_TileBytesPerRow(false, 100);
            const size_t bo = fsgpu::FS_OutBytesPerRow(64);
            const size_t budget = 2ull * c.margin * bt + (bt + bo) * (size_t)c.band;
            fsgpu::FS_TilePlan plan;
            if (!fsgpu::FS_PlanTiles(c.outH, c.srcH, c.srcOY, c.margin, bt, bo,
                                     budget, &plan)) { ok = false; continue; }
            int next = 0;
            for (const auto &b : plan.bands) {
                if (b.outY0 != next) ok = false;                       // adjacency, no gaps
                next = b.outY0 + b.outH;
                if (b.outH <= 0 || b.outH > plan.bandHMax) ok = false;
                if (b.srcY0 < 0 || b.srcY0 + b.srcRows > c.srcH) ok = false;
                if (b.srcRows > plan.tileRowsMax) ok = false;
                if (b.loOY < 0 || b.loOY + b.outH > b.srcRows) ok = false;
                if (b.outY0 + c.srcOY >= c.margin && b.loOY != c.margin)
                    ok = false;                                        // interior top margin intact
            }
            if (next != c.outH) ok = false;                            // full coverage
        }
        {   // budget below even the minimum band must be refused (CPU fallback)
            fsgpu::FS_TilePlan plan;
            if (fsgpu::FS_PlanTiles(48, 84, 18, 18, 1u << 20, 1u << 16, 1u << 20, &plan))
                ok = false;
        }
        check(ok, "FS_PlanTiles: coverage/adjacency/margin/clip invariants", ok ? 0.0 : 1.0);
    }

    // ---- Tiled Gaussian + smooth vs untiled reference (3 bands, HDR) ------
    {
        const int outW = 64, outH = 48;
        const double sepS = 4.0, smS = 2.0;
        const int margin = (int)fs::GaussianSupport(sepS) + (int)fs::GaussianSupport(smS);
        const int OX = margin, OY = margin;
        const int srcW = outW + 2 * OX, srcH = outH + 2 * OY;

        std::vector<float> srcARGB((size_t)srcW * srcH * 4);
        FillRandom(srcARGB, 4242u, true);
        for (size_t i = 0; i < srcARGB.size(); i += 4) {   // HDR: superwhite premult RGB
            srcARGB[i + 1] *= 2.5f; srcARGB[i + 2] *= 2.5f; srcARGB[i + 3] *= 2.5f;
        }
        const int srcPitch = srcW * 4 + 16, outPitch = outW * 4 + 8;
        std::vector<float> srcWorld;
        MakeBGRAWorld(srcARGB, srcW, srcH, srcPitch, srcWorld);

        const size_t bt = fsgpu::FS_TileBytesPerRow(false, srcW);
        const size_t bo = fsgpu::FS_OutBytesPerRow(outW);
        fsgpu::FS_TilePlan plan;
        const bool planned = fsgpu::FS_PlanTiles(outH, srcH, OY, margin, bt, bo,
            2ull * margin * bt + (bt + bo) * 17, &plan);   // → 3 bands of 16

        fs::FS_MatteField noMatte;   // inactive → SampleMatte == 1.0
        double worst = 0.0;
        for (int view = 0; view <= 2; ++view) {            // Result / Low / High
            std::vector<float> refW((size_t)outH * outPitch, 777.f);
            std::vector<float> tldW((size_t)outH * outPitch, 777.f);
            RunUntiledReference(srcARGB, srcW, srcH, outW, outH, OX, OY,
                false, sepS, 0, 0, 0.f, smS, noMatte, view + 1, 1.37f, 0.85f,
                refW, outPitch);
            RunTiledPipeline(srcWorld, srcPitch, srcW, outW, OX,
                false, sepS, 0, 0, 0.f, smS, view, 1.37f, 0.85f,
                nullptr, nullptr, plan, tldW, outPitch);
            const double d = MaxAbsDiff(refW, tldW);
            if (d > worst) worst = d;
        }
        check(planned && plan.bands.size() >= 3 && worst <= 1e-7,
              "tiled gauss+smooth == untiled reference (3 bands, HDR, views R/L/H)", worst);
    }

    // ---- Tiled Gaussian with margins clipped at the world edges -----------
    // srcOX/srcOY = 0: the layer edge sits exactly at the output's top-left,
    // so the top band has NO margin rows and tap narrowing at the tile edge
    // must reproduce the CPU's zero-extension at the same world edge.
    {
        const int outW = 64, outH = 48;
        const double sepS = 4.0;
        const int margin = (int)fs::GaussianSupport(sepS);
        const int srcW = outW + margin, srcH = outH + margin;   // right/bottom margin only

        std::vector<float> srcARGB((size_t)srcW * srcH * 4);
        FillRandom(srcARGB, 31337u, true);
        const int srcPitch = srcW * 4 + 4, outPitch = outW * 4 + 12;
        std::vector<float> srcWorld;
        MakeBGRAWorld(srcARGB, srcW, srcH, srcPitch, srcWorld);

        const size_t bt = fsgpu::FS_TileBytesPerRow(false, srcW);
        const size_t bo = fsgpu::FS_OutBytesPerRow(outW);
        fsgpu::FS_TilePlan plan;
        const bool planned = fsgpu::FS_PlanTiles(outH, srcH, 0, margin, bt, bo,
            2ull * margin * bt + (bt + bo) * 16, &plan);

        fs::FS_MatteField noMatte;
        std::vector<float> refW((size_t)outH * outPitch, 777.f);
        std::vector<float> tldW((size_t)outH * outPitch, 777.f);
        RunUntiledReference(srcARGB, srcW, srcH, outW, outH, 0, 0,
            false, sepS, 0, 0, 0.f, 0.0, noMatte, 1, 1.0f, 1.0f, refW, outPitch);
        RunTiledPipeline(srcWorld, srcPitch, srcW, outW, 0,
            false, sepS, 0, 0, 0.f, 0.0, 0, 1.0f, 1.0f,
            nullptr, nullptr, plan, tldW, outPitch);
        const double d = MaxAbsDiff(refW, tldW);
        check(planned && plan.bands.size() >= 2 && d <= 1e-7,
              "tiled gauss: margins clipped at world edges (srcOX/srcOY = 0)", d);
    }

    // ---- Capstone: tiled guided+smooth+MATTE vs untiled reference ---------
    {
        const int outW = 64, outH = 48;
        const double sepS = 6.0, smS = 1.5;
        const int r  = (int)fs::GuidedRadius(sepS);
        const int Rs = (int)fs::GaussianSupport(smS);
        const int margin = 2 * r + Rs;
        const int OX = margin, OY = margin;
        const int srcW = outW + 2 * OX, srcH = outH + 2 * OY;
        const float eps = 1e-3f;

        std::vector<float> srcARGB((size_t)srcW * srcH * 4);
        FillRandom(srcARGB, 60660u, true);
        const int srcPitch = srcW * 4 + 16, outPitch = outW * 4 + 8;
        std::vector<float> srcWorld;
        MakeBGRAWorld(srcARGB, srcW, srcH, srcPitch, srcWorld);

        // Matte: smaller layer, half-res render (ds = 0.5), luma channel, Fit.
        const int mW = 40, mH = 30, mPitch = mW * 4 + 8;
        std::vector<float> mARGB((size_t)mW * mH * 4);
        FillRandom(mARGB, 777u, true);
        std::vector<float> mWorld;
        MakeBGRAWorld(mARGB, mW, mH, mPitch, mWorld);

        MtParams mp = {};
        mp.mtW = mW; mp.mtH = mH; mp.mtPitch = mPitch;
        mp.mOriginX = 2;  mp.mOriginY = 1;
        mp.mRefW = mW * 2; mp.mRefH = mH * 2;
        mp.outOriginX = 4; mp.outOriginY = 3;
        mp.srcRefW = outW * 2; mp.srcRefH = outH * 2;
        mp.dsx = 0.5f; mp.dsy = 0.5f;
        mp.channel = 3; mp.fit = 1;

        fs::FS_MatteField mf;
        mf.data = mARGB.data(); mf.W = mW; mf.H = mH;
        mf.mOriginX = 2;  mf.mOriginY = 1;  mf.mRefW = mW * 2;  mf.mRefH = mH * 2;
        mf.outOriginX = 4; mf.outOriginY = 3;
        mf.srcRefW = outW * 2; mf.srcRefH = outH * 2;
        mf.dsx = 0.5f; mf.dsy = 0.5f;
        mf.channel = 3; mf.fit = 1; mf.active = true;

        const size_t bt = fsgpu::FS_TileBytesPerRow(true, srcW);
        const size_t bo = fsgpu::FS_OutBytesPerRow(outW);
        fsgpu::FS_TilePlan plan;
        const bool planned = fsgpu::FS_PlanTiles(outH, srcH, OY, margin, bt, bo,
            2ull * margin * bt + (bt + bo) * 16, &plan);

        double worst = 0.0;
        const int views[2] = { 0, 3 };                     // Result + Matte view
        for (int vi = 0; vi < 2; ++vi) {
            std::vector<float> refW((size_t)outH * outPitch, 777.f);
            std::vector<float> tldW((size_t)outH * outPitch, 777.f);
            RunUntiledReference(srcARGB, srcW, srcH, outW, outH, OX, OY,
                true, sepS, r, r, eps, smS, mf, views[vi] + 1, 1.2f, 0.9f,
                refW, outPitch);
            RunTiledPipeline(srcWorld, srcPitch, srcW, outW, OX,
                true, sepS, r, r, eps, smS, views[vi], 1.2f, 0.9f,
                mWorld.data(), &mp, plan, tldW, outPitch);
            const double d = MaxAbsDiff(refW, tldW);
            if (d > worst) worst = d;
        }
        check(planned && plan.bands.size() >= 3 && worst <= 1e-7,
              "tiled guided+smooth+matte == untiled reference (Result & Matte views)", worst);
    }

    // ---- fs_matte.kernel vs fs::SampleMatte: channels × fits, banded ------
    {
        const int outW = 64, outH = 48;
        const int mW = 40, mH = 30, mPitch = mW * 4 + 8;
        std::vector<float> mARGB((size_t)mW * mH * 4);
        FillRandom(mARGB, 51515u, true);
        std::vector<float> mWorld;
        MakeBGRAWorld(mARGB, mW, mH, mPitch, mWorld);

        static const char *fitNames[3] = {
            "fs_matte.kernel vs SampleMatte, Fit-to-Layer (4 channels, banded)",
            "fs_matte.kernel vs SampleMatte, Center (4 channels, banded)",
            "fs_matte.kernel vs SampleMatte, Top-Left (4 channels, banded)",
        };
        for (int fit = 1; fit <= 3; ++fit) {
            double worst = 0.0;
            for (int chan = 1; chan <= 4; ++chan) {
                MtParams mp = {};
                mp.mtW = mW; mp.mtH = mH; mp.mtPitch = mPitch;
                mp.mOriginX = 3;  mp.mOriginY = -2;
                mp.mRefW = mW * 3; mp.mRefH = mH * 3;
                mp.outOriginX = 7; mp.outOriginY = 5;
                mp.srcRefW = outW * 3; mp.srcRefH = outH * 3;
                mp.dsx = 1.f / 3.f; mp.dsy = 1.f / 3.f;
                mp.channel = chan; mp.fit = fit;

                fs::FS_MatteField mf;
                mf.data = mARGB.data(); mf.W = mW; mf.H = mH;
                mf.mOriginX = 3;  mf.mOriginY = -2;
                mf.mRefW = mW * 3; mf.mRefH = mH * 3;
                mf.outOriginX = 7; mf.outOriginY = 5;
                mf.srcRefW = outW * 3; mf.srcRefH = outH * 3;
                mf.dsx = 1.f / 3.f; mf.dsy = 1.f / 3.f;
                mf.channel = chan; mf.fit = fit; mf.active = true;

                // Kernel side in two bands (band seam must be invisible).
                std::vector<float> gpuPlane((size_t)outW * outH, -1.f);
                EmulateMatteBand(mWorld.data(), mp, outW, 24, 0,  gpuPlane.data());
                EmulateMatteBand(mWorld.data(), mp, outW, 24, 24, gpuPlane.data() + (size_t)24 * outW);

                double d = 0.0;
                for (int oy = 0; oy < outH; ++oy) for (int ox = 0; ox < outW; ++ox) {
                    const double ref = (double)fs::SampleMatte(mf, ox, oy);
                    const double dd  = std::fabs(ref - (double)gpuPlane[(size_t)oy * outW + ox]);
                    if (dd > d) d = dd;
                }
                if (d > worst) worst = d;
            }
            check(worst <= 1e-7, fitNames[fit - 1], worst);
        }
    }

    std::printf("\n  %d passed, %d failed\n\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
