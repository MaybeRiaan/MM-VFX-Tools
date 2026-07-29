/*  ER_KernelMath.h — Edge Repair GPU kernel math, SHARED across backends.

    THE POINT OF THIS FILE: one math body, four compile targets. Every GPU
    kernel's per-pixel mathematics lives here as pure functions on scalars /
    vectors, written in the common subset of HLSL / CUDA / Metal / OpenCL / C++.
    The per-backend wrapper files (EdgeRepairKernels.hlsl / .cu / .metal) contain
    ONLY entry-point decorations and buffer plumbing — never math. That is what
    makes the eventual Metal (and CUDA) builds a toolchain flip instead of a
    port: the wrappers are thin, the math is already shared. (Stage 6 decision,
    2026-07-23 — see docs/EdgeRepair.md.)

    Because this header also compiles as plain C++, the headless tests can call
    the EXACT functions the GPU runs — that is how CPU/GPU lockstep is checked
    without a GPU in the loop.

    Language selection: the including wrapper defines exactly ONE of
        ER_GPU_HLSL   — fxc/dxc, DirectX 12 compute (Windows, shipping now)
        ER_GPU_CUDA   — nvcc (source maintained; build enabled when the CUDA
                        toolkit is installed on the build machine)
        ER_GPU_METAL  — Apple metal compiler (source maintained; builds on Mac)
        ER_GPU_OPENCL — reserved, no wrapper yet
    or none of them, in which case this is a host C++ include (tests).

    CPU/GPU LOCKSTEP CONTRACT: functions here must mirror src/common/MM_*.h
    exactly (same formulas, same clamps, same edge cases). When a formula
    changes in MM_Composite.h / MM_Grain.h / MM_CoreMatte.h, it changes here in
    the same commit. The distance field is the ONE deliberate divergence:
    GPU uses Jump Flood (JFA+2, approximate) instead of the CPU's exact
    Meijster EDT — accepted 2026-07-23, which is why the Stage 6 parity gate is
    "within tolerance", not byte-identical.
*/

#ifndef ER_KERNEL_MATH_H
#define ER_KERNEL_MATH_H

/* ------------------------------------------------------------------------- */
/* Per-language shims. Keep this block the ONLY place backend #ifdefs appear; */
/* the functions below must read identically for every backend.              */
/* ------------------------------------------------------------------------- */

#if defined(ER_GPU_HLSL)
    /* HLSL: uint/int/float/float4 are native; no pointers; static = file-local.
       fxc's preprocessor handles this header like C. */
    #define ER_FN            /* plain function — HLSL inlines aggressively */

#elif defined(ER_GPU_CUDA)
    #define ER_FN            __device__ __forceinline__
    typedef unsigned int     uint;

#elif defined(ER_GPU_METAL)
    #define ER_FN            static inline
    /* metal_stdlib provides uint/float4 in the metal namespace; the wrapper
       does `using namespace metal;` before including this header. */

#elif defined(ER_GPU_OPENCL)
    #define ER_FN            static inline
    /* OpenCL C: uint/float4 native. */

#else
    /* Host C++ (headless tests / reference). */
    #define ER_FN            static inline
    typedef unsigned int     uint;
#endif

/* ------------------------------------------------------------------------- */
/* Addressing helpers.                                                       */
/*                                                                           */
/* GPU effect worlds are PF_PixelFormat_GPU_BGRA128: 4 x float32 per pixel,  */
/* BGRA channel order, row pitch given by PF_EffectWorld.rowbytes. All       */
/* kernels address buffers as flat float arrays with a per-row pitch in     */
/* FLOATS (rowbytes / 4) — never assume pitch == width*4.                    */
/* ------------------------------------------------------------------------- */

/* Index of pixel (x, y)'s first float in a BGRA128 buffer with row pitch
   `pitchF` floats. Byte address = 4 * this (ByteAddressBuffer.Load4 wants
   bytes; pointer backends index floats directly). */
ER_FN uint ER_PixelFloatIndex(uint x, uint y, uint pitchF)
{
    return y * pitchF + x * 4u;
}

/* Map an output pixel to its source pixel. SmartRender's output world is a
   sub-rect of the inflated source world; output (0,0) sits at source
   (srcOX, srcOY). Mirrors the CPU path's offset addressing in EdgeRepair.cpp
   (SmartRender), which derives srcOX/srcOY from the PreRender checkout rects. */
ER_FN uint ER_SrcFloatIndex(uint outX, uint outY,
                            uint srcOX, uint srcOY, uint srcPitchF)
{
    return ER_PixelFloatIndex(outX + srcOX, outY + srcOY, srcPitchF);
}

/* ------------------------------------------------------------------------- */
/* Real kernel math (core matte / JFA distance / pull–push / composite)      */
/* lands here with the full port — each function mirroring its MM_*.h twin.  */
/* The passthrough milestone deliberately adds no unported math.             */
/* ------------------------------------------------------------------------- */

#endif /* ER_KERNEL_MATH_H */
