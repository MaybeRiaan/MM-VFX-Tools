/*  MEB_KernelPasses.h — Modern Edge Blur GPU pass bodies, SHARED.

    Companion to MEB_KernelMath.h. That header holds the per-pixel FORMULAS;
    this one holds the pass bodies that walk buffers — tap loops,
    neighbourhood scans, world addressing and the BGRA<->ARGB swizzle. Split in
    two because these functions take `const float*` parameters, which HLSL has
    no equivalent for; the .hlsl wrapper includes only MEB_KernelMath.h, so fxc
    keeps type-checking the formula layer on every build.

    BRINGING UP DX12 OR METAL means adding MEB_RBUF / MEB_WBUF / MEB_LOAD /
    MEB_STORE macros to MEB_KernelMath.h's shim block (HLSL:
    RWByteAddressBuffer + asfloat(Load(i*4)); Metal: `device const float*`) and
    respelling the buffer accesses below through them. The MATH needs no port —
    that is the whole point of the split. Do not fork these bodies per backend.

    ONE FUNCTION PER OUTPUT ELEMENT. Every pass below computes exactly one
    output pixel/texel from its (x, y). The CUDA wrapper is then a two-line
    entry (bounds guard + call + store), and tests/test_gpuparity.cpp drives the
    identical functions from a plain host loop — so the test exercises the code
    the GPU runs, not a paraphrase of it.

    ── Buffer conventions ───────────────────────────────────────────────────
    WORLDS (src/dst) are BGRA128: 4 x float32 in **B,G,R,A** order, row pitch
    `srcPitchF` / `dstPitchF` in FLOATS. Index .z/.y/.x for R/G/B and .w for
    alpha. The CPU path's ARGB UnpackRow does not exist here; the swizzle
    happens in this file and nowhere else.

    PLANES (alpha, weight, tmp) are TIGHTLY PACKED at pitch == W, one float per
    pixel. The interleaved RGBA planes (the Blur Amount ping-pong) are packed at
    pitch == W with 4 floats per pixel, in **A,R,G,B** order — the CPU's
    internal convention, so MEB_CoreMath.h's formulas transfer unchanged.

    W,H are the OUTPUT world's dimensions and the scratch planes' dimensions.
    Modern Edge Blur does not inflate its checkout request, so the source world
    is the same size and origin — the same assumption the CPU path makes
    (MEB_IngestRow indexes src with the output's W). Source reads are still
    clamped to srcW/srcH: identical in the normal case, and a bad rect becomes
    a duplicated edge pixel rather than a device-memory overrun.

    LOCKSTEP CONTRACT: see MEB_KernelMath.h. Every tap loop below walks
    t = -half .. +half in that order and accumulates in float, because
    src/core/MEB_CoreMath.h does; reassociating would break the `==` gate.
*/

#ifndef MEB_KERNEL_PASSES_H
#define MEB_KERNEL_PASSES_H

#include "MEB_KernelMath.h"

/* ------------------------------------------------------------------------- */
/* World access (the only place BGRA is spoken)                              */
/* ------------------------------------------------------------------------- */

/* Read one source-world pixel as ARGB, with coordinates clamped into the
   world. <- UnpackRow's PF_PixelFormat_ARGB128 branch, minus the AE types. */
MEB_FN MEB_RGBA MEB_LoadWorldARGB(const float *src, int srcPitchF,
                                  int srcW, int srcH, int x, int y)
{
    const int cx = MEB_ClampI(x, 0, srcW - 1);
    const int cy = MEB_ClampI(y, 0, srcH - 1);
    const uint s = MEB_PixelFloatIndex((uint)cx, (uint)cy, (uint)srcPitchF);
    /* world is BGRA: [0]=B [1]=G [2]=R [3]=A */
    return MEB_MakeRGBA(src[s + 3u], src[s + 2u], src[s + 1u], src[s + 0u]);
}

/* Write one ARGB texel into the output world as BGRA. <- PackRow's
   PF_PixelFormat_ARGB128 branch, which does not clamp (the 8/16 bpc branches
   do, and those never see the GPU path — AE hands GPU effects float worlds). */
MEB_FN void MEB_StoreWorldARGB(float *dst, int dstPitchF, int x, int y,
                               MEB_RGBA v)
{
    const uint d = MEB_PixelFloatIndex((uint)x, (uint)y, (uint)dstPitchF);
    dst[d + 0u] = v.b;
    dst[d + 1u] = v.g;
    dst[d + 2u] = v.r;
    dst[d + 3u] = v.a;
}

/* ------------------------------------------------------------------------- */
/* Ingest                                                                    */
/* ------------------------------------------------------------------------- */

/* The alpha plane every later stage reads instead of striding through the
   world. Extracted BEFORE Shift Edge, which then rewrites it into a second
   plane (see MEB_PassMorph) — the CPU shifts alpha in place inside sharp_buf;
   the GPU cannot read and write one plane in the same dispatch, so it
   ping-pongs. Same values. */
MEB_FN float MEB_PassExtractAlpha(const float *src, int srcPitchF,
                                  int srcW, int srcH, int x, int y)
{
    return MEB_LoadWorldARGB(src, srcPitchF, srcW, srcH, x, y).a;
}

/* Phase 1 — circular morphological min/max on the alpha plane. <- MorphPlane.
   `ri` and `r2` are computed on the host from the SAME expressions the CPU
   uses (ceil(|radius|), |radius|^2) so the structuring element cannot differ.

   Cost note: this is O(radius^2) per pixel by construction — it is the exact
   circular filter, kept because "simple choker" behaviour is a deliberate
   design choice for Shift Edge, not a quality gap (NEXT_STEPS.md Priority 2).
   At Shift Edge 50 that is ~7850 taps/pixel. Still far cheaper than the CPU's
   serial version, which is why the GPU path runs it rather than declining. */
MEB_FN float MEB_PassMorph(const float *in, int W, int H, int x, int y,
                           int ri, float r2, int dilate)
{
    float best = MEB_MorphSeed(dilate);
    for (int dy = -ri; dy <= ri; ++dy) {
        for (int dx = -ri; dx <= ri; ++dx) {
            if (MEB_MorphInside(dx, dy, r2) == 0) continue;
            const int sx = MEB_ClampI(x + dx, 0, W - 1);
            const int sy = MEB_ClampI(y + dy, 0, H - 1);
            best = MEB_MorphKeep(best, in[MEB_PlaneIndex((uint)sx, (uint)sy, (uint)W)],
                                 dilate);
        }
    }
    return best;
}

/* The sharp (top) layer's texel: held colour, RGB zeroed at the a<=0 cliff,
   alpha taken from the (post-Shift-Edge) alpha plane. The CPU materialises
   this as sharp_buf; recomputing it here from the world plus the alpha plane
   is what lets the GPU skip a full-frame RGBA scratch plane. */
MEB_FN MEB_RGBA MEB_PassSharp(const float *src, int srcPitchF, int srcW, int srcH,
                              const float *alpha, int W, int x, int y)
{
    const MEB_RGBA w = MEB_LoadWorldARGB(src, srcPitchF, srcW, srcH, x, y);
    const float    a = alpha[MEB_PlaneIndex((uint)x, (uint)y, (uint)W)];
    return MEB_SharpTexel(a, w.r, w.g, w.b);
}

/* The Blur Amount input texel. <- MEB_DeriveRowInline's `bl` output. */
MEB_FN MEB_RGBA MEB_PassDerive(const float *src, int srcPitchF, int srcW, int srcH,
                               const float *alpha, int W, int x, int y, int premult)
{
    const MEB_RGBA w = MEB_LoadWorldARGB(src, srcPitchF, srcW, srcH, x, y);
    const float    a = alpha[MEB_PlaneIndex((uint)x, (uint)y, (uint)W)];
    return MEB_DeriveTexel(a, w.r, w.g, w.b, premult);
}

/* ------------------------------------------------------------------------- */
/* Phase 2 — separable Gaussian over the interleaved ARGB plane              */
/* ------------------------------------------------------------------------- */

/* Horizontal tap loop. <- MEB_BlurRowRGBA. Edge taps are CLAMPED, not skipped
   (see MEB_CoreMath.h's header note) — the weight sum stays 1.0 at the border. */
MEB_FN MEB_RGBA MEB_PassBlurHRGBA(const float *in, int W, int H, int x, int y,
                                  const float *k, int half)
{
    float acc0 = 0.0f, acc1 = 0.0f, acc2 = 0.0f, acc3 = 0.0f;
    for (int t = -half; t <= half; ++t) {
        const uint  i  = MEB_PlaneIndex4((uint)MEB_ClampI(x + t, 0, W - 1),
                                         (uint)y, (uint)W);
        const float kw = k[t + half];
        acc0 += in[i + 0u] * kw; acc1 += in[i + 1u] * kw;
        acc2 += in[i + 2u] * kw; acc3 += in[i + 3u] * kw;
    }
    return MEB_MakeRGBA(acc0, acc1, acc2, acc3);
}

/* Vertical tap loop, with Phase 2's unpremultiply folded into its tail exactly
   as the CPU folds it into MEB_BlurColsRGBA — no separate full-buffer walk. */
MEB_FN MEB_RGBA MEB_PassBlurVRGBA(const float *in, int W, int H, int x, int y,
                                  const float *k, int half, int unpremult)
{
    float acc0 = 0.0f, acc1 = 0.0f, acc2 = 0.0f, acc3 = 0.0f;
    for (int t = -half; t <= half; ++t) {
        const uint  i  = MEB_PlaneIndex4((uint)x,
                                         (uint)MEB_ClampI(y + t, 0, H - 1), (uint)W);
        const float kw = k[t + half];
        acc0 += in[i + 0u] * kw; acc1 += in[i + 1u] * kw;
        acc2 += in[i + 2u] * kw; acc3 += in[i + 3u] * kw;
    }
    const MEB_RGBA v = MEB_MakeRGBA(acc0, acc1, acc2, acc3);
    if (unpremult != 0) return MEB_UnpremultTexel(v);
    return v;
}

/* ------------------------------------------------------------------------- */
/* Phase 3 — octagon erosion + feather over the single-channel weight plane  */
/* ------------------------------------------------------------------------- */

/* One directional 1-D min filter. <- MEB_DirMinRowInline. Four of these in
   sequence — (1,0), (0,1), (1,1), (1,-1) — compose (Minkowski sum) into one
   erosion by an octagon, at O(radius) instead of O(radius^2). */
MEB_FN float MEB_PassDirMin(const float *in, int W, int H, int x, int y,
                            int dx, int dy, int halfLen)
{
    float m = 1.0f;
    for (int t = -halfLen; t <= halfLen; ++t) {
        const int sx = MEB_ClampI(x + t * dx, 0, W - 1);
        const int sy = MEB_ClampI(y + t * dy, 0, H - 1);
        m = MEB_MinKeep(m, in[MEB_PlaneIndex((uint)sx, (uint)sy, (uint)W)]);
    }
    return m;
}

/* Horizontal feather tap loop. <- MEB_BlurRowPlane. */
MEB_FN float MEB_PassBlurHPlane(const float *in, int W, int H, int x, int y,
                                const float *k, int half)
{
    float acc = 0.0f;
    for (int t = -half; t <= half; ++t)
        acc += in[MEB_PlaneIndex((uint)MEB_ClampI(x + t, 0, W - 1), (uint)y, (uint)W)]
             * k[t + half];
    return acc;
}

/* Vertical feather tap loop. <- MEB_BlurColsPlane. */
MEB_FN float MEB_PassBlurVPlane(const float *in, int W, int H, int x, int y,
                                const float *k, int half)
{
    float acc = 0.0f;
    for (int t = -half; t <= half; ++t)
        acc += in[MEB_PlaneIndex((uint)x, (uint)MEB_ClampI(y + t, 0, H - 1), (uint)W)]
             * k[t + half];
    return acc;
}

/* ------------------------------------------------------------------------- */
/* Phase 4 — composite                                                       */
/* ------------------------------------------------------------------------- */

/* Sharp layer (from the world + alpha plane) blended with the blurred layer by
   the feathered weight, alpha-weighted. <- MEB_CompositeRow's loop body.
   Straight colour in, straight colour out. */
MEB_FN MEB_RGBA MEB_PassComposite(const float *src, int srcPitchF, int srcW, int srcH,
                                  const float *alpha, const float *blurred,
                                  const float *weight, int W, int x, int y)
{
    const MEB_RGBA sh = MEB_PassSharp(src, srcPitchF, srcW, srcH, alpha, W, x, y);
    const uint     b  = MEB_PlaneIndex4((uint)x, (uint)y, (uint)W);
    const MEB_RGBA bl = MEB_MakeRGBA(blurred[b + 0u], blurred[b + 1u],
                                     blurred[b + 2u], blurred[b + 3u]);
    const float    w  = weight[MEB_PlaneIndex((uint)x, (uint)y, (uint)W)];
    return MEB_CompositeTexel(sh, bl, w);
}

#endif /* MEB_KERNEL_PASSES_H */
