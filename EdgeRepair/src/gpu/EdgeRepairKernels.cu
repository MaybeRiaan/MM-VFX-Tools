/*  EdgeRepairKernels.cu — CUDA wrappers for Edge Repair.

    THE SHIPPING GPU BACKEND. AE 2026 on Windows/NVIDIA offers effects
    what_gpu = CUDA (measured 2026-07-23); the DX12 twin serves Premiere hosts.
    Built to PTX by nvcc (CMake, when the toolkit is present) into
    CUDA_Assets/EdgeRepair_Kernels.ptx next to the .aex, loaded at
    PF_Cmd_GPU_DEVICE_SETUP via cuModuleLoadDataEx.

    THIN WRAPPER ONLY: entry decorations and buffer plumbing. All per-pixel
    mathematics lives in ER_KernelMath.h — see the lockstep contract there.
    Mirrors EdgeRepairKernels.hlsl / .metal one-to-one; the param struct must
    match ER_GPU_PassParams in src/EdgeRepair_GPU.cpp field for field.

    ── Buffer conventions ───────────────────────────────────────────────────
    WORLDS (src/dst) are BGRA128: 4 x float32, **B,G,R,A order**, row pitch
    p.srcPitchF / p.dstPitchF in FLOATS. The CPU pipeline's ARGB unpack does
    not exist here — read .z/.y/.x for R/G/B and .w for alpha.

    PLANES (alpha, interior, core, dist, chokeField) are TIGHTLY PACKED at
    pitch == p.W, one float per pixel, covering the whole INFLATED source. Do
    not address them with a world pitch.

    The pull–push CHAIN is one buffer holding every mip level end to end, 4
    floats per texel (premult R,G,B, coverage); the host passes each pass's
    level offsets and dimensions in the p.lvlOffFine / p.lvlOffCoarse /
    p.lvlWFine ... fields.
*/

#define ER_GPU_CUDA 1
#include "ER_KernelMath.h"

/* Matches cbuffer ERPassParams (HLSL) / ER_PassParamsMtl (Metal) /
   ER_GPU_PassParams (C++). Passed by value at launch (driver API: kernel
   params), so layout must stay POD and field-identical across all four.
   28 x 4 bytes = 112, laid out in 7 x 16-byte groups for HLSL cbuffer packing. */
struct ER_PassParamsCu
{
    /* worlds */
    int   srcPitchF;      /* inflated source world row pitch, in FLOATS */
    int   dstPitchF;      /* output world row pitch, in FLOATS */
    int   srcOX;          /* output (0,0) sits at source (srcOX, srcOY) */
    int   srcOY;
    int   outW;           /* output world dims (the composite/view grid) */
    int   outH;
    int   W;              /* inflated source dims == scratch plane dims */
    int   H;

    /* pass-local geometry */
    int   lvlOffFine;     /* float offset of the FINE mip level in the chain */
    int   lvlOffCoarse;   /* float offset of the COARSE mip level */
    int   lvlWFine;
    int   lvlHFine;
    int   lvlWCoarse;
    int   lvlHCoarse;
    int   jfaStep;        /* jump-flood step for this pass */
    int   mode;           /* ER_CLASSIFY_* / ER_CORE_MODE_*, pass-dependent */

    /* effect params */
    float erode;
    float extend;
    float softness;
    float detail01;
    float choke;
    float thresh;         /* core-matte alpha threshold (0.5) */
    int   chokeMode;      /* ER_CHOKE_MODE_* */
    int   view;           /* ER_VIEW_M_* */

    int   bandRequested;  /* host: Extend > 0 (field + fill were dispatched) */
    int   grainBlocks;    /* partial-sum slots for ER_GrainFinal */
    int   flagSlot;       /* which ER_FLAG_* this pass reports into */
    int   count;          /* element count for ER_Zero */
};

/* Standard 2-D pixel guard + coordinates. Every image-shaped kernel opens
   with this; the grid is always ceil(dim/16) blocks of 16x16. */
#define ER_TID_2D(wVar, hVar)                                        \
    const int x = (int)(blockIdx.x * blockDim.x + threadIdx.x);       \
    const int y = (int)(blockIdx.y * blockDim.y + threadIdx.y);       \
    if (x >= (wVar) || y >= (hVar)) return

/* ==========================================================================
   Milestone / utility
   ========================================================================== */

/* Raw 4-float pixel copy of the output rect out of the inflated source world.
   Kept as the Extend<=0 && Choke==0 early-out (the CPU path's ER_PassRow) and
   as the first thing to try when the GPU path misbehaves. */
extern "C" __global__ void ER_Passthrough(
    const float* __restrict__ src,
    float*       __restrict__ dst,
    ER_PassParamsCu           p)
{
    ER_TID_2D(p.outW, p.outH);

    const uint srcF = ER_SrcFloatIndex((uint)x, (uint)y,
                                       (uint)p.srcOX, (uint)p.srcOY,
                                       (uint)p.srcPitchF);
    const uint dstF = ER_PixelFloatIndex((uint)x, (uint)y, (uint)p.dstPitchF);

    dst[dstF + 0u] = src[srcF + 0u];
    dst[dstF + 1u] = src[srcF + 1u];
    dst[dstF + 2u] = src[srcF + 2u];
    dst[dstF + 3u] = src[srcF + 3u];
}

/* Zero p.count floats. Used for the flags/misc buffer, whose slots are OR-ed
   into by later kernels and therefore must start clean each frame. */
extern "C" __global__ void ER_Zero(float* __restrict__ buf, ER_PassParamsCu p)
{
    const int i = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    if (i >= p.count) return;
    buf[i] = 0.0f;
}

/* ==========================================================================
   Plane extraction / classification
   ========================================================================== */

/* Alpha plane from the BGRA128 source world. Every later stage reads alpha
   from this tightly-packed plane instead of striding through the world. */
extern "C" __global__ void ER_ExtractAlpha(
    const float* __restrict__ src,
    float*       __restrict__ alpha,
    ER_PassParamsCu           p)
{
    ER_TID_2D(p.W, p.H);
    const uint s = ER_PixelFloatIndex((uint)x, (uint)y, (uint)p.srcPitchF);
    alpha[ER_PlaneIndex((uint)x, (uint)y, (uint)p.W)] = src[s + 3u];
}

/* mask = predicate(alpha) per p.mode, and report "any" into flags[p.flagSlot].
   Mirrors the interior/seed construction loops in EdgeRepair.cpp. */
extern "C" __global__ void ER_Classify(
    const float* __restrict__ alpha,
    float*       __restrict__ mask,
    unsigned int* __restrict__ flags,
    ER_PassParamsCu           p)
{
    ER_TID_2D(p.W, p.H);
    const uint i = ER_PlaneIndex((uint)x, (uint)y, (uint)p.W);
    const float a = alpha[i];

    float v;
    if      (p.mode == ER_CLASSIFY_INTERIOR)   v = a >  p.thresh ? 1.0f : 0.0f;
    else if (p.mode == ER_CLASSIFY_EXTERIOR)   v = a <= p.thresh ? 1.0f : 0.0f;
    else if (p.mode == ER_CLASSIFY_CHOKE_IN)   v = a <= 0.001f   ? 1.0f : 0.0f;
    else                                       v = a >  0.5f     ? 1.0f : 0.0f;

    mask[i] = v;
    if (v > 0.5f) atomicOr(&flags[p.flagSlot], 1u);
}

/* core = ER_CoreTest(interior, dist, erode, mode). `dist` is the erode field
   (distance to the nearest exterior pixel for SHRINK, to the nearest interior
   pixel for GROW) and is ignored in COPY mode. */
extern "C" __global__ void ER_MakeCore(
    const float* __restrict__ interior,
    const float* __restrict__ dist,
    float*       __restrict__ core,
    unsigned int* __restrict__ flags,
    ER_PassParamsCu           p)
{
    ER_TID_2D(p.W, p.H);
    const uint i = ER_PlaneIndex((uint)x, (uint)y, (uint)p.W);
    /* COPY mode never consults a distance — don't even read it, so a render
       that skipped the erode flood cannot fault on an unbuilt field. */
    const float d = (p.mode == ER_CORE_MODE_COPY) ? 0.0f : dist[i];
    const float v = ER_CoreTest(interior[i], d, p.erode, p.mode);
    core[i] = v;
    if (v > 0.5f) atomicOr(&flags[ER_FLAG_CORE], 1u);
}

/* ==========================================================================
   Jump-flood distance (JFA+2)
   ========================================================================== */

/* Seed from an alpha predicate — fuses the CPU's "build a seed mask" loop into
   the flood's init so no temporary mask plane is needed. */
extern "C" __global__ void ER_JfaInitAlpha(
    const float* __restrict__ alpha,
    unsigned int* __restrict__ seed,
    unsigned int* __restrict__ flags,
    ER_PassParamsCu           p)
{
    ER_TID_2D(p.W, p.H);
    const uint i = ER_PlaneIndex((uint)x, (uint)y, (uint)p.W);
    const float a = alpha[i];

    bool isSeed;
    if      (p.mode == ER_CLASSIFY_INTERIOR)   isSeed = a >  p.thresh;
    else if (p.mode == ER_CLASSIFY_EXTERIOR)   isSeed = a <= p.thresh;
    else if (p.mode == ER_CLASSIFY_CHOKE_IN)   isSeed = a <= 0.001f;
    else                                       isSeed = a >  0.5f;

    seed[i] = isSeed ? ER_PackSeed((uint)x, (uint)y) : ER_SEED_INVALID;
    if (isSeed) atomicOr(&flags[p.flagSlot], 1u);
}

/* Seed from an existing 0/1 plane (the core matte drives the main field). */
extern "C" __global__ void ER_JfaInitMask(
    const float* __restrict__ mask,
    unsigned int* __restrict__ seed,
    ER_PassParamsCu           p)
{
    ER_TID_2D(p.W, p.H);
    const uint i = ER_PlaneIndex((uint)x, (uint)y, (uint)p.W);
    seed[i] = mask[i] > 0.5f ? ER_PackSeed((uint)x, (uint)y) : ER_SEED_INVALID;
}

/* One flood pass: keep the nearest of the 9 candidates at ±p.jfaStep. */
extern "C" __global__ void ER_JfaStep(
    const unsigned int* __restrict__ seedIn,
    unsigned int* __restrict__ seedOut,
    ER_PassParamsCu           p)
{
    ER_TID_2D(p.W, p.H);
    const uint i = ER_PlaneIndex((uint)x, (uint)y, (uint)p.W);

    uint  best  = seedIn[i];
    float bestD = ER_SeedDist2(best, x, y);

    for (int dy = -1; dy <= 1; ++dy) {
        const int sy = y + dy * p.jfaStep;
        if (sy < 0 || sy >= p.H) continue;
        for (int dx = -1; dx <= 1; ++dx) {
            const int sx = x + dx * p.jfaStep;
            if (sx < 0 || sx >= p.W) continue;
            const uint  cand = seedIn[ER_PlaneIndex((uint)sx, (uint)sy, (uint)p.W)];
            const float d    = ER_SeedDist2(cand, x, y);
            if (d < bestD) { bestD = d; best = cand; }
        }
    }
    seedOut[i] = best;
}

/* Nearest-seed coordinates -> Euclidean distance plane. */
extern "C" __global__ void ER_JfaResolve(
    const unsigned int* __restrict__ seed,
    float*       __restrict__ dist,
    ER_PassParamsCu           p)
{
    ER_TID_2D(p.W, p.H);
    const uint i = ER_PlaneIndex((uint)x, (uint)y, (uint)p.W);
    dist[i] = ER_Dist2ToDist(ER_SeedDist2(seed[i], x, y));
}

/* ==========================================================================
   Pull–push fill  (mirrors er::PullPushFill)
   ========================================================================== */

/* Level 0: trusted pixels contribute (straight RGB * alpha, alpha) — the
   INTERNAL premultiply that makes every later average and bilinear fetch
   coverage-weighted. Everything else is zero, so invalid pixels cannot leak
   colour. Reports ER_FLAG_FILL, the GPU stand-in for PullPushFill's
   "no usable valid pixel" return. */
extern "C" __global__ void ER_PPInit(
    const float* __restrict__ src,
    const float* __restrict__ core,
    float*       __restrict__ chain,
    unsigned int* __restrict__ flags,
    ER_PassParamsCu           p)
{
    ER_TID_2D(p.W, p.H);
    const uint i = ER_PlaneIndex((uint)x, (uint)y, (uint)p.W);
    const uint s = ER_PixelFloatIndex((uint)x, (uint)y, (uint)p.srcPitchF);

    const float a = src[s + 3u];
    float* t = chain + (size_t)i * 4u;      /* level 0 sits at offset 0 */

    if (ER_PPValid(core[i], a) > 0.5f) {
        t[0] = src[s + 2u] * a;             /* world is BGRA: .z is R */
        t[1] = src[s + 1u] * a;
        t[2] = src[s + 0u] * a;
        t[3] = a;
        atomicOr(&flags[ER_FLAG_FILL], 1u);
    } else {
        t[0] = 0.0f; t[1] = 0.0f; t[2] = 0.0f; t[3] = 0.0f;
    }
}

/* PULL: each coarse texel = plain average of its up-to-4 children. The divide
   is by the number of children that EXIST (2 or 1 at an odd edge), matching
   the CPU — dividing by a constant 4 would darken the last row/column. */
extern "C" __global__ void ER_PPPull(float* __restrict__ chain,
                                     ER_PassParamsCu p)
{
    ER_TID_2D(p.lvlWCoarse, p.lvlHCoarse);

    float acc0 = 0.0f, acc1 = 0.0f, acc2 = 0.0f, acc3 = 0.0f;
    int   n = 0;
    for (int dy = 0; dy < 2; ++dy) {
        const int fy = y * 2 + dy;
        if (fy >= p.lvlHFine) continue;
        for (int dx = 0; dx < 2; ++dx) {
            const int fx = x * 2 + dx;
            if (fx >= p.lvlWFine) continue;
            const float* s = chain + p.lvlOffFine +
                             ((size_t)fy * p.lvlWFine + fx) * 4u;
            acc0 += s[0]; acc1 += s[1]; acc2 += s[2]; acc3 += s[3];
            ++n;
        }
    }
    float* t = chain + p.lvlOffCoarse + ((size_t)y * p.lvlWCoarse + x) * 4u;
    t[0] = ER_PPAverage(acc0, n);
    t[1] = ER_PPAverage(acc1, n);
    t[2] = ER_PPAverage(acc2, n);
    t[3] = ER_PPAverage(acc3, n);
}

/* PUSH: fill ONLY coverage-0 fine texels with a clamped bilinear sample of the
   coarser level; texels carrying any coverage are left untouched. Interpolation
   happens on the PREMULTIPLIED tuple, so low-coverage neighbours contribute
   proportionally less colour — that is what stops bleed. */
extern "C" __global__ void ER_PPPush(float* __restrict__ chain,
                                     ER_PassParamsCu p)
{
    ER_TID_2D(p.lvlWFine, p.lvlHFine);

    float* t = chain + p.lvlOffFine + ((size_t)y * p.lvlWFine + x) * 4u;
    if (t[3] > 0.0f) return;                 /* has trusted data — untouched */

    const float su = (float)p.lvlWCoarse / (float)p.lvlWFine;
    const float sv = (float)p.lvlHCoarse / (float)p.lvlHFine;

    int x0, x1, y0, y1; float fu, fv;
    ER_PPBilinearAxis(ER_PPPushCoord(x, su), p.lvlWCoarse, x0, x1, fu);
    ER_PPBilinearAxis(ER_PPPushCoord(y, sv), p.lvlHCoarse, y0, y1, fv);

    const float* c = chain + p.lvlOffCoarse;
    const float* p00 = c + ((size_t)y0 * p.lvlWCoarse + x0) * 4u;
    const float* p10 = c + ((size_t)y0 * p.lvlWCoarse + x1) * 4u;
    const float* p01 = c + ((size_t)y1 * p.lvlWCoarse + x0) * 4u;
    const float* p11 = c + ((size_t)y1 * p.lvlWCoarse + x1) * 4u;

    for (int ch = 0; ch < 4; ++ch) {
        const float top = p00[ch] + (p10[ch] - p00[ch]) * fu;
        const float bot = p01[ch] + (p11[ch] - p01[ch]) * fu;
        t[ch] = top + (bot - top) * fv;
    }
}

/* UNPREMULTIPLY level 0 in place: the chain's premultiplied tuple becomes the
   straight extended colour every downstream reader wants (composite +
   Extended Colour view). Coverage is > 0 everywhere after the push unless the
   fill never had a seed, in which case this yields black — the documented
   empty-core fallback. */
extern "C" __global__ void ER_PPResolve(float* __restrict__ chain,
                                        ER_PassParamsCu p)
{
    ER_TID_2D(p.W, p.H);
    float* t = chain + ((size_t)ER_PlaneIndex((uint)x, (uint)y, (uint)p.W)) * 4u;
    const ER_RGB rgb = ER_PPUnpremult(t[0], t[1], t[2], t[3]);
    t[0] = rgb.r; t[1] = rgb.g; t[2] = rgb.b; t[3] = 1.0f;
}

/* ==========================================================================
   Synthetic-grain amplitude  (mirrors the estimator in EdgeRepair.cpp)
   ========================================================================== */

/* Mean |luma neighbour diff| over pixel pairs where BOTH ends are trusted
   core — the plate's own local grain energy, measured where it is clean.
   Fixed grid + shared-memory reduction -> one (sum, count) pair per block. */
extern "C" __global__ void ER_GrainPartial(
    const float* __restrict__ src,
    const float* __restrict__ core,
    float*       __restrict__ partials,
    ER_PassParamsCu           p)
{
    __shared__ float sSum[ER_GRAIN_THREADS];
    __shared__ float sCnt[ER_GRAIN_THREADS];

    const int tid    = (int)threadIdx.x;
    const int stride = (int)(blockDim.x * gridDim.x);
    const int total  = p.W * p.H;

    float sum = 0.0f, cnt = 0.0f;
    for (int i = (int)(blockIdx.x * blockDim.x + threadIdx.x);
         i < total; i += stride)
    {
        if (core[i] <= 0.5f) continue;
        const int x = i % p.W;
        const int y = i / p.W;
        const uint s = ER_PixelFloatIndex((uint)x, (uint)y, (uint)p.srcPitchF);
        const float l = ER_Luma(src[s + 2u], src[s + 1u], src[s + 0u]);

        if (x + 1 < p.W && core[i + 1] > 0.5f) {
            const uint s2 = ER_PixelFloatIndex((uint)(x + 1), (uint)y,
                                               (uint)p.srcPitchF);
            sum += ER_ABS(l - ER_Luma(src[s2 + 2u], src[s2 + 1u], src[s2 + 0u]));
            cnt += 1.0f;
        }
        if (y + 1 < p.H && core[i + p.W] > 0.5f) {
            const uint s2 = ER_PixelFloatIndex((uint)x, (uint)(y + 1),
                                               (uint)p.srcPitchF);
            sum += ER_ABS(l - ER_Luma(src[s2 + 2u], src[s2 + 1u], src[s2 + 0u]));
            cnt += 1.0f;
        }
    }

    sSum[tid] = sum; sCnt[tid] = cnt;
    __syncthreads();
    for (int half = ER_GRAIN_THREADS / 2; half > 0; half >>= 1) {
        if (tid < half) {
            sSum[tid] += sSum[tid + half];
            sCnt[tid] += sCnt[tid + half];
        }
        __syncthreads();
    }
    if (tid == 0) {
        partials[blockIdx.x * 2 + 0] = sSum[0];
        partials[blockIdx.x * 2 + 1] = sCnt[0];
    }
}

/* Sum the per-block partials and convert to a grain amplitude. Single thread —
   ER_GRAIN_BLOCKS is 256, and keeping it serial makes the result independent
   of scheduling (no float-atomic ordering). Result lands in misc[0], which the
   composite reads without any host round-trip. */
extern "C" __global__ void ER_GrainFinal(
    const float* __restrict__ partials,
    float*       __restrict__ amp,
    ER_PassParamsCu           p)
{
    if (threadIdx.x != 0 || blockIdx.x != 0) return;
    float sum = 0.0f, cnt = 0.0f;
    for (int i = 0; i < p.grainBlocks; ++i) {
        sum += partials[i * 2 + 0];
        cnt += partials[i * 2 + 1];
    }
    amp[0] = ER_GrainAmpFromMean(sum, cnt);
}

/* ==========================================================================
   Output passes — one per View Mode
   ========================================================================== */

/* The Composited deliverable. Reads the inflated source, the driving distance
   field, the extended colour (chain level 0) and the choke field, and writes
   the output rect. Straight alpha in, straight alpha out. */
extern "C" __global__ void ER_Composite(
    const float* __restrict__ src,
    const float* __restrict__ dist,
    const float* __restrict__ chain,
    const float* __restrict__ chokeField,
    const unsigned int* __restrict__ flags,
    const float* __restrict__ amp,
    float*       __restrict__ dst,
    ER_PassParamsCu           p)
{
    ER_TID_2D(p.outW, p.outH);

    const int sx = x + p.srcOX;             /* coordinate in the inflated src */
    const int sy = y + p.srcOY;
    const uint s = ER_PixelFloatIndex((uint)sx, (uint)sy, (uint)p.srcPitchF);
    const uint i = ER_PlaneIndex((uint)sx, (uint)sy, (uint)p.W);

    /* The extension is only meaningful if a core existed AND the fill found a
       seed — the two host-side booleans of the CPU path (`haveField`,
       PullPushFill's return), evaluated here from the device flags. */
    const int band = (p.bandRequested != 0 &&
                      flags[ER_FLAG_CORE] != 0u &&
                      flags[ER_FLAG_FILL] != 0u) ? 1 : 0;

    const float a = src[s + 3u];

    /* Alpha first: choke along the alpha-edge field. Gated on the field
       actually having seeds, mirroring the CPU's `haveChoke`. The field is
       only DEREFERENCED when the choke is live — when it is off the host is
       free to pass any allocated buffer rather than reserve a plane. */
    const int chokeOn = (p.chokeMode != ER_CHOKE_MODE_OFF &&
                         flags[ER_FLAG_CHOKE] != 0u) ? p.chokeMode
                                                     : ER_CHOKE_MODE_OFF;
    const float newA = (chokeOn == ER_CHOKE_MODE_OFF)
                     ? a
                     : ER_ApplyChoke(a, chokeField[i], p.choke, chokeOn);

    /* Band weight — only strictly OUTSIDE the core (d > 0). The gap carries
       full weight and Extend rolls off from the ALPHA EDGE, so the outward
       reach past the matte is Erode-independent. */
    float w = 0.0f;
    ER_RGB ext = ER_MakeRGB(0.0f, 0.0f, 0.0f);
    if (band != 0) {
        /* Same discipline as the choke field: the distance field and the
           pull–push chain are read ONLY when the band is live, so a frame
           that never runs the fill needs neither allocated. */
        /* p.erode carries the erode GAP width on this pass — the host applies
           the same no-erode deadband BuildCoreMatte uses, so the plateau and
           the real core boundary can never disagree. */
        const float d = dist[i];
        if (d > 0.0f) w = ER_BlendWeight(d, p.erode, p.extend, p.softness);
        const float* e = chain + (size_t)i * 4u;
        ext = ER_MakeRGB(e[0], e[1], e[2]);
    }

    const ER_RGB orig = ER_MakeRGB(src[s + 2u], src[s + 1u], src[s + 0u]);

    const ER_RGB out = ER_CompositeRGB(orig, ext, a, newA, w,
                                       p.detail01, amp[0], band, sx, sy);

    const uint d0 = ER_PixelFloatIndex((uint)x, (uint)y, (uint)p.dstPitchF);
    dst[d0 + 0u] = out.b;                   /* world is BGRA */
    dst[d0 + 1u] = out.g;
    dst[d0 + 2u] = out.r;
    dst[d0 + 3u] = newA;
}

/* Core Matte — diagnostic view, opaque greyscale. */
extern "C" __global__ void ER_ViewCore(
    const float* __restrict__ core,
    float*       __restrict__ dst,
    ER_PassParamsCu           p)
{
    ER_TID_2D(p.outW, p.outH);
    const uint i = ER_PlaneIndex((uint)(x + p.srcOX), (uint)(y + p.srcOY),
                                 (uint)p.W);
    const float v = core[i] > 0.5f ? 1.0f : 0.0f;
    const uint d0 = ER_PixelFloatIndex((uint)x, (uint)y, (uint)p.dstPitchF);
    dst[d0 + 0u] = v; dst[d0 + 1u] = v; dst[d0 + 2u] = v; dst[d0 + 3u] = 1.0f;
}

/* Distance Field — the opaque INWARD light-wrap matte of the erode gap.
   A frame with no alpha edge at all (all opaque or all transparent) has no
   distance to measure: the CPU zeroes the field in that case, so we feed 0
   here rather than the flood's unreachable sentinel. */
extern "C" __global__ void ER_ViewDist(
    const float* __restrict__ edgeDist,
    const float* __restrict__ interior,
    const unsigned int* __restrict__ flags,
    float*       __restrict__ dst,
    ER_PassParamsCu           p)
{
    ER_TID_2D(p.outW, p.outH);
    const uint i = ER_PlaneIndex((uint)(x + p.srcOX), (uint)(y + p.srcOY),
                                 (uint)p.W);

    /* No alpha edge anywhere (all-opaque or all-transparent frame) means there
       is nothing to measure from, so the matte is black. Feeding 0 through
       ER_DistViewValue instead would read as "right at the edge" and paint the
       whole frame white — mirrors the CPU's haveEdge gate in ER_DistViewRow. */
    const bool haveEdge = (flags[ER_FLAG_INTERIOR] != 0u &&
                           flags[ER_FLAG_EXTERIOR] != 0u);
    const float v = haveEdge
                  ? ER_DistViewValue(edgeDist[i], interior[i], p.erode, p.softness)
                  : 0.0f;

    const uint d0 = ER_PixelFloatIndex((uint)x, (uint)y, (uint)p.dstPitchF);
    dst[d0 + 0u] = v; dst[d0 + 1u] = v; dst[d0 + 2u] = v; dst[d0 + 3u] = 1.0f;
}

/* Extended Colour — the opaque clean-plate deliverable: the unlimited
   pull–push fill, straight RGB, alpha forced to 1. */
extern "C" __global__ void ER_ViewExt(
    const float* __restrict__ chain,
    float*       __restrict__ dst,
    ER_PassParamsCu           p)
{
    ER_TID_2D(p.outW, p.outH);
    const uint i = ER_PlaneIndex((uint)(x + p.srcOX), (uint)(y + p.srcOY),
                                 (uint)p.W);
    const float* e = chain + (size_t)i * 4u;

    const uint d0 = ER_PixelFloatIndex((uint)x, (uint)y, (uint)p.dstPitchF);
    dst[d0 + 0u] = e[2];                    /* world is BGRA */
    dst[d0 + 1u] = e[1];
    dst[d0 + 2u] = e[0];
    dst[d0 + 3u] = 1.0f;
}
