/*  ModernEdgeBlurKernels.cu — CUDA wrappers for Modern Edge Blur.

    THE SHIPPING GPU BACKEND. AE 2026 on Windows/NVIDIA offers effects
    what_gpu = CUDA (measured 2026-07-23, ../../docs/AE_GPU_Effects.md §1.1),
    and §1.2 makes CUDA the backend this suite maintains. Built to PTX by nvcc
    (CMake, when the toolkit is present) into
    CUDA_Assets/ModernEdgeBlur_Kernels.ptx next to the .aex, loaded at
    PF_Cmd_GPU_DEVICE_SETUP via cuModuleLoadDataEx. No toolkit at build time ->
    no PTX -> device setup declines quietly -> AE renders on the CPU.

    THIN WRAPPERS ONLY: entry decorations, the bounds guard, and stores. Every
    per-pixel formula lives in MEB_KernelMath.h and every pass body in
    MEB_KernelPasses.h — see the lockstep contract in those headers. The param
    struct must match MEB_GPU_PassParams in src/ModernEdgeBlur_GPU.cpp and
    cbuffer MEBPassParams in ModernEdgeBlurKernels.hlsl FIELD FOR FIELD.

    ── The pipeline these entries make up ───────────────────────────────────
    Mirrors SmartRender's CPU pipeline pass for pass (ModernEdgeBlur.cpp):

        MEB_ExtractAlpha                     alpha plane from the source world
        MEB_Morph            [Shift Edge]    circular min/max on that plane
        MEB_Passthrough      [blur=0,blend=0] straight out, RGB zeroed at a<=0
        MEB_Derive                           Blur Amount's premultiplied input
        MEB_BlurHRGBA / MEB_BlurVRGBA        [Blur Amount] + unpremultiply
        MEB_DirMin x4 / MEB_CopyPlane        [Blend Radius] octagon erosion
        MEB_BlurHPlane / MEB_BlurVPlane      [Blend Radius] feather
        MEB_Composite                        alpha-weighted blend -> output

    There is no host-side readback anywhere in it: unlike Edge Repair, no pass
    here asks a question between passes, so no device flags buffer is needed
    (../../docs/AE_GPU_Effects.md §4, "Branching when you cannot read back").
    Every branch is a PARAMETER decision the host already knows from the
    sliders, taken once when the pass sequence is built.

    ── Buffer conventions ───────────────────────────────────────────────────
    WORLDS (src/dst) are BGRA128: 4 x float32, **B,G,R,A** order, row pitch
    p.srcPitchF / p.dstPitchF in FLOATS.

    PLANES (alpha, weight, tmp) are TIGHTLY PACKED at pitch == p.W, one float
    per pixel. The two interleaved RGBA planes (Blur Amount's ping-pong) are
    packed at pitch == p.W, 4 floats per pixel in **A,R,G,B** order.
    Do not address either with a world pitch.
*/

#define MEB_GPU_CUDA 1
#include "MEB_KernelPasses.h"

/* Matches MEB_GPU_PassParams (C++) / cbuffer MEBPassParams (HLSL). Passed by
   value at launch (driver API: kernel params), so the layout must stay POD and
   field-identical across all three. 20 x 4 bytes = 80, laid out in 5 x 16-byte
   groups so an HLSL cbuffer packs identically. */
struct MEB_PassParamsCu
{
    /* worlds */
    int   srcPitchF;    /* source world row pitch, in FLOATS (rowbytes / 4) */
    int   dstPitchF;    /* output world row pitch, in FLOATS */
    int   srcW;         /* source world dims — reads are clamped into these */
    int   srcH;

    /* planes and dispatch grid */
    int   W;            /* output world dims == scratch plane dims */
    int   H;
    int   count;        /* element count for the 1-D kernels */
    int   half;         /* Gaussian half-width (taps = 2*half + 1) */

    /* pass-local geometry */
    int   dirX;         /* directional-min step */
    int   dirY;
    int   halfLen;      /* directional-min half length */
    int   premult;      /* MEB_Derive: multiply RGB by alpha */

    int   unpremult;    /* MEB_BlurVRGBA: divide RGB by the blurred alpha */
    int   dilate;       /* MEB_Morph: 1 = max filter (grow), 0 = min (shrink) */
    int   morphRi;      /* MEB_Morph: ceil(|Shift Edge|) */
    int   pad0;

    float morphR2;      /* MEB_Morph: |Shift Edge|^2 */
    float pad1;
    float pad2;
    float pad3;
};

/* Standard 2-D pixel guard + coordinates. Every image-shaped kernel opens with
   this; the grid is always ceil(dim/16) blocks of 16x16. Kept explicit rather
   than relying on an exact-fit dispatch so the same bodies stay correct under
   a backend whose dispatch is non-uniform (gpu-kernel-rules.md §4). */
#define MEB_TID_2D(wVar, hVar)                                        \
    const int x = (int)(blockIdx.x * blockDim.x + threadIdx.x);        \
    const int y = (int)(blockIdx.y * blockDim.y + threadIdx.y);        \
    if (x >= (wVar) || y >= (hVar)) return

/* Store one ARGB texel into a tightly-packed interleaved plane. */
__device__ __forceinline__ void MEB_StorePlane4(float *plane, int W,
                                                int x, int y, MEB_RGBA v)
{
    const uint i = MEB_PlaneIndex4((uint)x, (uint)y, (uint)W);
    plane[i + 0u] = v.a;
    plane[i + 1u] = v.r;
    plane[i + 2u] = v.g;
    plane[i + 3u] = v.b;
}

/* ==========================================================================
   Ingest
   ========================================================================== */

/* Alpha plane from the BGRA128 source world. Every later stage reads alpha
   from this tightly-packed plane instead of striding through the world. */
extern "C" __global__ void MEB_ExtractAlpha(
    const float* __restrict__ src,
    float*       __restrict__ alpha,
    MEB_PassParamsCu          p)
{
    MEB_TID_2D(p.W, p.H);
    alpha[MEB_PlaneIndex((uint)x, (uint)y, (uint)p.W)] =
        MEB_PassExtractAlpha(src, p.srcPitchF, p.srcW, p.srcH, x, y);
}

/* Phase 1 — Shift Edge. Reads the extracted alpha plane, writes a second one;
   the CPU shifts alpha in place inside sharp_buf, which a dispatch cannot do.

   Ordering matters and is the host's job: everything downstream (the zero-RGB
   cliff, the Blur Amount input, the Blend Radius erosion) must key off the
   POST-shift alpha, or a grow leaves newly-revealed pixels black and a shrink
   leaves a ring of un-zeroed colour. */
extern "C" __global__ void MEB_Morph(
    const float* __restrict__ in,
    float*       __restrict__ out,
    MEB_PassParamsCu          p)
{
    MEB_TID_2D(p.W, p.H);
    out[MEB_PlaneIndex((uint)x, (uint)y, (uint)p.W)] =
        MEB_PassMorph(in, p.W, p.H, x, y, p.morphRi, p.morphR2, p.dilate);
}

/* Blur Amount's input plane: straight colour with the a<=0 cliff zeroed, then
   honestly premultiplied when a blur will run (p.premult). */
extern "C" __global__ void MEB_Derive(
    const float* __restrict__ src,
    const float* __restrict__ alpha,
    float*       __restrict__ out,
    MEB_PassParamsCu          p)
{
    MEB_TID_2D(p.W, p.H);
    MEB_StorePlane4(out, p.W, x, y,
        MEB_PassDerive(src, p.srcPitchF, p.srcW, p.srcH,
                       alpha, p.W, x, y, p.premult));
}

/* Blur Amount = 0 AND Blend Radius = 0: the effect does nothing but enforce
   the zero-RGB cliff (and Shift Edge, if it ran). Same output the CPU path's
   passthrough packs — NOT a raw copy, which would keep held RGB the CPU
   discards. */
extern "C" __global__ void MEB_Passthrough(
    const float* __restrict__ src,
    const float* __restrict__ alpha,
    float*       __restrict__ dst,
    MEB_PassParamsCu          p)
{
    MEB_TID_2D(p.W, p.H);
    MEB_StoreWorldARGB(dst, p.dstPitchF, x, y,
        MEB_PassSharp(src, p.srcPitchF, p.srcW, p.srcH, alpha, p.W, x, y));
}

/* ==========================================================================
   Phase 2 — Blur Amount (separable Gaussian over interleaved ARGB)
   ========================================================================== */

extern "C" __global__ void MEB_BlurHRGBA(
    const float* __restrict__ in,
    float*       __restrict__ out,
    const float* __restrict__ k,
    MEB_PassParamsCu          p)
{
    MEB_TID_2D(p.W, p.H);
    MEB_StorePlane4(out, p.W, x, y,
        MEB_PassBlurHRGBA(in, p.W, p.H, x, y, k, p.half));
}

/* The vertical pass also carries Phase 2's unpremultiply (p.unpremult), folded
   into its tail exactly as the CPU folds it into MEB_BlurColsRGBA. */
extern "C" __global__ void MEB_BlurVRGBA(
    const float* __restrict__ in,
    float*       __restrict__ out,
    const float* __restrict__ k,
    MEB_PassParamsCu          p)
{
    MEB_TID_2D(p.W, p.H);
    MEB_StorePlane4(out, p.W, x, y,
        MEB_PassBlurVRGBA(in, p.W, p.H, x, y, k, p.half, p.unpremult));
}

/* ==========================================================================
   Phase 3 — Blend Radius (octagon erosion, then feather)
   ========================================================================== */

/* One of the four directional min passes. The host issues (1,0), (0,1), (1,1),
   (1,-1) in that order with the axis/diagonal half-lengths from
   MEB_OctagonAxisHalf / MEB_OctagonDiagHalf. */
extern "C" __global__ void MEB_DirMin(
    const float* __restrict__ in,
    float*       __restrict__ out,
    MEB_PassParamsCu          p)
{
    MEB_TID_2D(p.W, p.H);
    out[MEB_PlaneIndex((uint)x, (uint)y, (uint)p.W)] =
        MEB_PassDirMin(in, p.W, p.H, x, y, p.dirX, p.dirY, p.halfLen);
}

extern "C" __global__ void MEB_BlurHPlane(
    const float* __restrict__ in,
    float*       __restrict__ out,
    const float* __restrict__ k,
    MEB_PassParamsCu          p)
{
    MEB_TID_2D(p.W, p.H);
    out[MEB_PlaneIndex((uint)x, (uint)y, (uint)p.W)] =
        MEB_PassBlurHPlane(in, p.W, p.H, x, y, k, p.half);
}

extern "C" __global__ void MEB_BlurVPlane(
    const float* __restrict__ in,
    float*       __restrict__ out,
    const float* __restrict__ k,
    MEB_PassParamsCu          p)
{
    MEB_TID_2D(p.W, p.H);
    out[MEB_PlaneIndex((uint)x, (uint)y, (uint)p.W)] =
        MEB_PassBlurVPlane(in, p.W, p.H, x, y, k, p.half);
}

/* Plane copy — the GPU spelling of the CPU's memcpy early-outs: a
   DirectionalMinFilter with halfLen <= 0 (the octagon's diagonal step below
   radius ~1.7) and an OctagonErode below radius 0.5. Keeping them as a real
   pass means the pass sequence, not the kernel, decides. */
extern "C" __global__ void MEB_CopyPlane(
    const float* __restrict__ in,
    float*       __restrict__ out,
    MEB_PassParamsCu          p)
{
    const int i = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    if (i >= p.count) return;
    out[i] = in[i];
}

/* ==========================================================================
   Phase 4 — composite
   ========================================================================== */

extern "C" __global__ void MEB_Composite(
    const float* __restrict__ src,
    const float* __restrict__ alpha,
    const float* __restrict__ blurred,
    const float* __restrict__ weight,
    float*       __restrict__ dst,
    MEB_PassParamsCu          p)
{
    MEB_TID_2D(p.W, p.H);
    MEB_StoreWorldARGB(dst, p.dstPitchF, x, y,
        MEB_PassComposite(src, p.srcPitchF, p.srcW, p.srcH,
                          alpha, blurred, weight, p.W, x, y));
}
