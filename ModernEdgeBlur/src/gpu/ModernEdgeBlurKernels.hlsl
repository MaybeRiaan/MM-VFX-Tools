/*  ModernEdgeBlurKernels.hlsl — DirectX 12 compute wrapper for Modern Edge Blur.

    WHY THIS FILE EXISTS WITH NO HOST BEHIND IT. There is no DX12 host for
    Modern Edge Blur: AE-on-Windows offers effects CUDA (measured; see
    ../../docs/AE_GPU_Effects.md §1.1) and §1.2 makes CUDA the only backend this
    suite maintains, so MEB_GPUFrameworkSupported accepts CUDA alone and
    declines DIRECTX into a clean CPU fallback. What this file buys is the rule
    in AE_GPU_Effects.md §4 item 5: **fxc type-checks the whole translation
    unit, so one compiled entry point validates the entire shared math header
    against HLSL's rules on every build.** That has already caught, in
    EdgeRepair, a construct CUDA and C++ both accept and HLSL rejects (a
    ternary over a struct type) months before anyone compiled it for real.

    The .cso this produces is deliberately NOT deployed next to the .aex —
    nothing loads it. It is a compile-time assertion, not an asset.

    THIN WRAPPER ONLY: entry decorations, resource bindings, the root
    signature. All per-pixel mathematics lives in MEB_KernelMath.h.

    NOTE the include: only MEB_KernelMath.h, never MEB_KernelPasses.h. The pass
    bodies take `const float*` parameters, which HLSL has no equivalent for —
    see MEB_KernelPasses.h's header for what bringing DX12 up would take (four
    macros in the shim block, no math work).

    BINDING ABI (matches sdk/Util/DirectXUtils.cpp DescriptorSlotIndex, the ABI
    an eventual host would use — do not reorder): root param 0 = CBV table (b0),
    root param 1 = UAV table (u0..). Buffers are RAW (ByteAddressBuffer,
    R32_TYPELESS): addresses are in BYTES, Load4/Store4 moves one BGRA128 pixel.
*/

#define MEB_GPU_HLSL 1
#include "MEB_KernelMath.h"

/* One CBV, three UAVs (source world, alpha plane, output world) — the widest
   binding any Modern Edge Blur pass needs beyond the composite. */
#define MEB_ROOT_SIGNATURE \
    "DescriptorTable(CBV(b0))," \
    "DescriptorTable(UAV(u0, numDescriptors = 3))"

/* Keep in exact sync with MEB_GPU_PassParams (src/ModernEdgeBlur_GPU.cpp) and
   MEB_PassParamsCu (.cu): 20 x 4-byte fields = 80 bytes, laid out in 5 groups
   of 16 so this cbuffer packs identically. */
cbuffer MEBPassParams : register(b0)
{
    // worlds
    int gSrcPitchF;      // source world row pitch in FLOATS (rowbytes / 4)
    int gDstPitchF;      // output world row pitch in FLOATS
    int gSrcW;           // source world dims — reads are clamped into these
    int gSrcH;

    // planes and dispatch grid
    int gW;              // output world dims == scratch plane dims
    int gH;
    int gCount;          // element count for the 1-D kernels
    int gHalf;           // Gaussian half-width (taps = 2*half + 1)

    // pass-local geometry
    int gDirX;
    int gDirY;
    int gHalfLen;
    int gPremult;

    int gUnpremult;
    int gDilate;
    int gMorphRi;
    int gPad0;

    float gMorphR2;
    float gPad1;
    float gPad2;
    float gPad3;
};

RWByteAddressBuffer gSrc   : register(u0);   // source world (read)
RWByteAddressBuffer gAlpha : register(u1);   // alpha plane, pitch == gW (read)
RWByteAddressBuffer gDst   : register(u2);   // output world (write)

/* --------------------------------------------------------------------------
   MEB_Passthrough — the Blur Amount = 0, Blend Radius = 0 case, and the entry
   fxc compiles. Straight, held colour with RGB forced to zero at the a<=0
   cliff, alpha from the (post-Shift-Edge) alpha plane — the same output the
   CPU path packs, and deliberately not a raw dword copy, which would keep held
   RGB the CPU discards.
   -------------------------------------------------------------------------- */
[numthreads(16, 16, 1)]
void MEB_Passthrough(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gW || tid.y >= (uint)gH)
        return;

    const uint sx = (uint)MEB_ClampI((int)tid.x, 0, gSrcW - 1);
    const uint sy = (uint)MEB_ClampI((int)tid.y, 0, gSrcH - 1);
    const uint s  = MEB_PixelFloatIndex(sx, sy, (uint)gSrcPitchF);

    /* world is BGRA: [0]=B [1]=G [2]=R [3]=A */
    const float4 w = asfloat(gSrc.Load4(s * 4u));
    const float  a = asfloat(gAlpha.Load(
                         MEB_PlaneIndex(tid.x, tid.y, (uint)gW) * 4u));

    const MEB_RGBA o = MEB_SharpTexel(a, w.z, w.y, w.x);

    const uint d = MEB_PixelFloatIndex(tid.x, tid.y, (uint)gDstPitchF);
    gDst.Store4(d * 4u, asuint(float4(o.b, o.g, o.r, o.a)));
}
