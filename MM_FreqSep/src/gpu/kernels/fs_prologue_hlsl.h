/*  fs_prologue_hlsl.h — HLSL spellings of the shared kernel vocabulary.

    Included BEFORE a .kernel body. Defines every platform-specific name the
    kernel bodies are allowed to use; see docs/gpu-kernel-rules.md §2.

    The Metal counterpart (fs_prologue_metal.h) must define the SAME names with
    MSL spellings and, critically, the SAME binding ORDER — kernel bodies index
    buffers positionally and cannot see the difference.

    Nothing in this file is algorithm. Nothing in a .kernel file is syntax.
*/

#ifndef FS_PROLOGUE_HLSL_H
#define FS_PROLOGUE_HLSL_H

// ---------------------------------------------------------------------------
// Threadgroup geometry. Fixed HERE, never in a kernel body — Metal will likely
// want a different size (Apple GPUs prefer multiples of the 32-wide SIMD) and
// that must remain a prologue-only edit.
// ---------------------------------------------------------------------------
#define FS_TG_X 8
#define FS_TG_Y 8

// Entry point. DX12 has no partial threadgroups, so every kernel body carries
// its own explicit bounds guard (FS_GUARD below) — which also keeps the body
// correct under Metal's dispatchThreads: even though Metal wouldn't need it.
#define KERNEL_ENTRY(name)                     \
    [numthreads(FS_TG_X, FS_TG_Y, 1)]          \
    void name(uint3 fs_tid : SV_DispatchThreadID)

#define GID_X (int)fs_tid.x
#define GID_Y (int)fs_tid.y

// Buffers. StructuredBuffer<float> over AE's linear GPU world: AE hands us a
// raw device buffer, NOT a texture — no samplers, no bilinear, no clamp-to-edge
// behaviour. That is deliberate and load-bearing (rules §3.2): index math only,
// so out-of-range taps can be SKIPPED exactly as blur.cpp does.
#define BUF_IN(name, slot)  StructuredBuffer<float>   name : register(t##slot)
#define BUF_OUT(name, slot) RWStructuredBuffer<float> name : register(u##slot)

#define PARAM_BLOCK_BEGIN cbuffer FS_Params : register(b0) {
#define PARAM_BLOCK_END   };

// Math intrinsics that are not 1:1 between HLSL and MSL get a wrapper. Plain
// arithmetic does not need one.
#define KF_EXP(x)   exp(x)
#define KF_FLOOR(x) floor(x)
#define KF_ABS(x)   abs(x)
#define KF_MIN(a,b) min(a,b)
#define KF_MAX(a,b) max(a,b)

// Standard bounds guard — see note on KERNEL_ENTRY.
#define FS_GUARD(W, H) if (GID_X >= (W) || GID_Y >= (H)) return;

// ---------------------------------------------------------------------------
// Wide accumulator for box sums (fs_guided.kernel). NOT cosmetic — measured:
// a direct float window sum diverges from guided.cpp's double sliding window by
// up to 9.5e-06 at R=25 and grows with radius, which would eat the whole 1e-5
// tolerance. In double it is bit-exact (0.000e+00) at every size probed.
//
// HLSL SM6 supports double natively, so DX just uses it.
//
// >> METAL: Apple GPUs do NOT support double in shaders. The Metal prologue
// >> must implement these same five names as a compensated (Kahan) float sum:
// >>   struct { float s, c; }  with ACC_ADD doing the error-compensation step.
// >> The kernel body must not change — that is the entire point of routing
// >> accumulation through this vocabulary. See docs/gpu-kernel-rules.md §4.
// ---------------------------------------------------------------------------
#define ACCUM_T        double
#define ACC_ZERO       0.0
#define ACC_ADD(a, v)  ((a) + (double)(v))
#define ACC_SUB(a, v)  ((a) - (double)(v))
#define ACC_GET(a)     ((float)(a))

// Premultiplied ARGB indexing. Channel order matches mm::UnpackRow:
// 0=A 1=R 2=G 3=B (rules §3.4).
#define FS_IDX(x, y, W) (((y) * (W) + (x)) * 4)

#endif // FS_PROLOGUE_HLSL_H
