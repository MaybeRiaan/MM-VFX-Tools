/*  MM_PullPush.h — pull–push extension of trusted colour, AE-free and
    header-only (unit-testable headlessly — see tests/test_pullpush.cpp).

    This is Edge Repair's ONLY fill engine (the distance field never fills
    colour itself — it only drives erode/extend/choke/blend). The job: given
    a STRAIGHT-alpha ARGB image (AE's effect-world convention) and a
    validity mask (the core matte), produce a straight-RGB colour defined at
    EVERY pixel, where invalid pixels get a smooth extension of the
    nearest/weighted trusted colour. The fill runs effectively unlimited;
    the caller decides how far the result is *used* (the Extend clamp
    happens downstream, not here).

    Algorithm (Gortler et al. pull–push, hole-filling form):

    PULL — build a mip chain of (premult R, premult G, premult B, coverage).
      Level 0: valid pixels contribute their straight RGB premultiplied
      INTERNALLY by coverage = alpha; invalid pixels contribute (0,0,0,0).
      Each coarser texel is the plain average of its up-to-4 children —
      because colour is premultiplied by coverage inside the chain, that
      average IS the coverage-weighted mean of the trusted colour beneath
      it, and zero-alpha/invalid garbage contributes exactly nothing (this
      is the "coverage weighting has to be right" property the Stage 3 gate
      names).

    PUSH — from coarsest to finest, fill ONLY invalid texels (coverage == 0)
      with a clamped bilinear sample of the coarser level; texels with any
      coverage are left untouched. The coarser level is always fully covered
      by the time it is sampled (the 1x1 coarsest is covered iff any valid
      pixel exists), so the fill is total. Bilinear interpolation happens on
      the premultiplied tuple, so low-coverage neighbours contribute
      proportionally less colour — no bleed, and values propagating down
      through repeated bilinear upsampling come out smooth (no nearest-
      neighbour mip blockiness).

    UNPREMULTIPLY — out = rgb / coverage at every pixel. Valid pixels get
      back exactly their own straight colour; filled pixels get the smooth
      extension. ~2*log2(max dimension) passes total, O(n) work (the chain
      sums to 4/3 n texels).

    No PF/SDK types in this header — pure image algorithm, per the project
    rule that algorithm cores stay unit-testable without AE.
*/

#pragma once

#include "MM_Parallel.h"

#include <vector>
#include <cstdint>
#include <utility>

namespace mm {

namespace detail_pp {

// One mip level: 4 floats per texel — (r,g,b,c), RGB premultiplied by the
// coverage c. c == 0 means "no trusted data reaches this texel yet".
struct PPLevel {
    int W = 0, H = 0;
    std::vector<float> px;   // W*H*4
};

// Clamped bilinear fetch in texel-center coordinates (u == 0.0 is the center
// of texel 0). Interpolates the PREMULTIPLIED tuple — the caller's later
// divide by the interpolated coverage turns this into a coverage-weighted
// average of the four taps, which is what keeps low-coverage neighbours from
// bleeding colour.
inline void Bilinear4(const PPLevel &L, double u, double v, float out[4])
{
    if (u < 0.0) u = 0.0;
    if (v < 0.0) v = 0.0;
    if (u > (double)(L.W - 1)) u = (double)(L.W - 1);
    if (v > (double)(L.H - 1)) v = (double)(L.H - 1);

    const int x0 = (int)u;                     // floor — u,v are >= 0 here
    const int y0 = (int)v;
    const int x1 = (x0 + 1 < L.W) ? x0 + 1 : x0;
    const int y1 = (y0 + 1 < L.H) ? y0 + 1 : y0;
    const float fu = (float)(u - (double)x0);
    const float fv = (float)(v - (double)y0);

    const float *p00 = &L.px[((size_t)y0 * L.W + x0) * 4];
    const float *p10 = &L.px[((size_t)y0 * L.W + x1) * 4];
    const float *p01 = &L.px[((size_t)y1 * L.W + x0) * 4];
    const float *p11 = &L.px[((size_t)y1 * L.W + x1) * 4];

    for (int ch = 0; ch < 4; ++ch) {
        const float top = p00[ch] + (p10[ch] - p00[ch]) * fu;
        const float bot = p01[ch] + (p11[ch] - p01[ch]) * fu;
        out[ch] = top + (bot - top) * fv;
    }
}

} // namespace detail_pp

// Pull–push fill.
//   argb  : W*H*4 interleaved A,R,G,B in [0,1]-ish float, RGB in STRAIGHT
//           (unmatted) form — AE's effect-world convention (matches
//           mm::UnpackRow layout; see ModernEdgeBlur's black-halo history:
//           AE effects receive and must return straight alpha). The
//           coverage premultiply happens INTERNALLY at level-0 init; the
//           mip chain itself is premultiplied throughout.
//   valid : W*H mask; nonzero marks a TRUSTED pixel (the core matte). Pixels
//           outside the mask are never read — garbage RGB there cannot leak.
//           A "valid" pixel with alpha <= 0 carries no colour information and
//           is treated as invalid.
//   outRGB: W*H*3 straight (unpremultiplied) RGB, written at every pixel.
// Returns false if no usable valid pixel exists (outRGB untouched) — the
// caller must handle that the same way as an empty core matte.
inline bool PullPushFill(const float *argb, const uint8_t *valid,
                         float *outRGB, int W, int H)
{
    if (W <= 0 || H <= 0) return false;
    const size_t N = (size_t)W * H;

    // The mip chain is thread_local and reused across frames: its level count
    // and per-level sizes are a deterministic function of W×H, so after the
    // first frame at a given resolution NO level reallocates (grow-only .px),
    // killing the Stage 5 per-frame allocation churn. Per-thread → MFR-safe.
    // We size level 0 here, build coarser levels in the pull loop, and track a
    // live level count (the persistent vector may hold larger leftovers from a
    // previous, bigger frame — we ignore anything past `nLevels`).
    static thread_local std::vector<detail_pp::PPLevel> chain;
    if (chain.empty()) chain.emplace_back();
    int nLevels = 1;
    chain[0].W = W;
    chain[0].H = H;
    if (chain[0].px.size() < N * 4) chain[0].px.resize(N * 4);
    std::fill_n(chain[0].px.begin(), N * 4, 0.f);   // invalid pixels must be 0

    // Level 0: trusted pixels contribute (straight RGB * alpha, alpha) —
    // the internal premultiply that makes every later average and bilinear
    // fetch coverage-weighted; all else 0.
    bool any = false;
    for (size_t i = 0; i < N; ++i) {
        if (!valid[i]) continue;
        const float a = argb[i * 4 + 0];
        if (a <= 0.f) continue;   // zero coverage — no colour to trust
        float *t = &chain[0].px[i * 4];
        t[0] = argb[i * 4 + 1] * a;
        t[1] = argb[i * 4 + 2] * a;
        t[2] = argb[i * 4 + 3] * a;
        t[3] = a;
        any = true;
    }
    if (!any) return false;

    // PULL — average up-to-4 children per coarse texel down to 1x1. Coarse
    // levels are fully overwritten by the reduction below (every texel gets
    // acc/n), so their reused buffers need no pre-clear — just grow-to-size.
    while (chain[nLevels - 1].W > 1 || chain[nLevels - 1].H > 1) {
        const int fW = chain[nLevels - 1].W;
        const int fH = chain[nLevels - 1].H;
        const int cW = (fW + 1) / 2;
        const int cH = (fH + 1) / 2;
        // Grow the chain FIRST (may reallocate the vector), then take pointers
        // into the now-stable storage — never hold a reference across the
        // emplace_back that could invalidate it.
        if ((int)chain.size() <= nLevels) chain.emplace_back();
        chain[nLevels].W = cW;
        chain[nLevels].H = cH;
        if (chain[nLevels].px.size() < (size_t)cW * cH * 4)
            chain[nLevels].px.resize((size_t)cW * cH * 4);

        // Coarse texels are independent — parallelize over rows of c. Each
        // reads a disjoint 2x2 block of the finer level and writes its own
        // coarse texel; byte-identical to the serial reduction.
        {
            const float *fpx = chain[nLevels - 1].px.data();
            float       *cpx = chain[nLevels].px.data();
            ParallelFor(cH, [=](int /*threadIndex*/, int y) {
                for (int x = 0; x < cW; ++x) {
                    float acc[4] = { 0.f, 0.f, 0.f, 0.f };
                    int   n = 0;
                    for (int dy = 0; dy < 2; ++dy) {
                        const int fy = y * 2 + dy;
                        if (fy >= fH) continue;
                        for (int dx = 0; dx < 2; ++dx) {
                            const int fx = x * 2 + dx;
                            if (fx >= fW) continue;
                            const float *s = &fpx[((size_t)fy * fW + fx) * 4];
                            acc[0] += s[0]; acc[1] += s[1];
                            acc[2] += s[2]; acc[3] += s[3];
                            ++n;
                        }
                    }
                    const float inv = 1.f / (float)n;
                    float *t = &cpx[((size_t)y * cW + x) * 4];
                    t[0] = acc[0] * inv; t[1] = acc[1] * inv;
                    t[2] = acc[2] * inv; t[3] = acc[3] * inv;
                }
            });
        }
        ++nLevels;
    }

    // PUSH — coarsest-1 down to level 0: fill only coverage-0 texels from the
    // (already fully covered) coarser level. Texel-center mapping via the
    // dimension ratio handles odd sizes from the ceil-halving pull.
    for (int k = nLevels - 2; k >= 0; --k) {
        detail_pp::PPLevel       &f = chain[k];
        const detail_pp::PPLevel &c = chain[k + 1];
        const double su = (double)c.W / (double)f.W;
        const double sv = (double)c.H / (double)f.H;

        // Fine texels are independent — each writes only its own texel and
        // reads only the coarser level c (read-only here). Parallelize over
        // rows of f.
        {
            detail_pp::PPLevel       *fp = &f;
            const detail_pp::PPLevel *cp = &c;
            const int fW = f.W;
            ParallelFor(f.H, [=](int /*threadIndex*/, int y) {
                const double v = ((double)y + 0.5) * sv - 0.5;
                for (int x = 0; x < fW; ++x) {
                    float *t = &fp->px[((size_t)y * fW + x) * 4];
                    if (t[3] > 0.f) continue;   // has trusted data — untouched
                    float s[4];
                    detail_pp::Bilinear4(*cp, ((double)x + 0.5) * su - 0.5, v, s);
                    t[0] = s[0]; t[1] = s[1]; t[2] = s[2]; t[3] = s[3];
                }
            });
        }
    }

    // UNPREMULTIPLY — coverage is > 0 everywhere now (guarded anyway).
    const detail_pp::PPLevel &L0 = chain[0];
    for (size_t i = 0; i < N; ++i) {
        const float *t   = &L0.px[i * 4];
        const float  cov = t[3];
        const float  inv = cov > 0.f ? 1.f / cov : 0.f;
        outRGB[i * 3 + 0] = t[0] * inv;
        outRGB[i * 3 + 1] = t[1] * inv;
        outRGB[i * 3 + 2] = t[2] * inv;
    }
    return true;
}

} // namespace mm
