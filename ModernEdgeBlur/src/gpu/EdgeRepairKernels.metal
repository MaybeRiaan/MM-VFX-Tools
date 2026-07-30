/*  EdgeRepairKernels.metal — Metal wrappers for Edge Repair.

    SOURCE MAINTAINED, BUILDS ON MAC ONLY: kept in lockstep with
    EdgeRepairKernels.hlsl so the eventual Mac build is a toolchain step
    (metal → metallib, loaded via MTLDevice in ER_GPUDeviceSetup's Metal
    branch), not a port. No MATH work is needed on the Mac — that is the point
    of the shared-kernel layout (Stage 6 decision, 2026-07-23): every formula
    already lives in ER_KernelMath.h and compiles for Metal.

    STATUS after the Stage 6 kernel port: only ER_Passthrough is mirrored here,
    matching the HLSL wrapper. The full 19-entry pipeline lives in the CUDA
    wrapper because that is the framework AE offers effects on Windows/NVIDIA.
    Bringing Metal up = writing the same thin entry wrappers over the shared
    header's functions plus the Metal branch of ER_GPUDeviceSetup, against a
    Mac that can actually run the gate.

    THIN WRAPPER ONLY: entry decorations and buffer plumbing. All per-pixel
    mathematics lives in ER_KernelMath.h — see the lockstep contract there.
    The param struct must match ER_GPU_PassParams in src/EdgeRepair_GPU.cpp
    exactly.
*/

#include <metal_stdlib>
using namespace metal;

#define ER_GPU_METAL 1
#include "ER_KernelMath.h"

/* Matches cbuffer ERPassParams (HLSL) / ER_PassParamsCu (.cu) /
   ER_GPU_PassParams (C++) field for field — 28 x 4 bytes = 112. */
struct ER_PassParamsMtl
{
    // worlds
    int   srcPitchF;
    int   dstPitchF;
    int   srcOX;
    int   srcOY;
    int   outW;
    int   outH;
    int   W;
    int   H;

    // pass-local geometry
    int   lvlOffFine;
    int   lvlOffCoarse;
    int   lvlWFine;
    int   lvlHFine;
    int   lvlWCoarse;
    int   lvlHCoarse;
    int   jfaStep;
    int   mode;

    // effect params
    float erode;
    float extend;
    float softness;
    float detail01;
    float choke;
    float thresh;
    int   chokeMode;
    int   view;

    int   bandRequested;
    int   grainBlocks;
    int   flagSlot;
    int   count;
};

/* ER_Passthrough — see the HLSL twin for semantics. Raw 4-float pixel copy of
   the output rect out of the inflated source world. */
kernel void ER_Passthrough(
    device const float*          src [[buffer(0)]],
    device float*                dst [[buffer(1)]],
    constant ER_PassParamsMtl&   p   [[buffer(2)]],
    uint2                        tid [[thread_position_in_grid]])
{
    if (tid.x >= (uint)p.outW || tid.y >= (uint)p.outH)
        return;

    const uint srcF = ER_SrcFloatIndex(tid.x, tid.y,
                                       (uint)p.srcOX, (uint)p.srcOY,
                                       (uint)p.srcPitchF);
    const uint dstF = ER_PixelFloatIndex(tid.x, tid.y, (uint)p.dstPitchF);

    dst[dstF + 0u] = src[srcF + 0u];
    dst[dstF + 1u] = src[srcF + 1u];
    dst[dstF + 2u] = src[srcF + 2u];
    dst[dstF + 3u] = src[srcF + 3u];
}
