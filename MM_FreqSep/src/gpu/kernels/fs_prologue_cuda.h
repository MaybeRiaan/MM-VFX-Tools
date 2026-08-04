/*  fs_prologue_cuda.h — CUDA spellings of the shared kernel vocabulary.

    Included BEFORE the .kernel bodies (see ../cuda/fs_kernels.cu). Defines
    every platform-specific name the bodies are allowed to use; the HLSL twin is
    fs_prologue_hlsl.h and the host-C++ twin is fs_prologue_host.h. All three
    define the SAME names, so the bodies never know which one they got.

    Nothing in this file is algorithm. Nothing in a .kernel file is syntax.

    ── HOW THIS DIFFERS FROM THE HLSL PROLOGUE, AND WHY THE BRACKETS EXIST ────
    HLSL binds buffers by REGISTER at file scope: `RWStructuredBuffer<float> b :
    register(u0);` sits outside every entry point and all of them see it. CUDA
    has no such thing — a kernel's buffers and constants are its FUNCTION
    PARAMETERS. Metal is the same (`device float* b [[buffer(0)]]`), so this is
    not a CUDA quirk; it is every backend except HLSL.

    KERNEL_BEGIN/KERNEL_END bracket each entry point so that its BUF_* and
    PARAM_BLOCK declarations — unchanged, in their original order — become the
    MEMBERS of one per-entry `Args` struct, and the body becomes a method on it.
    The body then reads `gSrc` and `gW` as inherited scope, exactly as it read
    the file-scope HLSL globals. Register/binding order is preserved because the
    declaration order is preserved.

    Args is passed BY VALUE as the kernel argument: no __device__ globals, no
    per-launch constant-bank writes, therefore nothing shared between concurrent
    MFR renders (rules §6's MFR-safety argument holds by construction here).

    ── LAYOUT CONTRACT ───────────────────────────────────────────────────────
    The host builds the same structs by including the same .kernel files through
    fs_prologue_host.h. Member order and types therefore cannot drift: there is
    one declaration, compiled twice. Do NOT hand-write a mirror struct in the
    host — that is the drift this arrangement exists to prevent.
*/

#ifndef FS_PROLOGUE_CUDA_H
#define FS_PROLOGUE_CUDA_H

// ---------------------------------------------------------------------------
// Threadgroup geometry. Fixed HERE, never in a kernel body — same rule as the
// HLSL prologue. 8x8 = 64 threads, a whole warp pair; the host derives its grid
// from these two names so the two cannot disagree.
// ---------------------------------------------------------------------------
#define FS_TG_X 8
#define FS_TG_Y 8

// ---------------------------------------------------------------------------
// Entry-point brackets. See the header note above.
//
//   KERNEL_BEGIN(n)  opens  namespace fsk_n { struct Args {
//   BUF_IN/BUF_OUT   declare pointer members (declaration order == slot order)
//   PARAM_BLOCK_*    vanish, so the param fields become members too
//   KERNEL_ENTRY(n)  opens the body as a method
//   KERNEL_END(n)    closes both and emits the __global__ trampoline
// ---------------------------------------------------------------------------
#define KERNEL_BEGIN(name)                                                     \
    namespace fsk_##name { struct Args {

#define KERNEL_END(name)                                                       \
    }; }                                                                       \
    extern "C" __global__ void name(fsk_##name::Args fs_a)                     \
    {                                                                          \
        fs_a.run((int)(blockIdx.x * blockDim.x + threadIdx.x),                 \
                 (int)(blockIdx.y * blockDim.y + threadIdx.y));                \
    }

// The body. `const` because a kernel never mutates its own argument struct —
// it writes THROUGH the output pointers, which stay writable (`float *const`).
#define KERNEL_ENTRY(name) __device__ void run(int fs_gx, int fs_gy) const

// ---------------------------------------------------------------------------
// Buffers. AE hands us raw linear device memory, NOT textures — no samplers, no
// bilinear, no clamp-to-edge. Deliberate and load-bearing (rules §3.2): index
// math only, so out-of-range taps can be SKIPPED exactly as blur.cpp does.
//
// Inputs are const here where HLSL had to declare them RW (a DX12 resource
// cannot be SRV in one dispatch and UAV in the next without state transitions
// whose before-state the host cannot know). CUDA has no resource-state machine,
// so the honest const is free — and it makes "a kernel never writes its inputs"
// a compiler-enforced rule on this backend rather than a convention.
//
// The `slot` argument is documentation here: CUDA binds positionally and
// declaration order already IS the binding order. It must still match the HLSL
// register numbering, which is what makes the host's argument-filling code
// identical in shape across backends.
// ---------------------------------------------------------------------------
#define BUF_IN(name, slot)  const float *name
#define BUF_OUT(name, slot) float *name

#define PARAM_BLOCK_BEGIN
#define PARAM_BLOCK_END

#define GID_X fs_gx
#define GID_Y fs_gy

// Bounds guard. CUDA grids are whole blocks like DX12's, so partial tiles at
// the right/bottom edge must be masked exactly as they are on DX.
#define FS_GUARD(W, H) if (GID_X >= (W) || GID_Y >= (H)) return;

#define KF_EXP(x)   ::expf(x)
#define KF_FLOOR(x) ::floorf(x)
#define KF_ABS(x)   ::fabsf(x)
#define KF_MIN(a,b) ::fminf(a,b)
#define KF_MAX(a,b) ::fmaxf(a,b)

// ---------------------------------------------------------------------------
// Wide accumulator for box sums (fs_guided.kernel). double, matching HLSL —
// measured, not cosmetic: a direct float window sum diverges from guided.cpp's
// double sliding window by up to 9.5e-06 at R=25 and grows with radius, eating
// the whole 1e-5 tolerance.
//
// >> PERFORMANCE NOTE, and it is specific to this backend: consumer GeForce
// >> cards run fp64 at 1/64 of fp32 rate (0.34 vs 21.7 TFLOPS on a 3070 Ti).
// >> That is affordable only while the box sum is O(1) per pixel. It is NOT
// >> affordable against fs_guided.kernel's current O(r) direct sum at the radii
// >> this effect actually reaches — GuidedRadius(sigma=90) ~= 110, a 221-tap
// >> window, which is ~110x the CPU's work at ~3x the throughput. See the
// >> divergence note in fs_guided.kernel.
// ---------------------------------------------------------------------------
#define ACCUM_T        double
#define ACC_ZERO       0.0
#define ACC_ADD(a, v)  ((a) + (double)(v))
#define ACC_SUB(a, v)  ((a) - (double)(v))
#define ACC_GET(a)     ((float)(a))

// Premultiplied ARGB indexing. Channel order matches fs::UnpackRow:
// 0=A 1=R 2=G 3=B (rules §3.4).
#define FS_IDX(x, y, W) (((y) * (W) + (x)) * 4)

#endif // FS_PROLOGUE_CUDA_H
