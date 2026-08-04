/*  qk_kernels.cu — CUDA shim for the MM Qualify GPU render.

    THE SHIPPING GPU BACKEND. After Effects 2026 on Windows/NVIDIA offers
    effects what_gpu = CUDA and never probes DIRECTX (measured 2026-07-23 —
    ../../../docs/AE_GPU_Effects.md §1.1); the DX12 twin in ../dx/ serves
    Premiere hosts and stays maintained.

    This file is the CUDA counterpart of the ../dx/qk_*_cs.hlsl shims, and it
    is just as thin: a prologue and a list of includes. Every entry point,
    every buffer binding and every line of arithmetic comes from the
    platform-free .kernel bodies in ../kernels/ — the same bodies dxc compiles,
    and (through ../kernels/qk_prologue_host.h) the same bodies
    tests/kernel_test.cpp and tests/pipeline_test.cpp run on the CPU. There is
    no CUDA-specific math anywhere in the pipeline, which is the property that
    lets those tests' verdict cover this backend too.

    qk_core.h rides along inside the .kernel files, so the qualifier's
    per-pixel decisions — gates, clip, ratio, IIR step, compose — are the
    literal same source the CPU render path compiles.

    One difference from DX by necessity, not taste: dxc compiles one entry
    point per invocation (-E) from several shim files, whereas nvcc compiles
    this single translation unit into ONE PTX module carrying all nine entry
    points. That matches EdgeRepair's and MM_FreqSep's arrangement (one .cu ->
    one .ptx, loaded with cuModuleLoadDataEx and resolved by name) and means
    the deployed CUDA_Assets/MM_Qualify_Kernels.ptx is a single file to keep in
    step with the .aex beside it.

    Entry points, in the order qk_gpu_cuda.cpp's QK_CudaKernelId enumerates
    them — the names below are resolved by STRING at device setup, so they must
    match kQK_CudaKernelNames exactly.
*/

#include "../kernels/qk_prologue_cuda.h"

#include "../kernels/qk_tile.kernel"      // qk_load_tile, qk_store_tile, qk_fill_ones
#include "../kernels/qk_gauss.kernel"     // qk_gauss_rgba, qk_gauss_f1
#include "../kernels/qk_garbage.kernel"   // qk_garbage
#include "../kernels/qk_matte.kernel"     // qk_matte
#include "../kernels/qk_iir.kernel"       // qk_iir
#include "../kernels/qk_compose.kernel"   // qk_compose
