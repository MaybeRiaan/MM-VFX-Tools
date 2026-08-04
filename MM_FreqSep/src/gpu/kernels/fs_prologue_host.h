/*  fs_prologue_host.h — host-C++ spellings of the shared kernel vocabulary.

    The third prologue. fs_prologue_hlsl.h serves dxc, fs_prologue_cuda.h serves
    nvcc, and this one compiles the SAME .kernel bodies as ordinary C++.

    It exists for two reasons, both load-bearing:

    1. THE LAYOUT CONTRACT. mm_gpu_cuda.cpp must fill an argument struct whose
       members match the one nvcc compiled, member for member. Rather than
       hand-writing a mirror and hoping it stays in sync — the exact drift
       rules §0 and AE_GPU_Effects.md §4.2 warn about — the host includes the
       kernels through this header and gets the identical struct by
       construction. There is one declaration; it is compiled twice.

    2. FREE TYPE-CHECKING, on every build, with no GPU. AE_GPU_Effects.md §4.5
       keeps an HLSL entry in the build purely so fxc validates the whole
       translation unit; this does the same job for the C++ side, and caught the
       same class of construct.

    A body compiled through this header is also RUNNABLE: call
    fsk_<entry>::Args{...}.run(x, y) in a loop over the grid and you have
    executed the real kernel on the CPU. tests/gpu_parity_test.cpp currently
    hand-transcribes the bodies instead (see its header note) — this is the
    header that lets that transcription be deleted.

    Nothing in this file is algorithm. Nothing in a .kernel file is syntax.
*/

#ifndef FS_PROLOGUE_HOST_H
#define FS_PROLOGUE_HOST_H

#include <cmath>

// Mirrors the CUDA/HLSL threadgroup geometry. Nothing dispatches from the host,
// but the names must exist because the vocabulary is shared.
#define FS_TG_X 8
#define FS_TG_Y 8

// ---------------------------------------------------------------------------
// Entry-point brackets — the same struct shape nvcc builds, minus the
// __global__ trampoline (there is nothing to launch on the host). Keeping the
// member declarations byte-identical to the CUDA prologue's is the whole point:
// same declarations, same order, same types, therefore same layout.
// ---------------------------------------------------------------------------
#define KERNEL_BEGIN(name)                                                     \
    namespace fsk_##name { struct Args {

#define KERNEL_END(name)                                                       \
    }; }

#define KERNEL_ENTRY(name) void run(int fs_gx, int fs_gy) const

#define BUF_IN(name, slot)  const float *name
#define BUF_OUT(name, slot) float *name

#define PARAM_BLOCK_BEGIN
#define PARAM_BLOCK_END

#define GID_X fs_gx
#define GID_Y fs_gy

#define FS_GUARD(W, H) if (GID_X >= (W) || GID_Y >= (H)) return;

#define KF_EXP(x)   std::exp(x)
#define KF_FLOOR(x) std::floor(x)
#define KF_ABS(x)   std::fabs(x)
#define KF_MIN(a,b) ((a) < (b) ? (a) : (b))
#define KF_MAX(a,b) ((a) > (b) ? (a) : (b))

// double on every backend that has it — see fs_prologue_cuda.h for the measured
// justification and the fp64-rate caveat.
#define ACCUM_T        double
#define ACC_ZERO       0.0
#define ACC_ADD(a, v)  ((a) + (double)(v))
#define ACC_SUB(a, v)  ((a) - (double)(v))
#define ACC_GET(a)     ((float)(a))

#define FS_IDX(x, y, W) (((y) * (W) + (x)) * 4)

#endif // FS_PROLOGUE_HOST_H
