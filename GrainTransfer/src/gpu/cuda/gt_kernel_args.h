/*  gt_kernel_args.h — the launch-argument structs, compiled from the kernels.

    Host counterpart of gt_kernels.cu: the same .kernel file, through the host
    prologue instead of the CUDA one. Each entry point's BUF_* and PARAM_BLOCK
    declarations become `gtk_<entry>::Args`, which is exactly the struct nvcc
    made the kernel take.

    WHY THIS RATHER THAN A HAND-WRITTEN MIRROR: a hand-written struct is a
    second declaration of the same thing, and the two drift silently — reorder
    a PARAM_BLOCK field and the host keeps compiling while every launched
    kernel reads the wrong constant. Compiling the one declaration twice makes
    that impossible; there is nothing left to keep in sync, so there is nothing
    to assert.

    Including this header also type-checks the kernel body as ordinary C++ on
    every build, with no GPU and no CUDA toolkit present — and makes the body
    RUNNABLE, which is what tests/kernel_test.cpp executes. That test therefore
    diffs the shipped kernel source against gt::StampRow, not a transcription
    of it.
*/

#pragma once

#include <cstddef>
#include <cstring>

#include "../kernels/gt_prologue_host.h"

#include "../kernels/gt_stamp.kernel"   // gt_stamp, gt_copy

namespace gtgpu {

// Readable aliases for the host. Order matches GT_CudaKernelId /
// kGT_CudaKernelNames in gt_gpu_cuda.cpp.
using ArgsStamp = gtk_gt_stamp::Args;
using ArgsCopy  = gtk_gt_copy::Args;

// Fill an entry's GT_RESP_DECL floats from the block gt::PackResponse built.
// The macro declares them consecutively and they are all float, so the block
// copies verbatim — asserted here rather than assumed.
template <typename ArgsT>
inline void GT_StoreResponse(ArgsT &a, const float *packed)
{
    static_assert(offsetof(ArgsT, gRFlat2) - offsetof(ArgsT, gRMid0)
                      == (GT_RESP_FLOATS - 1) * sizeof(float),
                  "GT_RESP_DECL fields must stay contiguous and float");
    std::memcpy(&a.gRMid0, packed, GT_RESP_FLOATS * sizeof(float));
}

// Run one kernel over its whole grid on the CPU, exactly as a launch would —
// the same guard, the same per-thread entry. Used by the AE-free tests; the
// real host launches instead, but it launches THIS struct.
template <typename ArgsT>
inline void GT_RunGrid(const ArgsT &a, int w, int h)
{
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            a.run(x, y);
}

} // namespace gtgpu
