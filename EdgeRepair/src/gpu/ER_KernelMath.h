/*  ER_KernelMath.h — Edge Repair GPU kernel math, SHARED across backends.

    THE POINT OF THIS FILE: one math body, four compile targets. Every GPU
    kernel's per-pixel mathematics lives here as pure functions on scalars /
    vectors, written in the common subset of HLSL / CUDA / Metal / OpenCL / C++.
    The per-backend wrapper files (EdgeRepairKernels.hlsl / .cu / .metal) contain
    ONLY entry-point decorations and buffer plumbing — never math. That is what
    makes the eventual Metal (and CUDA) builds a toolchain flip instead of a
    port: the wrappers are thin, the math is already shared. (Stage 6 decision,
    2026-07-23 — see docs/EdgeRepair.md.)

    Because this header also compiles as plain C++, the headless tests can call
    the EXACT functions the GPU runs — that is how CPU/GPU lockstep is checked
    without a GPU in the loop (tests/test_gpuparity.cpp).

    Language selection: the including wrapper defines exactly ONE of
        ER_GPU_HLSL   — fxc/dxc, DirectX 12 compute (Windows, Premiere hosts)
        ER_GPU_CUDA   — nvcc (the framework AE-on-Windows actually offers)
        ER_GPU_METAL  — Apple metal compiler (source maintained; builds on Mac)
        ER_GPU_OPENCL — reserved, no wrapper yet
    or none of them, in which case this is a host C++ include (tests).

    CPU/GPU LOCKSTEP CONTRACT: functions here must mirror src/core/ER_*.h
    exactly (same formulas, same clamps, same edge cases). When a formula
    changes in ER_Composite.h / ER_Grain.h / ER_CoreMatte.h, it changes here in
    the same commit. The distance field is the ONE deliberate divergence:
    GPU uses Jump Flood (JFA+2, approximate) instead of the CPU's exact
    Meijster/F&H EDT — accepted 2026-07-23, which is why the Stage 6 parity gate
    is "within tolerance", not byte-identical.

    WHAT MIRRORS WHAT (each function names its CPU twin):
        ER_Smooth01        <- er::ER_Smooth01        (ER_Composite.h)
        ER_BlendWeight     <- er::BlendWeight        (ER_Composite.h)
        ER_ComposeBandRGB  <- er::ComposeBandRGB     (ER_Composite.h)
        ER_ChokeInAlpha    <- er::ChokeInAlpha       (ER_Composite.h)
        ER_GrowAlpha       <- er::GrowAlpha          (ER_Composite.h)
        ER_GrainHash/At    <- er::GrainHash/GrainAt  (ER_Grain.h)
        ER_CoreTest        <- er::BuildCoreMatte     (ER_CoreMatte.h)
        ER_DistViewValue   <- ER_DistViewRow         (EdgeRepair.cpp)
        ER_GrainAmpFromMean<- the grain-amp estimator (EdgeRepair.cpp)
        ER_PP...           <- er::PullPushFill       (ER_PullPush.h)
        ER_Seed... /
        ER_Jfa...          -- NO CPU twin: the JFA distance approximation
*/

#ifndef ER_KERNEL_MATH_H
#define ER_KERNEL_MATH_H

/* ------------------------------------------------------------------------- */
/* Per-language shims. Keep this block the ONLY place backend #ifdefs appear; */
/* the functions below must read identically for every backend.              */
/* ------------------------------------------------------------------------- */

#if defined(ER_GPU_HLSL)
    /* HLSL: uint/int/float are native; no pointers; structs and struct returns
       are supported. fxc's preprocessor handles this header like C. */
    #define ER_FN
    #define ER_OUT(T)        out T
    #define ER_SQRT(x)       sqrt(x)
    #define ER_ABS(x)        abs(x)
    #define ER_MIN(a, b)     min(a, b)
    #define ER_MAX(a, b)     max(a, b)
    #define ER_FLOOR(x)      floor(x)

#elif defined(ER_GPU_CUDA)
    #define ER_FN            __device__ __forceinline__
    #define ER_OUT(T)        T&
    #define ER_SQRT(x)       sqrtf(x)
    #define ER_ABS(x)        fabsf(x)
    #define ER_MIN(a, b)     fminf(a, b)
    #define ER_MAX(a, b)     fmaxf(a, b)
    #define ER_FLOOR(x)      floorf(x)
    typedef unsigned int     uint;

#elif defined(ER_GPU_METAL)
    #define ER_FN            static inline
    /* Metal references carry an address space; every ER_OUT below is a
       function-local (thread) variable at the call site. */
    #define ER_OUT(T)        thread T&
    #define ER_SQRT(x)       metal::sqrt(x)
    #define ER_ABS(x)        metal::fabs(x)
    #define ER_MIN(a, b)     metal::fmin(a, b)
    #define ER_MAX(a, b)     metal::fmax(a, b)
    #define ER_FLOOR(x)      metal::floor(x)
    /* metal_stdlib provides uint/float4; the wrapper does
       `using namespace metal;` before including this header. */

#elif defined(ER_GPU_OPENCL)
    #define ER_FN            static inline
    #define ER_OUT(T)        T*
    #define ER_SQRT(x)       sqrt(x)
    #define ER_ABS(x)        fabs(x)
    #define ER_MIN(a, b)     fmin(a, b)
    #define ER_MAX(a, b)     fmax(a, b)
    #define ER_FLOOR(x)      floor(x)

#else
    /* Host C++ (headless parity tests / reference). */
    #include <cmath>
    #define ER_FN            static inline
    #define ER_OUT(T)        T&
    #define ER_SQRT(x)       std::sqrt(x)
    #define ER_ABS(x)        std::fabs(x)
    #define ER_MIN(a, b)     ((a) < (b) ? (a) : (b))
    #define ER_MAX(a, b)     ((a) > (b) ? (a) : (b))
    #define ER_FLOOR(x)      std::floor(x)
    typedef unsigned int     uint;
#endif

/* ------------------------------------------------------------------------- */
/* Shared constants                                                          */
/* ------------------------------------------------------------------------- */

/* "No seed reachable" distance. Chosen so ER_D2_INF == ER_DIST_INF^2 stays
   comfortably inside float range: every consumer either compares against a
   parameter (<= 500 px) or divides, so a finite sentinel behaves like +inf
   without producing NaN in later arithmetic. Matches the CPU's degenerate-case
   behaviour: an empty seed set leaves d effectively unreachable, which makes
   BlendWeight 0 and ChokeInAlpha the identity. */
#define ER_DIST_INF   1.0e15f
#define ER_D2_INF     1.0e30f

/* Packed "no nearest seed yet" sentinel for the jump-flood buffers. */
#define ER_SEED_INVALID 0xFFFFFFFFu

/* Alpha below this is "no coverage" — the plate's RGB there is undefined
   garbage, so the composite substitutes the extension. Mirrors the 1e-6f
   literal in ER_ResultRow (EdgeRepair.cpp). */
#define ER_ALPHA_EPS  1.0e-6f

/* Core-matte modes, mirroring the three branches of er::BuildCoreMatte.
   The host picks the branch (it knows the Erode value); the kernel must not
   re-derive it, so that "no erode" stays a bit-exact copy of the interior. */
#define ER_CORE_MODE_COPY   0   /* |erode| < 0.01 : core = interior          */
#define ER_CORE_MODE_SHRINK 1   /* erode > 0      : interior && dist > erode */
#define ER_CORE_MODE_GROW   2   /* erode < 0      : dist <= |erode|          */

/* Classify modes for the mask-building kernel (see the wrappers). */
#define ER_CLASSIFY_INTERIOR      0   /* alpha >  thresh                     */
#define ER_CLASSIFY_EXTERIOR      1   /* alpha <= thresh                     */
#define ER_CLASSIFY_CHOKE_IN      2   /* alpha <= 0.001  (nearest zero-alpha)*/
#define ER_CLASSIFY_CHOKE_GROW    3   /* alpha >  0.5    (nearest solid)     */

/* Alpha-choke modes (host-selected from the signed Choke param). */
#define ER_CHOKE_MODE_OFF   0
#define ER_CHOKE_MODE_IN    1
#define ER_CHOKE_MODE_GROW  2

/* View modes — VALUES MUST MATCH the ER_VIEW_* enum in src/EdgeRepair.h
   (they are the popup's disk-persisted indices, locked forever). */
#define ER_VIEW_M_RESULT           1
#define ER_VIEW_M_CORE_MATTE       2
#define ER_VIEW_M_DISTANCE_FIELD   3
#define ER_VIEW_M_EXTENDED_COLOUR  4

/* ------------------------------------------------------------------------- */
/* Device-side "did anything qualify?" flags.                                */
/*                                                                           */
/* The CPU pipeline asks these questions on the host (`anySeed`, `haveField`, */
/* PullPushFill's return value, `anyIn && anyOut`) and branches. The GPU      */
/* cannot: reading the answer back would mean synchronizing AE's stream       */
/* mid-render. So the producing kernels OR a bit into this tiny device buffer */
/* and the CONSUMING kernels test it — same decisions, no host round-trip.    */
/* Slots are indices into a uint buffer of ER_FLAG_COUNT entries.            */
/* ------------------------------------------------------------------------- */
#define ER_FLAG_CORE      0   /* the core matte has at least one pixel       */
#define ER_FLAG_FILL      1   /* pull–push level 0 got at least one seed     */
#define ER_FLAG_CHOKE     2   /* the choke field has at least one seed       */
#define ER_FLAG_INTERIOR  3   /* some pixel is inside the alpha silhouette   */
#define ER_FLAG_EXTERIOR  4   /* some pixel is outside it (an edge exists)   */
#define ER_FLAG_COUNT     8

/* Grain-amplitude reduction geometry. A FIXED grid (not one thread per pixel)
   so the partial-sum buffer is a constant 2*ER_GRAIN_BLOCKS floats and the
   final reduce is a single short serial loop — deterministic regardless of
   frame size, and no float atomics (which cs_5_1 lacks). */
#define ER_GRAIN_BLOCKS   256
#define ER_GRAIN_THREADS  256

/* ------------------------------------------------------------------------- */
/* Small value types. Plain scalar structs so they compile identically in     */
/* HLSL / CUDA / Metal / C++ (no vector types, no address spaces).           */
/* ------------------------------------------------------------------------- */

struct ER_RGB { float r; float g; float b; };

ER_FN ER_RGB ER_MakeRGB(float r, float g, float b)
{
    ER_RGB v;
    v.r = r; v.g = g; v.b = b;
    return v;
}

/* ------------------------------------------------------------------------- */
/* Addressing helpers.                                                       */
/*                                                                           */
/* GPU effect worlds are PF_PixelFormat_GPU_BGRA128: 4 x float32 per pixel,  */
/* **BGRA** channel order, row pitch given by PF_EffectWorld.rowbytes. All    */
/* kernels address worlds as flat float arrays with a per-row pitch in        */
/* FLOATS (rowbytes / 4) — never assume pitch == width*4.                     */
/*                                                                           */
/* The CPU pipeline's own scratch planes (alpha, core, dist, the pull–push    */
/* chain) are TIGHTLY PACKED at pitch == W — that is a different pitch from   */
/* the worlds', and mixing them up is the easiest mistake to make here.       */
/* ------------------------------------------------------------------------- */

/* Index of pixel (x, y)'s first float in a BGRA128 buffer with row pitch
   `pitchF` floats. Byte address = 4 * this (ByteAddressBuffer.Load4 wants
   bytes; pointer backends index floats directly). */
ER_FN uint ER_PixelFloatIndex(uint x, uint y, uint pitchF)
{
    return y * pitchF + x * 4u;
}

/* Map an output pixel to its source pixel. SmartRender's output world is a
   sub-rect of the inflated source world; output (0,0) sits at source
   (srcOX, srcOY). Mirrors the CPU path's offset addressing in EdgeRepair.cpp
   (SmartRender), which derives srcOX/srcOY from the PreRender checkout rects. */
ER_FN uint ER_SrcFloatIndex(uint outX, uint outY,
                            uint srcOX, uint srcOY, uint srcPitchF)
{
    return ER_PixelFloatIndex(outX + srcOX, outY + srcOY, srcPitchF);
}

/* Index into a tightly-packed W-wide scratch plane (one float per pixel). */
ER_FN uint ER_PlaneIndex(uint x, uint y, uint W)
{
    return y * W + x;
}

/* ------------------------------------------------------------------------- */
/* Blend weight and composite — mirrors ER_Composite.h exactly.              */
/* ------------------------------------------------------------------------- */

/* smoothstep(0,1) on a pre-normalized t. <- er::ER_Smooth01 */
ER_FN float ER_Smooth01(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* Gap-plateau band weight with the original inner ease.
   <- er::BlendWeight(d, gap, extend, inner)
       outer  = d <= gap ? 1 : 1 - smoothstep(0, extend, d - gap)
       innerW = smoothstep(0, inner, d)
       w      = innerW * outer
   `d` is the core field (0 inside the trusted core, growing outward), `gap` the
   erode gap between the core boundary and the original alpha edge. The gap
   carries full weight and Extend rolls off from the ALPHA EDGE, so the reach
   past the matte is Erode-independent (fixed 2026-08-24). The inner ease is
   unchanged: w ramps 0 -> 1 over the first `inner` px out from the core, and
   the core itself is never recomposited. */
ER_FN float ER_BlendWeight(float d, float gap, float extend, float inner)
{
    if (extend <= 0.0f) return 0.0f;
    if (d <= 0.0f) return 0.0f;
    if (gap < 0.0f) gap = 0.0f;

    float outer = 1.0f;
    if (d > gap) {
        const float s = (d - gap) / extend;
        if (s >= 1.0f) return 0.0f;
        outer = 1.0f - s * s * (3.0f - 2.0f * s);
    }

    if (inner <= 0.0f) return outer;
    return ER_Smooth01(d / inner) * outer;
}

/* base = lerp(orig, ext, w); out = base + w*detail01*grain (straight RGB).
   <- er::ComposeBandRGB. At w->0 this returns orig exactly for ANY detail,
   which is what makes the band's outer boundary invisible. */
ER_FN ER_RGB ER_ComposeBandRGB(ER_RGB orig, ER_RGB ext,
                               float grain, float w, float detail01)
{
    const float g = w * detail01 * grain;
    ER_RGB o;
    o.r = orig.r + (ext.r - orig.r) * w + g;
    o.g = orig.g + (ext.g - orig.g) * w + g;
    o.b = orig.b + (ext.b - orig.b) * w + g;
    return o;
}

/* newA = a * clamp01(dOut - choke). <- er::ChokeInAlpha */
ER_FN float ER_ChokeInAlpha(float a, float dOut, float choke)
{
    const float t = dOut - choke;
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return a;
    return a * t;
}

/* newA = (dSolid <= 0) ? a : max(a, clamp01(grow + 0.5 - dSolid)).
   <- er::GrowAlpha */
ER_FN float ER_GrowAlpha(float a, float dSolid, float grow)
{
    if (dSolid <= 0.0f) return a;      /* inside the solid — never harden */
    float t = grow + 0.5f - dSolid;
    if (t <= 0.0f) return a;
    if (t > 1.0f)  t = 1.0f;
    return t > a ? t : a;
}

/* One place that decides the output alpha, so the CUDA/HLSL/Metal wrappers
   cannot drift on the choke branch. mode == ER_CHOKE_MODE_OFF is the identity
   (that is what keeps "Choke 0 => bit-identical alpha" true on the GPU too). */
ER_FN float ER_ApplyChoke(float a, float chokeField, float choke, int mode)
{
    if (mode == ER_CHOKE_MODE_IN)   return ER_ChokeInAlpha(a, chokeField, choke);
    if (mode == ER_CHOKE_MODE_GROW) return ER_GrowAlpha(a, chokeField, -choke);
    return a;
}

/* ------------------------------------------------------------------------- */
/* Synthetic grain — mirrors ER_Grain.h exactly (bit-for-bit: integer hash    */
/* then a 2^-24 scale, both exact in float on every backend).                */
/* ------------------------------------------------------------------------- */

ER_FN uint ER_GrainHash(uint x, uint y)
{
    uint h = x * 0x9E3779B1u ^ y * 0x85EBCA77u;
    h ^= h >> 15; h *= 0xD168AAADu;
    h ^= h >> 13; h *= 0xAF723597u;
    h ^= h >> 16;
    return h;
}

/* Zero-mean grain at integer pixel (x, y), roughly [-1, 1). <- er::GrainAt.
   Keyed to the INFLATED-SOURCE coordinate by the caller, so the pattern is
   stable frame to frame and independent of tiling. */
ER_FN float ER_GrainAt(int x, int y)
{
    const uint h = ER_GrainHash((uint)x, (uint)y);
    const float u = (float)(h >> 8) * (1.0f / 16777216.0f);   /* [0,1) */
    return u * 2.0f - 1.0f;
}

/* Rec.601 luma of a straight RGB triple — the grain-amplitude estimator's
   measure (EdgeRepair.cpp uses the same coefficients). */
ER_FN float ER_Luma(float r, float g, float b)
{
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

/* Mean |neighbour luma diff| -> grain amplitude. <- the *1.53 conversion in
   EdgeRepair.cpp: mean |diff| ~ grain std * 1.13, and ER_GrainAt spans [-1,1)
   (std ~0.577), so 1/(1.13*0.577) ~ 1.53 makes grain*amp match the plate's
   measured neighbour spread. */
ER_FN float ER_GrainAmpFromMean(float sum, float count)
{
    if (count <= 0.0f) return 0.0f;
    return (sum / count) * 1.53f;
}

/* ------------------------------------------------------------------------- */
/* Core matte — mirrors er::BuildCoreMatte.                                  */
/* ------------------------------------------------------------------------- */

/* Threshold test: alpha > thresh marks the pre-erode interior. */
ER_FN float ER_InteriorTest(float alpha, float thresh)
{
    return alpha > thresh ? 1.0f : 0.0f;
}

/* The signed-erode decision, given the interior flag and the relevant
   distance:
     COPY   : core = interior                       (|erode| < 0.01)
     SHRINK : core = interior && dist > erode       (dist = to nearest exterior)
     GROW   : core = dist <= |erode|                (dist = to nearest interior)
   Degenerate seed sets need no special case here: with no seeds the jump flood
   leaves dist == ER_DIST_INF, which reproduces the CPU's early-outs exactly
   (SHRINK keeps the whole interior; GROW keeps nothing). */
ER_FN float ER_CoreTest(float interior, float dist, float erode, int mode)
{
    if (mode == ER_CORE_MODE_SHRINK)
        return (interior > 0.5f && dist > erode) ? 1.0f : 0.0f;
    if (mode == ER_CORE_MODE_GROW)
        return (dist <= -erode) ? 1.0f : 0.0f;
    return interior;
}

/* ------------------------------------------------------------------------- */
/* Jump-flood distance (JFA+2) — the one deliberate CPU/GPU divergence.      */
/*                                                                           */
/* Each pixel carries the packed coordinate of the nearest seed found so far. */
/* Passes halve the step from 2^ceil(log2(maxdim))/2 down to 1, then repeat   */
/* steps 2 and 1 ("+2"), which removes the classic JFA islands; residual      */
/* error is sub-pixel. The host owns the pass schedule — see ER_JfaFirstStep. */
/* ------------------------------------------------------------------------- */

ER_FN uint ER_PackSeed(uint x, uint y)
{
    return (x << 16) | (y & 0xFFFFu);
}

ER_FN uint ER_SeedX(uint packed) { return packed >> 16; }
ER_FN uint ER_SeedY(uint packed) { return packed & 0xFFFFu; }

/* Squared distance from pixel (x,y) to the seed a neighbour is pointing at.
   Returns ER_D2_INF for the invalid sentinel so a plain min() picks winners. */
ER_FN float ER_SeedDist2(uint packed, int x, int y)
{
    if (packed == ER_SEED_INVALID) return ER_D2_INF;
    const float dx = (float)((int)ER_SeedX(packed) - x);
    const float dy = (float)((int)ER_SeedY(packed) - y);
    return dx * dx + dy * dy;
}

/* Squared distance -> Euclidean, mapping the sentinel to ER_DIST_INF. */
ER_FN float ER_Dist2ToDist(float d2)
{
    if (d2 >= ER_D2_INF) return ER_DIST_INF;
    return ER_SQRT(d2);
}

/* ------------------------------------------------------------------------- */
/* Pull–push — mirrors ER_PullPush.h. Buffer access lives in the wrappers;    */
/* everything that decides a VALUE lives here.                               */
/* ------------------------------------------------------------------------- */

/* Level-0 validity: a pixel seeds the fill only if it is core AND carries
   coverage. <- the `if (!valid[i]) continue; if (a <= 0.f) continue;` pair. */
ER_FN float ER_PPValid(float core, float alpha)
{
    return (core > 0.5f && alpha > 0.0f) ? 1.0f : 0.0f;
}

/* Coarse-level texel = plain average of the up-to-4 children that EXIST
   (n is 4 in the interior, 2 or 1 at an odd edge — the CPU divides by the
   child count, not by 4, and that difference is visible at odd dimensions). */
ER_FN float ER_PPAverage(float acc, int n)
{
    if (n <= 0) return 0.0f;
    return acc / (float)n;
}

/* Push mapping: fine texel index -> coarse texel-center coordinate.
   <- `((double)x + 0.5) * su - 0.5` with su = cW/fW. The CPU computes this in
   double and the GPU in float; the difference is far below the sub-pixel JFA
   tolerance the Stage 6 gate already accepts. */
ER_FN float ER_PPPushCoord(int i, float scale)
{
    return ((float)i + 0.5f) * scale - 0.5f;
}

/* Clamped bilinear axis setup: clamp u into [0, n-1], then split into the two
   taps and the fraction. <- detail_pp::Bilinear4's clamping and x1 = min(x0+1,
   W-1) edge rule. */
ER_FN void ER_PPBilinearAxis(float u, int n,
                             ER_OUT(int) i0, ER_OUT(int) i1, ER_OUT(float) f)
{
    float c = u;
    if (c < 0.0f) c = 0.0f;
    const float hi = (float)(n - 1);
    if (c > hi) c = hi;
    const int a = (int)c;                       /* floor: c >= 0 here */
    i0 = a;
    i1 = (a + 1 < n) ? (a + 1) : a;
    f  = c - (float)a;
}

/* Unpremultiply by the accumulated coverage. <- the final divide in
   PullPushFill; coverage 0 yields black, which is the documented empty-core
   fallback for the Extended Colour view. */
ER_FN ER_RGB ER_PPUnpremult(float r, float g, float b, float cov)
{
    const float inv = cov > 0.0f ? 1.0f / cov : 0.0f;
    return ER_MakeRGB(r * inv, g * inv, b * inv);
}

/* ------------------------------------------------------------------------- */
/* View-mode pixel values                                                    */
/* ------------------------------------------------------------------------- */

/* Distance Field view — the INWARD light-wrap matte of the erode gap.
   <- ER_DistViewRow (EdgeRepair.cpp). WHITE at the original alpha edge,
   gradienting inward to BLACK at the eroded core over a `gap`-wide band;
   `soft` px of that band is the smoothstep ramp, the rest a solid plateau.
       edgeDist : Euclidean distance INWARD from the alpha edge
       interior : 1 inside the pre-erode silhouette

   A frame with NO alpha edge at all (all opaque or all transparent) has no
   distance to measure: callers must return black directly rather than feeding
   edgeDist = 0 through here, which would read as "right at the edge" and paint
   the whole frame white. Both ER_ViewDist and ER_DistViewRow gate on that. */
ER_FN float ER_DistViewValue(float edgeDist, float interior,
                             float gap, float soft)
{
    if (gap <= 0.0f || interior <= 0.5f) return 0.0f;
    if (edgeDist >= gap) return 0.0f;
    const float ramp = soft < gap ? soft : gap;
    const float plateau = gap - ramp;
    if (edgeDist <= plateau || ramp <= 0.0f) return 1.0f;
    const float t = (edgeDist - plateau) / ramp;
    return 1.0f - ER_Smooth01(t);
}

/* ------------------------------------------------------------------------- */
/* The full Result-view composite, as ONE function.                          */
/*                                                                           */
/* Mirrors ER_ResultRow's per-pixel body (EdgeRepair.cpp) including its two   */
/* verbatim-copy branches, which is what makes "untouched outside the band"   */
/* and "Choke 0 => bit-identical alpha" hold on the GPU as well.             */
/*   orig     : the plate's STRAIGHT RGB at this pixel                        */
/*   ext      : pull–push extended colour (meaningful only when band != 0)    */
/*   a        : the plate's alpha                                             */
/*   newA     : alpha AFTER the choke (ER_ApplyChoke)                         */
/*   w        : band weight, already 0 outside the band                       */
/*   band     : 1 when the extension is valid (core non-empty AND fill ran)   */
/* Straight alpha in, straight alpha out — never scale RGB by alpha here.     */
/* That premultiply was the Stage 4 "dark edge along the alpha boundary" bug. */
ER_FN ER_RGB ER_CompositeRGB(ER_RGB orig, ER_RGB ext, float a, float newA,
                             float w, float detail01, float grainAmp,
                             int band, int gx, int gy)
{
    if (w <= 0.0f) {
        if (newA == a) return orig;                    /* bitwise passthrough */
        /* Choke changed alpha; colour is alpha-independent so it stays —
           except a GROWN zero-alpha pixel, whose own RGB is undefined
           garbage: the extension is its only real colour. */
        if (a <= ER_ALPHA_EPS && band != 0) return ext;
        return orig;
    }
    /* In-band. At a~0 the plate RGB is garbage, so `ext` stands in as orig.
       Written as an if, not a ternary: HLSL has no conditional operator over
       struct types (fxc: "type mismatch between conditional values"). */
    ER_RGB base;
    if (a > ER_ALPHA_EPS) base = orig; else base = ext;
    const float gr = ER_GrainAt(gx, gy) * grainAmp;
    return ER_ComposeBandRGB(base, ext, gr, w, detail01);
}

#endif /* ER_KERNEL_MATH_H */
