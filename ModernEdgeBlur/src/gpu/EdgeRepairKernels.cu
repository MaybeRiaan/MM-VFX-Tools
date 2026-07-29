/*  EdgeRepairKernels.cu — CUDA wrappers for Edge Repair.

    SOURCE MAINTAINED, BUILD NOT YET ENABLED: the build machine has no CUDA
    toolkit (no nvcc) as of 2026-07-23, so this file is kept in lockstep with
    EdgeRepairKernels.hlsl but is not compiled or shipped. Enabling it later:
    install the CUDA toolkit, add an nvcc step producing PTX, embed/load it via
    cuModuleLoadDataEx in ER_GPUDeviceSetup's CUDA branch (EdgeRepair_GPU.cpp),
    and extend the SmartRender GPU dispatch. No kernel-source work should be
    needed — that is the point of the shared-kernel layout.

    THIN WRAPPER ONLY: entry decorations and buffer plumbing. All per-pixel
    mathematics lives in ER_KernelMath.h — see the lockstep contract there.
    Mirrors EdgeRepairKernels.hlsl one-to-one; the param struct must match
    ER_GPU_PassParams in src/EdgeRepair_GPU.cpp exactly.
*/

#define ER_GPU_CUDA 1
#include "ER_KernelMath.h"

/* Matches cbuffer ERPassParams (HLSL) / ER_GPU_PassParams (C++). Passed by
   value at launch (driver API: kernel params), so layout must stay POD. */
struct ER_PassParamsCu
{
    int srcPitchF;
    int dstPitchF;
    int srcOX;
    int srcOY;
    int outW;
    int outH;
    int pad0;
    int pad1;
};

/* ER_Passthrough — see the HLSL twin for semantics. Raw 4-float pixel copy of
   the output rect out of the inflated source world. */
extern "C" __global__ void ER_Passthrough(
    const float* __restrict__ src,
    float*       __restrict__ dst,
    ER_PassParamsCu           p)
{
    const uint x = blockIdx.x * blockDim.x + threadIdx.x;
    const uint y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= (uint)p.outW || y >= (uint)p.outH)
        return;

    const uint srcF = ER_SrcFloatIndex(x, y,
                                       (uint)p.srcOX, (uint)p.srcOY,
                                       (uint)p.srcPitchF);
    const uint dstF = ER_PixelFloatIndex(x, y, (uint)p.dstPitchF);

    dst[dstF + 0u] = src[srcF + 0u];
    dst[dstF + 1u] = src[srcF + 1u];
    dst[dstF + 2u] = src[srcF + 2u];
    dst[dstF + 3u] = src[srcF + 3u];
}
