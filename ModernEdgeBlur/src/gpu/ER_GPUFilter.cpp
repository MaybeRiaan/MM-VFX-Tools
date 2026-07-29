/*  ER_GPUFilter.cpp — EdgeRepair's MediaCore GPUVideoFilter module
    (exports xGPUFilterEntry). Stage 6.

    WHY THIS EXISTS: AE 2026 routes effect GPU rendering through MediaCore
    GPUVideoFilter modules, bound to the effect via the module's PiPL match
    name — NOT through the AE-SDK SUPPORTS_GPU_RENDER_F32 / SMART_RENDER_GPU
    path (AE probes that path's GPU_DEVICE_SETUP at launch but never
    schedules a frame on it; established empirically 2026-07-29, evidence in
    docs/EdgeRepair.md "Stage 6 investigation state"). Adobe's own Fractal
    Noise / Transform / Lumetri and Red Giant / Artlist all ship exactly this
    module alongside their AE entry points, and AE records it as a
    `GPUVideoFilter.0` subkey in HKCU\...\After Effects\<ver>\PluginCache.

    BINDING: PrGPUFilterInfo.outMatchName left null = "default to the
    module's pipl" (PrSDKGPUFilter.h) — the host binds this filter to the one
    AE effect in this .aex ("MM Edge Repair") and stores it as
    "AE.MM Edge Repair".

    MILESTONE SKELETON (mirrors the AE-SDK passthrough milestone): CUDA only,
    render = raw device-to-device copy of the input frame (format-agnostic —
    handles both PrPixelFormat_GPU_BGRA_4444_32f and _16f). Declining
    Initialize on any other framework (or missing nvcuda) falls back to the
    software render, i.e. our proven CPU SmartRender path. The real kernel
    pipeline (JFA+2 distance, pull–push, composite) replaces the copy as the
    port lands.

    This TU compiles against the PREMIERE SDK headers only (sdk/Premiere,
    gitignored, non-redistributable like the AE SDK) — never mix AE-SDK
    headers in here; the one shared piece is ER_CudaShim.h (plain Win32).
*/

#ifdef _WIN32

// PrGPUFilterModule.h uses PF_TransitionSuite without including its header —
// Adobe's Vignette example gets it via the effect header chain; we include
// it directly (defines PF_TransitionSuite + kPFTransitionSuite*).
#include "PrSDKAESupport.h"
#include "PrGPUFilterModule.h"
#include "ER_CudaShim.h"

#include <cstdint>

/* Tagged diagnostic into %TEMP%\EdgeRepair_gpu.log (defined in
   EdgeRepair_GPU.cpp; declared here directly because its header pulls AE-SDK
   types that must not mix into this Premiere-SDK TU). */
void ER_GPULogEvent(const char *tag);

class ERGPUFilter : public PrGPUFilterBase
{
public:
    virtual prSuiteError Initialize(PrGPUFilterInstance *ioInstanceData)
    {
        // Log BEFORE any decision, so the log distinguishes "the host never
        // called us" from "we declined". Absence of this line is itself the
        // finding: registration succeeded but no instantiation was attempted.
        ER_GPULogEvent("GPUVF Initialize: ENTERED");

        PrGPUFilterBase::Initialize(ioInstanceData);

        if (mDeviceInfo.outDeviceFramework != PrGPUDeviceFramework_CUDA) {
            // This build renders GPU on CUDA only (the framework AE-on-
            // Windows/NVIDIA drives). Declining here = software fallback,
            // which is our CPU SmartRender — always correct.
            ER_GPULogEvent("GPUVF Initialize: non-CUDA framework declined");
            return suiteError_Fail;
        }

        const ER_CudaApi &cu = ER_CudaApi::Get();
        if (!cu.loaded) {
            ER_GPULogEvent("GPUVF Initialize: nvcuda.dll unavailable — declined");
            return suiteError_Fail;
        }

        mCtx    = reinterpret_cast<CUcontext>(mDeviceInfo.outContextHandle);
        mStream = reinterpret_cast<CUstream>(mDeviceInfo.outCommandQueueHandle);
        if (!mCtx) {
            ER_GPULogEvent("GPUVF Initialize: no CUcontext — declined");
            return suiteError_Fail;
        }

        ER_GPULogEvent("GPUVF Initialize: OK (CUDA)");
        return suiteError_NoError;
    }

    virtual prSuiteError Render(
        const PrGPUFilterRenderParams *inRenderParams,
        const PPixHand *inFrames,
        csSDK_size_t inFrameCount,
        PPixHand *outFrame)
    {
        ER_GPULogEvent("GPUVF Render (passthrough milestone)");

        if (!outFrame)
            return suiteError_Fail;

        // Passthrough: if the host gave a distinct input frame, copy it to
        // the output; if the filter is being run in place (outFrame already
        // holds the input, as SDK_ProcAmp assumes), there is nothing to do.
        if (inFrameCount < 1 || !inFrames || !inFrames[0])
            return suiteError_NoError;

        void *srcData = nullptr, *dstData = nullptr;
        mGPUDeviceSuite->GetGPUPPixData(inFrames[0], &srcData);
        mGPUDeviceSuite->GetGPUPPixData(*outFrame, &dstData);
        if (!srcData || !dstData)
            return suiteError_Fail;
        if (srcData == dstData)
            return suiteError_NoError;      // in-place: input already there

        prRect bounds = {};
        mPPixSuite->GetBounds(*outFrame, &bounds);
        const int height = bounds.bottom - bounds.top;

        csSDK_int32 dstRowBytes = 0, srcRowBytes = 0;
        mPPixSuite->GetRowBytes(*outFrame, &dstRowBytes);
        mPPixSuite->GetRowBytes(inFrames[0], &srcRowBytes);
        if (height <= 0 || dstRowBytes <= 0 || srcRowBytes != dstRowBytes) {
            // Geometry we don't handle in the skeleton — software fallback
            // would have been chosen at Initialize; failing a single render
            // here is loud on purpose so a mismatch is investigated.
            ER_GPULogEvent("GPUVF Render: geometry mismatch — FAILED");
            return suiteError_Fail;
        }

        const ER_CudaApi &cu = ER_CudaApi::Get();
        ER_CudaCtxGuard ctx(cu, mCtx);
        if (!ctx.pushed)
            return suiteError_Fail;

        CUresult res = cu.MemcpyDtoDAsync(
            static_cast<CUdeviceptr>(reinterpret_cast<uintptr_t>(dstData)),
            static_cast<CUdeviceptr>(reinterpret_cast<uintptr_t>(srcData)),
            static_cast<size_t>(dstRowBytes) * static_cast<size_t>(height),
            mStream);

        // TEMPORARY VISIBLE MARKER (remove once the kernel port begins).
        // A pure passthrough is indistinguishable from the CPU path, so the
        // Mercury CUDA-vs-Software A/B in docs/AE_GPU_Effects.md §4.3 cannot
        // detect success. Zeroing the top ~12% of the frame makes "the GPU
        // module ran" unmistakable on screen.
        if (res == ER_CUDA_SUCCESS && cu.MemsetD8Async) {
            const int band = height / 8;
            if (band > 0) {
                res = cu.MemsetD8Async(
                    static_cast<CUdeviceptr>(reinterpret_cast<uintptr_t>(dstData)),
                    0,
                    static_cast<size_t>(dstRowBytes) * static_cast<size_t>(band),
                    mStream);
            }
        }
        return res == ER_CUDA_SUCCESS ? suiteError_NoError : suiteError_Fail;
    }

private:
    CUcontext mCtx    = nullptr;
    CUstream  mStream = nullptr;
};

DECLARE_GPUFILTER_ENTRY(PrGPUFilterModule<ERGPUFilter>)

#endif /* _WIN32 */
