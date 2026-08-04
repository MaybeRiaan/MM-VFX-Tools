/*  fs_kernels.cu — CUDA shim for the MM FreqSep GPU render.

    THE SHIPPING GPU BACKEND. After Effects 2026 on Windows/NVIDIA offers
    effects what_gpu = CUDA and never probes DIRECTX (measured 2026-07-23 —
    ../../../docs/AE_GPU_Effects.md §1.1); the DX12 twin in ../dx/ serves
    Premiere hosts and stays maintained.

    This file is the CUDA counterpart of the ../dx/fs_*_cs.hlsl shims, and it is
    just as thin: a prologue and a list of includes. Every entry point, every
    buffer binding and every line of arithmetic comes from the platform-free
    .kernel bodies in ../kernels/ — the same bodies dxc compiles. There is no
    CUDA-specific math anywhere in the pipeline, which is the property that lets
    tests/gpu_parity_test.cpp's verdict cover this backend too.

    One difference from DX by necessity, not taste: dxc compiles one entry point
    per invocation (-E) from several shim files, whereas nvcc compiles this
    single translation unit into ONE PTX module carrying all ten entry points.
    That matches EdgeRepair's arrangement (one .cu -> one .ptx, loaded with
    cuModuleLoadDataEx and resolved by name) and means the deployed
    CUDA_Assets/MM_FreqSep_Kernels.ptx is a single file to keep in step with the
    .aex beside it.

    Entry points, in the order mm_gpu_cuda.cpp's FS_CudaKernelId enumerates them
    — the names below are resolved by STRING at device setup, so they must match
    kFS_CudaKernelNames exactly.
*/

#include "../kernels/fs_prologue_cuda.h"

#include "../kernels/fs_gauss.kernel"    // fs_gauss_pass
#include "../kernels/fs_guided.kernel"   // fs_guided_box, _squares, _ab, _final
#include "../kernels/fs_compose.kernel"  // fs_compose
#include "../kernels/fs_tile.kernel"     // fs_load_tile, fs_store_tile, fs_fill_ones
#include "../kernels/fs_matte.kernel"    // fs_matte
