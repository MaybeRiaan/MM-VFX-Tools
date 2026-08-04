/*  test_gpuparity.cpp — CPU/GPU lockstep gate for Modern Edge Blur.

    WHAT THIS PROVES. The GPU kernels compute exactly what the CPU pipeline
    computes. Both sides here are the REAL shipping code: the CPU side calls
    src/core/MEB_CoreMath.h — the same functions ModernEdgeBlur.cpp's render
    callbacks call — and the GPU side calls src/gpu/MEB_KernelPasses.h — the
    same functions ModernEdgeBlurKernels.cu's entry points call, compiled here
    as plain host C++. No GPU is involved and no formula is paraphrased.

    WHY `==` AND NOT AN EPSILON. It is the same arithmetic in the same order, so
    any difference is a real defect, not rounding. An epsilon here would hide
    exactly the class of bug this file exists to catch (docs/AE_GPU_Effects.md
    §4 item 3). The looser "max-abs <= 1e-5" tolerance in the suite docs applies
    to REAL-silicon vs CPU comparisons, where fma contraction legitimately
    differs — that is a different gate, run by eye in AE.

    IT TESTS THE COMPOSITION, NOT JUST THE FORMULAS. Per-pixel helpers passing
    individually says nothing about whether the host wired them together right;
    a swapped ping-pong buffer or a pass issued in the wrong order sails
    straight through a formula-only test. So §2 below re-runs the WHOLE
    multi-pass schedule both ways — including the passthrough case, the
    sub-threshold radii that degenerate into copies, Shift Edge in both
    directions, and radii wider than the image — and diffs the finished frames.

    IT ALSO TESTS THE WORLD BOUNDARY. The CPU path reads an ARGB float world;
    the GPU path reads a BGRA one with a row pitch that is not width*4. Both
    sides are fed the same logical image through their own layout, so the
    BGRA<->ARGB swizzle and the pitch arithmetic have to be right for the frames
    to match.

    Build/run: tests/build_tests.ps1 (plain cl.exe, no AE SDK).
*/

#include "MEB_CoreMath.h"       // the CPU path's real math
#include "MEB_KernelPasses.h"   // the GPU kernels' real math

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------------

static int g_fail = 0;
static int g_checks = 0;

static void Fail(const std::string &what, const std::string &detail)
{
    ++g_fail;
    std::printf("  FAIL  %s\n        %s\n", what.c_str(), detail.c_str());
}

// Exact equality. Deliberately not an epsilon — see the file header.
static void ExpectEq(float a, float b, const std::string &what,
                     const std::string &where)
{
    ++g_checks;
    if (a == b) return;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s: cpu=%.9g gpu=%.9g (delta %.3g)",
                  where.c_str(), (double)a, (double)b, (double)(b - a));
    Fail(what, buf);
}

static void ExpectPlaneEq(const std::vector<float> &cpu,
                          const std::vector<float> &gpu,
                          const std::string &what)
{
    ++g_checks;
    if (cpu.size() != gpu.size()) {
        Fail(what, "size mismatch");
        return;
    }
    for (size_t i = 0; i < cpu.size(); ++i) {
        if (cpu[i] != gpu[i]) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "first mismatch at float %zu of %zu: cpu=%.9g gpu=%.9g "
                          "(delta %.3g)",
                          i, cpu.size(), (double)cpu[i], (double)gpu[i],
                          (double)(gpu[i] - cpu[i]));
            Fail(what, buf);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Test images — one logical ARGB image, materialised into each path's layout.
// ---------------------------------------------------------------------------

struct Img {
    int W = 0, H = 0;
    std::vector<float> px;      // interleaved A,R,G,B, tightly packed
};

// Deterministic pseudo-random, so a failure is always reproducible.
static unsigned g_rng = 12345u;
static float Rnd(float lo, float hi)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    const float u = (float)(g_rng >> 8) * (1.0f / 16777216.0f);
    return lo + (hi - lo) * u;
}

/* A soft-edged disc over a keyed-plate background: the shape that exercises
   every branch at once.
     - an antialiased alpha ramp (partial coverage, where the alpha-weighted
       composite and the unpremultiply divide both matter),
     - a band of alpha == 0 pixels that still carry leftover RGB — the
       Color-Key-plus-Choker case that caused the original black halo, and the
       only thing the a<=0 zero cliff exists for,
     - superwhite values above 1.0, which nothing in the pipeline may clamp. */
static Img MakeDisc(int W, int H)
{
    Img im; im.W = W; im.H = H; im.px.assign((size_t)W * H * 4, 0.f);
    const float cx = W * 0.5f, cy = H * 0.5f;
    const float r  = (W < H ? W : H) * 0.30f;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float dx = (float)x - cx, dy = (float)y - cy;
            const float d  = std::sqrt(dx * dx + dy * dy);
            float a = 1.f - (d - r);                 // 1px antialiased ramp
            a = a < 0.f ? 0.f : (a > 1.f ? 1.f : a);

            float *p = &im.px[((size_t)y * W + x) * 4];
            p[0] = a;
            // Leftover colour survives outside the matte, exactly as an
            // upstream keyer that only touched alpha would leave it.
            p[1] = 0.20f + 0.60f * (float)x / (float)W + (a > 0.f ? 0.f : 0.35f);
            p[2] = 0.35f + 0.40f * (float)y / (float)H;
            p[3] = 0.15f + Rnd(0.f, 0.25f);
            if (x > W / 2 && y > H / 2 && a > 0.5f) p[1] += 2.5f;   // superwhite
        }
    }
    return im;
}

// A fully opaque noise field — no edge anywhere, which is the degenerate case
// for the erosion and for the composite's denominator guard.
static Img MakeOpaqueNoise(int W, int H)
{
    Img im; im.W = W; im.H = H; im.px.assign((size_t)W * H * 4, 0.f);
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        float *p = &im.px[i * 4];
        p[0] = 1.f;
        p[1] = Rnd(0.f, 1.2f);
        p[2] = Rnd(0.f, 1.2f);
        p[3] = Rnd(0.f, 1.2f);
    }
    return im;
}

// A fully transparent field that still carries RGB — every pixel is the a<=0
// cliff, so the whole frame must come back with RGB zeroed.
static Img MakeHeldRGB(int W, int H)
{
    Img im; im.W = W; im.H = H; im.px.assign((size_t)W * H * 4, 0.f);
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        float *p = &im.px[i * 4];
        p[0] = 0.f;
        p[1] = Rnd(0.1f, 0.9f);
        p[2] = Rnd(0.1f, 0.9f);
        p[3] = Rnd(0.1f, 0.9f);
    }
    return im;
}

// ---------------------------------------------------------------------------
// World layouts.
//
// AE hands the GPU path BGRA128 worlds whose rowbytes is NOT width*16 — pad the
// test's worlds so any code that assumed otherwise fails here rather than in AE.
// ---------------------------------------------------------------------------

static const int kPadPixels = 7;   // extra pixels of row padding

static std::vector<float> ToBGRAWorld(const Img &im, int &pitchF)
{
    pitchF = (im.W + kPadPixels) * 4;
    std::vector<float> w((size_t)pitchF * im.H, -999.f);   // poison the padding
    for (int y = 0; y < im.H; ++y) {
        for (int x = 0; x < im.W; ++x) {
            const float *s = &im.px[((size_t)y * im.W + x) * 4];
            float *d = &w[(size_t)y * pitchF + (size_t)x * 4];
            d[0] = s[3]; d[1] = s[2]; d[2] = s[1]; d[3] = s[0];   // A,R,G,B -> B,G,R,A
        }
    }
    return w;
}

static Img FromBGRAWorld(const std::vector<float> &w, int pitchF, int W, int H)
{
    Img im; im.W = W; im.H = H; im.px.assign((size_t)W * H * 4, 0.f);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float *s = &w[(size_t)y * pitchF + (size_t)x * 4];
            float *d = &im.px[((size_t)y * W + x) * 4];
            d[0] = s[3]; d[1] = s[2]; d[2] = s[1]; d[3] = s[0];   // B,G,R,A -> A,R,G,B
        }
    }
    return im;
}

// ---------------------------------------------------------------------------
// Parameters — doubles, because that is what PF_CHECKOUT_PARAM delivers and
// because two of the pipeline's thresholds compare in double while the passes
// they gate compare the same value cast to float. That difference is real and
// both paths have to agree about it.
// ---------------------------------------------------------------------------

struct Prm { double shift, blur, blend; };

// ---------------------------------------------------------------------------
// §1. The CPU reference pipeline.
//
// SmartRender's pass sequence, over the real MEB_CoreMath.h functions. The
// blurs mirror GaussBlurPlane / GaussBlurRGBA's SERIAL fallbacks — the blocked
// threaded versions accumulate the same taps in the same order, so the two are
// identical by construction and the serial one is the readable twin.
// ---------------------------------------------------------------------------

static void CpuBlurPlane(std::vector<float> &buf, int W, int H,
                         const std::vector<float> &k)
{
    if (k.empty()) return;
    const int half = (int)(k.size() / 2);
    std::vector<float> tmp((size_t)(W > H ? W : H));

    for (int y = 0; y < H; ++y) {
        float *row = &buf[(size_t)y * W];
        MEB_BlurRowPlane(row, tmp.data(), W, k.data(), half);
        for (int x = 0; x < W; ++x) row[x] = tmp[x];
    }
    for (int x = 0; x < W; ++x) {
        for (int y = 0; y < H; ++y) tmp[y] = buf[(size_t)y * W + x];
        for (int y = 0; y < H; ++y) {
            float acc;
            MEB_BlurColsPlane(tmp.data(), y, H, 1, k.data(), half, &acc);
            buf[(size_t)y * W + x] = acc;
        }
    }
}

static void CpuBlurRGBA(std::vector<float> &buf, int W, int H,
                        const std::vector<float> &k, bool unpremult)
{
    if (k.empty()) return;
    const int half = (int)(k.size() / 2);
    std::vector<float> tmp((size_t)(W > H ? W : H) * 4);

    for (int y = 0; y < H; ++y) {
        float *row = &buf[(size_t)y * W * 4];
        MEB_BlurRowRGBA(row, tmp.data(), W, k.data(), half);
        for (int j = 0; j < W * 4; ++j) row[j] = tmp[j];
    }
    for (int x = 0; x < W; ++x) {
        for (int y = 0; y < H; ++y) {
            const float *p = &buf[((size_t)y * W + x) * 4];
            float *o = &tmp[(size_t)y * 4];
            o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3];
        }
        for (int y = 0; y < H; ++y) {
            float acc[4];
            MEB_BlurColsRGBA(tmp.data(), y, H, 4, k.data(), half, unpremult, acc);
            float *d = &buf[((size_t)y * W + x) * 4];
            d[0] = acc[0]; d[1] = acc[1]; d[2] = acc[2]; d[3] = acc[3];
        }
    }
}

// DirectionalMinFilter + OctagonErode, transcribed from ModernEdgeBlur.cpp's
// serial paths (both call MEB_DirMinRowInline, the shared body).
static void CpuDirMin(const std::vector<float> &in, std::vector<float> &out,
                      int W, int H, int dx, int dy, int halfLen)
{
    if (halfLen <= 0) { out = in; return; }
    for (int y = 0; y < H; ++y)
        MEB_DirMinRowInline(in.data(), &out[(size_t)y * W], W, H, y, dx, dy, halfLen);
}

static void CpuOctagonErode(const std::vector<float> &in, std::vector<float> &out,
                            int W, int H, float radius)
{
    if (radius < 0.5f) { out = in; return; }
    const int a = MEB_OctagonAxisHalf(radius);
    const int c = MEB_OctagonDiagHalf(radius);
    std::vector<float> tmp((size_t)W * H);
    CpuDirMin(in,  tmp, W, H, 1,  0, a);
    CpuDirMin(tmp, out, W, H, 0,  1, a);
    CpuDirMin(out, tmp, W, H, 1,  1, c);
    CpuDirMin(tmp, out, W, H, 1, -1, c);
}

static Img CpuPipeline(const Img &in, const Prm &prm)
{
    const int W = in.W, H = in.H;

    // Thresholds copied from SmartRender verbatim, including which comparison
    // is in double and which in float.
    const bool do_blur     = (prm.blur  >= 0.5f);
    const bool do_blend    = (prm.blend >= 0.5f);
    const bool do_shift    = (std::abs((float)prm.shift) >= 0.5f);
    const bool passthrough = !do_blur && !do_blend;

    std::vector<float> sharp = in.px;                      // ingest: unpack
    std::vector<float> blurred, alpha, weight;
    if (!passthrough) {
        blurred.assign((size_t)W * H * 4, 0.f);
        alpha.assign((size_t)W * H, 0.f);
        weight.assign((size_t)W * H, 0.f);
    }

    auto derive = [&]() {
        for (int y = 0; y < H; ++y)
            MEB_DeriveRowInline(&sharp[(size_t)y * W * 4],
                                passthrough ? nullptr : &blurred[(size_t)y * W * 4],
                                passthrough ? nullptr : &alpha[(size_t)y * W],
                                W, do_blur);
    };

    if (!do_shift) {
        derive();
    } else {
        // Phase 1: ShiftEdgeAlpha, then rebuild the derived buffers from the
        // POST-shift alpha (ModernEdgeBlur.cpp's ingest/derive split).
        std::vector<float> ain((size_t)W * H), aout((size_t)W * H);
        for (int i = 0; i < W * H; ++i) ain[i] = sharp[(size_t)i * 4];
        MorphPlane(ain.data(), aout.data(), W, H, (float)prm.shift,
                   (float)prm.shift > 0.f);
        for (int i = 0; i < W * H; ++i) sharp[(size_t)i * 4] = aout[i];
        derive();
    }

    if (passthrough) {
        Img out; out.W = W; out.H = H; out.px = sharp;
        return out;
    }

    // Phase 2
    if (do_blur)
        CpuBlurRGBA(blurred, W, H, MakeBlurKernel((float)prm.blur), true);

    // Phase 3
    CpuOctagonErode(alpha, weight, W, H, (float)prm.blend);
    const std::vector<float> bk = MakeBlendKernel((float)prm.blend);
    if (!bk.empty()) CpuBlurPlane(weight, W, H, bk);

    // Phase 4
    Img out; out.W = W; out.H = H; out.px.assign((size_t)W * H * 4, 0.f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            MEB_CompositePixel(&sharp[((size_t)y * W + x) * 4],
                               &blurred[((size_t)y * W + x) * 4],
                               weight[(size_t)y * W + x],
                               &out.px[((size_t)y * W + x) * 4]);
    return out;
}

// ---------------------------------------------------------------------------
// §2. The GPU reference pipeline.
//
// MEB_RenderCUDA's pass sequence, over the real MEB_KernelPasses.h functions —
// one call per pixel per pass, which is exactly one CUDA thread per pixel per
// dispatch. Buffers ping-pong here for the same reason they do on the device:
// a dispatch cannot read and write one plane at once.
// ---------------------------------------------------------------------------

template <typename F>
static void ForEachPixel(int W, int H, F f)
{
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            f(x, y);
}

static Img GpuPipeline(const Img &in, const Prm &prm)
{
    const int W = in.W, H = in.H;
    const int srcW = W, srcH = H;

    int srcPitchF = 0;
    const std::vector<float> srcWorld = ToBGRAWorld(in, srcPitchF);
    const int dstPitchF = (W + kPadPixels) * 4;
    std::vector<float> dstWorld((size_t)dstPitchF * H, -999.f);

    const float *src = srcWorld.data();
    float       *dst = dstWorld.data();

    const bool  do_blur     = (prm.blur  >= 0.5f);
    const bool  do_blend    = (prm.blend >= 0.5f);
    const bool  do_shift    = (std::abs((float)prm.shift) >= 0.5f);
    const bool  passthrough = !do_blur && !do_blend;
    const float blendR      = (float)prm.blend;
    const bool  erodeActive = !passthrough && (blendR >= 0.5f);

    const std::vector<float> kBlur  = do_blur     ? MakeBlurKernel((float)prm.blur)
                                                  : std::vector<float>();
    const std::vector<float> kBlend = erodeActive ? MakeBlendKernel(blendR)
                                                  : std::vector<float>();
    const int halfBlur  = kBlur.empty()  ? 0 : (int)(kBlur.size()  / 2);
    const int halfBlend = kBlend.empty() ? 0 : (int)(kBlend.size() / 2);

    const float shiftR  = std::abs((float)prm.shift);
    const int   morphRi = do_shift ? (int)std::ceil(shiftR) : 0;
    const float morphR2 = shiftR * shiftR;
    const int   dilate  = ((float)prm.shift > 0.f) ? 1 : 0;

    const int octAxis = erodeActive ? MEB_OctagonAxisHalf(blendR) : 0;
    const int octDiag = erodeActive ? MEB_OctagonDiagHalf(blendR) : 0;

    std::vector<float> alpha((size_t)W * H, 0.f);
    std::vector<float> shift((size_t)W * H, 0.f);
    std::vector<float> planeA((size_t)W * H * 4, 0.f);
    std::vector<float> planeB((size_t)W * H * 4, 0.f);
    std::vector<float> weight((size_t)W * H, 0.f);
    std::vector<float> tmp((size_t)W * H, 0.f);

    // ---- alpha plane ----
    ForEachPixel(W, H, [&](int x, int y) {
        alpha[MEB_PlaneIndex((uint)x, (uint)y, (uint)W)] =
            MEB_PassExtractAlpha(src, srcPitchF, srcW, srcH, x, y);
    });

    // ---- Phase 1: Shift Edge ----
    const float *aCur = alpha.data();
    if (do_shift) {
        ForEachPixel(W, H, [&](int x, int y) {
            shift[MEB_PlaneIndex((uint)x, (uint)y, (uint)W)] =
                MEB_PassMorph(alpha.data(), W, H, x, y, morphRi, morphR2, dilate);
        });
        aCur = shift.data();
    }

    if (passthrough) {
        ForEachPixel(W, H, [&](int x, int y) {
            MEB_StoreWorldARGB(dst, dstPitchF, x, y,
                MEB_PassSharp(src, srcPitchF, srcW, srcH, aCur, W, x, y));
        });
        return FromBGRAWorld(dstWorld, dstPitchF, W, H);
    }

    // ---- derive ----
    ForEachPixel(W, H, [&](int x, int y) {
        const MEB_RGBA v = MEB_PassDerive(src, srcPitchF, srcW, srcH,
                                          aCur, W, x, y, do_blur ? 1 : 0);
        float *p = &planeA[MEB_PlaneIndex4((uint)x, (uint)y, (uint)W)];
        p[0] = v.a; p[1] = v.r; p[2] = v.g; p[3] = v.b;
    });

    // ---- Phase 2: Blur Amount ----
    if (do_blur) {
        ForEachPixel(W, H, [&](int x, int y) {
            const MEB_RGBA v = MEB_PassBlurHRGBA(planeA.data(), W, H, x, y,
                                                 kBlur.data(), halfBlur);
            float *p = &planeB[MEB_PlaneIndex4((uint)x, (uint)y, (uint)W)];
            p[0] = v.a; p[1] = v.r; p[2] = v.g; p[3] = v.b;
        });
        ForEachPixel(W, H, [&](int x, int y) {
            const MEB_RGBA v = MEB_PassBlurVRGBA(planeB.data(), W, H, x, y,
                                                 kBlur.data(), halfBlur, 1);
            float *p = &planeA[MEB_PlaneIndex4((uint)x, (uint)y, (uint)W)];
            p[0] = v.a; p[1] = v.r; p[2] = v.g; p[3] = v.b;
        });
    }

    // ---- Phase 3: Blend Radius ----
    if (erodeActive) {
        auto dirMin = [&](const float *inP, float *outP, int dx, int dy, int halfLen) {
            if (halfLen <= 0) {
                for (int i = 0; i < W * H; ++i) outP[i] = inP[i];
                return;
            }
            ForEachPixel(W, H, [&](int x, int y) {
                outP[MEB_PlaneIndex((uint)x, (uint)y, (uint)W)] =
                    MEB_PassDirMin(inP, W, H, x, y, dx, dy, halfLen);
            });
        };
        dirMin(aCur,           tmp.data(),    1,  0, octAxis);
        dirMin(tmp.data(),     weight.data(), 0,  1, octAxis);
        dirMin(weight.data(),  tmp.data(),    1,  1, octDiag);
        dirMin(tmp.data(),     weight.data(), 1, -1, octDiag);

        if (!kBlend.empty()) {
            ForEachPixel(W, H, [&](int x, int y) {
                tmp[MEB_PlaneIndex((uint)x, (uint)y, (uint)W)] =
                    MEB_PassBlurHPlane(weight.data(), W, H, x, y,
                                       kBlend.data(), halfBlend);
            });
            ForEachPixel(W, H, [&](int x, int y) {
                weight[MEB_PlaneIndex((uint)x, (uint)y, (uint)W)] =
                    MEB_PassBlurVPlane(tmp.data(), W, H, x, y,
                                       kBlend.data(), halfBlend);
            });
        }
    } else {
        for (int i = 0; i < W * H; ++i) weight[i] = aCur[i];
    }

    // ---- Phase 4: composite ----
    ForEachPixel(W, H, [&](int x, int y) {
        MEB_StoreWorldARGB(dst, dstPitchF, x, y,
            MEB_PassComposite(src, srcPitchF, srcW, srcH,
                              aCur, planeA.data(), weight.data(), W, x, y));
    });

    return FromBGRAWorld(dstWorld, dstPitchF, W, H);
}

// ---------------------------------------------------------------------------
// §3. Per-formula checks. These localise a failure that the whole-pipeline
// diff would only report as "frame differs".
// ---------------------------------------------------------------------------

static void TestFormulas()
{
    std::printf("[formulas] per-pixel twins\n");

    for (int i = 0; i < 4000; ++i) {
        const float a  = (i % 7 == 0) ? 0.f : Rnd(-0.2f, 1.4f);
        const float r  = Rnd(-0.5f, 3.f);
        const float g  = Rnd(-0.5f, 3.f);
        const float b  = Rnd(-0.5f, 3.f);
        const bool  pm = (i & 1) != 0;

        // MEB_DeriveTexel / MEB_SharpTexel <- MEB_DeriveRowInline
        {
            float sh[4] = { a, r, g, b };
            float bl[4] = { 0, 0, 0, 0 };
            float al    = 0.f;
            MEB_DeriveRowInline(sh, bl, &al, 1, pm);

            const MEB_RGBA gs = MEB_SharpTexel(a, r, g, b);
            const MEB_RGBA gb = MEB_DeriveTexel(a, r, g, b, pm ? 1 : 0);

            ExpectEq(sh[0], gs.a, "SharpTexel", "alpha");
            ExpectEq(sh[1], gs.r, "SharpTexel", "r");
            ExpectEq(sh[2], gs.g, "SharpTexel", "g");
            ExpectEq(sh[3], gs.b, "SharpTexel", "b");
            ExpectEq(bl[0], gb.a, "DeriveTexel", "alpha");
            ExpectEq(bl[1], gb.r, "DeriveTexel", "r");
            ExpectEq(bl[2], gb.g, "DeriveTexel", "g");
            ExpectEq(bl[3], gb.b, "DeriveTexel", "b");
            ExpectEq(al,    a,    "DeriveTexel", "alpha plane");
        }

        // MEB_CompositeTexel <- MEB_CompositePixel
        {
            const float sh[4] = { a, r, g, b };
            const float bl[4] = { Rnd(-0.1f, 1.2f), Rnd(0.f, 2.f),
                                  Rnd(0.f, 2.f), Rnd(0.f, 2.f) };
            const float w = Rnd(-0.3f, 1.3f);

            float cpu[4];
            MEB_CompositePixel(sh, bl, w, cpu);
            const MEB_RGBA gpu = MEB_CompositeTexel(
                MEB_MakeRGBA(sh[0], sh[1], sh[2], sh[3]),
                MEB_MakeRGBA(bl[0], bl[1], bl[2], bl[3]), w);

            ExpectEq(cpu[0], gpu.a, "CompositeTexel", "alpha");
            ExpectEq(cpu[1], gpu.r, "CompositeTexel", "r");
            ExpectEq(cpu[2], gpu.g, "CompositeTexel", "g");
            ExpectEq(cpu[3], gpu.b, "CompositeTexel", "b");
        }

        // MEB_UnpremultTexel <- the unpremult tail of MEB_BlurColsRGBA.
        // Driven through a 1-tap kernel so the CPU side is the real code path.
        {
            const float k1[1] = { 1.f };
            float gathered[4] = { a, r, g, b };
            float cpu[4];
            MEB_BlurColsRGBA(gathered, 0, 1, 4, k1, 0, true, cpu);
            const MEB_RGBA gpu = MEB_UnpremultTexel(MEB_MakeRGBA(a, r, g, b));
            ExpectEq(cpu[0], gpu.a, "UnpremultTexel", "alpha");
            ExpectEq(cpu[1], gpu.r, "UnpremultTexel", "r");
            ExpectEq(cpu[2], gpu.g, "UnpremultTexel", "g");
            ExpectEq(cpu[3], gpu.b, "UnpremultTexel", "b");
        }
    }

    // Morph structuring element: the circle test must agree exactly, including
    // at the boundary where a sqrt-derived bound would round the other way.
    for (float r = 0.5f; r <= 12.f; r += 0.25f) {
        const int   ri = (int)std::ceil(r);
        const float r2 = r * r;
        for (int dy = -ri; dy <= ri; ++dy) {
            for (int dx = -ri; dx <= ri; ++dx) {
                const int cpuIn = ((float)(dx * dx + dy * dy) > r2) ? 0 : 1;
                ++g_checks;
                if (cpuIn != MEB_MorphInside(dx, dy, r2)) {
                    char buf[160];
                    std::snprintf(buf, sizeof(buf),
                                  "r=%.2f dx=%d dy=%d: cpu=%d gpu=%d",
                                  (double)r, dx, dy, cpuIn,
                                  MEB_MorphInside(dx, dy, r2));
                    Fail("MorphInside", buf);
                    return;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// §4. Plane-level checks — the passes whose whole point is a neighbourhood
// walk, diffed on their own before the full pipeline runs.
// ---------------------------------------------------------------------------

static void TestPlanes()
{
    std::printf("[planes]   morph / directional min / separable blur\n");

    const int W = 29, H = 23;
    std::vector<float> in((size_t)W * H);
    for (auto &v : in) v = Rnd(0.f, 1.f);

    // MorphPlane vs MEB_PassMorph, both directions, several radii.
    for (float radius : { 0.75f, 1.f, 2.5f, 4.f, 7.f }) {
        for (int dilate = 0; dilate < 2; ++dilate) {
            const float signed_r = dilate ? radius : -radius;
            std::vector<float> cpu((size_t)W * H);
            MorphPlane(in.data(), cpu.data(), W, H, signed_r, dilate != 0);

            const float r  = std::abs(signed_r);
            const int   ri = (int)std::ceil(r);
            const float r2 = r * r;
            std::vector<float> gpu((size_t)W * H);
            ForEachPixel(W, H, [&](int x, int y) {
                gpu[(size_t)y * W + x] =
                    MEB_PassMorph(in.data(), W, H, x, y, ri, r2, dilate);
            });

            char name[96];
            std::snprintf(name, sizeof(name), "MorphPlane r=%.2f %s",
                          (double)radius, dilate ? "dilate" : "erode");
            ExpectPlaneEq(cpu, gpu, name);
        }
    }

    // Directional min, all four octagon directions.
    const int dirs[4][2] = { {1,0}, {0,1}, {1,1}, {1,-1} };
    for (int d = 0; d < 4; ++d) {
        for (int halfLen : { 1, 3, 9, 40 }) {
            std::vector<float> cpu((size_t)W * H), gpu((size_t)W * H);
            CpuDirMin(in, cpu, W, H, dirs[d][0], dirs[d][1], halfLen);
            ForEachPixel(W, H, [&](int x, int y) {
                gpu[(size_t)y * W + x] = MEB_PassDirMin(in.data(), W, H, x, y,
                                                        dirs[d][0], dirs[d][1],
                                                        halfLen);
            });
            char name[96];
            std::snprintf(name, sizeof(name), "DirMin dir=(%d,%d) half=%d",
                          dirs[d][0], dirs[d][1], halfLen);
            ExpectPlaneEq(cpu, gpu, name);
        }
    }

    // Separable single-channel Gaussian, including a kernel far wider than the
    // image (where every tap clamps to an edge).
    for (float radius : { 0.5f, 1.f, 4.f, 13.f, 60.f }) {
        const std::vector<float> k = MakeBlendKernel(radius);
        if (k.empty()) continue;
        const int half = (int)(k.size() / 2);

        std::vector<float> cpu = in;
        CpuBlurPlane(cpu, W, H, k);

        std::vector<float> a = in, b((size_t)W * H);
        ForEachPixel(W, H, [&](int x, int y) {
            b[(size_t)y * W + x] = MEB_PassBlurHPlane(a.data(), W, H, x, y,
                                                      k.data(), half);
        });
        std::vector<float> gpu((size_t)W * H);
        ForEachPixel(W, H, [&](int x, int y) {
            gpu[(size_t)y * W + x] = MEB_PassBlurVPlane(b.data(), W, H, x, y,
                                                        k.data(), half);
        });

        char name[96];
        std::snprintf(name, sizeof(name), "GaussBlurPlane radius=%.1f", (double)radius);
        ExpectPlaneEq(cpu, gpu, name);
    }

    // Separable 4-channel Gaussian, with and without the unpremultiply tail.
    std::vector<float> in4((size_t)W * H * 4);
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        in4[i * 4 + 0] = Rnd(0.f, 1.f);
        in4[i * 4 + 1] = Rnd(0.f, 2.f);
        in4[i * 4 + 2] = Rnd(0.f, 2.f);
        in4[i * 4 + 3] = Rnd(0.f, 2.f);
    }
    for (float radius : { 0.5f, 3.f, 11.f, 45.f }) {
        const std::vector<float> k = MakeBlurKernel(radius);
        if (k.empty()) continue;
        const int half = (int)(k.size() / 2);

        for (int unpre = 0; unpre < 2; ++unpre) {
            std::vector<float> cpu = in4;
            CpuBlurRGBA(cpu, W, H, k, unpre != 0);

            std::vector<float> a = in4, b((size_t)W * H * 4), gpu((size_t)W * H * 4);
            ForEachPixel(W, H, [&](int x, int y) {
                const MEB_RGBA v = MEB_PassBlurHRGBA(a.data(), W, H, x, y,
                                                     k.data(), half);
                float *p = &b[((size_t)y * W + x) * 4];
                p[0] = v.a; p[1] = v.r; p[2] = v.g; p[3] = v.b;
            });
            ForEachPixel(W, H, [&](int x, int y) {
                const MEB_RGBA v = MEB_PassBlurVRGBA(b.data(), W, H, x, y,
                                                     k.data(), half, unpre);
                float *p = &gpu[((size_t)y * W + x) * 4];
                p[0] = v.a; p[1] = v.r; p[2] = v.g; p[3] = v.b;
            });

            char name[96];
            std::snprintf(name, sizeof(name), "GaussBlurRGBA radius=%.1f unpremult=%d",
                          (double)radius, unpre);
            ExpectPlaneEq(cpu, gpu, name);
        }
    }
}

// ---------------------------------------------------------------------------
// §5. The whole pipeline, both ways, over the parameter combinations that
// select different pass sequences.
// ---------------------------------------------------------------------------

static void TestPipeline()
{
    std::printf("[pipeline] full multi-pass schedule\n");

    struct Case { const char *name; Prm p; };
    const Case cases[] = {
        { "passthrough (0,0,0)",              {  0.0,   0.0,   0.0 } },
        { "blur only",                        {  0.0,   4.0,   0.0 } },
        { "blend only",                       {  0.0,   0.0,   4.0 } },
        { "blur + blend (defaults)",          {  0.0,   4.0,   4.0 } },
        { "blur + blend, larger",             {  0.0,  12.0,   7.0 } },
        { "shift grow + blur + blend",        {  3.0,   4.0,   4.0 } },
        { "shift shrink + blur + blend",      { -3.0,   4.0,   4.0 } },
        { "shift only (passthrough)",         {  2.5,   0.0,   0.0 } },
        { "sub-threshold radii",              {  0.4,   0.4,   0.4 } },
        { "blend where octagon diag rounds to 0", { 0.0, 4.0,  1.0 } },
        { "blend exactly at the 0.5 gate",    {  0.0,   4.0,   0.5 } },
        { "blur exactly at the 0.5 gate",     {  0.0,   0.5,   4.0 } },
        { "radii wider than the frame",       {  0.0, 100.0, 100.0 } },
    };

    struct Src { const char *name; Img img; };
    std::vector<Src> sources;
    sources.push_back({ "disc 41x33",        MakeDisc(41, 33) });
    sources.push_back({ "disc 8x8",          MakeDisc(8, 8) });
    sources.push_back({ "opaque noise 23x19", MakeOpaqueNoise(23, 19) });
    sources.push_back({ "held RGB 17x13",     MakeHeldRGB(17, 13) });

    for (const Src &s : sources) {
        for (const Case &c : cases) {
            const Img cpu = CpuPipeline(s.img, c.p);
            const Img gpu = GpuPipeline(s.img, c.p);
            char name[192];
            std::snprintf(name, sizeof(name), "%s / %s", s.name, c.name);
            ExpectPlaneEq(cpu.px, gpu.px, name);
        }
    }
}

// ---------------------------------------------------------------------------
// §6. Invariants the pipeline must hold regardless of what the other side
// does. These catch a change that breaks BOTH paths identically — which the
// diff above, by construction, cannot see.
// ---------------------------------------------------------------------------

static void TestInvariants()
{
    std::printf("[invariant] properties both paths must satisfy\n");

    // Passthrough must return the input, with RGB zeroed exactly where alpha
    // is <= 0 and everything else — including superwhite — untouched.
    {
        const Img in  = MakeDisc(31, 27);
        const Img out = GpuPipeline(in, { 0.0, 0.0, 0.0 });
        for (size_t i = 0; i < (size_t)in.W * in.H; ++i) {
            const float *s = &in.px[i * 4];
            const float *d = &out.px[i * 4];
            ExpectEq(s[0], d[0], "passthrough", "alpha preserved");
            if (s[0] <= 0.f) {
                ExpectEq(0.f, d[1], "passthrough", "RGB zeroed at the a<=0 cliff");
                ExpectEq(0.f, d[2], "passthrough", "RGB zeroed at the a<=0 cliff");
                ExpectEq(0.f, d[3], "passthrough", "RGB zeroed at the a<=0 cliff");
            } else {
                ExpectEq(s[1], d[1], "passthrough", "held RGB preserved");
                ExpectEq(s[2], d[2], "passthrough", "held RGB preserved");
                ExpectEq(s[3], d[3], "passthrough", "held RGB preserved");
            }
        }
    }

    // Nothing clamps intermediates: a superwhite interior must survive a
    // blur+blend render intact where the blend weight is 1 and alpha is 1.
    {
        Img in; in.W = 25; in.H = 25; in.px.assign((size_t)25 * 25 * 4, 0.f);
        for (size_t i = 0; i < (size_t)25 * 25; ++i) {
            float *p = &in.px[i * 4];
            p[0] = 1.f; p[1] = 4.25f; p[2] = 4.25f; p[3] = 4.25f;
        }
        const Img out = GpuPipeline(in, { 0.0, 4.0, 4.0 });
        const float *c = &out.px[((size_t)12 * 25 + 12) * 4];
        ++g_checks;
        if (!(c[1] > 4.0f)) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "centre R came back %.4f — superwhite was clamped",
                          (double)c[1]);
            Fail("no clamping of intermediates", buf);
        }
    }

    // Output alpha is the BLURRED alpha, never the sharp one — the scope note
    // in NEXT_STEPS.md Priority 1 that Blend Radius must not touch the matte.
    {
        const Img in   = MakeDisc(33, 33);
        const Img wide = GpuPipeline(in, { 0.0, 4.0, 20.0 });
        const Img narw = GpuPipeline(in, { 0.0, 4.0,  4.0 });
        for (size_t i = 0; i < (size_t)33 * 33; ++i)
            ExpectEq(wide.px[i * 4], narw.px[i * 4],
                     "output alpha independent of Blend Radius", "alpha");
    }
}

// ---------------------------------------------------------------------------

int main()
{
    std::printf("=== Modern Edge Blur CPU/GPU parity ===\n");
    std::printf("    CPU side: src/core/MEB_CoreMath.h\n");
    std::printf("    GPU side: src/gpu/MEB_KernelMath.h + MEB_KernelPasses.h\n");
    std::printf("    comparison: exact (==)\n\n");

    TestFormulas();
    TestPlanes();
    TestPipeline();
    TestInvariants();

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_fail);
    if (g_fail) {
        std::printf("PARITY BROKEN — the GPU path no longer matches the CPU path.\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
