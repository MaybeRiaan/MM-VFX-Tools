/*  fs_prologue_hlsl.h — HLSL spellings of the shared kernel vocabulary.

    Included BEFORE a .kernel body. Defines every platform-specific name the
    kernel bodies are allowed to use; see ../docs/gpu-kernel-rules.md §2.

    The Metal counterpart (fs_prologue_metal.h) must define the SAME names with
    MSL spellings and, critically, the SAME binding ORDER — kernel bodies index
    buffers positionally and cannot see the difference.

    Nothing in this file is algorithm. Nothing in a .kernel file is syntax.
*/

#ifndef FS_PROLOGUE_HLSL_H
#define FS_PROLOGUE_HLSL_H

// Token pasting through one level of expansion (so __COUNTER__ and macro
// arguments are expanded before the paste).
#define FS_PASTE2(a, b) a##b
#define FS_PASTE(a, b)  FS_PASTE2(a, b)

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

// ---------------------------------------------------------------------------
// Entry-point brackets. KERNEL_BEGIN precedes an entry's BUF_*/PARAM_BLOCK
// declarations; KERNEL_END follows its body. Both expand to NOTHING here, so
// the preprocessed HLSL is byte-for-byte what it was before the brackets
// existed — HLSL binds buffers by REGISTER at file scope and needs no help.
//
// They exist for CUDA and Metal, which both bind buffers as kernel ARGUMENTS
// and therefore cannot reference a file-scope buffer declaration at all. Those
// prologues expand this pair into a per-entry argument struct whose members are
// exactly the BUF_*/PARAM_BLOCK declarations, which the body then reads by the
// same bare names. Without the brackets, rules §0's promise ("validated on
// DirectX == validated on Metal modulo host plumbing") is unreachable: the
// vocabulary would be expressible only in HLSL.
// ---------------------------------------------------------------------------
#define KERNEL_BEGIN(name)
#define KERNEL_END(name)

#define GID_X (int)fs_tid.x
#define GID_Y (int)fs_tid.y

// ---------------------------------------------------------------------------
// Buffers. AE hands us raw linear device buffers, NOT textures — no samplers,
// no bilinear, no clamp-to-edge behaviour. That is deliberate and load-bearing
// (rules §3.2): index math only, so out-of-range taps can be SKIPPED exactly
// as blur.cpp does.
//
// EVERYTHING binds as a UAV (RWStructuredBuffer), including read-only inputs.
// The host does not own the state of AE's world/scratch resources, and on
// DX12 a resource cannot be SRV in one dispatch and UAV in the next within a
// single submission without explicit state transitions whose before-state we
// would have to guess. One binding class = one implicit COMMON→UNORDERED_ACCESS
// promotion per submission and zero transitions. Declaring an input "RW" and
// never writing it costs nothing measurable on buffer loads.
//
// REGISTER MAP (this IS the cross-platform binding contract):
//   BUF_IN (name, slot)  → u(slot)      inputs  occupy u0..u3
//   BUF_OUT(name, slot)  → u(4 + slot)  outputs occupy u4..u7
// The Metal prologue binds the same numbering: [[buffer(slot)]] for inputs,
// [[buffer(4 + slot)]] for outputs. Order, not spelling, is what must match.
// ---------------------------------------------------------------------------
#define BUF_IN(name, slot)  RWStructuredBuffer<float> name : register(FS_PASTE(u, slot))

#define FS_OUT_REG_0 u4
#define FS_OUT_REG_1 u5
#define FS_OUT_REG_2 u6
#define FS_OUT_REG_3 u7
#define BUF_OUT(name, slot) RWStructuredBuffer<float> name : register(FS_PASTE(FS_OUT_REG_, slot))

// Param blocks map to root constants at b0. A kernel FILE may contain several
// PARAM_BLOCKs (one per entry point family — fs_guided.kernel has three), so
// each expansion mints a unique cbuffer NAME via __COUNTER__; they all share
// register b0, which is legal because any single entry point uses exactly one
// of them and dxc strips the unreferenced ones.
#define PARAM_BLOCK_BEGIN cbuffer FS_PASTE(FS_Params_, __COUNTER__) : register(b0) {
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
// HLSL SM6 supports double natively (the host refuses the GPU path on devices
// without DoublePrecisionFloatShaderOps), so DX just uses it.
//
// >> METAL: Apple GPUs do NOT support double in shaders. The Metal prologue
// >> must implement these same five names as a compensated (Kahan) float sum:
// >>   struct { float s, c; }  with ACC_ADD doing the error-compensation step.
// >> The kernel body must not change — that is the entire point of routing
// >> accumulation through this vocabulary. See ../docs/gpu-kernel-rules.md §4.
// ---------------------------------------------------------------------------
#define ACCUM_T        double
#define ACC_ZERO       0.0
#define ACC_ADD(a, v)  ((a) + (double)(v))
#define ACC_SUB(a, v)  ((a) - (double)(v))
#define ACC_GET(a)     ((float)(a))

// Premultiplied ARGB indexing. Channel order matches fs::UnpackRow:
// 0=A 1=R 2=G 3=B (rules §3.4).
#define FS_IDX(x, y, W) (((y) * (W) + (x)) * 4)

#endif // FS_PROLOGUE_HLSL_H
