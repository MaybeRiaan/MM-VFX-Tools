/*  MM_PullPush.h — pull–push extension of trusted colour, AE-free and
    header-only (unit-testable headlessly — see tests/test_pullpush.cpp).

    This is Edge Repair's ONLY fill engine (the distance field never fills
    colour itself — it only drives erode/extend/choke/blend). The job: given
    a premultiplied ARGB image and a validity mask (the core matte), produce
    a straight-RGB colour defined at EVERY pixel, where invalid pixels get a
    smooth extension of the nearest/weighted trusted colour. The fill runs
    effectively unlimited; the caller decides how far the result is *used*
    (the Extend clamp happens downstream, not here).

    Algorithm (Gortler et al. pull–push, hole-filling form):

    PULL — build a mip chain of (premult R, premult G, premult B, coverage).
      Level 0: valid pixels contribute their premultiplied RGB with coverage
      = alpha; invalid pixels contribute (0,0,0,0). Each coarser texel is the
      plain average of its up-to-4 children — because colour is premultiplied
      by coverage, that average IS the coverage-weighted mean of the trusted
      colour beneath it, and zero-alpha/invalid garbage contributes exactly
      nothing (this is the "coverage weighting has to be right" property the
      Stage 3 gate names).

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
//   argb  : W*H*4 interleaved A,R,G,B in [0,1]-ish float, RGB premultiplied
//           by A (AE's native delivery — matches mm::UnpackRow layout).
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

    std::vector<detail_pp::PPLevel> chain;
    chain.emplace_back();
    chain[0].W = W;
    chain[0].H = H;
    chain[0].px.assign(N * 4, 0.f);

    // Level 0: trusted pixels contribute (premult RGB, alpha); all else 0.
    bool any = false;
    for (size_t i = 0; i < N; ++i) {
        if (!valid[i]) continue;
        const float a = argb[i * 4 + 0];
        if (a <= 0.f) continue;   // zero coverage — no colour to trust
        float *t = &chain[0].px[i * 4];
        t[0] = argb[i * 4 + 1];
        t[1] = argb[i * 4 + 2];
        t[2] = argb[i * 4 + 3];
        t[3] = a;
        any = true;
    }
    if (!any) return false;

    // PULL — average up-to-4 children per coarse texel down to 1x1.
    while (chain.back().W > 1 || chain.back().H > 1) {
        const detail_pp::PPLevel &f = chain.back();
        detail_pp::PPLevel c;
        c.W = (f.W + 1) / 2;
        c.H = (f.H + 1) / 2;
        c.px.assign((size_t)c.W * c.H * 4, 0.f);

        for (int y = 0; y < c.H; ++y) {
            for (int x = 0; x < c.W; ++x) {
                float acc[4] = { 0.f, 0.f, 0.f, 0.f };
                int   n = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    const int fy = y * 2 + dy;
                    if (fy >= f.H) continue;
                    for (int dx = 0; dx < 2; ++dx) {
                        const int fx = x * 2 + dx;
                        if (fx >= f.W) continue;
                        const float *s = &f.px[((size_t)fy * f.W + fx) * 4];
                        acc[0] += s[0]; acc[1] += s[1];
                        acc[2] += s[2]; acc[3] += s[3];
                        ++n;
                    }
                }
                const float inv = 1.f / (float)n;
                float *t = &c.px[((size_t)y * c.W + x) * 4];
                t[0] = acc[0] * inv; t[1] = acc[1] * inv;
                t[2] = acc[2] * inv; t[3] = acc[3] * inv;
            }
        }
        chain.push_back(std::move(c));
    }

    // PUSH — coarsest-1 down to level 0: fill only coverage-0 texels from the
    // (already fully covered) coarser level. Texel-center mapping via the
    // dimension ratio handles odd sizes from the ceil-halving pull.
    for (int k = (int)chain.size() - 2; k >= 0; --k) {
        detail_pp::PPLevel       &f = chain[k];
        const detail_pp::PPLevel &c = chain[k + 1];
        const double su = (double)c.W / (double)f.W;
        const double sv = (double)c.H / (double)f.H;

        for (int y = 0; y < f.H; ++y) {
            const double v = ((double)y + 0.5) * sv - 0.5;
            for (int x = 0; x < f.W; ++x) {
                float *t = &f.px[((size_t)y * f.W + x) * 4];
                if (t[3] > 0.f) continue;   // has trusted data — untouched
                float s[4];
                detail_pp::Bilinear4(c, ((double)x + 0.5) * su - 0.5, v, s);
                t[0] = s[0]; t[1] = s[1]; t[2] = s[2]; t[3] = s[3];
            }
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
