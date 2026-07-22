/*  MM_Gaussian.h — separable Gaussian blur, AE-free and header-only
    (unit-testable headlessly — see tests/test_gaussian.cpp).

    Used by Edge Repair's Detail frequency split: the "low" band is a PLAIN
    Gaussian low-pass of the plate's straight RGB (the brief's
    `gaussianBlur(origRGB, r)` — unweighted; see MM_Composite.h for why an
    alpha-weighted variant is wrong there). This header provides only the
    planar blur passes; callers compose whatever weighting their maths
    needs (the weighted-divide property is still covered by the unit test
    as a primitive guarantee).

    Border policy: clamp-to-edge (replicate the outermost sample). Kernel:
    half-width = ceil(radius) taps each side, sigma = radius/2, normalized
    to sum 1 — so the kernel support is ~±2σ and a constant plane is
    preserved exactly.

    No PF/SDK types — pure image algorithm, per the project rule that
    algorithm cores stay unit-testable without AE.
*/

#pragma once

#include "MM_Parallel.h"

#include <vector>
#include <cmath>

namespace mm {

// Normalized 1-D Gaussian kernel: 2*ceil(radius)+1 taps, sigma = radius/2
// (minimum half-width 1 / sigma 0.5 so tiny radii still blur a little).
inline std::vector<float> BuildGaussianKernel(float radius)
{
    int half = (int)std::ceil((double)radius);
    if (half < 1) half = 1;
    const double sigma = radius > 1.f ? (double)radius * 0.5 : 0.5;
    const double inv2s2 = 1.0 / (2.0 * sigma * sigma);

    std::vector<float> k((size_t)half * 2 + 1);
    double sum = 0.0;
    for (int i = -half; i <= half; ++i) {
        const double v = std::exp(-(double)(i * i) * inv2s2);
        k[(size_t)(i + half)] = (float)v;
        sum += v;
    }
    const float inv = (float)(1.0 / sum);
    for (auto &v : k) v *= inv;
    return k;
}

// Horizontal pass over a planar W*H float image, clamp-to-edge.
// src and dst must not alias.
inline void GaussianBlurH(const float *src, float *dst, int W, int H,
                          const std::vector<float> &k)
{
    const int half = (int)k.size() / 2;
    const float *kd = k.data();
    // Rows are independent — each output row reads only its own source row.
    // Within a row, split off the clamped borders so the interior runs a
    // branch-free tap loop. Summation order (i = -half..half) is unchanged, so
    // the result is bit-for-bit identical to the naive clamped loop — the
    // Stage 5 "must not change output values" invariant holds.
    ParallelFor(H, [=](int /*threadIndex*/, int y) {
        const float *row = src + (size_t)y * W;
        float       *out = dst + (size_t)y * W;
        const int xi0 = half;              // first interior x with full support
        const int xi1 = W - 1 - half;      // last interior x
        // Left border (and, for narrow rows, the whole row) — clamped.
        const int leftEnd = (xi0 <= xi1) ? xi0 : W;
        for (int x = 0; x < leftEnd; ++x) {
            double acc = 0.0;
            for (int i = -half; i <= half; ++i) {
                int sx = x + i;
                if (sx < 0)  sx = 0;
                if (sx >= W) sx = W - 1;
                acc += (double)row[sx] * (double)kd[i + half];
            }
            out[x] = (float)acc;
        }
        // Interior — no clamp needed (x+i always in [0,W)).
        for (int x = xi0; x <= xi1; ++x) {
            double acc = 0.0;
            const float *p = row + (x - half);
            for (int i = 0; i <= 2 * half; ++i)
                acc += (double)p[i] * (double)kd[i];
            out[x] = (float)acc;
        }
        // Right border — clamped.
        for (int x = (xi0 <= xi1) ? xi1 + 1 : W; x < W; ++x) {
            double acc = 0.0;
            for (int i = -half; i <= half; ++i) {
                int sx = x + i;
                if (sx < 0)  sx = 0;
                if (sx >= W) sx = W - 1;
                acc += (double)row[sx] * (double)kd[i + half];
            }
            out[x] = (float)acc;
        }
    });
}

// Vertical pass over a planar W*H float image, clamp-to-edge.
// src and dst must not alias.
inline void GaussianBlurV(const float *src, float *dst, int W, int H,
                          const std::vector<float> &k)
{
    const int half = (int)k.size() / 2;
    const float *kd = k.data();
    // Each output row y reads only source rows in [y-half, y+half] and writes
    // its own row — output rows are disjoint, so parallelizing over y is safe
    // (src and dst must not alias, per the contract). Interior rows
    // (y in [half, H-1-half]) need no vertical clamp, so their tap loop is
    // branch-free and streams contiguous source rows (cache-friendly). Border
    // rows keep the clamped form. Per-output-pixel summation order (i =
    // -half..half) is identical to the naive loop → bit-for-bit same output.
    ParallelFor(H, [=](int /*threadIndex*/, int y) {
        float *out = dst + (size_t)y * W;
        const bool interior = (y >= half && y <= H - 1 - half);
        if (interior) {
            const float *base = src + (size_t)(y - half) * W;   // row y-half
            for (int x = 0; x < W; ++x) {
                double acc = 0.0;
                const float *p = base + x;
                for (int i = 0; i <= 2 * half; ++i)
                    acc += (double)p[(size_t)i * W] * (double)kd[i];
                out[x] = (float)acc;
            }
        } else {
            for (int x = 0; x < W; ++x) {
                double acc = 0.0;
                for (int i = -half; i <= half; ++i) {
                    int sy = y + i;
                    if (sy < 0)  sy = 0;
                    if (sy >= H) sy = H - 1;
                    acc += (double)src[(size_t)sy * W + x] * (double)kd[i + half];
                }
                out[x] = (float)acc;
            }
        }
    });
}

// ── Region-limited passes ────────────────────────────────────────────────
// Same clamp-to-edge Gaussian, but the OUTPUT is written only inside the
// half-open rectangle [x0,x1) x [y0,y1); pixels outside are left untouched.
// Source reads still clamp against the FULL image (W,H), so a pixel inside the
// rectangle gets EXACTLY the same value it would in the full-frame pass —
// bit-for-bit. This is what lets Edge Repair blur only the Detail blend band's
// bounding box (plus kernel margin) and skip the rest of a 4K frame without
// changing any pixel the composite actually reads. The per-output-pixel tap
// order is identical to the full-frame functions above.
//
// Rectangle is clamped to the image; an empty/degenerate rect is a no-op.
inline void GaussianBlurH_Rect(const float *src, float *dst, int W, int H,
                               const std::vector<float> &k,
                               int x0, int x1, int y0, int y1)
{
    const int half = (int)k.size() / 2;
    const float *kd = k.data();
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W; if (y1 > H) y1 = H;
    if (x0 >= x1 || y0 >= y1) return;
    const int rx0 = x0, rx1 = x1;
    ParallelFor(y1 - y0, [=](int /*t*/, int yy) {
        const int y = y0 + yy;
        const float *row = src + (size_t)y * W;
        float       *out = dst + (size_t)y * W;
        for (int x = rx0; x < rx1; ++x) {
            double acc = 0.0;
            for (int i = -half; i <= half; ++i) {
                int sx = x + i;
                if (sx < 0)  sx = 0;
                if (sx >= W) sx = W - 1;
                acc += (double)row[sx] * (double)kd[i + half];
            }
            out[x] = (float)acc;
        }
    });
}

inline void GaussianBlurV_Rect(const float *src, float *dst, int W, int H,
                               const std::vector<float> &k,
                               int x0, int x1, int y0, int y1)
{
    const int half = (int)k.size() / 2;
    const float *kd = k.data();
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W; if (y1 > H) y1 = H;
    if (x0 >= x1 || y0 >= y1) return;
    const int rx0 = x0, rx1 = x1;
    ParallelFor(y1 - y0, [=](int /*t*/, int yy) {
        const int y = y0 + yy;
        float *out = dst + (size_t)y * W;
        for (int x = rx0; x < rx1; ++x) {
            double acc = 0.0;
            for (int i = -half; i <= half; ++i) {
                int sy = y + i;
                if (sy < 0)  sy = 0;
                if (sy >= H) sy = H - 1;
                acc += (double)src[(size_t)sy * W + x] * (double)kd[i + half];
            }
            out[x] = (float)acc;
        }
    });
}

} // namespace mm
