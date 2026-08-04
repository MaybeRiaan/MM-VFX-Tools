/*  qk_kernel_args.h — the launch-argument structs, compiled from the kernels.

    Host counterpart of qk_kernels.cu: the same six .kernel files, through the
    host prologue instead of the CUDA one. Each entry point's BUF_* and
    PARAM_BLOCK declarations become `qkk_<entry>::Args`, which is exactly the
    struct nvcc made the kernel take.

    WHY THIS RATHER THAN A HAND-WRITTEN MIRROR: a hand-written struct is a
    second declaration of the same thing, and the two drift silently — reorder
    a PARAM_BLOCK field and the host keeps compiling while every launched
    kernel reads the wrong constant. Compiling the one declaration twice makes
    that impossible; there is nothing left to keep in sync, so there is nothing
    to assert.

    Including this header also type-checks every kernel body as ordinary C++ on
    every build, with no GPU and no CUDA toolkit present — and makes the bodies
    RUNNABLE, which is what tests/kernel_test.cpp and tests/pipeline_test.cpp
    execute. Those tests therefore diff the shipped kernel source, not a
    transcription of it.
*/

#pragma once

#include <cstddef>
#include <cstring>

#include "../kernels/qk_prologue_host.h"

#include "../kernels/qk_tile.kernel"      // qk_load_tile, qk_store_tile, qk_fill_ones
#include "../kernels/qk_gauss.kernel"     // qk_gauss_rgba, qk_gauss_f1
#include "../kernels/qk_garbage.kernel"   // qk_garbage
#include "../kernels/qk_matte.kernel"     // qk_matte
#include "../kernels/qk_iir.kernel"       // qk_iir
#include "../kernels/qk_compose.kernel"   // qk_compose

namespace qkgpu {

// Readable aliases for the host. Order matches QK_CudaKernelId /
// kQK_CudaKernelNames in qk_gpu_cuda.cpp and the QK_PSO_* enum in mm_gpu_dx.cpp.
using ArgsLoad      = qkk_qk_load_tile::Args;
using ArgsStore     = qkk_qk_store_tile::Args;
using ArgsFill      = qkk_qk_fill_ones::Args;
using ArgsGaussRGBA = qkk_qk_gauss_rgba::Args;
using ArgsGaussF1   = qkk_qk_gauss_f1::Args;
using ArgsGarbage   = qkk_qk_garbage::Args;
using ArgsMatte     = qkk_qk_matte::Args;
using ArgsIIR       = qkk_qk_iir::Args;
using ArgsCompose   = qkk_qk_compose::Args;

// Fill an entry's 20 QK_GATES_DECL floats from the block QK_PackGates built.
// The macro declares them consecutively and they are all float, so the block
// copies verbatim — asserted here rather than assumed, which is the same
// guarantee mm_gpu_dx.cpp's PMatte/PCompose static_asserts give the DX side.
template <typename ArgsT>
inline void QK_StoreGates(ArgsT &a, const float *packed)
{
	static_assert(offsetof(ArgsT, gQin) - offsetof(ArgsT, gHueEn)
	                  == 19 * sizeof(float),
	              "QK_GATES_DECL fields must stay contiguous and float");
	std::memcpy(&a.gHueEn, packed, 20 * sizeof(float));
}

// Run one kernel over its whole grid on the CPU, exactly as a launch would —
// the same guard, the same per-thread entry. Used by the AE-free tests; the
// real hosts launch instead, but they launch THIS struct.
template <typename ArgsT>
inline void QK_RunGrid(const ArgsT &a, int w, int h)
{
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
			a.run(x, y);
}

} // namespace qkgpu
