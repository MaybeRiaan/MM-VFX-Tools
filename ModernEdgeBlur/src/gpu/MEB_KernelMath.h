/*  MEB_KernelMath.h — Modern Edge Blur GPU kernel math, SHARED across backends.

    THE POINT OF THIS FILE: one math body, N compile targets. Every per-pixel
    formula the GPU evaluates lives here as pure functions on scalars and small
    plain structs, written in the common subset of CUDA / HLSL / Metal / host
    C++. The per-backend wrapper files (ModernEdgeBlurKernels.cu / .hlsl)
    contain ONLY entry decorations and buffer plumbing — never arithmetic. That
    is what makes a later Metal or DX12 build a toolchain step instead of a
    port.

    THIS IS THE FORMULA LAYER. The pass bodies that actually walk buffers —
    tap loops, neighbourhood scans, world addressing — live in the companion
    header MEB_KernelPasses.h, which is included by the CUDA wrapper and by the
    parity test but NOT by the HLSL wrapper. Reason: those functions take
    `const float*` parameters, which HLSL has no equivalent for. Bringing DX12
    or Metal up means introducing MEB_RBUF / MEB_LOAD / MEB_STORE macros in the
    shim block below and switching the pass bodies to them — a change confined
    to that one block, exactly as gpu-kernel-rules.md §0 requires. Until then
    this file is the part fxc type-checks on every build (see the .hlsl
    wrapper), which is what keeps the formulas honest about HLSL's rules.

    CPU/GPU LOCKSTEP CONTRACT: functions here mirror src/core/MEB_CoreMath.h
    exactly — same formulas, same clamps, same edge cases, same accumulation
    order. When one changes, both change in the SAME commit.
    tests/test_gpuparity.cpp compiles this header as host C++ and diffs it
    against MEB_CoreMath.h with `==`, not an epsilon: it is the same arithmetic
    in the same order, so anything looser would hide a real divergence.

    WHAT MIRRORS WHAT (each function names its CPU twin):
        MEB_ClampI / MEB_ClampF   <- clampL / clampF
        MEB_DeriveTexel           <- MEB_DeriveRowInline (blurred output)
        MEB_SharpTexel            <- MEB_DeriveRowInline (the a<=0 zero-cliff)
        MEB_UnpremultTexel        <- the unpremult tail of MEB_BlurColsRGBA
        MEB_CompositeTexel        <- MEB_CompositePixel
        MEB_MorphInside / Better  <- MorphPlane's circle test and min/max
        MEB_MinKeep               <- MEB_DirMinRowInline's running min

    Language selection: the including wrapper defines exactly ONE of
        MEB_GPU_CUDA   — nvcc (the framework AE-on-Windows actually offers)
        MEB_GPU_HLSL   — fxc/dxc; validation only today, no DX12 host exists
        MEB_GPU_METAL  — reserved; no wrapper yet
    or none of them, in which case this is a host C++ include (the parity test).
*/

#ifndef MEB_KERNEL_MATH_H
#define MEB_KERNEL_MATH_H

/* ------------------------------------------------------------------------- */
/* Per-language shims. Keep this block the ONLY place backend #ifdefs appear;  */
/* every function below must read identically for every backend.              */
/* ------------------------------------------------------------------------- */

#if defined(MEB_GPU_CUDA)
    #define MEB_FN         __device__ __forceinline__
    #define MEB_OUT(T)     T&
    #define MEB_MIN(a, b)  fminf(a, b)
    #define MEB_MAX(a, b)  fmaxf(a, b)
    #define MEB_ABS(x)     fabsf(x)
    #define MEB_FLOOR(x)   floorf(x)
    typedef unsigned int   uint;

#elif defined(MEB_GPU_HLSL)
    #define MEB_FN
    #define MEB_OUT(T)     out T
    #define MEB_MIN(a, b)  min(a, b)
    #define MEB_MAX(a, b)  max(a, b)
    #define MEB_ABS(x)     abs(x)
    #define MEB_FLOOR(x)   floor(x)

#elif defined(MEB_GPU_METAL)
    #define MEB_FN         static inline
    /* Metal references carry an address space; every MEB_OUT below is a
       function-local (thread) variable at the call site. */
    #define MEB_OUT(T)     thread T&
    #define MEB_MIN(a, b)  metal::fmin(a, b)
    #define MEB_MAX(a, b)  metal::fmax(a, b)
    #define MEB_ABS(x)     metal::fabs(x)
    #define MEB_FLOOR(x)   metal::floor(x)

#else
    /* Host C++ — the headless parity test and the reference pipeline. */
    #include <cmath>
    #define MEB_FN         static inline
    #define MEB_OUT(T)     T&
    #define MEB_MIN(a, b)  ((a) < (b) ? (a) : (b))
    #define MEB_MAX(a, b)  ((a) > (b) ? (a) : (b))
    #define MEB_ABS(x)     std::fabs(x)
    #define MEB_FLOOR(x)   std::floor(x)
    typedef unsigned int   uint;
#endif

/* ------------------------------------------------------------------------- */
/* Shared constants — twins of the same names in MEB_CoreMath.h.             */
/* Guarded because the parity test includes BOTH headers.                    */
/* ------------------------------------------------------------------------- */

#ifndef MEB_UNPREMULT_EPS
#define MEB_UNPREMULT_EPS 1.0e-4f
#endif

#ifndef MEB_COMPOSITE_EPS
#define MEB_COMPOSITE_EPS 1.0e-6f
#endif

/* ------------------------------------------------------------------------- */
/* Small value type. A plain scalar struct so it compiles identically in      */
/* CUDA / HLSL / Metal / C++ (no vector types, no address spaces).           */
/*                                                                           */
/* Channel order is A,R,G,B — Modern Edge Blur's internal convention, the    */
/* one src/core/MEB_CoreMath.h and every scratch plane use. GPU effect worlds */
/* are BGRA; the swizzle happens at the world boundary in MEB_KernelPasses.h  */
/* and nowhere else.                                                          */
/* ------------------------------------------------------------------------- */

struct MEB_RGBA { float a; float r; float g; float b; };

MEB_FN MEB_RGBA MEB_MakeRGBA(float a, float r, float g, float b)
{
    MEB_RGBA v;
    v.a = a; v.r = r; v.g = g; v.b = b;
    return v;
}

/* ------------------------------------------------------------------------- */
/* Clamps. <- clampL / clampF (MEB_CoreMath.h)                               */
/* ------------------------------------------------------------------------- */

MEB_FN int MEB_ClampI(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

MEB_FN float MEB_ClampF(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ------------------------------------------------------------------------- */
/* Addressing.                                                               */
/*                                                                           */
/* GPU effect worlds are PF_PixelFormat_GPU_BGRA128: 4 x float32 per pixel,   */
/* **BGRA** channel order, row pitch given by PF_EffectWorld.rowbytes. Always */
/* address worlds with a per-row pitch in FLOATS (rowbytes / 4) — never       */
/* assume pitch == width * 4.                                                 */
/*                                                                           */
/* Our own scratch planes are TIGHTLY PACKED at pitch == W. That is a         */
/* different pitch from the worlds', and mixing them up is the easiest        */
/* mistake to make here.                                                      */
/* ------------------------------------------------------------------------- */

/* Index of pixel (x,y)'s first float in a BGRA128 world with row pitch
   `pitchF` floats. */
MEB_FN uint MEB_PixelFloatIndex(uint x, uint y, uint pitchF)
{
    return y * pitchF + x * 4u;
}

/* Index into a tightly-packed W-wide scratch plane (one float per pixel). */
MEB_FN uint MEB_PlaneIndex(uint x, uint y, uint W)
{
    return y * W + x;
}

/* Index into a tightly-packed W-wide interleaved ARGB scratch plane. */
MEB_FN uint MEB_PlaneIndex4(uint x, uint y, uint W)
{
    return (y * W + x) * 4u;
}

/* ------------------------------------------------------------------------- */
/* Ingest — the a<=0 zero cliff and the Blur Amount input.                   */
/* Both <- MEB_DeriveRowInline (MEB_CoreMath.h).                             */
/* ------------------------------------------------------------------------- */

/* The sharp (top) layer's texel: straight, held colour with RGB forced to
   zero wherever alpha is exactly 0.

   The CPU materialises this into sharp_buf; the GPU recomputes it from the
   source world plus the alpha plane at the two points that need it
   (passthrough and composite), which is what lets the GPU skip a whole
   full-frame RGBA scratch plane. Same values either way — the alpha plane
   carries the POST-Shift-Edge alpha, which is exactly what the CPU's zero
   pass keys off. */
MEB_FN MEB_RGBA MEB_SharpTexel(float a, float r, float g, float b)
{
    MEB_RGBA o;
    o.a = a;
    if (a <= 0.0f) { o.r = 0.0f; o.g = 0.0f; o.b = 0.0f; }
    else           { o.r = r;    o.g = g;    o.b = b;    }
    return o;
}

/* The Blur Amount input texel. premult != 0 -> honestly premultiplied
   (rgb * a, every pixel), which is what stops a partial-alpha edge pixel from
   contributing its FULL colour to the kernel sum instead of its
   coverage-weighted share. premult == 0 is the blend-only frame's straight
   copy, which Phase 4 consumes directly with no blur in between. */
MEB_FN MEB_RGBA MEB_DeriveTexel(float a, float r, float g, float b, int premult)
{
    const MEB_RGBA s = MEB_SharpTexel(a, r, g, b);
    MEB_RGBA o;
    o.a = a;
    if (premult != 0) { o.r = s.r * a; o.g = s.g * a; o.b = s.b * a; }
    else              { o.r = s.r;     o.g = s.g;     o.b = s.b;     }
    return o;
}

/* ------------------------------------------------------------------------- */
/* Phase 2 tail — unpremultiply.                                             */
/* <- the `unpremult` block of MEB_BlurColsRGBA (MEB_CoreMath.h).            */
/* ------------------------------------------------------------------------- */

/* rgb /= max(blurred alpha, eps). Safe on honestly premultiplied input:
   blur(c*a)/blur(a) is a convex weighted average of true colours inside the
   kernel support, mathematically bounded by them — no amplification possible.
   The clamp only bites where blurred alpha ~ 0, where RGB is ~ 0 too. */
MEB_FN MEB_RGBA MEB_UnpremultTexel(MEB_RGBA v)
{
    const float inv = 1.0f / MEB_MAX(v.a, MEB_UNPREMULT_EPS);
    MEB_RGBA o;
    o.a = v.a;
    o.r = v.r * inv;
    o.g = v.g * inv;
    o.b = v.b * inv;
    return o;
}

/* ------------------------------------------------------------------------- */
/* Phase 1 helpers — circular morphological min/max. <- MorphPlane.          */
/* ------------------------------------------------------------------------- */

/* The structuring element's membership test, on the INTEGER offsets, exactly
   as the CPU spells it. Deriving a per-row dx bound from a sqrt instead would
   round differently at the exact boundary and silently change the shape. */
MEB_FN int MEB_MorphInside(int dx, int dy, float r2)
{
    return ((float)(dx * dx + dy * dy) > r2) ? 0 : 1;
}

/* Starting accumulator: 0 for a dilate (max filter), 1 for an erode (min). */
MEB_FN float MEB_MorphSeed(int dilate)
{
    return dilate != 0 ? 0.0f : 1.0f;
}

/* Keep whichever of (best, a) the filter direction wants. */
MEB_FN float MEB_MorphKeep(float best, float a, int dilate)
{
    if (dilate != 0) { if (a > best) return a; }
    else             { if (a < best) return a; }
    return best;
}

/* Running minimum for the directional erosion. <- MEB_DirMinRowInline. */
MEB_FN float MEB_MinKeep(float m, float a)
{
    return a < m ? a : m;
}

/* ------------------------------------------------------------------------- */
/* Phase 4 — composite. <- MEB_CompositePixel (MEB_CoreMath.h).              */
/* ------------------------------------------------------------------------- */

/* Alpha-weighted blend of the sharp (top) and blurred (base) layers, both in
   STRAIGHT colour, producing straight colour plus the blurred alpha:

       out_rgb   = (sh*w*sh_a + bl*(1-w)*bl_a) / (w*sh_a + (1-w)*bl_a)
       out_alpha = bl_a

   A plain lerp by w would mix the zeroed-black cliff in just outside the sharp
   matte — where Blend Radius's feathered mask still gives w a small nonzero
   value but sharp coverage is 0 — drawing a thin dark line that grows with
   Blend Radius. Weighting each layer by (blend weight x its own alpha) drops
   the sharp term exactly where its coverage is zero and reduces to the plain
   lerp in the interior. NO alpha multiply on the result: AE applies alpha
   itself at composite time.

   Written with ifs rather than a ternary over the struct: HLSL has no
   conditional operator over struct types (fxc: "type mismatch between
   conditional values"). */
MEB_FN MEB_RGBA MEB_CompositeTexel(MEB_RGBA sh, MEB_RGBA bl, float w_raw)
{
    const float w     = MEB_ClampF(w_raw, 0.0f, 1.0f);
    const float sw    = w * sh.a;
    const float bw    = (1.0f - w) * bl.a;
    const float denom = sw + bw;

    MEB_RGBA o;
    o.a = bl.a;
    if (denom > MEB_COMPOSITE_EPS) {
        const float norm = 1.0f / denom;
        o.r = (sh.r * sw + bl.r * bw) * norm;
        o.g = (sh.g * sw + bl.g * bw) * norm;
        o.b = (sh.b * sw + bl.b * bw) * norm;
    } else {
        o.r = bl.r;
        o.g = bl.g;
        o.b = bl.b;
    }
    return o;
}

#endif /* MEB_KERNEL_MATH_H */
