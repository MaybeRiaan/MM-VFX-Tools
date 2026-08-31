/*  ModernEdgeBlur_GPU.cpp — GPU path implementation (CUDA).

    See ModernEdgeBlur_GPU.h for the backend policy. In short: AE 2026 on
    Windows/NVIDIA hands effects what_gpu = CUDA and never probes DIRECTX even
    with SUPPORTS_DIRECTX_RENDERING set (measured 2026-07-23 with EdgeRepair —
    that flag serves AE-SDK effects hosted in Premiere Pro). CUDA is therefore
    the only backend here; DIRECTX is declined into a clean CPU fallback rather
    than served a passthrough that would silently no-op the effect.

    ── What runs where ──────────────────────────────────────────────────────
    ER_RenderCUDA's twin below, MEB_RenderCUDA, carries the FULL pipeline —
    Shift Edge, Blur Amount, Blend Radius, composite, and the passthrough case
    — mirroring SmartRender's CPU pipeline pass for pass. Both paths share the
    same MATH: src/gpu/MEB_KernelMath.h + MEB_KernelPasses.h compile for CUDA,
    HLSL (formula layer) and host C++, and tests/test_gpuparity.cpp diffs them
    against src/core/MEB_CoreMath.h — the CPU path's real code — with no GPU in
    the loop.

    ── No host-side readback, and none needed ───────────────────────────────
    Edge Repair needs a device flags buffer because its CPU pipeline asks
    questions between passes ("did the core matte come out empty?") and
    branches on the answer, which on the GPU would cost a stream
    synchronization. Modern Edge Blur asks none: every branch in its pipeline
    is a decision about the three sliders, which the host already knows. So the
    pass sequence is chosen once, up front, and there is nothing to read back.

    ── Where the GPU pipeline legitimately differs from the CPU one ──────────
    Two shape differences, both invisible in the output (docs/AE_GPU_Effects.md
    §4 item 4 — measure divergences and write them down; these are exact, not
    approximate):

      1. NO sharp_buf. The CPU materialises the sharp layer into a full-frame
         RGBA plane at ingest. The GPU recomputes it from the source world plus
         the alpha plane at the two points that consume it (passthrough and
         composite) — same values, one fewer 16-bytes-per-pixel plane resident.
      2. PING-PONG instead of in-place. The CPU's separable blurs run in place
         with per-row/per-column scratch; a dispatch cannot read and write one
         plane at once, so each pass reads one buffer and writes another. The
         tap order, the clamped edge sampling and the accumulation type are
         identical, so the numbers are too.

    Neither changes a formula. The one thing that WOULD change numbers — the
    Gaussian weights — is not recomputed here at all: MakeBlurKernel /
    MakeBlendKernel from src/core/MEB_CoreMath.h build them on the host, for
    both paths, and the float array is uploaded verbatim
    (docs/gpu-kernel-rules.md §3.1).

    ── CUDA host mechanics ──────────────────────────────────────────────────
    The host side goes through a runtime nvcuda.dll shim (src/gpu/MEB_CudaShim.h
    — compiles and runs with NO toolkit installed); the kernels ship as PTX text
    in CUDA_Assets/ModernEdgeBlur_Kernels.ptx next to the .aex (built by nvcc
    when the toolkit is present — CMake skips it gracefully otherwise, and setup
    then declines -> CPU fallback). AE provides the CUcontext/CUstream via
    PF_GPUDeviceSuite1; per that suite's header the context is pushed/popped
    around every driver-call sequence (MEB_CudaCtxGuard). Kernels launch on AE'S
    OWN stream and are NOT synchronized here — AE owns stream synchronization.

    THREADING: AE does not promise GPU SmartRenders are serialized, and the
    pipeline scratch is per DEVICE, so one mutex covers scratch growth and the
    whole enqueue sequence. Because AE's stream is FIFO, serializing the ENQUEUE
    is enough to serialize execution: render A's passes all run before render
    B's touch the buffers (docs/AE_GPU_Effects.md §4, "Multi-pass scratch").
    Nothing here is mutable global state — the blob is AE-owned per-device data
    handed back to us per render, MFR-safe by the same argument as
    pre_render_data.
*/

/* ---------------------------------------------------------------------------
   PLATFORM FORK. Everything below is the WINDOWS GPU host: CUDA through the
   nvcuda.dll shim. It is Windows-only by construction, and Metal is unwritten
   (../../docs/gpu-kernel-rules.md §4), so on any other platform this file
   compiles down to the DECLINE stubs at the bottom: PF_Err_NONE with null
   gpu_data at device setup and MEB_GPUFrameworkSupported false, so PreRender
   never raises GPU_RENDER_POSSIBLE and AE routes every frame to the CPU
   reference — the always-correct path on both platforms.

   The fork lives HERE, not at ModernEdgeBlur.cpp's call sites: the CPU render
   pipeline is settled and this keeps every #ifdef out of it. Same shape as
   MM_Qualify's QK_GPUDeviceSetup fallback (MM_Qualify/src/smart_render.cpp).
   --------------------------------------------------------------------------- */
#ifdef _WIN32

#define NOMINMAX
#include "ModernEdgeBlur.h"        // SDK includes, MEB_Params, MEB_* enums
#include "ModernEdgeBlur_GPU.h"

#include "AE_EffectGPUSuites.h"
#include "AEFX_SuiteHelper.h"

#include "gpu/MEB_CudaShim.h"      // runtime nvcuda.dll driver-API surface
#include "core/MEB_CoreMath.h"     // the CPU path's own Gaussian kernel builders

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <new>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// GPU diagnostics. Set MEB_GPU_LOG to 1 to append the GPU lifecycle to
// %TEMP%\ModernEdgeBlur_gpu.log: every DEVICE_SETUP/SETDOWN with the framework
// AE offered (1=OpenCL 2=Metal 3=CUDA 4=DirectX) and each setup step's outcome,
// plus the first few PreRender decisions and the per-frame pipeline plan.
//
// This is not optional scaffolding — the GPU failure mode is SILENT. A missed
// step means AE renders the effect on the CPU, correctly, at CPU speed, with no
// error anywhere (AE_GPU_Effects.md opens by warning about exactly this). The
// log is how you tell "never probed" from "wrong framework" from "kernel load
// failed" from "flag not raised". `what_gpu = 0 at every PreRender` means the
// gate is shut; go read §2.2 first.
//
// The first-N rate limit uses write-only atomic counters — diagnostic-only, no
// render behaviour may ever depend on them (MFR discipline).
// ---------------------------------------------------------------------------
#define MEB_GPU_LOG 1

#if MEB_GPU_LOG
#include <atomic>
#include <cstdarg>
#include <cstdlib>
static void MEB_GPULog(const char *fmt, ...)
{
    char path[1024];
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = ".";
    snprintf(path, sizeof(path), "%s\\ModernEdgeBlur_gpu.log", tmp);
    FILE *f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}
// Log only the first `limit` hits per call site (PreRender/SmartRender fire per
// frame — unbounded logging would swamp the file and pollute timings).
// The counters are PROCESS statics: a few seconds of scrubbing at default
// params exhausts them, after which the log is silent and an A/B sweep leaves
// no trace at all. Set MM_GPU_LOG_ALL=1 to lift every cap across the whole
// suite for a validation session (../../docs/gpu-validation-checklist.md §3).
static bool MEB_GPULogAll()
{
    static const bool all = [] {
        char v[8];
        const DWORD n = ::GetEnvironmentVariableA("MM_GPU_LOG_ALL", v, (DWORD)sizeof(v));
        return n > 0 && n < (DWORD)sizeof(v) && v[0] != '0';
    }();
    return all;
}
#define MEB_GPU_LOG_FIRST(counter, limit, ...) \
    do { static std::atomic<int> counter(0); \
         if (counter.fetch_add(1) < (limit) || MEB_GPULogAll()) \
             MEB_GPULog(__VA_ARGS__); } while (0)
#define MEB_GPU_LOGF(...) MEB_GPULog(__VA_ARGS__)
#else
#define MEB_GPU_LOG_FIRST(counter, limit, ...) ((void)0)
#define MEB_GPU_LOGF(...) ((void)0)
#endif

// A_long is what the AE SDK hands us; MEB_CoreMath.h spells the same type as
// MEB_Long so it can stay free of the SDK. If a future SDK ever widened one of
// them, every index in the shared math would silently truncate.
static_assert(sizeof(A_long) == sizeof(MEB_Long),
              "MEB_Long must match A_long — see src/core/MEB_CoreMath.h");

// ---------------------------------------------------------------------------
// The kernel table. One entry per __global__ entry point in
// src/gpu/ModernEdgeBlurKernels.cu — names must match EXACTLY
// (cuModuleGetFunction resolves them by string).
// ---------------------------------------------------------------------------
enum MEB_KernelId {
    MEB_K_EXTRACT_ALPHA = 0,
    MEB_K_MORPH,
    MEB_K_DERIVE,
    MEB_K_PASSTHROUGH,
    MEB_K_BLUR_H_RGBA,
    MEB_K_BLUR_V_RGBA,
    MEB_K_DIR_MIN,
    MEB_K_BLUR_H_PLANE,
    MEB_K_BLUR_V_PLANE,
    MEB_K_COPY_PLANE,
    MEB_K_COMPOSITE,
    MEB_K_COUNT
};

static const char *const kMEB_KernelNames[MEB_K_COUNT] = {
    "MEB_ExtractAlpha",
    "MEB_Morph",
    "MEB_Derive",
    "MEB_Passthrough",
    "MEB_BlurHRGBA",
    "MEB_BlurVRGBA",
    "MEB_DirMin",
    "MEB_BlurHPlane",
    "MEB_BlurVPlane",
    "MEB_CopyPlane",
    "MEB_Composite",
};

// ---------------------------------------------------------------------------
// Per-device GPU state, owned by AE via extra->output->gpu_data.
// ---------------------------------------------------------------------------
struct MEB_GPUData {
    PF_GPU_Framework framework   = PF_GPU_Framework_NONE;
    A_u_long         deviceIndex = 0;   // needed to free scratch at setdown

    // Serializes the whole GPU render: the per-device scratch block below,
    // which every concurrent render would otherwise share. CUDA work is
    // enqueued on AE's single per-device stream, so serializing the ENQUEUE is
    // enough to serialize execution — the stream is FIFO.
    std::mutex       renderLock;

    // CUDA (driver API via MEB_CudaShim; objects immutable post-setup)
    CUcontext        cuCtx    = nullptr;          // AE's context (not owned)
    CUstream         cuStream = nullptr;          // AE's stream (not owned)
    CUmodule         cuModule = nullptr;          // loaded PTX (owned)
    CUfunction       cuFn[MEB_K_COUNT] = { nullptr };

    // Device scratch for the multi-pass pipeline. ONE allocation, sub-divided
    // per render; grow-only and cached across frames so a steady-state render
    // allocates nothing — the device-side analogue of the CPU path's MEB_Pool,
    // except this one is per DEVICE (guarded by renderLock) rather than per
    // render thread. Freed at device setdown.
    //
    // scratchBytes is ATOMIC because PreRender's budget gate reads it without
    // taking renderLock — blocking a PreRender behind a whole enqueue sequence
    // to read one size_t would be absurd. The gate only needs it as a hint, and
    // the hint is safe in the one direction that matters: the block is
    // grow-only, so a value read at PreRender can only be an UNDER-estimate of
    // what is held by render time, never an over-estimate.
    void                *scratch      = nullptr;
    std::atomic<size_t>  scratchBytes{ 0 };
};

// Kernel launch params — must match MEB_PassParamsCu in
// src/gpu/ModernEdgeBlurKernels.cu and cbuffer MEBPassParams in
// ModernEdgeBlurKernels.hlsl FIELD FOR FIELD. 20 x 4 bytes = 80, arranged in
// 5 x 16-byte groups so an HLSL cbuffer packs identically.
struct MEB_GPU_PassParams {
    // worlds
    A_long srcPitchF;      // source world row pitch in FLOATS (rowbytes / 4)
    A_long dstPitchF;      // output world row pitch in FLOATS
    A_long srcW;           // source world dims — reads are clamped into these
    A_long srcH;

    // planes and dispatch grid
    A_long W;              // output world dims == scratch plane dims
    A_long H;
    A_long count;          // element count for the 1-D kernels
    A_long half;           // Gaussian half-width (taps = 2*half + 1)

    // pass-local geometry
    A_long dirX;           // directional-min step
    A_long dirY;
    A_long halfLen;        // directional-min half length
    A_long premult;        // MEB_Derive: multiply RGB by alpha

    A_long unpremult;      // MEB_BlurVRGBA: divide RGB by the blurred alpha
    A_long dilate;         // MEB_Morph: 1 = max filter (grow), 0 = min (shrink)
    A_long morphRi;        // MEB_Morph: ceil(|Shift Edge|)
    A_long pad0;

    float  morphR2;        // MEB_Morph: |Shift Edge|^2
    float  pad1;
    float  pad2;
    float  pad3;
};
static_assert(sizeof(MEB_GPU_PassParams) == 80,
              "param block must stay 80 bytes and field-identical to the "
              "CUDA struct / HLSL cbuffer");

bool MEB_GPUFrameworkSupported(PF_GPU_Framework what_gpu)
{
    // CUDA only — see ModernEdgeBlur_GPU.h for why DIRECTX is declined rather
    // than served a passthrough. This still gates further on device setup
    // succeeding (AE only drives GPU renders for devices that returned
    // gpu_data), so listing a framework here is safe even when its assets or
    // driver are absent at runtime.
    return what_gpu == PF_GPU_Framework_CUDA;
}

// ---------------------------------------------------------------------------
// Asset-path helper. Resolve the folder from THIS module's own path so the
// assets always travel next to the deployed .aex — the CUDA_Assets/ folder AE
// expects beside the plugin (AE_GPU_Effects.md §2.5).
// ---------------------------------------------------------------------------
static bool MEB_CudaPtxPath(std::wstring &outPath)
{
    wchar_t path[1024];
    HMODULE hm = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&MEB_CudaPtxPath), &hm))
        return false;
    if (::GetModuleFileNameW(hm, path, (DWORD)(sizeof(path) / sizeof(path[0]))) == 0)
        return false;
    std::wstring dir(path);
    const size_t slash = dir.rfind(L'\\');
    if (slash == std::wstring::npos) return false;
    outPath = dir.substr(0, slash) + L"\\CUDA_Assets\\ModernEdgeBlur_Kernels.ptx";
    return true;
}

// Read the whole PTX file, NUL-terminated (cuModuleLoadDataEx takes a
// NUL-terminated text image).
static bool MEB_ReadTextFile(const std::wstring &path, std::vector<char> &out)
{
    FILE *f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    rewind(f);
    if (size <= 0) { fclose(f); return false; }
    out.resize((size_t)size + 1);
    const size_t got = fread(out.data(), 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) return false;
    out[(size_t)size] = '\0';
    return true;
}

// ---------------------------------------------------------------------------
// Device setup / setdown
// ---------------------------------------------------------------------------

// Load the PTX module on AE's context. Failures return false and the caller
// declines QUIETLY (CPU fallback) — a missing toolkit/PTX is a normal
// configuration (build machines without nvcc), never worth an AE error.
static bool MEB_SetupCUDA(const PF_GPUDeviceInfo &device_info,
                          MEB_GPUData *gpu_data)
{
    const MEB_CudaApi &cu = MEB_CudaApi::Get();
    if (!cu.loaded) {
        MEB_GPU_LOGF("  -> nvcuda.dll not available - CUDA declined\n");
        return false;
    }

    gpu_data->cuCtx    = reinterpret_cast<CUcontext>(device_info.contextPV);
    gpu_data->cuStream = reinterpret_cast<CUstream>(device_info.command_queuePV);
    if (!gpu_data->cuCtx) {
        MEB_GPU_LOGF("  -> AE provided no CUcontext - CUDA declined\n");
        return false;
    }

    std::wstring ptxPath;
    std::vector<char> ptx;
    if (!MEB_CudaPtxPath(ptxPath) || !MEB_ReadTextFile(ptxPath, ptx)) {
        MEB_GPU_LOGF("  -> PTX missing/unreadable (%ls) - CUDA declined "
                     "(build with the CUDA toolkit installed to produce it)\n",
                     ptxPath.c_str());
        return false;
    }

    MEB_CudaCtxGuard ctx(cu, gpu_data->cuCtx);
    if (!ctx.pushed) {
        MEB_GPU_LOGF("  -> cuCtxPushCurrent FAILED - CUDA declined\n");
        return false;
    }

    CUresult res = cu.ModuleLoadDataEx(&gpu_data->cuModule, ptx.data(),
                                       0, nullptr, nullptr);
    if (res != MEB_CUDA_SUCCESS || !gpu_data->cuModule) {
        MEB_GPU_LOGF("  -> cuModuleLoadDataEx FAILED (CUresult=%d) - CUDA declined\n",
                     (int)res);
        gpu_data->cuModule = nullptr;
        return false;
    }

    // Resolve every kernel up front. A missing entry means the deployed PTX is
    // stale relative to this binary (the classic symptom of a half-finished
    // deploy) — decline the whole backend rather than discover it mid-render,
    // where there is no CPU fallback because the worlds are device memory.
    for (int k = 0; k < MEB_K_COUNT; ++k) {
        res = cu.ModuleGetFunction(&gpu_data->cuFn[k], gpu_data->cuModule,
                                   kMEB_KernelNames[k]);
        if (res != MEB_CUDA_SUCCESS || !gpu_data->cuFn[k]) {
            MEB_GPU_LOGF("  -> cuModuleGetFunction(%s) FAILED (CUresult=%d) - "
                         "CUDA declined (stale CUDA_Assets PTX?)\n",
                         kMEB_KernelNames[k], (int)res);
            cu.ModuleUnload(gpu_data->cuModule);
            gpu_data->cuModule = nullptr;
            return false;
        }
    }
    return true;
}

PF_Err MEB_GPUDeviceSetup(PF_InData *in_data, PF_OutData *out_data,
                          PF_GPUDeviceSetupExtra *extra)
{
    PF_Err err = PF_Err_NONE;

    MEB_GPU_LOGF("[ModernEdgeBlur] DEVICE_SETUP what_gpu=%d device_index=%u\n",
                 (int)extra->input->what_gpu,
                 (unsigned)extra->input->device_index);

    if (!MEB_GPUFrameworkSupported(extra->input->what_gpu)) {
        // Decline QUIETLY: PF_Err_NONE with gpu_data left null. AE then never
        // renders this effect on GPU for devices of this framework — the
        // intended CPU fallback for DirectX (Premiere), Metal and OpenCL. An
        // error return here would risk a user-visible AE error for what is a
        // normal, expected configuration.
        MEB_GPU_LOGF("  -> declined (framework unsupported in this build)\n");
        return PF_Err_NONE;
    }

    PF_GPUDeviceSuite1 *gpuS = nullptr;
    err = AEFX_AcquireSuite(in_data, out_data, kPFGPUDeviceSuite,
                            kPFGPUDeviceSuiteVersion1, "",
                            reinterpret_cast<void**>(&gpuS));
    if (err || !gpuS) return err ? err : PF_Err_BAD_CALLBACK_PARAM;

    PF_GPUDeviceInfo device_info;
    AEFX_CLR_STRUCT(device_info);
    err = gpuS->GetDeviceInfo(in_data->effect_ref,
                              extra->input->device_index, &device_info);

    MEB_GPUData *gpu_data = nullptr;
    if (!err) {
        gpu_data = new (std::nothrow) MEB_GPUData;
        if (!gpu_data) err = PF_Err_OUT_OF_MEMORY;
    }

    if (!err) {
        gpu_data->framework   = extra->input->what_gpu;
        gpu_data->deviceIndex = extra->input->device_index;
        if (!MEB_SetupCUDA(device_info, gpu_data)) {
            // Quiet decline (see MEB_SetupCUDA) — CPU fallback.
            delete gpu_data;
            AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
                              kPFGPUDeviceSuiteVersion1, "");
            return PF_Err_NONE;
        }
    }

    if (!err) {
        extra->output->gpu_data = gpu_data;
        // Re-affirm GPU support for THIS device. Adobe's AE GPU sample
        // (Examples/Effect/SDK_Invert_ProcAmp) does exactly this — a plain
        // ASSIGNMENT, not an OR — in every successful backend branch. Without
        // it AE completes GPU_DEVICE_SETUP happily and then offers
        // what_gpu=NONE at every PreRender forever, a perfect imitation of "AE
        // doesn't support third-party GPU effects". That single missing line
        // cost seven failed experiments on EdgeRepair; see
        // ../../docs/AE_GPU_Effects.md §2.2.
        out_data->out_flags2 = PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;
        MEB_GPU_LOGF("  -> OK (CUDA backend ready)\n");
    } else {
        MEB_GPU_LOGF("  -> err=%d (GetDeviceInfo/alloc step failed)\n", (int)err);
        delete gpu_data;    // safe on nullptr
    }

    AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
                      kPFGPUDeviceSuiteVersion1, "");
    return err;
}

PF_Err MEB_GPUDeviceSetdown(PF_InData *in_data, PF_OutData *out_data,
                            PF_GPUDeviceSetdownExtra *extra)
{
    MEB_GPU_LOGF("[ModernEdgeBlur] DEVICE_SETDOWN what_gpu=%d gpu_data=%p\n",
                 (int)extra->input->what_gpu, extra->input->gpu_data);

    MEB_GPUData *gpu_data = static_cast<MEB_GPUData*>(extra->input->gpu_data);

    // Hand the pipeline scratch back BEFORE the module unload: the suite owns
    // this memory and outliving the device would leak it for the session.
    if (gpu_data && gpu_data->scratch) {
        PF_GPUDeviceSuite1 *gpuS = nullptr;
        if (!AEFX_AcquireSuite(in_data, out_data, kPFGPUDeviceSuite,
                               kPFGPUDeviceSuiteVersion1, "",
                               reinterpret_cast<void**>(&gpuS)) && gpuS) {
            gpuS->FreeDeviceMemory(in_data->effect_ref, gpu_data->deviceIndex,
                                   gpu_data->scratch);
            AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
                              kPFGPUDeviceSuiteVersion1, "");
        }
        gpu_data->scratch      = nullptr;
        gpu_data->scratchBytes = 0;
    }

    if (gpu_data && gpu_data->cuModule) {
        const MEB_CudaApi &cu = MEB_CudaApi::Get();
        if (cu.loaded) {
            MEB_CudaCtxGuard ctx(cu, gpu_data->cuCtx);
            cu.ModuleUnload(gpu_data->cuModule);
        }
        gpu_data->cuModule = nullptr;
    }
    delete gpu_data;
    return PF_Err_NONE;
}

// Diagnostic hook for PreRender — records what AE offered per frame and whether
// we raised GPU_RENDER_POSSIBLE. First few frames only.
void MEB_GPULogSmartRenderCPU(int srcW, int srcH, int outW, int outH)
{
    // The counterpart to the SMART_RENDER GPU line. Without it a frame that
    // falls back to the CPU is INVISIBLE — you can only infer it from a
    // missing GPU line, which the per-site caps make unreliable.
    MEB_GPU_LOG_FIRST(s_cpu, 6,
        "[ModernEdgeBlur] SMART_RENDER CPU src=%dx%d out=%dx%d\n",
        srcW, srcH, outW, outH);
}

void MEB_GPULogPreRenderDecision(PF_GPU_Framework what_gpu,
                                 const void *gpu_data, bool raised,
                                 int bitdepth, int dsx_num, int dsx_den)
{
#if MEB_GPU_LOG
    // Separate first-N budgets for NONE vs GPU offers — a single shared cap
    // hides the GPU-mode frames behind earlier CPU-mode ones. bitdepth and
    // downsample expose the render context, so AE's CPU/GPU choice can be
    // correlated with project depth and preview resolution (expect
    // what_gpu=NONE for thumbnails and heavy draft renders even when GPU is
    // working perfectly — AE_GPU_Effects.md §2.3).
    if (what_gpu == PF_GPU_Framework_NONE) {
        MEB_GPU_LOG_FIRST(s_preNone, 12,
            "[ModernEdgeBlur] PRERENDER what_gpu=0 bitdepth=%d ds=%d/%d raised=%s\n",
            bitdepth, dsx_num, dsx_den, raised ? "YES" : "no");
    } else {
        MEB_GPU_LOG_FIRST(s_preGpu, 12,
            "[ModernEdgeBlur] PRERENDER what_gpu=%d gpu_data=%p bitdepth=%d "
            "ds=%d/%d -> GPU_RENDER_POSSIBLE=%s\n",
            (int)what_gpu, gpu_data, bitdepth, dsx_num, dsx_den,
            raised ? "YES" : "no");
    }
#else
    (void)what_gpu; (void)gpu_data; (void)raised;
    (void)bitdepth; (void)dsx_num; (void)dsx_den;
#endif
}

// ---------------------------------------------------------------------------
// Device scratch layout.
//
// Everything the pipeline needs comes out of ONE device allocation, sub-divided
// here, so a frame costs one (cached) allocation instead of six. Only what THIS
// render actually reads is reserved: a passthrough frame pays for the alpha
// plane alone, a blend-only frame skips the blur's ping-pong buffer, and the
// Gaussian weight buffers are sized to the actual kernels.
//
// Offsets are 256-byte aligned; every consumer treats its region as a tightly
// packed W-wide plane (pitch == W), which is NOT the worlds' pitch.
//
// Cost per pixel, in floats:
//   alpha       1   always
//   alphaShift  1   Shift Edge != 0
//   planeA      4   anything but passthrough
//   planeB      4   Blur Amount >= 0.5   (the separable blur's ping-pong)
//   weight      1   anything but passthrough
//   tmp         1   Blend Radius >= 0.5
// So 11 floats/px at the defaults (Shift Edge 0, Blur and Blend both on) and 12
// with Shift Edge active — 641 MB / 700 MB at 4.6K, 365 MB / 398 MB at UHD,
// 91 MB / 100 MB at HD. Same order as the CPU path's own thread-local pool, and
// per DEVICE rather than per render thread, so MFR does not multiply it.
//
// If AllocateDeviceMemory refuses, the render fails rather than degrading
// silently: there is no CPU fallback once the worlds are on the device.
// ---------------------------------------------------------------------------
struct MEB_GPULayout {
    size_t alpha      = 0;
    size_t alphaShift = 0;
    size_t planeA     = 0;     // Blur Amount input / blurred result (ARGB)
    size_t planeB     = 0;     // Blur Amount horizontal-pass target  (ARGB)
    size_t weight     = 0;     // Blend Radius mask
    size_t tmp        = 0;     // erode / feather ping-pong
    size_t kBlur      = 0;
    size_t kBlend     = 0;
    size_t total      = 0;

    bool hasAlphaShift = false;
    bool hasPlaneA     = false;
    bool hasPlaneB     = false;
    bool hasWeight     = false;
    bool hasTmp        = false;
};

static size_t MEB_Align256(size_t v) { return (v + 255u) & ~(size_t)255u; }

static void MEB_BuildLayout(MEB_GPULayout &L, A_long W, A_long H,
                            bool needShift, bool needPlanes, bool needBlurPing,
                            bool needErode, size_t kBlurFloats, size_t kBlendFloats)
{
    const size_t planeBytes  = (size_t)W * (size_t)H * sizeof(float);
    const size_t plane4Bytes = planeBytes * 4u;
    size_t off = 0;
    // Each take() rounds the END up to 256, so every start stays aligned.
    #define MEB_TAKE(field, bytes) do { L.field = off; \
                                        off = MEB_Align256(off + (bytes)); } while (0)

    MEB_TAKE(alpha, planeBytes);

    L.hasAlphaShift = needShift;
    if (needShift) MEB_TAKE(alphaShift, planeBytes);

    L.hasPlaneA = needPlanes;
    if (needPlanes) MEB_TAKE(planeA, plane4Bytes);

    L.hasPlaneB = needBlurPing;
    if (needBlurPing) MEB_TAKE(planeB, plane4Bytes);

    L.hasWeight = needPlanes;
    if (needPlanes) MEB_TAKE(weight, planeBytes);

    // tmp is only touched by the octagon erosion and the feather blur, both of
    // which a sub-0.5 Blend Radius skips entirely (it degenerates to a copy).
    L.hasTmp = needErode;
    if (needErode) MEB_TAKE(tmp, planeBytes);

    if (kBlurFloats)  MEB_TAKE(kBlur,  kBlurFloats  * sizeof(float));
    if (kBlendFloats) MEB_TAKE(kBlend, kBlendFloats * sizeof(float));

    #undef MEB_TAKE
    L.total = off;
}

// ---------------------------------------------------------------------------
// The frame plan: every host-side decision a GPU render makes before it
// enqueues anything — which passes run, the Gaussian weights, the morphology
// geometry, and the scratch layout that follows from all of it.
//
// THIS IS THE SINGLE PLANNER. PreRender's budget gate and MEB_RenderCUDA both
// call it, so the size the gate approves and the size the render allocates come
// from one MEB_BuildLayout call site and cannot drift apart. (MM_FreqSep states
// the same property for FS_PlanTiles — ../../docs/gpu-kernel-rules.md §6.)
// ---------------------------------------------------------------------------
struct MEB_FramePlan {
    bool  do_blur     = false;
    bool  do_blend    = false;
    bool  do_shift    = false;
    bool  passthrough = false;
    bool  erodeActive = false;

    std::vector<float> kBlur, kBlend;
    int   halfBlur  = 0;
    int   halfBlend = 0;

    int   morphRi = 0;      // ceil(|Shift Edge|)
    int   dilate  = 0;      // 1 = grow, 0 = shrink
    float morphR2 = 0.f;    // |Shift Edge|^2

    int   octAxis = 0;      // octagon structuring element, axis half-length
    int   octDiag = 0;      // ...and diagonal half-length

    MEB_GPULayout L;
};

static void MEB_PlanFrame(MEB_FramePlan &P, A_long W, A_long H,
                          const MEB_Params &prm)
{
    // Thresholds copied from SmartRender verbatim, INCLUDING which comparison
    // is done in double and which in float. They differ, and the difference is
    // observable: `do_blend` gates the passthrough decision on the double, but
    // OctagonErode / MakeBlendKernel gate themselves on the float. A radius
    // that rounds up to exactly 0.5f across that cast takes the erode branch
    // while `do_blend` is false, and the GPU has to agree.
    P.do_blur     = (prm.blur_amountF  >= 0.5f);
    P.do_blend    = (prm.blend_radiusF >= 0.5f);
    P.do_shift    = (std::abs((float)prm.shift_edgeF) >= 0.5f);
    P.passthrough = !P.do_blur && !P.do_blend;

    const float blendR = (float)prm.blend_radiusF;
    P.erodeActive = !P.passthrough && (blendR >= 0.5f);   // <- OctagonErode's own gate

    // Gaussian weights: built by the CPU path's OWN kernel builders and
    // uploaded verbatim, so both paths convolve with identical bits
    // (gpu-kernel-rules.md §3.1 — a GPU-side rebuild would diverge in the last
    // bits and turn the parity gate into a coin flip).
    if (P.do_blur)     P.kBlur  = MakeBlurKernel((float)prm.blur_amountF);
    if (P.erodeActive) P.kBlend = MakeBlendKernel(blendR);
    P.halfBlur  = P.kBlur.empty()  ? 0 : (int)(P.kBlur.size()  / 2);
    P.halfBlend = P.kBlend.empty() ? 0 : (int)(P.kBlend.size() / 2);

    // Shift Edge geometry, spelled exactly as MorphPlane spells it.
    const float shiftR = std::abs((float)prm.shift_edgeF);
    P.morphRi = P.do_shift ? (int)std::ceil(shiftR) : 0;
    P.morphR2 = shiftR * shiftR;
    P.dilate  = ((float)prm.shift_edgeF > 0.f) ? 1 : 0;

    // Octagon half-lengths, from the shared helpers so CPU and GPU cannot
    // disagree about the structuring element's size.
    P.octAxis = P.erodeActive ? MEB_OctagonAxisHalf(blendR) : 0;
    P.octDiag = P.erodeActive ? MEB_OctagonDiagHalf(blendR) : 0;

    MEB_BuildLayout(P.L, W, H, P.do_shift, !P.passthrough, P.do_blur,
                    P.erodeActive, P.kBlur.size(), P.kBlend.size());
}

// ---------------------------------------------------------------------------
// PreRender's scratch budget gate.
//
// WHY IT EXISTS. The layout above is all-or-nothing for a full frame — 641 MB
// at 4.6K, 1.5 GB at 8K. If AllocateDeviceMemory refuses it inside
// SMART_RENDER_GPU the frame FAILS: the input and output worlds are already
// device memory by then and CPU code cannot address them, so there is nothing
// to fall back to. The decision has to be taken in PreRender, where declining
// costs nothing but speed.
//
// WHERE IT DIVERGES FROM MM_FreqSep'S (gpu-kernel-rules.md §6), and why. That
// one budgets a fraction of VRAM and feeds it to a TILE PLANNER, so the number
// only moves how many bands a frame is cut into — never correctness. Here there
// is no tiling: the budget is a hard feasibility threshold, so two of its
// choices do not carry over.
//
//   1. NO FIXED UPPER CLAMP. MM_FreqSep clamps to 1 GB because no band ever
//      needs more and it is polite not to hog the card. The same clamp here
//      would mean "never GPU-render above about 5K", permanently, on any card —
//      and 8K comps are precisely the case this gate was written for.
//   2. MEASURED AGAINST FREE VRAM, PER FRAME. Its DX host takes a quarter of
//      DEDICATED VRAM once at device setup; its CUDA host already moved to FREE
//      VRAM, which is the better question, because whatever AE's frame cache is
//      holding is subtracted for us. This goes one step further and re-measures
//      at every gated PreRender rather than caching the setup-time answer: the
//      cache is cold when a device is set up and warm minutes later, so a cached
//      number is measuring the one moment the answer is least representative.
//      cuMemGetInfo is a driver query, no sync, microseconds against a render.
//
// THE FRACTION IS THE ONE UNMEASURED NUMBER — kept at MM_FreqSep's quarter
// rather than invented, so the suite has one value to revise, not two. What
// decides whether a quarter is generous or reckless is whether AE's own
// allocator competes with its frame cache under pressure, and that needs an
// in-AE measurement (docs/NEXT_STEPS.md). The log lines below exist to take it:
// they record free VRAM, the budget, and the requirement at every gated
// PreRender, and MEB_EnsureScratch records free VRAM either side of the real
// allocation.
// ---------------------------------------------------------------------------

// Free device memory in bytes, 0 if unknown. Caller must already hold AE's
// CUcontext current.
static size_t MEB_QueryFreeVRAM(const MEB_CudaApi &cu)
{
    if (!cu.loaded || !cu.MemGetInfo) return 0;
    size_t freeBytes = 0, totalBytes = 0;
    if (cu.MemGetInfo(&freeBytes, &totalBytes) != MEB_CUDA_SUCCESS) return 0;
    return freeBytes;
}

// Budget in bytes for ONE scratch allocation. Pushes AE's context itself, so
// call it from PreRender (nothing is current there), not from inside a render's
// existing guard.
static size_t MEB_CudaScratchBudget(const MEB_GPUData *g, size_t *freeOut)
{
    // No measurement available (no MemGetInfo, push failed, driver refused):
    // a fixed default, same value MM_FreqSep falls back to. Big enough for HD
    // and UHD, too small for 4.6K — the conservative direction.
    const size_t kFallback  = 384ull << 20;
    const size_t kMinBudget = 256ull << 20;

    const MEB_CudaApi &cu = MEB_CudaApi::Get();
    if (!cu.loaded) return kFallback;

    MEB_CudaCtxGuard ctx(cu, g->cuCtx);
    if (!ctx.pushed) return kFallback;

    const size_t freeBytes = MEB_QueryFreeVRAM(cu);
    if (freeBytes == 0) return kFallback;
    if (freeOut) *freeOut = freeBytes;

    size_t budget = freeBytes / 4;

    // Floor, so a momentarily busy card does not refuse work an HD frame (91 MB)
    // would comfortably fit. Applied only while the card can plausibly serve it:
    // MM_FreqSep floors unconditionally, which would have us promise 256 MB on a
    // card with 200 MB free — harmless where the number only picks a band height,
    // a guaranteed hard frame failure here.
    if (budget < kMinBudget && freeBytes >= 2u * kMinBudget) budget = kMinBudget;

    return budget;
}

bool MEB_GPUBudgetAllows(PF_GPU_Framework what_gpu, const void *gpu_dataPV,
                         A_long W, A_long H, const MEB_Params &prm)
{
    // Nothing to size the frame against. Return true — do NOT shut the gate on
    // ignorance. AE offers what_gpu = NONE at every PreRender until it has seen
    // GPU_RENDER_POSSIBLE at least once, so a gate that required a known device
    // to raise the flag would never raise it and never be offered a device: the
    // deadlock EdgeRepair spent seven experiments on (AE_GPU_Effects.md §2.2,
    // §2.3 — "unconditional is fine"). This gate exists to decline frames it can
    // PROVE do not fit, and nothing else.
    if (!MEB_GPUFrameworkSupported(what_gpu) || !gpu_dataPV) return true;
    if (W <= 0 || H <= 0) return true;

    const MEB_GPUData *g = static_cast<const MEB_GPUData*>(gpu_dataPV);

    MEB_FramePlan P;
    MEB_PlanFrame(P, W, H, prm);

    // Worst case on two axes, both erring toward "the render will need less":
    //   - W x H is the full output REQUEST; the world SmartRender is handed is
    //     clipped to our result_rect, which PreRender clips into that request.
    //   - the cached block is grow-only, so it can only be bigger by then.
    const size_t held = g->scratchBytes.load(std::memory_order_relaxed);
    const size_t need = (P.L.total <= held) ? 0u : P.L.total;

    size_t freeBytes = 0;
    const size_t budget = (need == 0u) ? 0u : MEB_CudaScratchBudget(g, &freeBytes);

    // need == 0 means the block we already hold covers this frame: the render
    // allocates NOTHING, so no budget can refuse it. Worth spelling out because
    // it is the steady state — after the first frame of a given size, every
    // later frame of that size passes here for free, and a card whose free
    // memory has since been eaten by AE's own cache keeps rendering on the GPU
    // instead of silently reverting to the CPU.
    const bool fits = (need == 0u) || (need <= budget);

    // free/budget read 0 in the cached case because neither was queried — say
    // so, or the line looks like a failed cuMemGetInfo.
    MEB_GPU_LOG_FIRST(s_budget, 10,
        "[ModernEdgeBlur] PRERENDER budget %ldx%ld need=%.1fMB held=%.1fMB "
        "free=%.0fMB budget=%.1fMB -> %s\n",
        (long)W, (long)H,
        (double)P.L.total / (1024.0 * 1024.0),
        (double)held      / (1024.0 * 1024.0),
        (double)freeBytes / (1024.0 * 1024.0),
        (double)budget    / (1024.0 * 1024.0),
        (need == 0u) ? "fits (scratch already held, no allocation)"
                     : (fits ? "fits" : "TOO BIG - CPU"));

    return fits;
}

// Grow-only device scratch, cached on the per-device gpu_data. Caller holds
// renderLock. Returns false (and leaves the old block intact) on failure.
// Note the ordering: the new block is allocated BEFORE the old one is freed, so
// a growing render transiently holds both. The budget gate is right to compare
// only the new block against free VRAM — the old block was never part of the
// free pool it measured.
//
// The free-VRAM readings either side of the allocation are the measurement
// PreRender's gate is still missing (see MEB_GPUBudgetAllows): a drop of
// roughly `bytes` means AE's allocator took the memory straight from the free
// pool, while a much larger drop — or a success at a free level below `bytes` —
// means it is trading against AE's own frame cache, which is what decides
// whether a quarter of free VRAM is a generous budget or a reckless one.
static bool MEB_EnsureScratch(PF_ProgPtr effect_ref, PF_GPUDeviceSuite1 *gpuS,
                              MEB_GPUData *g, size_t bytes)
{
    if (g->scratch && g->scratchBytes.load(std::memory_order_relaxed) >= bytes)
        return true;

#if MEB_GPU_LOG
    // Diagnostic only — never let the budget measurement cost a driver call in
    // a build with logging compiled out. The context is already current here.
    const MEB_CudaApi &cu = MEB_CudaApi::Get();
    const size_t freeBefore = MEB_QueryFreeVRAM(cu);
#endif

    void *mem = nullptr;
    if (gpuS->AllocateDeviceMemory(effect_ref, g->deviceIndex, bytes, &mem) ||
        !mem) {
        MEB_GPU_LOG_FIRST(s_allocFail, 4,
            "[ModernEdgeBlur] AllocateDeviceMemory(%.1f MB) FAILED "
            "(free before %.0f MB) - frame fails, PreRender's budget gate "
            "should have caught this\n",
            (double)bytes / (1024.0 * 1024.0),
            (double)freeBefore / (1024.0 * 1024.0));
        return false;
    }
    if (g->scratch)
        gpuS->FreeDeviceMemory(effect_ref, g->deviceIndex, g->scratch);
    g->scratch = mem;
    g->scratchBytes.store(bytes, std::memory_order_relaxed);
    MEB_GPU_LOG_FIRST(s_allocOk, 4,
        "[ModernEdgeBlur] GPU scratch grown to %.1f MB "
        "(free %.0f -> %.0f MB)\n",
        (double)bytes / (1024.0 * 1024.0),
        (double)freeBefore / (1024.0 * 1024.0),
        (double)MEB_QueryFreeVRAM(cu) / (1024.0 * 1024.0));
    return true;
}

// ---------------------------------------------------------------------------
// CUDA pass launcher.
//
// Every launch goes on AE'S OWN stream and is NOT synchronized here — AE owns
// stream synchronization. Because the stream is FIFO, serializing the ENQUEUE
// (renderLock) is enough to make the shared per-device scratch safe against
// concurrent MFR renders.
//
// First failure wins and every later launch is skipped, so a broken pass cannot
// cascade into launching kernels against half-written buffers.
// ---------------------------------------------------------------------------
struct MEB_CudaLauncher {
    const MEB_CudaApi &cu;
    MEB_GPUData       *g;
    CUresult           res    = MEB_CUDA_SUCCESS;
    int                failed = -1;

    MEB_CudaLauncher(const MEB_CudaApi &api, MEB_GPUData *gp) : cu(api), g(gp) {}

    void Launch(MEB_KernelId k, unsigned gx, unsigned gy,
                unsigned bx, unsigned by, void **args)
    {
        if (res != MEB_CUDA_SUCCESS) return;
        if (gx == 0u || gy == 0u) return;
        res = cu.LaunchKernel(g->cuFn[k], gx, gy, 1u, bx, by, 1u,
                              0u, g->cuStream, args, nullptr);
        if (res != MEB_CUDA_SUCCESS) failed = (int)k;
    }

    // One thread per pixel over a w x h grid (16x16 blocks).
    void Launch2D(MEB_KernelId k, A_long w, A_long h, void **args)
    {
        if (w <= 0 || h <= 0) return;
        Launch(k, (unsigned)((w + 15) / 16), (unsigned)((h + 15) / 16),
               16u, 16u, args);
    }

    // One thread per element (256-wide blocks).
    void Launch1D(MEB_KernelId k, A_long n, void **args)
    {
        if (n <= 0) return;
        Launch(k, (unsigned)((n + 255) / 256), 1u, 256u, 1u, args);
    }
};

// ---------------------------------------------------------------------------
// The GPU pipeline. Mirrors SmartRender's CPU pipeline pass for pass:
//
//   alpha plane
//     -> [Shift Edge]      circular morphological grow/shrink on that plane
//     -> passthrough, if Blur Amount and Blend Radius are both 0
//     -> derive            zero the a<=0 cliff, premultiply for the blur
//     -> [Blur Amount]     separable Gaussian H,V + unpremultiply
//     -> [Blend Radius]    octagon erosion (4 directional mins) + feather
//     -> composite         alpha-weighted blend of sharp over blurred
//
// Every `if` below is a decision about the three sliders that the CPU path
// takes at exactly the same point, in exactly the same order.
// ---------------------------------------------------------------------------
static PF_Err MEB_RenderCUDA(PF_InData *in_data, PF_OutData *out_data,
                             MEB_GPUData *g,
                             PF_EffectWorld *src_world, PF_EffectWorld *out_world,
                             const MEB_Params &prm)
{
    const MEB_CudaApi &cu = MEB_CudaApi::Get();
    if (!cu.loaded || !g->cuFn[MEB_K_COMPOSITE])
        return PF_Err_INTERNAL_STRUCT_DAMAGED;

    const A_long W    = out_world->width;
    const A_long H    = out_world->height;
    const A_long srcW = src_world->width;
    const A_long srcH = src_world->height;
    if (W <= 0 || H <= 0 || srcW <= 0 || srcH <= 0) return PF_Err_NONE;

    // ---- Decide the pipeline shape (the CPU path's host-side branches) ----
    // Same planner PreRender's budget gate ran, on the real (<= worst-case)
    // geometry, so gate and render cannot disagree about what this frame costs.
    MEB_FramePlan P;
    MEB_PlanFrame(P, W, H, prm);

    const bool  do_blur     = P.do_blur;
    const bool  do_shift    = P.do_shift;
    const bool  passthrough = P.passthrough;
    const bool  erodeActive = P.erodeActive;
    const std::vector<float> &kBlur  = P.kBlur;
    const std::vector<float> &kBlend = P.kBlend;
    const int   halfBlur  = P.halfBlur;
    const int   halfBlend = P.halfBlend;
    const int   morphRi   = P.morphRi;
    const float morphR2   = P.morphR2;
    const int   dilate    = P.dilate;
    const int   octAxis   = P.octAxis;
    const int   octDiag   = P.octDiag;
    const MEB_GPULayout &L = P.L;

    MEB_GPU_LOG_FIRST(s_plan, 6,
        "[ModernEdgeBlur] GPU plan out=%ldx%ld src=%ldx%ld shift=%d(ri=%d) "
        "blur=%d(half=%d) blend=%d(half=%d oct=%d/%d) pass=%d scratch=%.1fMB\n",
        (long)W, (long)H, (long)srcW, (long)srcH,
        do_shift ? 1 : 0, morphRi, do_blur ? 1 : 0, halfBlur,
        erodeActive ? 1 : 0, halfBlend, octAxis, octDiag,
        passthrough ? 1 : 0, (double)L.total / (1024.0 * 1024.0));

    PF_GPUDeviceSuite1 *gpuS = nullptr;
    PF_Err err = AEFX_AcquireSuite(in_data, out_data, kPFGPUDeviceSuite,
                                   kPFGPUDeviceSuiteVersion1, "",
                                   reinterpret_cast<void**>(&gpuS));
    if (err || !gpuS) return err ? err : PF_Err_BAD_CALLBACK_PARAM;

    // For the CUDA framework GetGPUWorldData returns the device pointer.
    void *src_mem = nullptr, *dst_mem = nullptr;
    err = gpuS->GetGPUWorldData(in_data->effect_ref, src_world, &src_mem);
    if (!err) err = gpuS->GetGPUWorldData(in_data->effect_ref, out_world, &dst_mem);
    if (!err && (!src_mem || !dst_mem)) err = PF_Err_BAD_CALLBACK_PARAM;
    if (err) {
        AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
                          kPFGPUDeviceSuiteVersion1, "");
        return err;
    }

    {
        // The lock covers scratch growth AND the whole enqueue sequence — see
        // MEB_CudaLauncher's note on why enqueue order is enough.
        std::lock_guard<std::mutex> lock(g->renderLock);

        // AE's CUcontext is pushed BEFORE the allocation, not just around the
        // launches: the suite's device allocator runs driver calls of its own,
        // and the suite header's discipline is that a context is current for
        // any driver-call sequence we trigger.
        MEB_CudaCtxGuard ctx(cu, g->cuCtx);
        if (!ctx.pushed) {
            AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
                              kPFGPUDeviceSuiteVersion1, "");
            return PF_Err_INTERNAL_STRUCT_DAMAGED;
        }

        if (!MEB_EnsureScratch(in_data->effect_ref, gpuS, g, L.total)) {
            AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
                              kPFGPUDeviceSuiteVersion1, "");
            return PF_Err_OUT_OF_MEMORY;
        }

        const CUdeviceptr base = reinterpret_cast<CUdeviceptr>(g->scratch);
        CUdeviceptr dSrc   = reinterpret_cast<CUdeviceptr>(src_mem);
        CUdeviceptr dDst   = reinterpret_cast<CUdeviceptr>(dst_mem);
        CUdeviceptr dAlpha = base + L.alpha;
        // Buffers a given render never builds are never DEREFERENCED (the pass
        // sequence below simply does not issue those kernels), but they must
        // still be a VALID address — point them at the alpha plane, which is
        // always reserved. Belt and braces: unreserved regions already have
        // offset 0, which is alpha's own offset, but say it outright so a
        // future reordering of MEB_BuildLayout cannot quietly turn a skipped
        // buffer into a wild pointer.
        CUdeviceptr dShift  = L.hasAlphaShift ? base + L.alphaShift : dAlpha;
        CUdeviceptr dPlaneA = L.hasPlaneA     ? base + L.planeA     : dAlpha;
        CUdeviceptr dPlaneB = L.hasPlaneB     ? base + L.planeB     : dAlpha;
        CUdeviceptr dWeight = L.hasWeight     ? base + L.weight     : dAlpha;
        CUdeviceptr dTmp    = L.hasTmp        ? base + L.tmp        : dAlpha;
        CUdeviceptr dKBlur  = kBlur.empty()   ? dAlpha : base + L.kBlur;
        CUdeviceptr dKBlend = kBlend.empty()  ? dAlpha : base + L.kBlend;

        // Upload the weights. cuMemcpyHtoDAsync from PAGEABLE host memory
        // copies into a driver staging buffer before returning, so kBlur /
        // kBlend may go out of scope right after this — the documented
        // behaviour, and the reason no pinned allocation is needed for a few
        // hundred floats.
        if (!kBlur.empty())
            cu.MemcpyHtoDAsync(dKBlur, kBlur.data(),
                               kBlur.size() * sizeof(float), g->cuStream);
        if (!kBlend.empty())
            cu.MemcpyHtoDAsync(dKBlend, kBlend.data(),
                               kBlend.size() * sizeof(float), g->cuStream);

        // The param block is re-filled per pass; cuLaunchKernel copies it at
        // call time, so reusing one instance across passes is safe.
        MEB_GPU_PassParams b;
        AEFX_CLR_STRUCT(b);
        b.srcPitchF = src_world->rowbytes / (A_long)sizeof(float);
        b.dstPitchF = out_world->rowbytes / (A_long)sizeof(float);
        b.srcW = srcW;  b.srcH = srcH;
        b.W    = W;     b.H    = H;

        MEB_CudaLauncher lx(cu, g);
        MEB_GPU_PassParams p;

        // ---- alpha plane ----
        p = b;
        { void *a[] = { &dSrc, &dAlpha, &p };
          lx.Launch2D(MEB_K_EXTRACT_ALPHA, W, H, a); }

        // ---- Phase 1: Shift Edge ----
        // Runs BEFORE everything that keys off alpha: it changes the alpha
        // SHAPE, and the zero-RGB cliff plus both derived buffers must see the
        // FINAL alpha, or a grow leaves newly-revealed pixels black and a
        // shrink leaves a ring of un-zeroed colour behind.
        CUdeviceptr dAlphaCur = dAlpha;
        if (do_shift) {
            p = b;
            p.morphRi = morphRi;
            p.morphR2 = morphR2;
            p.dilate  = dilate;
            void *a[] = { &dAlpha, &dShift, &p };
            lx.Launch2D(MEB_K_MORPH, W, H, a);
            dAlphaCur = dShift;
        }

        if (passthrough) {
            // Blur Amount = 0 AND Blend Radius = 0. Held RGB is preserved
            // except at the a<=0 cliff, same as the CPU path's passthrough.
            p = b;
            void *a[] = { &dSrc, &dAlphaCur, &dDst, &p };
            lx.Launch2D(MEB_K_PASSTHROUGH, W, H, a);
        } else {
            // ---- derive Blur Amount's input plane ----
            p = b;
            p.premult = do_blur ? 1 : 0;
            { void *a[] = { &dSrc, &dAlphaCur, &dPlaneA, &p };
              lx.Launch2D(MEB_K_DERIVE, W, H, a); }

            // ---- Phase 2: Blur Amount ----
            // premultiply (done at derive) -> blur -> UNPREMULTIPLY (folded
            // into the vertical pass). Premultiplied space for the blur itself
            // so a partial-alpha edge pixel contributes only its
            // coverage-weighted share of colour; straight colour on the way
            // out because AE's effect pipeline is straight alpha on both sides
            // and applies alpha itself at composite time.
            if (do_blur) {
                p = b; p.half = halfBlur;
                { void *a[] = { &dPlaneA, &dPlaneB, &dKBlur, &p };
                  lx.Launch2D(MEB_K_BLUR_H_RGBA, W, H, a); }
                p = b; p.half = halfBlur; p.unpremult = 1;
                { void *a[] = { &dPlaneB, &dPlaneA, &dKBlur, &p };
                  lx.Launch2D(MEB_K_BLUR_V_RGBA, W, H, a); }
            }
            // else: dPlaneA holds the straight-colour copy written at derive —
            // already in the right space for the composite, nothing to do.

            // ---- Phase 3: Blend Radius ----
            // Erode the sharp layer's reveal mask inward (octagon = 4
            // directional mins composed under Minkowski sum), then Gaussian-
            // feather the new, smaller edge.
            if (erodeActive) {
                // One directional pass, or a plain copy when its half-length
                // rounds to 0 — exactly what DirectionalMinFilter does.
                auto dirMin = [&](CUdeviceptr &in, CUdeviceptr &out,
                                  int dx, int dy, int halfLen) {
                    if (halfLen <= 0) {
                        p = b; p.count = W * H;
                        void *a[] = { &in, &out, &p };
                        lx.Launch1D(MEB_K_COPY_PLANE, p.count, a);
                        return;
                    }
                    p = b; p.dirX = dx; p.dirY = dy; p.halfLen = halfLen;
                    void *a[] = { &in, &out, &p };
                    lx.Launch2D(MEB_K_DIR_MIN, W, H, a);
                };
                dirMin(dAlphaCur, dTmp,     1,  0, octAxis);   // horizontal
                dirMin(dTmp,      dWeight,  0,  1, octAxis);   // vertical
                dirMin(dWeight,   dTmp,     1,  1, octDiag);   // diagonal
                dirMin(dTmp,      dWeight,  1, -1, octDiag);   // anti-diagonal

                if (!kBlend.empty()) {
                    p = b; p.half = halfBlend;
                    { void *a[] = { &dWeight, &dTmp, &dKBlend, &p };
                      lx.Launch2D(MEB_K_BLUR_H_PLANE, W, H, a); }
                    p = b; p.half = halfBlend;
                    { void *a[] = { &dTmp, &dWeight, &dKBlend, &p };
                      lx.Launch2D(MEB_K_BLUR_V_PLANE, W, H, a); }
                }
            } else {
                // Blend Radius below 0.5: OctagonErode degenerates to a copy,
                // so the weight is the alpha itself (clamped in the composite).
                p = b; p.count = W * H;
                void *a[] = { &dAlphaCur, &dWeight, &p };
                lx.Launch1D(MEB_K_COPY_PLANE, p.count, a);
            }

            // ---- Phase 4: composite ----
            p = b;
            void *a[] = { &dSrc, &dAlphaCur, &dPlaneA, &dWeight, &dDst, &p };
            lx.Launch2D(MEB_K_COMPOSITE, W, H, a);
        }

        if (lx.res != MEB_CUDA_SUCCESS) {
            MEB_GPU_LOG_FIRST(s_launchFail, 6,
                "[ModernEdgeBlur] cuLaunchKernel(%s) FAILED (CUresult=%d)\n",
                (lx.failed >= 0 && lx.failed < MEB_K_COUNT)
                    ? kMEB_KernelNames[lx.failed] : "?",
                (int)lx.res);
            err = PF_Err_INTERNAL_STRUCT_DAMAGED;
        }
    }

    AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
                      kPFGPUDeviceSuiteVersion1, "");
    return err;
}

PF_Err MEB_SmartRenderGPU(PF_InData *in_data, PF_OutData *out_data,
                          PF_SmartRenderExtra *extra,
                          PF_EffectWorld *src_world, PF_EffectWorld *out_world,
                          const MEB_Params &prm)
{
    MEB_GPU_LOG_FIRST(s_gpuRenderCount, 8,
        "[ModernEdgeBlur] SMART_RENDER GPU what_gpu=%d out=%ldx%ld src=%ldx%ld\n",
        (int)extra->input->what_gpu,
        (long)out_world->width, (long)out_world->height,
        (long)src_world->width, (long)src_world->height);

    MEB_GPUData *gpu_data =
        static_cast<MEB_GPUData*>(const_cast<void*>(extra->input->gpu_data));
    if (!gpu_data || gpu_data->framework != extra->input->what_gpu) {
        // AE should never get here: it only drives GPU renders for devices that
        // returned gpu_data. If it does, the worlds are device memory and the
        // CPU pipeline cannot address them — an error is the only honest
        // outcome.
        MEB_GPU_LOG_FIRST(s_badBlob, 4,
            "[ModernEdgeBlur] SMART_RENDER GPU with gpu_data=%p framework=%d "
            "what_gpu=%d - refusing\n",
            (void*)gpu_data, gpu_data ? (int)gpu_data->framework : -1,
            (int)extra->input->what_gpu);
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    if (gpu_data->framework != PF_GPU_Framework_CUDA)
        return PF_Err_INTERNAL_STRUCT_DAMAGED;

    return MEB_RenderCUDA(in_data, out_data, gpu_data,
                          src_world, out_world, prm);
}

#else   /* !_WIN32 — no GPU host on this platform yet */

#include "ModernEdgeBlur.h"
#include "ModernEdgeBlur_GPU.h"

/*  Decline stubs — see the fork note at the top of this file. The behaviour
    matches what the Windows host does for a framework it cannot render on:
    accept setup, return no device data, let the CPU pipeline take the frame. */

PF_Err MEB_GPUDeviceSetup(PF_InData *, PF_OutData *, PF_GPUDeviceSetupExtra *extra)
{
    extra->output->gpu_data = nullptr;
    return PF_Err_NONE;
}

PF_Err MEB_GPUDeviceSetdown(PF_InData *, PF_OutData *, PF_GPUDeviceSetdownExtra *)
{
    return PF_Err_NONE;
}

bool MEB_GPUFrameworkSupported(PF_GPU_Framework) { return false; }

/* TRUE, deliberately, and it is not a contradiction with the line above. The
   budget gate's contract is that it may only return false for a frame it can
   PROVE does not fit; withholding the flag out of ignorance is the deadlock
   documented in AE_GPU_Effects.md §2.2-2.3. Nothing here can size a frame, so
   nothing here may decline one — GPU_RENDER_POSSIBLE stays unraised because
   MEB_GPUFrameworkSupported is false, which is the honest reason. */
bool MEB_GPUBudgetAllows(PF_GPU_Framework, const void *, A_long, A_long,
                         const MEB_Params &) { return true; }

void MEB_GPULogPreRenderDecision(PF_GPU_Framework, const void *, bool,
                                 int, int, int) {}
void MEB_GPULogSmartRenderCPU(int, int, int, int) {}

/* Unreachable while MEB_GPUFrameworkSupported is false; fail loudly rather
   than silently returning a black frame if that ever stops holding. */
PF_Err MEB_SmartRenderGPU(PF_InData *, PF_OutData *, PF_SmartRenderExtra *,
                          PF_EffectWorld *, PF_EffectWorld *,
                          const MEB_Params &)
{
    return PF_Err_INTERNAL_STRUCT_DAMAGED;
}

#endif  /* _WIN32 */
