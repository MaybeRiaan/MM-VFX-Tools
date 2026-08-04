/*  fs_kernel_args.h — the launch-argument structs, compiled from the kernels.

    Host counterpart of fs_kernels.cu: the same five .kernel files, through the
    host prologue instead of the CUDA one. Each entry point's BUF_* and
    PARAM_BLOCK declarations become `fsk_<entry>::Args`, which is exactly the
    struct nvcc made the kernel take.

    WHY THIS RATHER THAN A HAND-WRITTEN MIRROR: a hand-written struct is a
    second declaration of the same thing, and the two drift silently — reorder a
    PARAM_BLOCK field and the host keeps compiling while every launched kernel
    reads the wrong constant. Compiling the one declaration twice makes that
    impossible. It is the same reasoning EdgeRepair applies with its
    static_assert-guarded ER_GPU_PassParams, taken one step further: there is
    nothing left to assert BECAUSE there is nothing to keep in sync.

    Including this header also type-checks every kernel body as ordinary C++ on
    every build, with no GPU and no toolkit present.
*/

#pragma once

#include "../kernels/fs_prologue_host.h"

#include "../kernels/fs_gauss.kernel"
#include "../kernels/fs_guided.kernel"
#include "../kernels/fs_compose.kernel"
#include "../kernels/fs_tile.kernel"
#include "../kernels/fs_matte.kernel"

namespace fsgpu {

// Readable aliases for the host. Order matches FS_CudaKernelId /
// kFS_CudaKernelNames in mm_gpu_cuda.cpp.
using ArgsGauss    = fsk_fs_gauss_pass::Args;
using ArgsBox      = fsk_fs_guided_box::Args;
using ArgsSquares  = fsk_fs_guided_squares::Args;
using ArgsAB       = fsk_fs_guided_ab::Args;
using ArgsFinal    = fsk_fs_guided_final::Args;
using ArgsCompose  = fsk_fs_compose::Args;
using ArgsLoad     = fsk_fs_load_tile::Args;
using ArgsStore    = fsk_fs_store_tile::Args;
using ArgsFill     = fsk_fs_fill_ones::Args;
using ArgsMatte    = fsk_fs_matte::Args;

} // namespace fsgpu
