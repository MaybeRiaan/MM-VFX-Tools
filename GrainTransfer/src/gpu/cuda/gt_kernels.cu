/*  gt_kernels.cu — CUDA shim for the Grain Transfer GPU render.

    THE SHIPPING GPU BACKEND, and the only one. After Effects 2026 on
    Windows/NVIDIA offers effects what_gpu = CUDA and never probes DIRECTX
    (measured 2026-07-23 — ../../../docs/AE_GPU_Effects.md §1.1/§1.2). Unlike
    MM_Qualify and MM_FreqSep this plugin has no DX12 twin: it was written
    after that measurement, so it was never aimed at the wrong backend. The
    HLSL prologue still exists and one entry still type-checks through dxc
    (§4 item 5), which is a compile-time guard, not a second host.

    This file is deliberately thin: a prologue and one include. Every entry
    point, every buffer binding and every line of arithmetic comes from the
    platform-free body in ../kernels/gt_stamp.kernel — the same body dxc
    compiles, and (through ../kernels/gt_prologue_host.h) the same body
    tests/test_kernel.cpp runs on the CPU and diffs against gt::StampRow at
    exactly 0.0. There is no CUDA-specific math anywhere in this plugin, which
    is the property that lets that test's verdict cover this backend too.

    ../../gt_core.h rides along inside the .kernel file, so the stamp's
    per-pixel decisions — apply-space encode/decode, the response curve
    lookup, the alpha contract — are the literal same source the CPU render
    path compiles.

    nvcc compiles this single translation unit into ONE PTX module carrying
    both entry points, matching EdgeRepair's, MM_FreqSep's and MM_Qualify's
    arrangement (one .cu -> one .ptx, loaded with cuModuleLoadDataEx and
    resolved by name). The deployed CUDA_Assets/GrainTransfer_Kernels.ptx is a
    single file to keep in step with the .aex beside it.

    Entry points, in the order gt_gpu_cuda.cpp's GT_CudaKernelId enumerates
    them — the names are resolved by STRING at device setup, so they must match
    kGT_CudaKernelNames exactly.
*/

#include "../kernels/gt_prologue_cuda.h"

#include "../kernels/gt_stamp.kernel"   // gt_stamp, gt_copy
