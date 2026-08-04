/*  MEB_CoreMath.h — Modern Edge Blur's CPU pixel mathematics, AE-free.

    WHAT THIS IS. Every formula the CPU render actually evaluates, lifted out
    of ModernEdgeBlur.cpp so it can be compiled and tested with no After
    Effects SDK in the loop. Nothing here knows about PF_InData, effect worlds,
    iterate_generic, threading or buffer pools — those all stayed in
    ModernEdgeBlur.cpp, unchanged. The callbacks there keep their thread_local
    scratch, their 64/16-column blocking and MEB_Pool exactly as before; only
    the innermost arithmetic became a call into this header.

    WHY IT EXISTS. The GPU path (src/gpu/) has to compute the same numbers as
    the CPU path forever, and a drift between them is silent — it shows up
    months later as "the GPU looks slightly different" and is miserable to
    bisect (docs/AE_GPU_Effects.md §4). tests/test_gpuparity.cpp diffs the
    shared GPU kernel math against THIS header with `==`, not an epsilon: it is
    the same arithmetic in the same order, so anything looser would hide a real
    divergence. That gate is only honest because both sides are the real
    shipping code — hence the lift.

    LOCKSTEP CONTRACT. A formula changes here and in src/gpu/MEB_KernelMath.h /
    MEB_KernelPasses.h in the SAME commit, or not at all. Each function below
    names its GPU twin.

        clampL / clampF          <- MEB_ClampI / MEB_ClampF   (MEB_KernelMath.h)
        MakeBlurKernel           -- host-side only; the GPU uploads the weights
        MakeBlendKernel             this builds, verbatim (see below)
        MorphPlane               <- MEB_PassMorph              (MEB_KernelPasses.h)
        MEB_DeriveRowInline      <- MEB_PassDerive / MEB_PassSharp
        MEB_DirMinRowInline      <- MEB_PassDirMin
        MEB_BlurRowPlane         <- MEB_PassBlurHPlane
        MEB_BlurColsPlane        <- MEB_PassBlurVPlane
        MEB_BlurRowRGBA          <- MEB_PassBlurHRGBA
        MEB_BlurColsRGBA         <- MEB_PassBlurVRGBA
        MEB_CompositePixel       <- MEB_PassComposite

    GAUSSIAN WEIGHTS ARE BUILT ONCE, ON THE HOST. MakeBlurKernel /
    MakeBlendKernel run on the CPU for both paths and the GPU uploads the
    resulting float array verbatim (docs/gpu-kernel-rules.md §3.1). A GPU-side
    rebuild would diverge in the last bits — expf() is not required to match
    std::exp() — and would turn the parity gate into a coin flip.

    EDGE TAPS ARE CLAMPED, NOT SKIPPED. Every kernel here clamps the sample
    coordinate to the buffer edge (clampL) so the weight sum stays 1.0 at the
    border. That is Modern Edge Blur's own convention and differs from
    MM_FreqSep's skip-and-renormalise rule (gpu-kernel-rules.md §3.2); it is
    load-bearing for the blurred-alpha divide in Phase 2 and must be mirrored
    exactly on the GPU, not "fixed".
*/

#pragma once

#ifndef MEB_CORE_MATH_H
#define MEB_CORE_MATH_H

#include <cmath>
#include <cstring>
#include <cstddef>
#include <algorithm>
#include <vector>

/* A_long is int32_t on every platform this suite builds for; spelling it as a
   local typedef is what keeps this header free of the AE SDK. ModernEdgeBlur.cpp
   static_asserts the two agree, so a future SDK change cannot silently widen
   one side of a call. */
typedef int MEB_Long;

/* Restores the aliasing freedom the extracted loops had while they were still
   inline bodies inside the callbacks — MSVC's auto-vectoriser needs it to keep
   the blocked inner loops vectorised, which is where the 2026-07-06 perf pass's
   gains live (NEXT_STEPS.md Priority 3). */
#if defined(_MSC_VER)
    #define MEB_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
    #define MEB_RESTRICT __restrict__
#else
    #define MEB_RESTRICT
#endif

/* Unpremultiply guard: blurred alpha below this is treated as this. Provably
   safe on honestly premultiplied input — blur(c*a)/blur(a) is a convex weighted
   average of true colours, so no amplification is possible — and the clamp only
   bites where RGB is ~0 anyway. Matches the value the shipped BuildCleanSource
   used (commit 84e713f). */
#define MEB_UNPREMULT_EPS 1.0e-4f

/* Composite denominator guard. Below this, both layers' coverage is ~0 and the
   normalised blend is meaningless, so the blurred colour passes through. */
#define MEB_COMPOSITE_EPS 1.0e-6f

/* ------------------------------------------------------------------------- */
/* Clamps                                                                    */
/* ------------------------------------------------------------------------- */

static inline MEB_Long clampL(MEB_Long v, MEB_Long lo, MEB_Long hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float clampF(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ------------------------------------------------------------------------- */
/* Gaussian kernels                                                          */
/*                                                                           */
/* Two kernels, tuned independently, and deliberately NOT shared:            */
/* Blur Amount blurs a picture, Blend Radius erodes a mask.                  */
/* ------------------------------------------------------------------------- */

/* Blur Amount. sigma = radius/2, support = ceil(3*sigma) — support must reach
   ~3 sigma or the clipped tails make the blur read visibly smaller than its
   sigma. (Was radius/4, tuned by eye against the old premult bug whose
   darkening made the blur LOOK wider than it was; with honest colour that
   mapping reads far too small next to Red Giant Edge Blur at the same value.) */
static inline std::vector<float> MakeBlurKernel(float radius)
{
    if (radius < 0.5f) return {};
    const float sigma = radius * 0.5f;
    const int   half  = (int)std::ceil(3.f * sigma);
    const int   size  = 2 * half + 1;
    std::vector<float> k(size);
    float sum = 0.f;
    for (int i = 0; i < size; ++i) {
        float x = (float)(i - half);
        k[i] = std::exp(-(x * x) / (2.f * sigma * sigma));
        sum += k[i];
    }
    for (auto &v : k) v /= sum;
    return k;
}

/* Blend Radius. sigma = radius, support = ceil(radius*3) — wide, aggressive
   edge erosion. */
static inline std::vector<float> MakeBlendKernel(float radius)
{
    if (radius < 0.5f) return {};
    const float sigma = radius;
    const int   half  = (int)std::ceil(radius * 3.f);
    const int   size  = 2 * half + 1;
    std::vector<float> k(size);
    float sum = 0.f;
    for (int i = 0; i < size; ++i) {
        float x = (float)(i - half);
        k[i] = std::exp(-(x * x) / (2.f * sigma * sigma));
        sum += k[i];
    }
    for (auto &v : k) v /= sum;
    return k;
}

/* ------------------------------------------------------------------------- */
/* Phase 1 — morphological shift (Shift Edge)                                */
/* ------------------------------------------------------------------------- */

/* Morphological min/max filter on a single float plane, circular kernel,
   brute-force. dilate=true grows the plane (max filter); dilate=false shrinks
   it (min filter / erosion).

   The circle test is `(float)(dx*dx + dy*dy) > r2` on the INTEGER offsets — the
   GPU twin evaluates the identical comparison rather than deriving a per-row dx
   bound from a sqrt, because a bound would round differently at the exact
   boundary and silently change the structuring element's shape. */
static inline void MorphPlane(const float *in, float *out,
                              MEB_Long W, MEB_Long H, float radius, bool dilate)
{
    const float r = std::abs(radius);
    if (r < 0.5f) {
        std::memcpy(out, in, sizeof(float) * (size_t)W * H);
        return;
    }

    const int   ri = (int)std::ceil(r);
    const float r2 = r * r;

    for (MEB_Long y = 0; y < H; ++y) {
        for (MEB_Long x = 0; x < W; ++x) {
            float best = dilate ? 0.f : 1.f;
            for (int dy = -ri; dy <= ri; ++dy) {
                for (int dx = -ri; dx <= ri; ++dx) {
                    if ((float)(dx*dx + dy*dy) > r2) continue;
                    const MEB_Long sx = clampL(x + dx, 0, W - 1);
                    const MEB_Long sy = clampL(y + dy, 0, H - 1);
                    const float    a  = in[(size_t)sy * W + sx];
                    if (dilate) { if (a > best) best = a; }
                    else        { if (a < best) best = a; }
                }
            }
            out[(size_t)y * W + x] = best;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Ingest / derive                                                           */
/* ------------------------------------------------------------------------- */

/* Zero RGB at the a<=0 cliff and derive optional per-frame buffers from one
   sharp row. blurred/alpha may be null. premult_blurred selects a*rgb (Blur
   Amount active) vs a plain straight copy (blend-only frames).

   The zero-RGB pass exists because requesting preserve_rgb_of_zero_alpha=FALSE
   on checkout only lets AE discard hidden RGB it manages itself — it cannot
   retroactively clean pixels an upstream effect already baked in (e.g. a
   precomp with an internal keyer that only touches alpha). */
static inline void MEB_DeriveRowInline(float *sh, float *bl, float *al,
                                       MEB_Long W, bool premult_blurred)
{
    for (MEB_Long x = 0; x < W; ++x, sh += 4) {
        const float a = sh[0];
        if (a <= 0.f) { sh[1] = 0.f; sh[2] = 0.f; sh[3] = 0.f; }
        if (bl) {
            bl[0] = a;
            if (premult_blurred) { bl[1] = sh[1] * a; bl[2] = sh[2] * a; bl[3] = sh[3] * a; }
            else                 { bl[1] = sh[1];     bl[2] = sh[2];     bl[3] = sh[3];     }
            bl += 4;
        }
        if (al) *al++ = a;
    }
}

/* ------------------------------------------------------------------------- */
/* Phase 3a — octagon erosion component (one directional 1-D min filter)     */
/* ------------------------------------------------------------------------- */

/* 1-D min-filter of half-width halfLen along direction (dx,dy), clamped to the
   image edges, for one output row. Shared by the threaded per-row callback and
   the serial fallback — before the lift those two carried the same loop twice. */
static inline void MEB_DirMinRowInline(const float *MEB_RESTRICT in,
                                       float *MEB_RESTRICT outRow,
                                       MEB_Long W, MEB_Long H, MEB_Long y,
                                       int dx, int dy, int halfLen)
{
    for (MEB_Long x = 0; x < W; ++x) {
        float m = 1.f;
        for (int t = -halfLen; t <= halfLen; ++t) {
            const MEB_Long sx = clampL(x + t * dx, 0, W - 1);
            const MEB_Long sy = clampL(y + t * dy, 0, H - 1);
            const float    a  = in[(size_t)sy * W + sx];
            if (a < m) m = a;
        }
        outRow[x] = m;
    }
}

/* ------------------------------------------------------------------------- */
/* Separable Gaussian — single-channel plane                                 */
/* ------------------------------------------------------------------------- */

/* Horizontal pass over one row: out[x] = sum_t row[clamp(x+t)] * k[t+half].
   Accumulation order is -half..+half and must not be reassociated — the GPU
   twin walks the taps in the same order so the two agree bit for bit. */
static inline void MEB_BlurRowPlane(const float *MEB_RESTRICT row,
                                    float *MEB_RESTRICT out,
                                    MEB_Long W, const float *MEB_RESTRICT k, int half)
{
    for (MEB_Long x = 0; x < W; ++x) {
        float acc = 0.f;
        for (int t = -half; t <= half; ++t)
            acc += row[clampL(x + t, 0, W - 1)] * k[t + half];
        out[x] = acc;
    }
}

/* Vertical pass over one row of a gathered COLUMN BLOCK. `gathered` is
   row-major [y][c] with `nc` floats per row; the caller gathers it so the
   inner loops stay contiguous (one column at a time strides a full row pitch
   per tap — every tap a cache and TLB miss, the disease that made AE's native
   convolve 10-20x slow). The serial fallback passes nc = 1, which is the same
   arithmetic on a single column. */
static inline void MEB_BlurColsPlane(const float *MEB_RESTRICT gathered,
                                     MEB_Long y, MEB_Long H, MEB_Long nc,
                                     const float *MEB_RESTRICT k, int half,
                                     float *MEB_RESTRICT acc)
{
    for (MEB_Long c = 0; c < nc; ++c) acc[c] = 0.f;
    for (int t = -half; t <= half; ++t) {
        const float *p  = gathered + (size_t)clampL(y + t, 0, H - 1) * nc;
        const float  kw = k[t + half];
        for (MEB_Long c = 0; c < nc; ++c) acc[c] += p[c] * kw;
    }
}

/* ------------------------------------------------------------------------- */
/* Separable Gaussian — interleaved 4-channel (Blur Amount)                  */
/*                                                                           */
/* A Gaussian is per-channel-independent, so blurring all four together is    */
/* numerically identical to four single-channel passes — this is purely the   */
/* dispatch/copy saving, not a behaviour change.                             */
/* ------------------------------------------------------------------------- */

/* Horizontal pass over one interleaved ARGB row. */
static inline void MEB_BlurRowRGBA(const float *MEB_RESTRICT row,
                                   float *MEB_RESTRICT out,
                                   MEB_Long W, const float *MEB_RESTRICT k, int half)
{
    for (MEB_Long x = 0; x < W; ++x) {
        float acc0 = 0.f, acc1 = 0.f, acc2 = 0.f, acc3 = 0.f;
        for (int t = -half; t <= half; ++t) {
            const float *p  = row + (size_t)clampL(x + t, 0, W - 1) * 4;
            const float  kw = k[t + half];
            acc0 += p[0] * kw; acc1 += p[1] * kw;
            acc2 += p[2] * kw; acc3 += p[3] * kw;
        }
        float *o = out + (size_t)x * 4;
        o[0] = acc0; o[1] = acc1; o[2] = acc2; o[3] = acc3;
    }
}

/* Vertical pass over one row of a gathered ARGB column block: `nf` floats per
   block row (nf = 4 * columns; the serial fallback passes nf = 4).

   `unpremult` folds Phase 2's unpremultiply into this pass instead of paying a
   separate full-buffer walk: the blur ran in premultiplied space so a
   partial-alpha edge pixel contributes only its coverage-weighted share of
   colour, and the result handed back must be STRAIGHT colour, because AE's
   effect pipeline is straight alpha on both sides and multiplies by our alpha
   itself at composite time. Returning colour*alpha made AE display
   colour*alpha^2 — a dark halo on every source, including the transparency
   grid. */
static inline void MEB_BlurColsRGBA(const float *MEB_RESTRICT gathered,
                                    MEB_Long y, MEB_Long H, MEB_Long nf,
                                    const float *MEB_RESTRICT k, int half,
                                    bool unpremult, float *MEB_RESTRICT acc)
{
    for (MEB_Long j = 0; j < nf; ++j) acc[j] = 0.f;
    for (int t = -half; t <= half; ++t) {
        const float *p  = gathered + (size_t)clampL(y + t, 0, H - 1) * nf;
        const float  kw = k[t + half];
        for (MEB_Long j = 0; j < nf; ++j) acc[j] += p[j] * kw;
    }
    if (unpremult) {
        const MEB_Long nc = nf / 4;
        for (MEB_Long c = 0; c < nc; ++c) {
            const float inv = 1.f / std::max(acc[c * 4 + 0], MEB_UNPREMULT_EPS);
            acc[c * 4 + 1] *= inv;
            acc[c * 4 + 2] *= inv;
            acc[c * 4 + 3] *= inv;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Phase 4 — composite                                                       */
/* ------------------------------------------------------------------------- */

/* Alpha-weighted blend of the sharp (top) and blurred (base) layers, both in
   STRAIGHT colour, producing straight colour plus the blurred alpha:

       out_rgb   = (sh*w*sh_a + bl*(1-w)*bl_a) / (w*sh_a + (1-w)*bl_a)
       out_alpha = bl_a

   The weighting is not cosmetic. A plain lerp by w mixes the zeroed-black
   cliff in just outside the sharp matte — where Blend Radius's feathered mask
   still gives w a small nonzero value but sharp coverage is 0 — drawing a thin
   dark line that grows with Blend Radius. Weighting each layer by (blend
   weight x its own alpha) drops the sharp term exactly where its coverage is
   zero and reduces to the plain lerp in the interior, so it is exact at every
   radius. NO alpha multiply on the result: AE applies alpha itself.

   `w_raw` is the unclamped weight straight out of Phase 3; the [0,1] clamp is
   applied here so the composite pass is the only walk that touches it. */
static inline void MEB_CompositePixel(const float *sh, const float *bl,
                                      float w_raw, float *dst)
{
    const float w     = clampF(w_raw, 0.f, 1.f);
    const float sw    = w * sh[0];
    const float bw    = (1.f - w) * bl[0];
    const float denom = sw + bw;
    dst[0] = bl[0];
    if (denom > MEB_COMPOSITE_EPS) {
        const float norm = 1.f / denom;
        dst[1] = (sh[1] * sw + bl[1] * bw) * norm;
        dst[2] = (sh[2] * sw + bl[2] * bw) * norm;
        dst[3] = (sh[3] * sw + bl[3] * bw) * norm;
    } else {
        dst[1] = bl[1];
        dst[2] = bl[2];
        dst[3] = bl[3];
    }
}

/* ------------------------------------------------------------------------- */
/* Phase 3 geometry — octagon half-lengths                                   */
/*                                                                           */
/* Erosion composes exactly under Minkowski sum, so eroding along 4 directions */
/* in sequence equals eroding once by their combined octagon. Sizing the axis  */
/* half-length as radius*(sqrt(2)-1) and the diagonal as radius*(1-1/sqrt(2))  */
/* makes the octagon reach exactly `radius` along both the axis and 45-degree  */
/* directions. The host computes these once and hands them to both paths, so   */
/* CPU and GPU cannot disagree about the structuring element's size.           */
/* ------------------------------------------------------------------------- */

/* Axis half-length. Clamped to >=1 so any radius past the 0.5 threshold erodes
   by at least one pixel (avoids a dead zone at very low radius, where
   (sqrt(2)-1)*radius would otherwise round down to 0). */
static inline int MEB_OctagonAxisHalf(float radius)
{
    return std::max(1, (int)std::lround(radius * 0.41421356f));   /* sqrt(2)-1 */
}

/* Diagonal half-length. Legitimately 0 below radius ~1.7, which the caller
   turns into a straight copy. */
static inline int MEB_OctagonDiagHalf(float radius)
{
    return (int)std::lround(radius * 0.29289322f);                /* 1-1/sqrt(2) */
}

#endif /* MEB_CORE_MATH_H */
