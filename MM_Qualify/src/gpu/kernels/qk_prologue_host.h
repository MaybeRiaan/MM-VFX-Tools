/*  qk_prologue_host.h — host-C++ spellings of the shared kernel vocabulary.

    The third prologue. qk_prologue_hlsl.h serves dxc, qk_prologue_cuda.h
    serves nvcc, and this one compiles the SAME .kernel bodies as ordinary
    C++. It exists for two reasons, both load-bearing:

    1. THE LAYOUT CONTRACT. qk_gpu_cuda.cpp must fill an argument struct whose
       members match the one nvcc compiled, member for member. Rather than
       hand-writing a mirror and hoping it stays in sync — the exact drift
       gpu-kernel-rules §0 and AE_GPU_Effects.md §4.2 warn about — the host
       includes the kernels through this header and gets the identical struct
       by construction. There is one declaration; it is compiled twice.

    2. FREE TYPE-CHECKING AND A RUNNABLE KERNEL, on every build, with no GPU
       and no CUDA toolkit. AE_GPU_Effects.md §4.5 keeps an HLSL entry in the
       build purely so the compiler validates the whole translation unit; this
       does the same job for the C++ side. And because a body compiled here is
       ordinary code, `qkk_<entry>::Args{...}.run(x, y)` over a grid EXECUTES
       the real kernel on the CPU — which is what lets tests/kernel_test.cpp
       and tests/pipeline_test.cpp diff the actual shipped kernel bodies
       against the CPU reference instead of a hand transcription that can rot.

    The math vocabulary (QK_FLOOR, QK_SAT, QK_OUTF, ...) comes straight from
    ../../qk_cpu_prologue.h — the SAME spellings the CPU render path compiles
    qk_core.h with, so "kernel == CPU" is not approximately true here, it is
    the same object code.

    Nothing in this file is algorithm. Nothing in a .kernel file is syntax.
*/

#ifndef QK_PROLOGUE_HOST_H
#define QK_PROLOGUE_HOST_H

#include "../../qk_cpu_prologue.h"

/* Mirrors the CUDA/HLSL threadgroup geometry. Nothing dispatches from the
   host, but the names must exist because the vocabulary is shared. */
#define QK_TG_X 8
#define QK_TG_Y 8

/* Entry-point brackets — the same struct shape nvcc builds, minus the
   __global__ trampoline (there is nothing to launch here). Keeping the member
   declarations byte-identical to the CUDA prologue's is the whole point: same
   declarations, same order, same types, therefore same layout. */
#define KERNEL_BEGIN(name)                                                     \
    namespace qkk_##name { struct Args {

#define KERNEL_END(name)                                                       \
    }; }

#define KERNEL_ENTRY(name) void run(int qk_gx, int qk_gy) const

#define BUF_IN(name, slot)  const float *name
#define BUF_OUT(name, slot) float *name

#define PARAM_BLOCK_BEGIN
#define PARAM_BLOCK_END

#define GID_X qk_gx
#define GID_Y qk_gy

#define QK_GUARD(W, H) if (GID_X >= (W) || GID_Y >= (H)) return;

#define QK_IDX(x, y, W) (((y) * (W) + (x)) * 4)

#endif /* QK_PROLOGUE_HOST_H */
