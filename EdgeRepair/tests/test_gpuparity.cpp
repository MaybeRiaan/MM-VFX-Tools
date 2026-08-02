/*  test_gpuparity.cpp — headless CPU/GPU lockstep test for Stage 6.

    Compiles with plain cl.exe, no AE SDK and NO GPU (via tests\build_tests.ps1).
    Exit code 0 = all pass, 1 = a failure.

    WHY THIS EXISTS: src/gpu/ER_KernelMath.h is written in the common subset of
    HLSL/CUDA/Metal/C++ precisely so the headless tests can call THE EXACT
    FUNCTIONS THE GPU RUNS. Everything here compares those functions against
    their src/common/MM_*.h twins — the CPU path being the always-correct
    reference. If a formula ever drifts on one side only, this test fails
    before AE ever sees it.

    Two classes of check:

    1. BIT-IDENTICAL — every per-pixel formula (blend weight, composite, choke,
       grain hash, core-matte decision, view-mode values, pull–push value math).
       These are the same arithmetic in the same order, so `==` is the right
       comparison, not an epsilon. Anything looser would hide a real divergence.

    2. WITHIN TOLERANCE — the distance field ONLY. The GPU replaces the CPU's
       exact Felzenszwalb–Huttenlocher EDT with a jump flood (JFA+2), accepted
       2026-07-23 as a deliberate sub-pixel approximation. This test runs the
       SAME jump-flood schedule the host issues (ER_RenderCUDA's Flood lambda)
       on the CPU and measures the error against the exact transform, so the
       "sub-pixel" claim is a measured number rather than an assertion.
*/

#include "MM_Composite.h"
#include "MM_Grain.h"
#include "MM_CoreMatte.h"
#include "MM_DistanceField.h"
#include "MM_PullPush.h"

#include "ER_KernelMath.h"      // the GPU's own math, compiled as host C++

#include <cstdio>
#include <cmath>
#include <cstdint>
#include <vector>
#include <algorithm>

static int g_fail = 0;

static void Check(bool ok, const char *name, const char *detail)
{
    printf("  [%-26s] %s%s%s\n", name, ok ? "OK" : "FAIL",
           detail[0] ? "  " : "", detail);
    if (!ok) ++g_fail;
}

// Deterministic pseudo-random floats — no <random>, so the sequence is stable
// across toolchains and the failure is always reproducible.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float unit() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }
    float range(float a, float b) { return a + (b - a) * unit(); }
};

// ---------------------------------------------------------------------------
// The jump flood, run on the CPU with the GPU's own helpers and the HOST'S OWN
// pass schedule. Keep this in step with ER_RenderCUDA's Flood lambda in
// src/EdgeRepair_GPU.cpp — that is the schedule this test is certifying.
// ---------------------------------------------------------------------------
static void JumpFlood(const std::vector<uint8_t> &seedMask,
                      std::vector<float> &dist, int W, int H)
{
    const size_t N = (size_t)W * H;
    std::vector<uint32_t> a(N), b(N);

    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            a[(size_t)y * W + x] = seedMask[(size_t)y * W + x]
                                 ? ER_PackSeed((uint32_t)x, (uint32_t)y)
                                 : ER_SEED_INVALID;

    std::vector<uint32_t> *in = &a, *out = &b;
    auto pass = [&](int step) {
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                uint32_t best = (*in)[(size_t)y * W + x];
                float bestD = ER_SeedDist2(best, x, y);
                for (int dy = -1; dy <= 1; ++dy) {
                    const int sy = y + dy * step;
                    if (sy < 0 || sy >= H) continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int sx = x + dx * step;
                        if (sx < 0 || sx >= W) continue;
                        const uint32_t cand = (*in)[(size_t)sy * W + sx];
                        const float d = ER_SeedDist2(cand, x, y);
                        if (d < bestD) { bestD = d; best = cand; }
                    }
                }
                (*out)[(size_t)y * W + x] = best;
            }
        }
        std::swap(in, out);
    };

    const int maxDim = W > H ? W : H;
    int step = 1;
    while (step < maxDim) step <<= 1;
    step >>= 1;
    for (; step >= 1; step >>= 1) pass(step);
    pass(2);      // the "+2"
    pass(1);

    dist.resize(N);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            dist[(size_t)y * W + x] =
                ER_Dist2ToDist(ER_SeedDist2((*in)[(size_t)y * W + x], x, y));
}

int main()
{
    printf("Stage 6 CPU/GPU kernel-math parity test\n");

    // ---------------------------------------------------------------------
    // 1. Blend weight — bit-identical, including the inner ease.
    // ---------------------------------------------------------------------
    {
        bool ok = true;
        Rng r(11);
        for (int i = 0; i < 20000; ++i) {
            const float d      = r.range(-5.f, 520.f);
            const float extend = r.range(0.f, 500.f);
            const float inner  = r.range(0.f, 250.f);
            if (mm::BlendWeight(d, extend, inner) !=
                ER_BlendWeight(d, extend, inner)) { ok = false; break; }
        }
        // Endpoints the Stage 4 gate leans on.
        ok &= ER_BlendWeight(0.f, 32.f, 5.f) == 0.f;
        ok &= ER_BlendWeight(32.f, 32.f, 5.f) == 0.f;
        ok &= ER_BlendWeight(1e15f, 32.f, 5.f) == 0.f;   // unreachable distance
        Check(ok, "blendweight", "bit-identical over 20k random triples");
    }

    // ---------------------------------------------------------------------
    // 2. Smoothstep helper — bit-identical.
    // ---------------------------------------------------------------------
    {
        bool ok = true;
        Rng r(12);
        for (int i = 0; i < 20000; ++i) {
            const float t = r.range(-1.f, 2.f);
            if (mm::ER_Smooth01(t) != ER_Smooth01(t)) { ok = false; break; }
        }
        Check(ok, "smooth01", "");
    }

    // ---------------------------------------------------------------------
    // 3. Band composite — bit-identical, and w=0 still returns orig exactly.
    // ---------------------------------------------------------------------
    {
        bool ok = true;
        Rng r(13);
        for (int i = 0; i < 20000; ++i) {
            const float orig[3] = { r.unit(), r.unit(), r.unit() };
            const float ext [3] = { r.unit(), r.unit(), r.unit() };
            const float grain   = r.range(-1.f, 1.f);
            const float w       = r.unit();
            const float det     = r.unit();
            float cpu[3];
            mm::ComposeBandRGB(orig, ext, grain, w, det, cpu);
            const ER_RGB gpu = ER_ComposeBandRGB(
                ER_MakeRGB(orig[0], orig[1], orig[2]),
                ER_MakeRGB(ext[0],  ext[1],  ext[2]), grain, w, det);
            if (cpu[0] != gpu.r || cpu[1] != gpu.g || cpu[2] != gpu.b) {
                ok = false; break;
            }
        }
        Check(ok, "composebandrgb", "bit-identical over 20k random pixels");
    }

    // ---------------------------------------------------------------------
    // 4. Alpha choke, both directions — bit-identical.
    // ---------------------------------------------------------------------
    {
        bool ok = true;
        Rng r(14);
        for (int i = 0; i < 20000; ++i) {
            const float a  = r.unit();
            const float d  = r.range(0.f, 60.f);
            const float c  = r.range(0.f, 50.f);
            if (mm::ChokeInAlpha(a, d, c) != ER_ChokeInAlpha(a, d, c)) { ok = false; break; }
            if (mm::GrowAlpha(a, d, c)    != ER_GrowAlpha(a, d, c))    { ok = false; break; }
            // ...and through the dispatcher the kernels actually call.
            if (ER_ApplyChoke(a, d,  c, ER_CHOKE_MODE_IN)   != mm::ChokeInAlpha(a, d, c)) { ok = false; break; }
            if (ER_ApplyChoke(a, d, -c, ER_CHOKE_MODE_GROW) != mm::GrowAlpha(a, d, c))    { ok = false; break; }
            if (ER_ApplyChoke(a, d,  c, ER_CHOKE_MODE_OFF)  != a) { ok = false; break; }
        }
        Check(ok, "alpha choke", "in/grow/off, bit-identical");
    }

    // ---------------------------------------------------------------------
    // 5. Grain — the hash and the sample must match BIT for bit, or the GPU
    //    would lay a different noise field into the band than the CPU.
    // ---------------------------------------------------------------------
    {
        bool ok = true;
        for (int y = -40; y < 400 && ok; ++y)
            for (int x = -40; x < 400; ++x)
                if (mm::GrainAt(x, y) != ER_GrainAt(x, y)) { ok = false; break; }
        Check(ok, "grain sample", "bit-identical over 440x440 coords");

        // Amplitude conversion: mean |neighbour diff| -> amplitude (*1.53).
        bool ok2 = true;
        Rng r(15);
        for (int i = 0; i < 5000; ++i) {
            const float sum = r.range(0.f, 1e5f);
            const float cnt = (float)(1 + (r.next() % 100000u));
            const float ref = (float)((double)(sum / cnt) * 1.53);
            const float got = ER_GrainAmpFromMean(sum, cnt);
            if (std::fabs(ref - got) > 1e-6f * (std::fabs(ref) + 1.f)) { ok2 = false; break; }
        }
        ok2 &= ER_GrainAmpFromMean(1.f, 0.f) == 0.f;   // no core pixels -> no grain
        Check(ok2, "grain amplitude", "");
    }

    // ---------------------------------------------------------------------
    // 6. Core matte — ER_CoreTest must reproduce mm::BuildCoreMatte for the
    //    same inputs, on a real image, in all three erode branches. This is
    //    the decision, fed the same distances; the DISTANCE itself is checked
    //    separately in test 9.
    // ---------------------------------------------------------------------
    {
        const int W = 97, H = 61;
        const size_t N = (size_t)W * H;
        std::vector<float>   alpha(N);
        std::vector<uint8_t> interior(N), nonint(N);
        Rng r(16);
        // A soft blob + noise: gives a real interior, exterior and soft rim.
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float dx = (float)x - 48.f, dy = (float)y - 30.f;
                const float rad = std::sqrt(dx * dx + dy * dy);
                float a = 1.f - (rad - 14.f) / 8.f;
                a = a < 0.f ? 0.f : (a > 1.f ? 1.f : a);
                alpha[(size_t)y * W + x] = a * r.range(0.9f, 1.0f);
            }
        for (size_t i = 0; i < N; ++i) {
            interior[i] = alpha[i] > 0.5f ? 1 : 0;
            nonint[i]   = interior[i] ? 0 : 1;
        }

        std::vector<float> dShrink(N), dGrow(N);
        mm::EuclideanDistanceTransform(nonint.data(),   dShrink.data(), W, H);
        mm::EuclideanDistanceTransform(interior.data(), dGrow.data(),   W, H);

        bool ok = true;
        const float erodes[] = { 0.f, 0.005f, 2.f, 7.3f, -3.f, -0.5f };
        std::vector<uint8_t> cpuCore(N);
        for (float e : erodes) {
            mm::BuildCoreMatte(alpha.data(), cpuCore.data(), W, H, 0.5f, e);

            int mode = ER_CORE_MODE_COPY;
            if      (e >  0.01f) mode = ER_CORE_MODE_SHRINK;
            else if (e < -0.01f) mode = ER_CORE_MODE_GROW;
            const std::vector<float> &fld = (mode == ER_CORE_MODE_GROW) ? dGrow : dShrink;

            for (size_t i = 0; i < N; ++i) {
                const float ip  = interior[i] ? 1.f : 0.f;
                const float gpu = ER_CoreTest(ip, fld[i], e, mode);
                if ((gpu > 0.5f) != (cpuCore[i] != 0)) { ok = false; break; }
            }
            if (!ok) break;
        }
        Check(ok, "core matte decision", "6 erode values incl. both signs + 0");

        // The degenerate seed sets: with no seeds anywhere the flood leaves
        // ER_DIST_INF, which must reproduce the CPU's host-side early-outs
        // (shrink keeps the whole interior; grow keeps nothing).
        bool ok2 = true;
        ok2 &= ER_CoreTest(1.f, ER_DIST_INF, 5.f,  ER_CORE_MODE_SHRINK) == 1.f;
        ok2 &= ER_CoreTest(0.f, ER_DIST_INF, 5.f,  ER_CORE_MODE_SHRINK) == 0.f;
        ok2 &= ER_CoreTest(1.f, ER_DIST_INF, -5.f, ER_CORE_MODE_GROW)   == 0.f;
        Check(ok2, "core matte degenerate", "empty seed set matches CPU early-outs");
    }

    // ---------------------------------------------------------------------
    // 7. Distance Field view value — bit-identical to ER_DistViewRow's body
    //    (reproduced here from EdgeRepair.cpp; if that changes, this fails).
    // ---------------------------------------------------------------------
    {
        bool ok = true;
        Rng r(17);
        for (int i = 0; i < 20000; ++i) {
            const float e    = r.range(0.f, 60.f);
            const float gap  = r.range(0.f, 50.f);
            const float soft = r.range(0.f, 250.f);
            const bool  in   = (r.next() & 1u) != 0u;

            float ref = 0.f;
            if (gap > 0.f && in) {
                if (e < gap) {
                    const float ramp = soft < gap ? soft : gap;
                    const float plateau = gap - ramp;
                    if (e <= plateau || ramp <= 0.f) ref = 1.f;
                    else ref = 1.f - mm::ER_Smooth01((e - plateau) / ramp);
                }
            }
            if (ref != ER_DistViewValue(e, in ? 1.f : 0.f, gap, soft)) { ok = false; break; }
        }
        Check(ok, "distance-field view", "bit-identical to ER_DistViewRow");
    }

    // ---------------------------------------------------------------------
    // 8. Pull–push value math: the coverage-weighted average, the push
    //    coordinate mapping and its clamped bilinear taps, and unpremultiply.
    // ---------------------------------------------------------------------
    {
        bool ok = true;
        // Averaging by the CHILD COUNT (not a constant 4) is what keeps odd
        // dimensions from darkening at the last row/column.
        ok &= ER_PPAverage(4.f, 4) == 1.f;
        ok &= ER_PPAverage(3.f, 2) == 1.5f;
        ok &= ER_PPAverage(1.f, 1) == 1.f;
        ok &= ER_PPAverage(0.f, 0) == 0.f;

        Check(ok, "pull-push average", "divides by the child count, not 4");

        // The push sample must land on the same COLOUR as detail_pp::Bilinear4.
        //
        // Deliberately compared by VALUE, not by tap index: the CPU computes
        // the source coordinate in double and the GPU in float, so where the
        // coordinate lands exactly on a texel centre the two floor() to
        // different sides — taps (n-1,n) with fraction ~1 vs (n,n+1) with
        // fraction 0. Both sample the SAME texel; only the discrete
        // decomposition differs. Asserting on tap indices would fail on a
        // difference no pixel can ever show (measured: only at integer
        // coordinates, e.g. fW=29 x=14 -> 6.9999995 vs 7.0).
        bool okv = true;
        float worst = 0.f;
        Rng r(18);
        for (int t = 0; t < 600 && okv; ++t) {
            const int fW = 1 + (int)(r.next() % 60u);
            const int fH = 1 + (int)(r.next() % 60u);
            const int cW = (fW + 1) / 2, cH = (fH + 1) / 2;

            mm::detail_pp::PPLevel C;
            C.W = cW; C.H = cH;
            C.px.resize((size_t)cW * cH * 4);
            for (size_t i = 0; i < C.px.size(); ++i) C.px[i] = r.range(-2.f, 2.f);

            const double suD = (double)cW / (double)fW;
            const double svD = (double)cH / (double)fH;
            const float  suF = (float)cW / (float)fW;
            const float  svF = (float)cH / (float)fH;

            for (int y = 0; y < fH && okv; ++y) {
                for (int x = 0; x < fW; ++x) {
                    float ref[4];
                    mm::detail_pp::Bilinear4(C,
                        ((double)x + 0.5) * suD - 0.5,
                        ((double)y + 0.5) * svD - 0.5, ref);

                    int x0, x1, y0, y1; float fu, fv;
                    ER_PPBilinearAxis(ER_PPPushCoord(x, suF), cW, x0, x1, fu);
                    ER_PPBilinearAxis(ER_PPPushCoord(y, svF), cH, y0, y1, fv);
                    for (int ch = 0; ch < 4; ++ch) {
                        const float p00 = C.px[((size_t)y0 * cW + x0) * 4 + ch];
                        const float p10 = C.px[((size_t)y0 * cW + x1) * 4 + ch];
                        const float p01 = C.px[((size_t)y1 * cW + x0) * 4 + ch];
                        const float p11 = C.px[((size_t)y1 * cW + x1) * 4 + ch];
                        const float top = p00 + (p10 - p00) * fu;
                        const float bot = p01 + (p11 - p01) * fu;
                        const float got = top + (bot - top) * fv;
                        const float e = std::fabs(got - ref[ch]);
                        if (e > worst) worst = e;
                        if (e > 1e-4f) okv = false;
                    }
                }
            }
        }
        char d[96];
        snprintf(d, sizeof(d), "worst |push sample - Bilinear4| = %.3e", worst);
        Check(okv, "pull-push push sample", d);

        bool ok2 = true;
        for (int i = 0; i < 2000; ++i) {
            Rng rr(100u + (uint32_t)i);
            const float cov = rr.unit();
            const float rgb[3] = { rr.unit() * cov, rr.unit() * cov, rr.unit() * cov };
            const ER_RGB u = ER_PPUnpremult(rgb[0], rgb[1], rgb[2], cov);
            const float inv = cov > 0.f ? 1.f / cov : 0.f;
            if (u.r != rgb[0] * inv || u.g != rgb[1] * inv || u.b != rgb[2] * inv) {
                ok2 = false; break;
            }
        }
        ok2 &= ER_PPUnpremult(1.f, 1.f, 1.f, 0.f).r == 0.f;   // no coverage -> black
        // Validity gate: core AND coverage, matching PullPushFill's two skips.
        ok2 &= ER_PPValid(1.f, 0.5f) == 1.f;
        ok2 &= ER_PPValid(1.f, 0.f)  == 0.f;   // "valid" but zero alpha -> no colour
        ok2 &= ER_PPValid(0.f, 1.f)  == 0.f;
        Check(ok2, "pull-push unpremult", "");
    }

    // ---------------------------------------------------------------------
    // 9. THE ONE ACCEPTED DIVERGENCE: jump flood vs the exact EDT.
    //    Runs the host's real JFA+2 schedule and measures the error. The
    //    Stage 6 gate is "within tolerance", and this is the number behind it.
    // ---------------------------------------------------------------------
    {
        struct Case { const char *name; int W, H; };
        const Case cases[] = {
            { "single seed",   129,  97 },
            { "circle",        201, 151 },
            { "sparse noise",  256, 256 },
            { "thin line",     181, 143 },
            { "two corners",   127, 127 },
        };

        bool ok = true;
        float worstAbs = 0.f, worstRel = 0.f;
        for (int ci = 0; ci < (int)(sizeof(cases) / sizeof(cases[0])); ++ci) {
            const int W = cases[ci].W, H = cases[ci].H;
            const size_t N = (size_t)W * H;
            std::vector<uint8_t> seed(N, 0);
            Rng r(200u + (uint32_t)ci);

            switch (ci) {
                case 0: seed[(size_t)(H / 3) * W + (W / 4)] = 1; break;
                case 1:
                    for (int y = 0; y < H; ++y)
                        for (int x = 0; x < W; ++x) {
                            const float dx = (float)x - W * 0.5f, dy = (float)y - H * 0.5f;
                            if (dx * dx + dy * dy < 18.f * 18.f) seed[(size_t)y * W + x] = 1;
                        }
                    break;
                case 2:
                    for (size_t i = 0; i < N; ++i) if ((r.next() % 400u) == 0u) seed[i] = 1;
                    break;
                case 3:
                    for (int x = 10; x < W - 10; ++x) seed[(size_t)(H / 2) * W + x] = 1;
                    break;
                default:
                    seed[0] = 1;
                    seed[N - 1] = 1;
                    break;
            }

            std::vector<float> exact(N), jfa;
            mm::EuclideanDistanceTransform(seed.data(), exact.data(), W, H);
            JumpFlood(seed, jfa, W, H);

            for (size_t i = 0; i < N; ++i) {
                const float e = std::fabs(jfa[i] - exact[i]);
                if (e > worstAbs) worstAbs = e;
                const float rel = exact[i] > 1.f ? e / exact[i] : e;
                if (rel > worstRel) worstRel = rel;
            }
        }
        // JFA+2 is exact on these shapes in practice; allow a sub-pixel margin
        // so the test states the tolerance the gate actually promises rather
        // than accidentally locking in "happens to be exact".
        ok = worstAbs <= 0.5f;
        char detail[160];
        snprintf(detail, sizeof(detail),
                 "worst |JFA+2 - exact EDT| = %.4f px (rel %.5f), tol 0.5 px",
                 worstAbs, worstRel);
        Check(ok, "jump flood vs EDT", detail);
    }

    // ---------------------------------------------------------------------
    // 10. Whole-pixel composite: ER_CompositeRGB must reproduce ER_ResultRow's
    //     branch structure — including the two VERBATIM-copy paths that make
    //     "untouched outside the band" and "Choke 0 -> bit-identical alpha"
    //     hold bitwise rather than approximately.
    // ---------------------------------------------------------------------
    {
        bool ok = true;
        Rng r(19);
        for (int i = 0; i < 20000; ++i) {
            const ER_RGB orig = ER_MakeRGB(r.unit(), r.unit(), r.unit());
            const ER_RGB ext  = ER_MakeRGB(r.unit(), r.unit(), r.unit());
            const float a     = (r.next() & 7u) == 0u ? 0.f : r.unit();
            const float choke = (r.next() & 3u) == 0u ? r.range(0.f, 20.f) : 0.f;
            const float dOut  = r.range(0.f, 40.f);
            const int   band  = (int)(r.next() & 1u);
            const float w     = (r.next() & 1u) ? 0.f : r.unit();
            const float det   = r.unit();
            const float amp   = r.range(0.f, 0.1f);
            const int   gx = (int)(r.next() % 4000u), gy = (int)(r.next() % 4000u);

            const float newA = choke > 0.f ? ER_ChokeInAlpha(a, dOut, choke) : a;

            // The CPU reference, transcribed from ER_ResultRow.
            ER_RGB ref;
            if (w <= 0.f) {
                if (newA == a)                       ref = orig;
                else if (a <= 1e-6f && band != 0)    ref = ext;
                else                                 ref = orig;
            } else {
                const float origArr[3] = { a > 1e-6f ? orig.r : ext.r,
                                           a > 1e-6f ? orig.g : ext.g,
                                           a > 1e-6f ? orig.b : ext.b };
                const float extArr[3]  = { ext.r, ext.g, ext.b };
                const float grain = mm::GrainAt(gx, gy) * amp;
                float o[3];
                mm::ComposeBandRGB(origArr, extArr, grain, w, det, o);
                ref = ER_MakeRGB(o[0], o[1], o[2]);
            }

            const ER_RGB got = ER_CompositeRGB(orig, ext, a, newA, w, det, amp,
                                               band, gx, gy);
            if (ref.r != got.r || ref.g != got.g || ref.b != got.b) { ok = false; break; }
        }
        Check(ok, "composite branches", "bit-identical to ER_ResultRow, 20k pixels");
    }

    // ---------------------------------------------------------------------
    // 11. Addressing helpers — a wrong pitch or offset is the most damaging
    //     and least visible GPU bug, so pin them down explicitly.
    // ---------------------------------------------------------------------
    {
        bool ok = true;
        ok &= ER_PixelFloatIndex(0, 0, 4096) == 0u;
        ok &= ER_PixelFloatIndex(3, 0, 4096) == 12u;          // 4 floats/pixel
        ok &= ER_PixelFloatIndex(0, 2, 4096) == 8192u;        // pitch, not W*4
        ok &= ER_SrcFloatIndex(5, 7, 40, 40, 4096) == ER_PixelFloatIndex(45, 47, 4096);
        ok &= ER_PlaneIndex(5, 7, 100) == 705u;               // tight pitch == W
        Check(ok, "addressing", "world pitch vs tightly-packed planes");

        // Seed packing round-trips across the full 16-bit range per axis.
        bool ok2 = true;
        const uint32_t coords[] = { 0u, 1u, 255u, 4095u, 30000u, 65534u };
        for (uint32_t cx : coords)
            for (uint32_t cy : coords) {
                const uint32_t p = ER_PackSeed(cx, cy);
                if (p == ER_SEED_INVALID) continue;           // 65535,65535 only
                if (ER_SeedX(p) != cx || ER_SeedY(p) != cy) ok2 = false;
            }
        ok2 &= ER_SeedDist2(ER_SEED_INVALID, 3, 4) == ER_D2_INF;
        ok2 &= ER_SeedDist2(ER_PackSeed(3, 8), 3, 4) == 16.f;
        ok2 &= ER_Dist2ToDist(ER_D2_INF) == ER_DIST_INF;
        ok2 &= ER_Dist2ToDist(9.f) == 3.f;
        Check(ok2, "seed packing", "");
    }

    // ---------------------------------------------------------------------
    // 12. Whole pull–push fill, GPU-SHAPED: the mip level schedule, the pull
    //     loop, the push loop and the resolve, composed exactly as
    //     ER_RenderCUDA issues them (ER_BuildLayout's ceil-halving chain, pull
    //     1..n-1, push n-2..0, resolve level 0) — against mm::PullPushFill.
    //
    //     Test 8 checks the value helpers in isolation; this checks that the
    //     HOST wired them together right. An off-by-one in the level loop or a
    //     swapped fine/coarse would sail past test 8 and produce a plausible
    //     but wrong clean plate in AE.
    // ---------------------------------------------------------------------
    {
        struct Case { const char *name; int W, H; };
        const Case cases[] = { { "even", 64, 48 }, { "odd", 61, 47 },
                               { "thin", 129, 3 }, { "tall", 5, 130 } };
        bool ok = true;
        float worst = 0.f;
        char which[64] = "";

        for (int ci = 0; ci < 4 && ok; ++ci) {
            const int W = cases[ci].W, H = cases[ci].H;
            const size_t N = (size_t)W * H;
            std::vector<float>   argb(N * 4);
            std::vector<uint8_t> valid(N, 0);
            Rng r(300u + (uint32_t)ci);

            // A blob of trusted pixels with varied colour and coverage, plus
            // deliberate garbage in the invalid region (which must not leak).
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const size_t i = (size_t)y * W + x;
                    const float dx = (float)x - W * 0.35f, dy = (float)y - H * 0.55f;
                    const bool  in = (dx * dx + dy * dy) < (float)(W * H) * 0.03f;
                    valid[i] = in ? 1 : 0;
                    argb[i * 4 + 0] = in ? r.range(0.2f, 1.f) : r.range(0.f, 1.f);
                    argb[i * 4 + 1] = r.unit();
                    argb[i * 4 + 2] = r.unit();
                    argb[i * 4 + 3] = r.unit();
                }

            std::vector<float> cpuRGB(N * 3, -1.f);
            if (!mm::PullPushFill(argb.data(), valid.data(), cpuRGB.data(), W, H))
                continue;   // no seeds — nothing to compare

            // --- the GPU's chain, on the CPU ---
            std::vector<int> lw, lh, loff;
            {
                int w = W, h = H, floats = 0;
                for (;;) {
                    lw.push_back(w); lh.push_back(h); loff.push_back(floats);
                    floats += w * h * 4;
                    if (w <= 1 && h <= 1) break;
                    w = (w + 1) / 2; h = (h + 1) / 2;
                }
            }
            const int nLevels = (int)lw.size();
            std::vector<float> chain((size_t)loff.back() + (size_t)lw.back() * lh.back() * 4, 0.f);

            for (size_t i = 0; i < N; ++i) {          // ER_PPInit
                const float a = argb[i * 4 + 0];
                float *t = &chain[i * 4];
                if (ER_PPValid(valid[i] ? 1.f : 0.f, a) > 0.5f) {
                    t[0] = argb[i * 4 + 1] * a;
                    t[1] = argb[i * 4 + 2] * a;
                    t[2] = argb[i * 4 + 3] * a;
                    t[3] = a;
                } else { t[0] = t[1] = t[2] = t[3] = 0.f; }
            }
            for (int k = 1; k < nLevels; ++k) {       // ER_PPPull
                for (int y = 0; y < lh[k]; ++y)
                    for (int x = 0; x < lw[k]; ++x) {
                        float acc[4] = { 0.f, 0.f, 0.f, 0.f };
                        int n = 0;
                        for (int dy = 0; dy < 2; ++dy) {
                            const int fy = y * 2 + dy;
                            if (fy >= lh[k - 1]) continue;
                            for (int dx = 0; dx < 2; ++dx) {
                                const int fx = x * 2 + dx;
                                if (fx >= lw[k - 1]) continue;
                                const float *s = &chain[(size_t)loff[k - 1] +
                                                        ((size_t)fy * lw[k - 1] + fx) * 4];
                                for (int c = 0; c < 4; ++c) acc[c] += s[c];
                                ++n;
                            }
                        }
                        float *t = &chain[(size_t)loff[k] + ((size_t)y * lw[k] + x) * 4];
                        for (int c = 0; c < 4; ++c) t[c] = ER_PPAverage(acc[c], n);
                    }
            }
            for (int k = nLevels - 2; k >= 0; --k) {  // ER_PPPush
                const float su = (float)lw[k + 1] / (float)lw[k];
                const float sv = (float)lh[k + 1] / (float)lh[k];
                for (int y = 0; y < lh[k]; ++y)
                    for (int x = 0; x < lw[k]; ++x) {
                        float *t = &chain[(size_t)loff[k] + ((size_t)y * lw[k] + x) * 4];
                        if (t[3] > 0.f) continue;
                        int x0, x1, y0, y1; float fu, fv;
                        ER_PPBilinearAxis(ER_PPPushCoord(x, su), lw[k + 1], x0, x1, fu);
                        ER_PPBilinearAxis(ER_PPPushCoord(y, sv), lh[k + 1], y0, y1, fv);
                        const float *c = &chain[(size_t)loff[k + 1]];
                        for (int ch = 0; ch < 4; ++ch) {
                            const float p00 = c[((size_t)y0 * lw[k + 1] + x0) * 4 + ch];
                            const float p10 = c[((size_t)y0 * lw[k + 1] + x1) * 4 + ch];
                            const float p01 = c[((size_t)y1 * lw[k + 1] + x0) * 4 + ch];
                            const float p11 = c[((size_t)y1 * lw[k + 1] + x1) * 4 + ch];
                            const float top = p00 + (p10 - p00) * fu;
                            const float bot = p01 + (p11 - p01) * fu;
                            t[ch] = top + (bot - top) * fv;
                        }
                    }
            }
            for (size_t i = 0; i < N; ++i) {          // ER_PPResolve
                const float *t = &chain[i * 4];
                const ER_RGB u = ER_PPUnpremult(t[0], t[1], t[2], t[3]);
                const float e0 = std::fabs(u.r - cpuRGB[i * 3 + 0]);
                const float e1 = std::fabs(u.g - cpuRGB[i * 3 + 1]);
                const float e2 = std::fabs(u.b - cpuRGB[i * 3 + 2]);
                const float e = std::max(e0, std::max(e1, e2));
                if (e > worst) { worst = e; snprintf(which, sizeof(which), "%s", cases[ci].name); }
                if (e > 2e-3f) ok = false;
            }
        }
        char d[128];
        snprintf(d, sizeof(d), "worst |GPU chain - PullPushFill| = %.3e (%s)", worst, which);
        Check(ok, "pull-push whole fill", d);
    }

    // ---------------------------------------------------------------------
    // 13. Grain amplitude, GPU-SHAPED: the fixed-grid, block-reduced FLOAT sum
    //     the kernels compute vs the CPU's serial DOUBLE accumulation. Not
    //     bit-identical by construction (different summation order and
    //     precision) — this pins down how far apart they can drift, since the
    //     amplitude scales every grain sample the band adds.
    // ---------------------------------------------------------------------
    {
        const int W = 311, H = 197;
        const size_t N = (size_t)W * H;
        std::vector<float>   src(N * 4);      // BGRA, as the GPU world is
        std::vector<uint8_t> core(N, 0);
        Rng r(400);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = (size_t)y * W + x;
                const float base = 0.35f + 0.25f * std::sin((float)x * 0.05f);
                const float g = base + r.range(-0.02f, 0.02f);   // plate + grain
                src[i * 4 + 0] = g * 0.9f;    // B
                src[i * 4 + 1] = g;           // G
                src[i * 4 + 2] = g * 1.1f;    // R
                src[i * 4 + 3] = 1.f;
                core[i] = (x > 30 && x < W - 30 && y > 20 && y < H - 20) ? 1 : 0;
            }

        // CPU reference — the estimator in EdgeRepair.cpp, double accumulator.
        double acc = 0.0; size_t cnt = 0;
        auto lumAt = [&](size_t j) {
            return 0.299 * src[j * 4 + 2] + 0.587 * src[j * 4 + 1] + 0.114 * src[j * 4 + 0];
        };
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W - 1; ++x) {
                const size_t j = (size_t)y * W + x;
                if (!core[j] || !core[j + 1]) continue;
                acc += std::fabs(lumAt(j) - lumAt(j + 1)); ++cnt;
            }
        for (int y = 0; y < H - 1; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t j = (size_t)y * W + x;
                if (!core[j] || !core[j + W]) continue;
                acc += std::fabs(lumAt(j) - lumAt(j + (size_t)W)); ++cnt;
            }
        const float cpuAmp = (float)((cnt ? acc / (double)cnt : 0.0) * 1.53);

        // GPU shape: ER_GRAIN_BLOCKS x ER_GRAIN_THREADS, grid-stride, block
        // reduce to partials, then the serial finalize.
        std::vector<float> partials((size_t)ER_GRAIN_BLOCKS * 2, 0.f);
        const int total = W * H;
        const int stride = ER_GRAIN_BLOCKS * ER_GRAIN_THREADS;
        for (int blk = 0; blk < ER_GRAIN_BLOCKS; ++blk) {
            std::vector<float> sSum(ER_GRAIN_THREADS, 0.f), sCnt(ER_GRAIN_THREADS, 0.f);
            for (int tid = 0; tid < ER_GRAIN_THREADS; ++tid) {
                float s = 0.f, c = 0.f;
                for (int i = blk * ER_GRAIN_THREADS + tid; i < total; i += stride) {
                    if (!core[i]) continue;
                    const int x = i % W, y = i / W;
                    const float l = ER_Luma(src[(size_t)i * 4 + 2], src[(size_t)i * 4 + 1],
                                            src[(size_t)i * 4 + 0]);
                    if (x + 1 < W && core[i + 1]) {
                        const size_t j = (size_t)i + 1;
                        s += ER_ABS(l - ER_Luma(src[j * 4 + 2], src[j * 4 + 1], src[j * 4 + 0]));
                        c += 1.f;
                    }
                    if (y + 1 < H && core[i + W]) {
                        const size_t j = (size_t)i + W;
                        s += ER_ABS(l - ER_Luma(src[j * 4 + 2], src[j * 4 + 1], src[j * 4 + 0]));
                        c += 1.f;
                    }
                }
                sSum[tid] = s; sCnt[tid] = c;
            }
            for (int half = ER_GRAIN_THREADS / 2; half > 0; half >>= 1)
                for (int tid = 0; tid < half; ++tid) {
                    sSum[tid] += sSum[tid + half];
                    sCnt[tid] += sCnt[tid + half];
                }
            partials[(size_t)blk * 2 + 0] = sSum[0];
            partials[(size_t)blk * 2 + 1] = sCnt[0];
        }
        float sum = 0.f, gcnt = 0.f;
        for (int i = 0; i < ER_GRAIN_BLOCKS; ++i) {
            sum += partials[(size_t)i * 2 + 0];
            gcnt += partials[(size_t)i * 2 + 1];
        }
        const float gpuAmp = ER_GrainAmpFromMean(sum, gcnt);

        const bool sameCount = (gcnt == (float)cnt);   // the PAIR SET must match exactly
        const float rel = cpuAmp > 0.f ? std::fabs(gpuAmp - cpuAmp) / cpuAmp : 0.f;
        char d[144];
        snprintf(d, sizeof(d), "cpu=%.6f gpu=%.6f rel=%.2e, pairs %.0f vs %zu",
                 cpuAmp, gpuAmp, rel, gcnt, cnt);
        Check(sameCount && rel < 1e-4f, "grain amp reduction", d);
    }

    printf(g_fail ? "\nFAILED (%d)\n" : "\nAll parity checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
