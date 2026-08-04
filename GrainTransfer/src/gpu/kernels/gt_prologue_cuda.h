/*  gt_prologue_cuda.h — CUDA spellings of the shared kernel vocabulary.

    Included BEFORE the .kernel bodies (see ../cuda/gt_kernels.cu). Defines
    every platform-specific name the bodies — and gt_core.h, which they
    include — are allowed to use. The HLSL twin is gt_prologue_hlsl.h and the
    host-C++ twin is gt_prologue_host.h. All three define the SAME names, so a
    body never knows which one it got.

    Nothing in this file is algorithm. Nothing in a .kernel file is syntax.

    -- WHY THE BRACKETS EXIST ------------------------------------------------
    HLSL binds buffers by REGISTER at file scope: `RWStructuredBuffer<float> b
    : register(u0);` sits outside every entry point and all of them see it.
    CUDA has no such thing — a kernel's buffers and constants are its FUNCTION
    PARAMETERS. Metal is the same (`device float* b [[buffer(0)]]`), so this is
    not a CUDA quirk; HLSL is the outlier (gpu-kernel-rules §2.1).

    KERNEL_BEGIN/KERNEL_END bracket each entry point so its BUF_* and
    PARAM_BLOCK declarations — unchanged, in their original order — become the
    MEMBERS of one per-entry `Args` struct, and the body becomes a method on
    it. The body then reads `gGtTiles` and `gGtSrc` as inherited scope, exactly
    as it read the file-scope HLSL globals; binding order is preserved because
    declaration order is preserved.

    Args is passed BY VALUE as the kernel argument: no __device__ globals, no
    per-launch constant-bank writes, therefore nothing shared between
    concurrent MFR renders by construction.

    -- LAYOUT CONTRACT -------------------------------------------------------
    The host builds the same structs by including the same .kernel files
    through gt_prologue_host.h (../cuda/gt_kernel_args.h). Member order and
    types cannot drift: one declaration, compiled twice. Do NOT hand-write a
    mirror struct in the host — that is the drift this arrangement prevents.
*/

#ifndef GT_PROLOGUE_CUDA_H
#define GT_PROLOGUE_CUDA_H

/* ---- threadgroup geometry (prologue-only decision) ----------------------
   8x8 = 64 threads, a whole warp pair. The host derives its grid from these
   two names so grid and in-kernel guard cannot disagree. */
#define GT_TG_X 8
#define GT_TG_Y 8

/* ---- entry-point brackets ------------------------------------------------
     KERNEL_BEGIN(n)  opens  namespace gtk_n { struct Args {
     BUF_IN/BUF_OUT   declare pointer members (declaration order == slot order)
     PARAM_BLOCK_*    vanish, so the param fields become members too
     KERNEL_ENTRY(n)  opens the body as a method
     KERNEL_END(n)    closes both and emits the __global__ trampoline        */
#define KERNEL_BEGIN(name)                                                     \
    namespace gtk_##name { struct Args {

#define KERNEL_END(name)                                                       \
    }; }                                                                       \
    extern "C" __global__ void name(gtk_##name::Args gt_a)                     \
    {                                                                          \
        gt_a.run((int)(blockIdx.x * blockDim.x + threadIdx.x),                 \
                 (int)(blockIdx.y * blockDim.y + threadIdx.y));                \
    }

/* The body. `const` because a kernel never mutates its own argument struct —
   it writes THROUGH the output pointers, which stay writable. */
#define KERNEL_ENTRY(name) __device__ void run(int gt_gx, int gt_gy) const

/* ---- buffers ------------------------------------------------------------
   AE hands us raw linear device memory, NOT textures — no samplers, no
   hardware bilinear, no wrap addressing. Deliberate and load-bearing: the
   tile lookup does its own index math (gt_core.h GT_GrainAddr) so both
   backends wrap identically rather than inheriting a sampler's rounding.

   Inputs are const here where HLSL had to declare them RW (a DX12 resource
   cannot be SRV in one dispatch and UAV in the next without state transitions
   whose before-state the host cannot know). CUDA has no resource-state
   machine, so the honest const is free.

   The `slot` argument is documentation on this backend: CUDA binds
   positionally and declaration order already IS the binding order. It must
   still match the HLSL register numbering (in u0..u3, out u4..u7), which is
   what keeps the two hosts' argument-filling code identical in shape.      */
#define BUF_IN(name, slot)  const float *name
#define BUF_OUT(name, slot) float *name

#define PARAM_BLOCK_BEGIN
#define PARAM_BLOCK_END

#define GID_X gt_gx
#define GID_Y gt_gy

/* Bounds guard. CUDA grids are whole blocks like DX12's, so partial tiles at
   the right/bottom edge must be masked exactly as they are on DX. */
#define GT_GUARD(W, H) if (GID_X >= (W) || GID_Y >= (H)) return;

/* ---- math vocabulary (must mirror ../../gt_cpu_prologue.h) ---------------
   gt_core.h is compiled through this prologue too — every per-pixel decision
   the stamp makes comes from that one shared body, on both backends.

   MIN/MAX are spelled as the CPU prologue's ternaries rather than
   ::fminf/::fmaxf ON PURPOSE. The C++ and CUDA compiles of gt_core.h are the
   two sides the AE-free harness diffs at EXACTLY 0.0 (tests/kernel_test.cpp),
   so the two spellings must agree bit-for-bit including their NaN and
   signed-zero behaviour. fma contraction is the one difference left, and
   rules §3.6 absorbs it in the 1e-5 real-GPU tolerance.

   log2f/exp2f are the ACEScct path (gt_core.h GT_LinToLog / GT_LogToLin) and
   are the single most likely place for a backend to disagree — which is why
   the validation checklist sweeps a scene-linear project explicitly.       */
#define GT_FUNC     __device__ static
#define GT_FLOOR(x) ::floorf(x)
#define GT_FABS(x)  ::fabsf(x)
#define GT_LOG2(x)  ::log2f(x)
#define GT_EXP2(x)  ::exp2f(x)
#define GT_MIN(a,b) ((a) < (b) ? (a) : (b))
#define GT_MAX(a,b) ((a) > (b) ? (a) : (b))
#define GT_OUTF(n)  float &n
#define GT_OUTI(n)  int &n

#endif /* GT_PROLOGUE_CUDA_H */
