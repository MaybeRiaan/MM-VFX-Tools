/*  EdgeRepairKernels.hlsl — DirectX 12 compute wrappers for Edge Repair.

    THIN WRAPPER ONLY: entry decorations, resource bindings, and the root
    signature. All per-pixel mathematics lives in ER_KernelMath.h (the shared
    multi-backend header) — see the lockstep contract there. The .cu / .metal
    wrappers mirror this file one-to-one.

    Build (CMake custom command, fxc from the Windows 10/11 SDK):
        fxc /T cs_5_1      /E ER_Passthrough    → DirectX_Assets/EdgeRepair_Passthrough.cso
        fxc /T rootsig_1_0 /E ER_ROOT_SIGNATURE → DirectX_Assets/EdgeRepair_Passthrough.rs
    The .cso/.rs pair is loaded at PF_Cmd_GPU_DEVICE_SETUP by DXContext::
    LoadShader (sdk/Util/DirectXUtils) from the DirectX_Assets/ folder next to
    EdgeRepair.aex.

    BINDING ABI (fixed by sdk/Util/DirectXUtils.cpp DescriptorSlotIndex — do
    not reorder): root param 0 = CBV table (b0), root param 1 = UAV table
    (u0..), root param 2 = SRV table (t0..) if present. Buffers are RAW
    (ByteAddressBuffer, R32_TYPELESS): addresses are in BYTES, Load4/Store4
    moves one BGRA128 pixel (4 dwords). The C++ side must call SetParamBuffer /
    SetUnorderedAccessView in exactly the order the resources are declared
    here (src before dst).
*/

#define ER_GPU_HLSL 1
#include "ER_KernelMath.h"

/* Root signature for every Edge Repair kernel so far: one CBV, two UAVs, no
   SRVs. Compiled standalone into the .rs blob (fxc /T rootsig_1_0). */
#define ER_ROOT_SIGNATURE \
    "DescriptorTable(CBV(b0))," \
    "DescriptorTable(UAV(u0, numDescriptors = 2))"

/* Keep in exact sync with ER_GPU_PassParams in src/EdgeRepair_GPU.cpp
   (8 x 4-byte fields, 32 bytes — well under the 256-byte CBV slot). */
cbuffer ERPassParams : register(b0)
{
    int gSrcPitchF;   // source row pitch in FLOATS (rowbytes / 4)
    int gDstPitchF;   // dest   row pitch in FLOATS
    int gSrcOX;       // output (0,0) sits at source (gSrcOX, gSrcOY)
    int gSrcOY;
    int gOutW;        // output world dimensions
    int gOutH;
    int gPad0;        // keep 16-byte HLSL packing explicit
    int gPad1;
};

RWByteAddressBuffer gSrc : register(u0);   // inflated source world (read)
RWByteAddressBuffer gDst : register(u1);   // output world (write)

/* --------------------------------------------------------------------------
   ER_Passthrough — Stage 6 plumbing milestone. Copies the output rect out of
   the inflated source world (raw dword copy: bit-exact, channel-order
   agnostic). Proves device setup, .cso/.rs loading, world addressing with the
   PreRender offset, dispatch, and readback — before any real math ports.
   -------------------------------------------------------------------------- */
[numthreads(16, 16, 1)]
void ER_Passthrough(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gOutW || tid.y >= (uint)gOutH)
        return;

    const uint srcF = ER_SrcFloatIndex(tid.x, tid.y,
                                       (uint)gSrcOX, (uint)gSrcOY,
                                       (uint)gSrcPitchF);
    const uint dstF = ER_PixelFloatIndex(tid.x, tid.y, (uint)gDstPitchF);

    gDst.Store4(dstF * 4u, gSrc.Load4(srcF * 4u));
}
