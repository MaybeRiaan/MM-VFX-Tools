/*  EdgeRepair_GPU.cpp — Stage 6 GPU path implementation (CUDA + DirectX 12).

    See EdgeRepair_GPU.h for the backend policy. MEASURED 2026-07-23 (see the
    diagnostics below + docs/EdgeRepair.md): After Effects 2026 on Windows/
    NVIDIA hands effects what_gpu = CUDA — it never probes DIRECTX even with
    SUPPORTS_DIRECTX_RENDERING set (that flag serves AE-SDK effects hosted in
    Premiere Pro, whose Windows GPU renderer is DX12). So CUDA is the backend
    AE actually exercises on the test box; DX12 remains for Premiere hosts.

    ── What runs where (after the kernel port) ──────────────────────────────
    CUDA carries the FULL pipeline — core matte, JFA+2 distance, pull–push
    fill, grain-amplitude reduction, composite, and all four View Modes — in
    ER_RenderCUDA, mirroring SmartRender's CPU pipeline pass for pass. DX12 is
    still the plumbing-milestone passthrough (ER_RenderDX12's header explains
    why it was not ported blind). Both share the same kernel MATH:
    src/gpu/ER_KernelMath.h compiles for HLSL, CUDA, Metal and host C++, and
    tests/test_gpuparity.cpp diffs it against the MM_*.h originals with no GPU
    in the loop.

    ── Host-side branching without host-side readback ───────────────────────
    The CPU pipeline asks questions between passes ("did the core matte come
    out empty?", "did the fill find a seed?") and branches on the answer. The
    GPU cannot: reading a result back mid-render would mean synchronizing AE's
    stream. Instead the producing kernels OR a bit into a tiny device flags
    buffer and the consuming kernels test it — same decisions, no round-trip.
    See ER_FLAG_* in ER_KernelMath.h.

    CUDA: host side goes through a runtime nvcuda.dll shim
    (src/gpu/ER_CudaShim.h — compiles/runs with NO toolkit installed); the
    kernel ships as PTX text in CUDA_Assets/EdgeRepair_Kernels.ptx next to
    the .aex (built by nvcc when the toolkit is present — CMake skips it
    gracefully otherwise, and setup declines → CPU fallback). AE provides
    the CUcontext/CUstream via PF_GPUDeviceSuite1; per that suite's header,
    the context is pushed/popped around every driver-call sequence
    (ER_CudaCtxGuard). Kernels launch on AE'S OWN stream and are NOT
    synchronized here — AE owns stream synchronization (same contract the
    SDK CUDA samples rely on).

    DX12: everything device-side goes through AE's PF_GPUDeviceSuite1 (never
    raw allocation — the suite owns GPU memory) and Adobe's DXContext/
    DXShaderExecution helpers (sdk/Util/DirectXUtils), whose binding ABI is:
    root param 0 = CBV table, 1 = UAV table, 2 = SRV table; raw
    ByteAddressBuffers. Kernels are precompiled at build time (fxc) into
    DirectX_Assets/<name>.cso + .rs next to EdgeRepair.aex.

    THREADING: one DXContext (command list/allocator/fence) exists per AE GPU
    device; a mutex serializes its use (AE does not promise GPU SmartRenders
    are serialized). The CUDA path needs no lock: module/function are
    immutable after setup and launches enqueue on AE's stream, which
    serializes execution order. Neither is mutable global state — the blob is
    AE-owned per-device data handed back to us per render (MFR-safe by the
    same argument as pre_render_data).
*/

/* ---------------------------------------------------------------------------
   PLATFORM FORK. Everything below is the WINDOWS GPU host: CUDA through the
   nvcuda.dll shim, plus the DX12 path Premiere uses. Both are Windows-only by
   construction (nvcuda.dll, d3d12.h), and Metal is unwritten
   (../../docs/gpu-kernel-rules.md §4), so on any other platform this file
   compiles down to the DECLINE stubs at the bottom: PF_Err_NONE with null
   gpu_data at device setup and ER_GPUFrameworkSupported false, so PreRender
   never raises GPU_RENDER_POSSIBLE and AE routes every frame to the CPU
   reference — the always-correct path on both platforms.

   The fork lives HERE rather than at EdgeRepair.cpp's call sites on purpose:
   the command dispatch and PreRender read identically on both platforms, and
   there is exactly one place to unpick when a Metal host lands. Same shape as
   MM_Qualify's QK_GPUDeviceSetup fallback (MM_Qualify/src/smart_render.cpp).
   --------------------------------------------------------------------------- */
#ifdef _WIN32

#define NOMINMAX
#include "EdgeRepair.h"          // SDK includes, ER_* enums
#include "EdgeRepair_GPU.h"

#include "AE_EffectGPUSuites.h"
#include "AEFX_SuiteHelper.h"
#include "PrSDKAESupport.h"      // PF_PixelFormatSuite (MediaCore-side formats)
#include "PrSDKPixelFormat.h"    // PrPixelFormat_* labels

#include "DirectXUtils.h"        // DXContext / DXShaderExecution / GetShaderPath
#include "gpu/ER_CudaShim.h"     // runtime nvcuda.dll driver-API surface

// The shared kernel header, compiled here as plain HOST C++ (no ER_GPU_*
// language macro defined). The host needs the same ER_CORE_MODE_* /
// ER_CLASSIFY_* / ER_FLAG_* / ER_GRAIN_* constants the kernels branch on —
// single-sourcing them is what stops the host and the device disagreeing
// about, say, which flag slot means "the core is non-empty".
#include "gpu/ER_KernelMath.h"

#include <d3d12.h>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stage 6 GPU diagnostics. Set ER_GPU_LOG to 1 to append the GPU lifecycle to
// %TEMP%\EdgeRepair_gpu.log: every DEVICE_SETUP/SETDOWN with the framework AE
// offered (1=OpenCL 2=Metal 3=CUDA 4=DirectX) and each setup step's outcome,
// plus the first few PreRender decisions and SmartRender dispatches. This is
// how the milestone gate gets diagnosed when AE silently renders on CPU: the
// log shows exactly where the chain stops (never probed / wrong framework /
// kernel load failed / flag not raised). OFF for shipping builds.
// The first-N rate limit uses write-only atomic counters — diagnostic-only,
// no render behavior may ever depend on them (MFR discipline).
// ---------------------------------------------------------------------------
#define ER_GPU_LOG 1

// (The temporary ER_GPU_VISIBLE_MARKER that zeroed the top eighth of the frame
// is GONE as of the kernel port — it existed only because the milestone
// passthrough rendered bit-identically to the CPU path, so nothing on screen
// could tell you whether the GPU had run. The real kernels are their own proof.)

#if ER_GPU_LOG
#include <atomic>
#include <cstdarg>
#include <cstdlib>
static void ER_GPULog(const char *fmt, ...)
{
    char path[1024];
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = ".";
    snprintf(path, sizeof(path), "%s\\EdgeRepair_gpu.log", tmp);
    FILE *f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}
// Log only the first `limit` hits per call site (PreRender/SmartRender fire
// per frame — unbounded logging would swamp the file).
// The counters are PROCESS statics: a few seconds of scrubbing at default
// params exhausts them, after which the log is silent and an A/B sweep leaves
// no trace at all. Set MM_GPU_LOG_ALL=1 to lift every cap across the whole
// suite for a validation session (../../docs/gpu-validation-checklist.md §3).
static bool ER_GPULogAll()
{
    static const bool all = [] {
        char v[8];
        const DWORD n = ::GetEnvironmentVariableA("MM_GPU_LOG_ALL", v, (DWORD)sizeof(v));
        return n > 0 && n < (DWORD)sizeof(v) && v[0] != '0';
    }();
    return all;
}
#define ER_GPU_LOG_FIRST(counter, limit, ...) \
    do { static std::atomic<int> counter(0); \
         if (counter.fetch_add(1) < (limit) || ER_GPULogAll()) \
             ER_GPULog(__VA_ARGS__); } while (0)
#define ER_GPU_LOGF(...) ER_GPULog(__VA_ARGS__)
#else
#define ER_GPU_LOG_FIRST(counter, limit, ...) ((void)0)
#define ER_GPU_LOGF(...) ((void)0)
#endif

// ---------------------------------------------------------------------------
// The kernel table. One entry per __global__ entry point in
// src/gpu/EdgeRepairKernels.cu — names must match EXACTLY (cuModuleGetFunction
// resolves them by string). The HLSL/Metal wrappers mirror the same list.
// ---------------------------------------------------------------------------
enum ER_KernelId {
    ER_K_PASSTHROUGH = 0,
    ER_K_ZERO,
    ER_K_EXTRACT_ALPHA,
    ER_K_CLASSIFY,
    ER_K_MAKE_CORE,
    ER_K_JFA_INIT_ALPHA,
    ER_K_JFA_INIT_MASK,
    ER_K_JFA_STEP,
    ER_K_JFA_RESOLVE,
    ER_K_PP_INIT,
    ER_K_PP_PULL,
    ER_K_PP_PUSH,
    ER_K_PP_RESOLVE,
    ER_K_GRAIN_PARTIAL,
    ER_K_GRAIN_FINAL,
    ER_K_COMPOSITE,
    ER_K_VIEW_CORE,
    ER_K_VIEW_DIST,
    ER_K_VIEW_EXT,
    ER_K_COUNT
};

static const char *const kER_KernelNames[ER_K_COUNT] = {
    "ER_Passthrough",
    "ER_Zero",
    "ER_ExtractAlpha",
    "ER_Classify",
    "ER_MakeCore",
    "ER_JfaInitAlpha",
    "ER_JfaInitMask",
    "ER_JfaStep",
    "ER_JfaResolve",
    "ER_PPInit",
    "ER_PPPull",
    "ER_PPPush",
    "ER_PPResolve",
    "ER_GrainPartial",
    "ER_GrainFinal",
    "ER_Composite",
    "ER_ViewCore",
    "ER_ViewDist",
    "ER_ViewExt",
};

// ---------------------------------------------------------------------------
// Per-device GPU state, owned by AE via extra->output->gpu_data.
// ---------------------------------------------------------------------------
struct ER_GPUData {
    PF_GPU_Framework framework = PF_GPU_Framework_NONE;
    A_u_long         deviceIndex = 0;  // needed to free scratch at setdown

    // DIRECTX backend
    DXContextPtr     dxContext;        // wraps AE's device + queue
    ShaderObjectPtr  psPassthrough;    // EdgeRepair_Passthrough.cso/.rs

    // Serializes the whole GPU render: the DX12 command list (AE does not
    // promise GPU SmartRenders are serialized) AND the per-device scratch
    // block below, which every concurrent render would otherwise share. CUDA
    // work is enqueued on AE's single per-device stream, so serializing the
    // ENQUEUE is enough to serialize execution — the stream is FIFO.
    std::mutex       renderLock;

    // CUDA backend (driver API via ER_CudaShim; objects immutable post-setup)
    CUcontext        cuCtx    = nullptr;          // AE's context (not owned)
    CUstream         cuStream = nullptr;          // AE's stream (not owned)
    CUmodule         cuModule = nullptr;          // loaded PTX (owned)
    CUfunction       cuFn[ER_K_COUNT] = { nullptr };

    // Device scratch for the multi-pass pipeline (planes, jump-flood seed
    // buffers, the pull–push mip chain). ONE allocation, sub-divided per
    // render; grow-only and cached across frames so a steady-state render
    // allocates nothing — the same discipline as the CPU path's ER_Scratch,
    // except this one is per DEVICE (guarded by renderLock) rather than
    // per thread. Freed at device setdown.
    void            *scratch      = nullptr;
    size_t           scratchBytes = 0;
};

// Kernel launch params — must match cbuffer ERPassParams in
// EdgeRepairKernels.hlsl, ER_PassParamsCu in EdgeRepairKernels.cu and
// ER_PassParamsMtl in EdgeRepairKernels.metal FIELD FOR FIELD. 28 x 4 bytes =
// 112, arranged in 7 x 16-byte groups so the HLSL cbuffer packs identically.
// DXShaderExecution::SetParamBuffer requires < 256 bytes.
struct ER_GPU_PassParams {
    // worlds
    A_long srcPitchF;      // source world row pitch in FLOATS (rowbytes / 4)
    A_long dstPitchF;      // dest   world row pitch in FLOATS
    A_long srcOX;          // output (0,0) sits at source (srcOX, srcOY)
    A_long srcOY;
    A_long outW;           // output world dims
    A_long outH;
    A_long W;              // inflated source dims == scratch plane dims
    A_long H;

    // pass-local geometry
    A_long lvlOffFine;     // float offset of the FINE mip level in the chain
    A_long lvlOffCoarse;   // float offset of the COARSE mip level
    A_long lvlWFine;
    A_long lvlHFine;
    A_long lvlWCoarse;
    A_long lvlHCoarse;
    A_long jfaStep;        // jump-flood step for this pass
    A_long mode;           // ER_CLASSIFY_* / ER_CORE_MODE_*, pass-dependent

    // effect params
    float  erode;
    float  extend;
    float  softness;
    float  detail01;
    float  choke;
    float  thresh;         // core-matte alpha threshold (0.5)
    A_long chokeMode;      // ER_CHOKE_MODE_*
    A_long view;           // ER_VIEW_M_*

    A_long bandRequested;  // host: Extend > 0 (field + fill were dispatched)
    A_long grainBlocks;    // partial-sum slots for ER_GrainFinal
    A_long flagSlot;       // which ER_FLAG_* this pass reports into
    A_long count;          // element count for ER_Zero
};
static_assert(sizeof(ER_GPU_PassParams) == 112,
              "param block must stay 112 bytes and field-identical to the "
              "HLSL cbuffer / CUDA struct / Metal struct");

bool ER_GPUFrameworkSupported(PF_GPU_Framework what_gpu)
{
    // CUDA = what AE-on-Windows actually offers (measured 2026-07-23);
    // DIRECTX = Premiere hosts. Both gate further on device setup succeeding
    // (PreRender requires non-null gpu_data before raising
    // GPU_RENDER_POSSIBLE), so listing a framework here is safe even when its
    // assets/driver are absent at runtime.
    return what_gpu == PF_GPU_Framework_DIRECTX ||
           what_gpu == PF_GPU_Framework_CUDA;
}

// ---------------------------------------------------------------------------
// Asset-path helpers (CUDA). The DX12 twin is DirectXUtils' GetShaderPath;
// same discipline: resolve the folder from THIS module's own path so the
// assets always travel next to the deployed .aex.
// ---------------------------------------------------------------------------
static bool ER_CudaPtxPath(std::wstring &outPath)
{
    wchar_t path[1024];
    HMODULE hm = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&ER_CudaPtxPath), &hm))
        return false;
    if (::GetModuleFileNameW(hm, path, static_cast<DWORD>(std::size(path))) == 0)
        return false;
    std::wstring dir(path);
    const size_t slash = dir.rfind(L'\\');
    if (slash == std::wstring::npos) return false;
    outPath = dir.substr(0, slash) + L"\\CUDA_Assets\\EdgeRepair_Kernels.ptx";
    return true;
}

// Read the whole PTX file, NUL-terminated (cuModuleLoadDataEx takes a
// NUL-terminated text image).
static bool ER_ReadTextFile(const std::wstring &path, std::vector<char> &out)
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
// Per-backend device setup
// ---------------------------------------------------------------------------

// DX12: wrap AE's device/queue in Adobe's DXContext, load precompiled
// kernels. Failures are LOUD (PF_Err) — on a healthy install they indicate a
// broken deploy (missing DirectX_Assets), which should be visible.
static PF_Err ER_SetupDX12(const PF_GPUDeviceInfo &device_info,
                           ER_GPUData *gpu_data)
{
    gpu_data->dxContext = std::make_shared<DXContext>();
    if (!gpu_data->dxContext->Initialize(
            reinterpret_cast<ID3D12Device*>(device_info.devicePV),
            reinterpret_cast<ID3D12CommandQueue*>(device_info.command_queuePV))) {
        ER_GPU_LOGF("  -> DXContext::Initialize FAILED (device=%p queue=%p)\n",
                    device_info.devicePV, device_info.command_queuePV);
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    std::wstring csoPath, sigPath;
    gpu_data->psPassthrough = std::make_shared<ShaderObject>();
    if (!GetShaderPath(L"EdgeRepair_Passthrough", csoPath, sigPath)) {
        ER_GPU_LOGF("  -> GetShaderPath FAILED\n");
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    if (!gpu_data->dxContext->LoadShader(csoPath.c_str(), sigPath.c_str(),
                                         gpu_data->psPassthrough)) {
        ER_GPU_LOGF("  -> LoadShader FAILED: %ls / %ls\n",
                    csoPath.c_str(), sigPath.c_str());
        return PF_Err_INTERNAL_STRUCT_DAMAGED;   // missing/broken .cso|.rs
    }
    return PF_Err_NONE;
}

// CUDA: load the PTX module on AE's context. Failures return false and the
// caller declines QUIETLY (CPU fallback) — a missing toolkit/PTX is a normal
// configuration (build machines without nvcc), never worth an AE error.
static bool ER_SetupCUDA(const PF_GPUDeviceInfo &device_info,
                         ER_GPUData *gpu_data)
{
    const ER_CudaApi &cu = ER_CudaApi::Get();
    if (!cu.loaded) {
        ER_GPU_LOGF("  -> nvcuda.dll not available — CUDA declined\n");
        return false;
    }

    gpu_data->cuCtx    = reinterpret_cast<CUcontext>(device_info.contextPV);
    gpu_data->cuStream = reinterpret_cast<CUstream>(device_info.command_queuePV);
    if (!gpu_data->cuCtx) {
        ER_GPU_LOGF("  -> AE provided no CUcontext — CUDA declined\n");
        return false;
    }

    std::wstring ptxPath;
    std::vector<char> ptx;
    if (!ER_CudaPtxPath(ptxPath) || !ER_ReadTextFile(ptxPath, ptx)) {
        ER_GPU_LOGF("  -> PTX missing/unreadable (%ls) — CUDA declined "
                    "(build with the CUDA toolkit installed to produce it)\n",
                    ptxPath.c_str());
        return false;
    }

    ER_CudaCtxGuard ctx(cu, gpu_data->cuCtx);
    if (!ctx.pushed) {
        ER_GPU_LOGF("  -> cuCtxPushCurrent FAILED — CUDA declined\n");
        return false;
    }

    CUresult res = cu.ModuleLoadDataEx(&gpu_data->cuModule, ptx.data(),
                                       0, nullptr, nullptr);
    if (res != ER_CUDA_SUCCESS || !gpu_data->cuModule) {
        ER_GPU_LOGF("  -> cuModuleLoadDataEx FAILED (CUresult=%d) — CUDA declined\n",
                    (int)res);
        gpu_data->cuModule = nullptr;
        return false;
    }

    // Resolve every kernel up front. A missing entry means the deployed PTX is
    // stale relative to this binary (the classic symptom of a half-finished
    // deploy) — decline the whole backend rather than discover it mid-render,
    // where there is no CPU fallback because the worlds are device memory.
    for (int k = 0; k < ER_K_COUNT; ++k) {
        res = cu.ModuleGetFunction(&gpu_data->cuFn[k], gpu_data->cuModule,
                                   kER_KernelNames[k]);
        if (res != ER_CUDA_SUCCESS || !gpu_data->cuFn[k]) {
            ER_GPU_LOGF("  -> cuModuleGetFunction(%s) FAILED (CUresult=%d) — "
                        "CUDA declined (stale CUDA_Assets PTX?)\n",
                        kER_KernelNames[k], (int)res);
            cu.ModuleUnload(gpu_data->cuModule);
            gpu_data->cuModule = nullptr;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Device setup / setdown
// ---------------------------------------------------------------------------
PF_Err ER_GPUDeviceSetup(PF_InData *in_data, PF_OutData *out_data,
                         PF_GPUDeviceSetupExtra *extra)
{
    PF_Err err = PF_Err_NONE;

    ER_GPU_LOGF("[EdgeRepair] DEVICE_SETUP what_gpu=%d device_index=%u\n",
                (int)extra->input->what_gpu,
                (unsigned)extra->input->device_index);

    if (!ER_GPUFrameworkSupported(extra->input->what_gpu)) {
        // Decline QUIETLY: PF_Err_NONE with gpu_data left null. AE then never
        // renders this effect on GPU for devices of this framework (PreRender
        // additionally requires non-null gpu_data before raising
        // GPU_RENDER_POSSIBLE) — the intended CPU fallback for Metal-on-Mac
        // and OpenCL. An error return here would risk a user-visible AE error
        // for what is a normal, expected configuration.
        ER_GPU_LOGF("  -> declined (framework unsupported in this build)\n");
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

    ER_GPUData *gpu_data = nullptr;
    if (!err) {
        gpu_data = new (std::nothrow) ER_GPUData;
        if (!gpu_data) err = PF_Err_OUT_OF_MEMORY;
    }

    if (!err) {
        gpu_data->framework   = extra->input->what_gpu;
        gpu_data->deviceIndex = extra->input->device_index;
        if (extra->input->what_gpu == PF_GPU_Framework_DIRECTX) {
            err = ER_SetupDX12(device_info, gpu_data);
        } else { // PF_GPU_Framework_CUDA
            if (!ER_SetupCUDA(device_info, gpu_data)) {
                // Quiet decline (see ER_SetupCUDA) — CPU fallback.
                delete gpu_data;
                AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
                                  kPFGPUDeviceSuiteVersion1, "");
                return PF_Err_NONE;
            }
        }
    }

    if (!err) {
        extra->output->gpu_data = gpu_data;
        // Re-affirm GPU support for THIS device. Adobe's AE GPU sample
        // (Examples/Effect/SDK_Invert_ProcAmp) does exactly this — a plain
        // assignment, not an OR — in every successful backend branch (CUDA,
        // OpenCL, DirectX, Metal). Without it AE completes GPU_DEVICE_SETUP
        // and then offers what_gpu=NONE at every PreRender forever, which is
        // precisely the symptom we chased for seven failed experiments.
        out_data->out_flags2 = PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;
        ER_GPU_LOGF("  -> OK (%s backend ready)\n",
                    gpu_data->framework == PF_GPU_Framework_CUDA
                        ? "CUDA" : "DX12");
    } else {
        ER_GPU_LOGF("  -> err=%d (GetDeviceInfo/alloc step failed above if not logged)\n",
                    (int)err);
        delete gpu_data;    // safe on nullptr; ComPtrs release cleanly
    }

    AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
                      kPFGPUDeviceSuiteVersion1, "");
    return err;
}

PF_Err ER_GPUDeviceSetdown(PF_InData *in_data, PF_OutData *out_data,
                           PF_GPUDeviceSetdownExtra *extra)
{
    ER_GPU_LOGF("[EdgeRepair] DEVICE_SETDOWN what_gpu=%d gpu_data=%p\n",
                (int)extra->input->what_gpu, extra->input->gpu_data);

    ER_GPUData *gpu_data = static_cast<ER_GPUData*>(extra->input->gpu_data);

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
        const ER_CudaApi &cu = ER_CudaApi::Get();
        if (cu.loaded) {
            ER_CudaCtxGuard ctx(cu, gpu_data->cuCtx);
            cu.ModuleUnload(gpu_data->cuModule);
        }
        gpu_data->cuModule = nullptr;
    }
    delete gpu_data;
    return PF_Err_NONE;
}

// Diagnostic hook for PreRender (EdgeRepair.cpp) — records what AE offered
// per frame and whether we raised GPU_RENDER_POSSIBLE. First few frames only.
void ER_GPULogSmartRenderCPU(int srcW, int srcH, int outW, int outH,
                             int srcOX, int srcOY)
{
    // The counterpart to the SMART_RENDER GPU line. Without it a frame that
    // falls back to the CPU is INVISIBLE — you can only infer it from a
    // missing GPU line, which the per-site caps make unreliable.
    ER_GPU_LOG_FIRST(s_cpu, 6,
        "[EdgeRepair] SMART_RENDER CPU src=%dx%d out=%dx%d off=(%d,%d)\n",
        srcW, srcH, outW, outH, srcOX, srcOY);
}

void ER_GPULogPreRenderDecision(PF_GPU_Framework what_gpu,
                                const void *gpu_data, bool raised,
                                int bitdepth, int dsx_num, int dsx_den)
{
#if ER_GPU_LOG
    // Separate first-N budgets for NONE vs GPU offers — a single shared cap
    // hid the GPU-mode frames behind earlier CPU-mode ones (2026-07-23).
    // bitdepth/downsample expose the render context so AE's CPU/GPU choice
    // can be correlated with project depth and preview resolution.
    if (what_gpu == PF_GPU_Framework_NONE) {
        ER_GPU_LOG_FIRST(s_preNone, 12,
            "[EdgeRepair] PRERENDER what_gpu=0 bitdepth=%d ds=%d/%d raised=%s\n",
            bitdepth, dsx_num, dsx_den, raised ? "YES" : "no");
    } else {
        ER_GPU_LOG_FIRST(s_preGpu, 12,
            "[EdgeRepair] PRERENDER what_gpu=%d gpu_data=%p bitdepth=%d ds=%d/%d -> GPU_RENDER_POSSIBLE=%s\n",
            (int)what_gpu, gpu_data, bitdepth, dsx_num, dsx_den,
            raised ? "YES" : "no");
    }
#else
    (void)what_gpu; (void)gpu_data; (void)raised;
    (void)bitdepth; (void)dsx_num; (void)dsx_den;
#endif
}

// Declare the GPU pixel formats we accept, so the MediaCore side will hand
// frames to our GPUVideoFilter module. The GPU_* labels are defined in the
// Premiere GPU headers (sdk/Premiere/Headers/PrSDKGPUDeviceSuite.h), which
// must NOT be included in this AE-SDK TU — but they are built from
// MAKE_ADOBE_PRIVATE_PIXEL_FORMAT_FOURCC, which our AE SDK's own
// PrSDKPixelFormat.h already provides, so use Adobe's macro (never hand-roll
// the FourCC: the private prefix is '@' and the byte order is reversed).
#define ER_PrPixelFormat_GPU_BGRA_4444_16f \
    MAKE_ADOBE_PRIVATE_PIXEL_FORMAT_FOURCC('C', 'D', 'a')
#define ER_PrPixelFormat_GPU_BGRA_4444_32f \
    MAKE_ADOBE_PRIVATE_PIXEL_FORMAT_FOURCC('C', 'D', 'A')

void ER_DeclareGPUPixelFormats(PF_InData *in_data, PF_OutData *out_data)
{
    // NOTE: two *different* suites share the name "PF Pixel Format Suite",
    // told apart only by version:
    //   v1 (PrSDKAESupport.h)    AddSupportedPixelFormat(PrPixelFormat)   <- GPU formats
    //   v2 (AE_EffectCBSuites.h) AddSupportedPixelFormat(PF_PixelFormat)  <- CPU formats only
    // Only v1 can express PrPixelFormat_GPU_*, so v2 is no substitute.
    //
    // Acquire through SPBasicSuite directly, NOT via AEFX_AcquireSuite. On
    // failure that helper sets PF_OutFlag_DISPLAY_ERROR_MESSAGE (0x100) on
    // out_data->out_flags and writes out_data->return_msg. Called from
    // GLOBAL_SETUP, that silently corrupts our reported global outflags and AE
    // rejects the plugin at load with "global outflags mismatch ... code flags
    // are 2000500 and PiPL flags are 2000400" (25::16) — 0x500 vs 0x400 being
    // exactly that bit. Declaring GPU formats is optional; failing to must
    // stay completely silent.
    const A_long out_flags_on_entry  = out_data->out_flags;
    const A_long out_flags2_on_entry = out_data->out_flags2;

    SPBasicSuite *bsuite = in_data->pica_basicP;
    if (!bsuite) {
        ER_GPULogEventF("No PICA basic suite in GLOBAL_SETUP "
                        "(pica_basicP=null) - cannot declare GPU formats");
        out_data->out_flags  = out_flags_on_entry;
        out_data->out_flags2 = out_flags2_on_entry;
        return;
    }

    PF_PixelFormatSuite1 *pfs = nullptr;
    const SPErr sp_err = bsuite->AcquireSuite(
        (char*)kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1,
        (const void**)&pfs);
    if (sp_err || !pfs) {
        // Probe v2 only to tell "this host vends no such suite at all" from
        // "it vends the CPU-only version". v2 cannot express GPU formats, so
        // it is a diagnostic, never a fallback.
        PF_PixelFormatSuite2 *pfs2 = nullptr;
        const SPErr sp_err2 = bsuite->AcquireSuite(
            (char*)kPFPixelFormatSuite, kPFPixelFormatSuiteVersion2,
            (const void**)&pfs2);
        ER_GPULogEventF("PixelFormatSuite v1 sp_err=%d ptr=%p; "
                        "v2 sp_err=%d ptr=%p",
                        (int)sp_err, (void*)pfs, (int)sp_err2, (void*)pfs2);
        if (!sp_err2 && pfs2) {
            bsuite->ReleaseSuite((char*)kPFPixelFormatSuite,
                                 kPFPixelFormatSuiteVersion2);
        }
        out_data->out_flags  = out_flags_on_entry;
        out_data->out_flags2 = out_flags2_on_entry;
        return;  // CPU path is unaffected either way.
    }

    // Highest preference first. 32f is our native precision (the CPU pipeline
    // is float throughout); 16f is accepted so the host can pick the cheaper
    // format when the timeline is 16-bit — the kernel port must handle both.
    const PF_Err e_clear = pfs->ClearSupportedPixelFormats(in_data->effect_ref);
    const PF_Err e_32f   = pfs->AddSupportedPixelFormat(
                               in_data->effect_ref,
                               ER_PrPixelFormat_GPU_BGRA_4444_32f);
    const PF_Err e_16f   = pfs->AddSupportedPixelFormat(
                               in_data->effect_ref,
                               ER_PrPixelFormat_GPU_BGRA_4444_16f);
    // Log the per-call results: the host can vend the suite and still reject
    // an individual format, which would otherwise look identical to success.
    ER_GPULogEventF("Declared GPU pixel formats: clear=%d 32f=%d 16f=%d",
                    (int)e_clear, (int)e_32f, (int)e_16f);

    bsuite->ReleaseSuite((char*)kPFPixelFormatSuite,
                         kPFPixelFormatSuiteVersion1);

    // Belt and braces: nothing in here may alter the flags GlobalSetup
    // reports, or we resurrect the 25::16 load failure described above.
    out_data->out_flags  = out_flags_on_entry;
    out_data->out_flags2 = out_flags2_on_entry;
}

// Tagged one-liner for rare host commands and GPUVideoFilter milestones.
// One shared budget across all tags — sized so QDF chatter (8+ at apply)
// cannot swallow the GPUVideoFilter events that follow.
void ER_GPULogEvent(const char *tag)
{
#if ER_GPU_LOG
    ER_GPU_LOG_FIRST(s_event, 40, "[EdgeRepair] EVENT %s\n", tag);
#else
    (void)tag;
#endif
}

// printf-style variant, for events whose error codes matter. Shares the same
// "log the first N" discipline so a per-frame caller can never flood the file.
void ER_GPULogEventF(const char *fmt, ...)
{
#if ER_GPU_LOG
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ER_GPU_LOG_FIRST(s_eventf, 40, "[EdgeRepair] EVENT %s\n", msg);
#else
    (void)fmt;
#endif
}

// ---------------------------------------------------------------------------
// Device scratch layout.
//
// The pipeline needs several full-frame intermediates over the INFLATED source
// (not just the output rect): the alpha plane, the pre-erode interior, the core
// matte, the driving distance field, the alpha-choke field, two jump-flood seed
// buffers, and the pull–push mip chain. They come out of ONE device allocation,
// sub-divided here, so a frame costs one (cached) allocation instead of ten.
//
// Only what THIS render actually reads is reserved — the mip chain alone is
// ~4/3 x 16 bytes per pixel, so a Core Matte view must not pay for it. Offsets
// are 256-byte aligned; every consumer treats its region as a tightly-packed
// W-wide plane (pitch == W), which is NOT the worlds' pitch.
// ---------------------------------------------------------------------------
static const int ER_MAX_MIP_LEVELS = 32;   // 2^32 px — far past any AE frame

struct ER_GPULayout {
    size_t alpha    = 0;
    size_t interior = 0;
    size_t core     = 0;
    size_t dist     = 0;
    size_t choke    = 0;
    size_t seedA    = 0;
    size_t seedB    = 0;
    size_t chain    = 0;
    size_t partials = 0;
    size_t misc     = 0;      // [0..7] = ER_FLAG_* bits, [8] = grain amplitude
    size_t total    = 0;

    bool   hasCore  = false;
    bool   hasDist  = false;
    bool   hasChoke = false;
    bool   hasChain = false;

    int    nLevels  = 0;
    A_long lvlW  [ER_MAX_MIP_LEVELS];
    A_long lvlH  [ER_MAX_MIP_LEVELS];
    A_long lvlOff[ER_MAX_MIP_LEVELS];   // FLOAT offset within the chain region
};

static size_t ER_Align256(size_t v) { return (v + 255u) & ~(size_t)255u; }

// misc slots: 8 flag words + the grain amplitude + headroom.
static const size_t ER_MISC_FLOATS = 16;

static void ER_BuildLayout(ER_GPULayout &L, A_long W, A_long H,
                           bool needCore, bool needDist, bool needChoke,
                           bool needChain, bool needPartials)
{
    const size_t planeBytes = (size_t)W * (size_t)H * sizeof(float);
    size_t off = 0;
    // Each take() rounds the END up to 256, so every start stays aligned.
    #define ER_TAKE(field, bytes) do { L.field = off; \
                                       off = ER_Align256(off + (bytes)); } while (0)

    ER_TAKE(misc, ER_MISC_FLOATS * sizeof(float));
    ER_TAKE(alpha, planeBytes);
    ER_TAKE(interior, planeBytes);

    L.hasCore = needCore;
    if (needCore) ER_TAKE(core, planeBytes);

    // The distance plane is reserved whenever ANY field is built — the erode
    // field, the core field and the Distance Field view all take turns in it
    // (each consumes its predecessor before the next overwrites it).
    L.hasDist = needDist;
    if (needDist) {
        ER_TAKE(dist, planeBytes);
        ER_TAKE(seedA, planeBytes);      // uint per pixel — same byte size
        ER_TAKE(seedB, planeBytes);
    }

    // The alpha choke needs its OWN field: at composite time `dist` is holding
    // the core field, so the two cannot share.
    L.hasChoke = needChoke;
    if (needChoke) {
        ER_TAKE(choke, planeBytes);
        if (!needDist) {                 // choke-only render still floods
            ER_TAKE(seedA, planeBytes);
            ER_TAKE(seedB, planeBytes);
        }
    }

    L.hasChain = needChain;
    L.nLevels = 0;
    if (needChain) {
        A_long w = W, h = H, floats = 0;
        for (;;) {
            if (L.nLevels >= ER_MAX_MIP_LEVELS) break;
            L.lvlW  [L.nLevels] = w;
            L.lvlH  [L.nLevels] = h;
            L.lvlOff[L.nLevels] = floats;
            floats += w * h * 4;
            ++L.nLevels;
            if (w <= 1 && h <= 1) break;
            w = (w + 1) / 2;
            h = (h + 1) / 2;
        }
        ER_TAKE(chain, (size_t)floats * sizeof(float));
    }

    if (needPartials) ER_TAKE(partials, (size_t)ER_GRAIN_BLOCKS * 2u * sizeof(float));

    #undef ER_TAKE
    L.total = off;
}

// Grow-only device scratch, cached on the per-device gpu_data. Caller holds
// renderLock. Returns false (and leaves the old block intact) on failure.
static bool ER_EnsureScratch(PF_ProgPtr effect_ref, PF_GPUDeviceSuite1 *gpuS,
                             ER_GPUData *g, size_t bytes)
{
    if (g->scratch && g->scratchBytes >= bytes) return true;

    void *mem = nullptr;
    if (gpuS->AllocateDeviceMemory(effect_ref, g->deviceIndex, bytes, &mem) ||
        !mem) {
        ER_GPU_LOG_FIRST(s_allocFail, 4,
            "[EdgeRepair] AllocateDeviceMemory(%zu bytes) FAILED\n", bytes);
        return false;
    }
    if (g->scratch)
        gpuS->FreeDeviceMemory(effect_ref, g->deviceIndex, g->scratch);
    g->scratch      = mem;
    g->scratchBytes = bytes;
    ER_GPU_LOG_FIRST(s_allocOk, 4,
        "[EdgeRepair] GPU scratch grown to %.1f MB\n",
        (double)bytes / (1024.0 * 1024.0));
    return true;
}

// ---------------------------------------------------------------------------
// CUDA pass launcher.
//
// Every launch goes on AE'S OWN stream and is NOT synchronized here — AE owns
// stream synchronization (the contract the SDK CUDA samples rely on). Because
// the stream is FIFO, serializing the ENQUEUE (renderLock) is enough to make
// the shared per-device scratch safe against concurrent MFR renders: render
// A's passes all execute before render B's.
//
// First failure wins and every later launch is skipped, so a broken pass
// cannot cascade into launching kernels against half-written buffers.
// ---------------------------------------------------------------------------
struct ER_CudaLauncher {
    const ER_CudaApi &cu;
    ER_GPUData       *g;
    CUresult          res    = ER_CUDA_SUCCESS;
    int               failed = -1;

    ER_CudaLauncher(const ER_CudaApi &api, ER_GPUData *gp) : cu(api), g(gp) {}

    void Launch(ER_KernelId k, unsigned gx, unsigned gy,
                unsigned bx, unsigned by, void **args)
    {
        if (res != ER_CUDA_SUCCESS) return;
        if (gx == 0u || gy == 0u) return;
        res = cu.LaunchKernel(g->cuFn[k], gx, gy, 1u, bx, by, 1u,
                              0u, g->cuStream, args, nullptr);
        if (res != ER_CUDA_SUCCESS) failed = (int)k;
    }

    // One thread per pixel over a w x h grid (16x16 blocks).
    void Launch2D(ER_KernelId k, A_long w, A_long h, void **args)
    {
        if (w <= 0 || h <= 0) return;
        Launch(k, (unsigned)((w + 15) / 16), (unsigned)((h + 15) / 16),
               16u, 16u, args);
    }

    // One thread per element (256-wide blocks).
    void Launch1D(ER_KernelId k, A_long n, void **args)
    {
        if (n <= 0) return;
        Launch(k, (unsigned)((n + 255) / 256), 1u, 256u, 1u, args);
    }

    // Fixed grid — the grain reduction's constant geometry.
    void LaunchFixed(ER_KernelId k, unsigned blocks, unsigned threads, void **args)
    {
        Launch(k, blocks, 1u, threads, 1u, args);
    }
};

// ---------------------------------------------------------------------------
// The GPU pipeline. Mirrors SmartRender's CPU pipeline pass for pass:
//
//   alpha plane -> interior -> [erode field] -> core matte
//                -> core distance field  (the single driving field)
//                -> pull–push fill       (the only fill engine)
//                -> grain amplitude      (measured on the trusted core)
//                -> [alpha-choke field]
//                -> composite / view
//
// Distances come from a jump flood (JFA+2) rather than the CPU's exact EDT —
// the one accepted divergence (sub-pixel), which is why the Stage 6 gate is
// "within tolerance" and not byte-identical.
// ---------------------------------------------------------------------------
static PF_Err ER_RenderCUDA(PF_InData *in_data, PF_OutData *out_data,
                            ER_GPUData *g,
                            PF_EffectWorld *src_world, PF_EffectWorld *out_world,
                            const ER_Params &prm, A_long srcOX, A_long srcOY)
{
    const ER_CudaApi &cu = ER_CudaApi::Get();
    if (!cu.loaded || !g->cuFn[ER_K_COMPOSITE])
        return PF_Err_INTERNAL_STRUCT_DAMAGED;

    const A_long W    = src_world->width;
    const A_long H    = src_world->height;
    const A_long outW = out_world->width;
    const A_long outH = out_world->height;

    // The jump flood packs a seed coordinate into 16 bits per axis. AE caps
    // comps at 30000 px, so this is unreachable in practice — but a silent
    // wraparound would corrupt every distance, so refuse loudly instead.
    if (W > 65535 || H > 65535) {
        ER_GPU_LOG_FIRST(s_tooBig, 2,
            "[EdgeRepair] source %ldx%ld exceeds the JFA seed packing limit\n",
            (long)W, (long)H);
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

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

    // ---- Decide the pipeline shape (the CPU path's host-side branches) ----
    const A_long view = prm.viewL;
    const bool resultView = (view != ER_VIEW_CORE_MATTE &&
                             view != ER_VIEW_DISTANCE_FIELD &&
                             view != ER_VIEW_EXTENDED_COLOUR);
    const bool chokeActive = (prm.chokeF >= 0.01 || prm.chokeF <= -0.01);
    const bool earlyOut    = resultView && prm.extendF <= 0.0 && !chokeActive;
    const bool doBand      = resultView && prm.extendF > 0.0;

    // Signed erode -> the core-matte branch, exactly as er::BuildCoreMatte
    // chooses it (|erode| < 0.01 is "no erode" and must stay a bit-exact copy
    // of the interior — not a shrink by ~0).
    A_long coreMode = ER_CORE_MODE_COPY;
    if      (prm.erodeF >  0.01) coreMode = ER_CORE_MODE_SHRINK;
    else if (prm.erodeF < -0.01) coreMode = ER_CORE_MODE_GROW;

    const bool needCore  = !earlyOut && (doBand ||
                                         view == ER_VIEW_CORE_MATTE ||
                                         view == ER_VIEW_EXTENDED_COLOUR);
    const bool needChain = !earlyOut && (doBand || view == ER_VIEW_EXTENDED_COLOUR);
    const bool needChoke = !earlyOut && resultView && chokeActive;
    // The distance plane (and the two flood buffers) are only reserved when a
    // field is actually built: the driving core field, the Distance Field
    // view's inward edge field, or the erode field. Erode 0 takes
    // ER_CORE_MODE_COPY and consults no distance at all, so an Extended Colour
    // render at default Erode never pays for them.
    const bool needDist  = !earlyOut && (doBand ||
                                         view == ER_VIEW_DISTANCE_FIELD ||
                                         (needCore && coreMode != ER_CORE_MODE_COPY));
    const bool needPart  = doBand;

    ER_GPULayout L;
    if (!earlyOut)
        ER_BuildLayout(L, W, H, needCore, needDist, needChoke, needChain, needPart);

    ER_GPU_LOG_FIRST(s_plan, 6,
        "[EdgeRepair] GPU plan view=%ld src=%ldx%ld out=%ldx%ld off=(%ld,%ld) "
        "erodeMode=%ld band=%d choke=%d chain=%d levels=%d scratch=%.1fMB\n",
        (long)view, (long)W, (long)H, (long)outW, (long)outH,
        (long)srcOX, (long)srcOY, (long)coreMode, doBand ? 1 : 0,
        needChoke ? 1 : 0, needChain ? 1 : 0, L.nLevels,
        (double)L.total / (1024.0 * 1024.0));

    {
        // The lock covers scratch growth AND the whole enqueue sequence — see
        // ER_CudaLauncher's note on why enqueue order is enough.
        std::lock_guard<std::mutex> lock(g->renderLock);

        // AE's CUcontext is pushed BEFORE the allocation, not just around the
        // launches: the suite's device allocator runs driver calls of its own,
        // and the suite header's discipline is that a context is current for
        // any driver-call sequence we trigger.
        ER_CudaCtxGuard ctx(cu, g->cuCtx);
        if (!ctx.pushed) {
            AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
                              kPFGPUDeviceSuiteVersion1, "");
            return PF_Err_INTERNAL_STRUCT_DAMAGED;
        }

        if (!earlyOut &&
            !ER_EnsureScratch(in_data->effect_ref, gpuS, g, L.total)) {
            AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
                              kPFGPUDeviceSuiteVersion1, "");
            return PF_Err_OUT_OF_MEMORY;
        }

        const CUdeviceptr base = reinterpret_cast<CUdeviceptr>(g->scratch);
        CUdeviceptr dSrc  = reinterpret_cast<CUdeviceptr>(src_mem);
        CUdeviceptr dDst  = reinterpret_cast<CUdeviceptr>(dst_mem);
        CUdeviceptr dMisc = base + L.misc;
        CUdeviceptr dAmp  = base + L.misc + 8u * sizeof(float);   // misc[8]
        CUdeviceptr dAlpha    = base + L.alpha;
        CUdeviceptr dInterior = base + L.interior;
        CUdeviceptr dCore     = base + L.core;
        CUdeviceptr dDist     = base + L.dist;
        CUdeviceptr dSeedA    = base + L.seedA;
        CUdeviceptr dSeedB    = base + L.seedB;
        CUdeviceptr dChain    = base + L.chain;
        CUdeviceptr dPart     = base + L.partials;
        // Buffers a given render never builds are never DEREFERENCED (the
        // kernels gate on p.bandRequested / p.chokeMode / p.mode / the device
        // flags), but they must still be a VALID address — point them at misc.
        // Belt and braces: unreserved regions already have offset 0, which is
        // misc's own offset, but say it outright so a future reordering of
        // ER_BuildLayout cannot quietly turn a skipped buffer into a wild
        // pointer.
        if (!L.hasChain) dChain = dMisc;
        if (!L.hasCore)  dCore  = dMisc;
        if (!L.hasDist) { dDist = dMisc; dSeedA = dMisc; dSeedB = dMisc; }
        CUdeviceptr dChoke = L.hasChoke ? (base + L.choke) : dMisc;
        // A choke-only render (Extend 0, Choke != 0) builds no field but still
        // floods, so it gets its own seed buffers from the layout.
        if (!L.hasDist && L.hasChoke) { dSeedA = base + L.seedA; dSeedB = base + L.seedB; }

        // The param block is re-filled per pass; cuLaunchKernel copies it at
        // call time, so reusing one instance across passes is safe.
        ER_GPU_PassParams b;
        AEFX_CLR_STRUCT(b);
        b.srcPitchF = src_world->rowbytes / (A_long)sizeof(float);
        b.dstPitchF = out_world->rowbytes / (A_long)sizeof(float);
        b.srcOX = srcOX;  b.srcOY = srcOY;
        b.outW  = outW;   b.outH  = outH;
        b.W     = W;      b.H     = H;
        b.erode    = (float)prm.erodeF;
        b.extend   = (float)prm.extendF;
        b.softness = (float)prm.softF;
        b.detail01 = (float)(prm.detailF / 100.0);
        b.choke    = (float)prm.chokeF;
        b.thresh   = 0.5f;
        b.view     = view;
        b.chokeMode = !chokeActive ? ER_CHOKE_MODE_OFF
                    : (prm.chokeF > 0.0 ? ER_CHOKE_MODE_IN : ER_CHOKE_MODE_GROW);
        b.bandRequested = doBand ? 1 : 0;
        b.grainBlocks   = ER_GRAIN_BLOCKS;

        ER_CudaLauncher lx(cu, g);
        ER_GPU_PassParams p;

        if (earlyOut) {
            // Extend <= 0 and Choke == 0: the effect does nothing at all.
            // Same bitwise passthrough the CPU path takes, no fields built.
            p = b;
            void *a[] = { &dSrc, &dDst, &p };
            lx.Launch2D(ER_K_PASSTHROUGH, outW, outH, a);
        } else {
            // ---- flags/amplitude block starts clean every frame ----
            p = b; p.count = (A_long)ER_MISC_FLOATS;
            { void *a[] = { &dMisc, &p }; lx.Launch1D(ER_K_ZERO, p.count, a); }

            // ---- alpha plane + pre-erode interior ----
            p = b;
            { void *a[] = { &dSrc, &dAlpha, &p };
              lx.Launch2D(ER_K_EXTRACT_ALPHA, W, H, a); }

            p = b; p.mode = ER_CLASSIFY_INTERIOR; p.flagSlot = ER_FLAG_INTERIOR;
            { void *a[] = { &dAlpha, &dInterior, &dMisc, &p };
              lx.Launch2D(ER_K_CLASSIFY, W, H, a); }

            // ---- jump flood helpers ----
            // Seeds either from an alpha predicate (edge/choke fields) or from
            // an existing 0/1 plane (the core matte). `outDist` receives the
            // Euclidean distance to the nearest seed.
            CUdeviceptr sIn = dSeedA, sOut = dSeedB;
            auto Flood = [&](CUdeviceptr &outDist) {
                const A_long maxDim = W > H ? W : H;
                A_long step = 1;
                while (step < maxDim) step <<= 1;
                step >>= 1;
                auto pass = [&](A_long s) {
                    p = b; p.jfaStep = s;
                    void *a[] = { &sIn, &sOut, &p };
                    lx.Launch2D(ER_K_JFA_STEP, W, H, a);
                    CUdeviceptr t = sIn; sIn = sOut; sOut = t;
                };
                for (; step >= 1; step >>= 1) pass(step);
                pass(2);   // the "+2": removes JFA's classic island artefacts
                pass(1);
                p = b;
                void *a[] = { &sIn, &outDist, &p };
                lx.Launch2D(ER_K_JFA_RESOLVE, W, H, a);
            };
            auto FloodFromAlpha = [&](A_long classifyMode, A_long slot,
                                      CUdeviceptr &outDist) {
                sIn = dSeedA; sOut = dSeedB;
                p = b; p.mode = classifyMode; p.flagSlot = slot;
                void *a[] = { &dAlpha, &sIn, &dMisc, &p };
                lx.Launch2D(ER_K_JFA_INIT_ALPHA, W, H, a);
                Flood(outDist);
            };
            auto FloodFromMask = [&](CUdeviceptr &mask, CUdeviceptr &outDist) {
                sIn = dSeedA; sOut = dSeedB;
                p = b;
                void *a[] = { &mask, &sIn, &p };
                lx.Launch2D(ER_K_JFA_INIT_MASK, W, H, a);
                Flood(outDist);
            };

            // ---- core matte (threshold + signed erode) ----
            if (needCore) {
                if (coreMode == ER_CORE_MODE_SHRINK) {
                    // Shrink: distance to the nearest EXTERIOR pixel, i.e. how
                    // deep inside the alpha edge we are.
                    FloodFromAlpha(ER_CLASSIFY_EXTERIOR, ER_FLAG_EXTERIOR, dDist);
                } else if (coreMode == ER_CORE_MODE_GROW) {
                    FloodFromAlpha(ER_CLASSIFY_INTERIOR, ER_FLAG_INTERIOR, dDist);
                }
                p = b; p.mode = coreMode;
                void *a[] = { &dInterior, &dDist, &dCore, &dMisc, &p };
                lx.Launch2D(ER_K_MAKE_CORE, W, H, a);
            }

            // ---- per-view pipeline tail ----
            if (view == ER_VIEW_CORE_MATTE) {
                p = b;
                void *a[] = { &dCore, &dDst, &p };
                lx.Launch2D(ER_K_VIEW_CORE, outW, outH, a);

            } else if (view == ER_VIEW_DISTANCE_FIELD) {
                // The inward light-wrap matte measures from the ORIGINAL alpha
                // edge (not the eroded core), so its field is seeded by the
                // exterior — and its reach is Erode, not Extend.
                FloodFromAlpha(ER_CLASSIFY_EXTERIOR, ER_FLAG_EXTERIOR, dDist);
                p = b;
                p.erode = prm.erodeF > 0.0 ? (float)prm.erodeF : 0.f;  // gap width
                void *a[] = { &dDist, &dInterior, &dMisc, &dDst, &p };
                lx.Launch2D(ER_K_VIEW_DIST, outW, outH, a);

            } else {
                // Both remaining views need the pull–push fill.
                if (needChain) {
                    p = b;
                    { void *a[] = { &dSrc, &dCore, &dChain, &dMisc, &p };
                      lx.Launch2D(ER_K_PP_INIT, W, H, a); }

                    for (int k = 1; k < L.nLevels; ++k) {      // PULL
                        p = b;
                        p.lvlOffFine   = L.lvlOff[k - 1];
                        p.lvlWFine     = L.lvlW  [k - 1];
                        p.lvlHFine     = L.lvlH  [k - 1];
                        p.lvlOffCoarse = L.lvlOff[k];
                        p.lvlWCoarse   = L.lvlW  [k];
                        p.lvlHCoarse   = L.lvlH  [k];
                        void *a[] = { &dChain, &p };
                        lx.Launch2D(ER_K_PP_PULL, p.lvlWCoarse, p.lvlHCoarse, a);
                    }
                    for (int k = L.nLevels - 2; k >= 0; --k) { // PUSH
                        p = b;
                        p.lvlOffFine   = L.lvlOff[k];
                        p.lvlWFine     = L.lvlW  [k];
                        p.lvlHFine     = L.lvlH  [k];
                        p.lvlOffCoarse = L.lvlOff[k + 1];
                        p.lvlWCoarse   = L.lvlW  [k + 1];
                        p.lvlHCoarse   = L.lvlH  [k + 1];
                        void *a[] = { &dChain, &p };
                        lx.Launch2D(ER_K_PP_PUSH, p.lvlWFine, p.lvlHFine, a);
                    }
                    p = b;
                    { void *a[] = { &dChain, &p };
                      lx.Launch2D(ER_K_PP_RESOLVE, W, H, a); }
                }

                if (view == ER_VIEW_EXTENDED_COLOUR) {
                    p = b;
                    void *a[] = { &dChain, &dDst, &p };
                    lx.Launch2D(ER_K_VIEW_EXT, outW, outH, a);

                } else {
                    // ---- Composited: driving field, grain, choke, composite ----
                    if (doBand) {
                        FloodFromMask(dCore, dDist);

                        p = b;
                        { void *a[] = { &dSrc, &dCore, &dPart, &p };
                          lx.LaunchFixed(ER_K_GRAIN_PARTIAL,
                                         ER_GRAIN_BLOCKS, ER_GRAIN_THREADS, a); }
                        p = b;
                        { void *a[] = { &dPart, &dAmp, &p };
                          lx.LaunchFixed(ER_K_GRAIN_FINAL, 1u, 1u, a); }
                    }
                    if (needChoke) {
                        // Anchored at the ALPHA'S OWN edge (Erode-independent):
                        // choke-in measures to the nearest zero-alpha pixel,
                        // grow to the nearest solid one.
                        FloodFromAlpha(prm.chokeF > 0.0 ? ER_CLASSIFY_CHOKE_IN
                                                        : ER_CLASSIFY_CHOKE_GROW,
                                       ER_FLAG_CHOKE, dChoke);
                    }
                    p = b;
                    // p.erode carries the erode GAP width on this pass: the
                    // full-weight plateau between the core boundary and the
                    // original alpha edge. Same 0.01 deadband BuildCoreMatte /
                    // coreMode use, so the plateau and the real core boundary
                    // agree. Extend then rolls off from the ALPHA EDGE, which
                    // makes the outward reach independent of Erode.
                    p.erode = (prm.erodeF > 0.01) ? (float)prm.erodeF : 0.f;
                    void *a[] = { &dSrc, &dDist, &dChain, &dChoke,
                                  &dMisc, &dAmp, &dDst, &p };
                    lx.Launch2D(ER_K_COMPOSITE, outW, outH, a);
                }
            }
        }

        if (lx.res != ER_CUDA_SUCCESS) {
            ER_GPU_LOG_FIRST(s_launchFail, 6,
                "[EdgeRepair] cuLaunchKernel(%s) FAILED (CUresult=%d)\n",
                (lx.failed >= 0 && lx.failed < ER_K_COUNT)
                    ? kER_KernelNames[lx.failed] : "?",
                (int)lx.res);
            err = PF_Err_INTERNAL_STRUCT_DAMAGED;
        }
    }

    AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
                      kPFGPUDeviceSuiteVersion1, "");
    return err;
}

// ---------------------------------------------------------------------------
// DirectX 12 backend — still the plumbing-milestone PASSTHROUGH.
//
// Deliberately not ported alongside CUDA: nothing on the test box exercises it.
// AE-on-Windows offers effects CUDA only (measured), and the Premiere
// GPUVideoFilter module that would drive DX12 is currently built OFF by default
// (see MEB_ENABLE_PREMIERE_GPU in CMakeLists.txt). Porting it blind would mean
// ~400 lines of multi-pass D3D12 descriptor plumbing that no gate can check.
// The kernel MATH is already shared and the HLSL wrappers mirror the CUDA
// entries one-to-one, so finishing this is orchestration work only — bind each
// pass's buffers as UAVs in declaration order and issue the same pass sequence
// ER_RenderCUDA does.
// ---------------------------------------------------------------------------
static PF_Err ER_RenderDX12(PF_InData *in_data, PF_OutData *out_data,
                            ER_GPUData *gpu_data,
                            PF_EffectWorld *src_world, PF_EffectWorld *out_world,
                            const ER_GPU_PassParams &p)
{
    if (!gpu_data->dxContext || !gpu_data->psPassthrough)
        return PF_Err_INTERNAL_STRUCT_DAMAGED;

    PF_GPUDeviceSuite1 *gpuS = nullptr;
    PF_Err err = AEFX_AcquireSuite(in_data, out_data, kPFGPUDeviceSuite,
                                   kPFGPUDeviceSuiteVersion1, "",
                                   reinterpret_cast<void**>(&gpuS));
    if (err || !gpuS) return err ? err : PF_Err_BAD_CALLBACK_PARAM;

    // GPU world data are ID3D12Resource* for the DIRECTX framework; sizes via
    // the suite (authoritative — includes any pitch padding).
    void *src_mem = nullptr, *dst_mem = nullptr;
    size_t src_bytes = 0, dst_bytes = 0;
    err = gpuS->GetGPUWorldData(in_data->effect_ref, src_world, &src_mem);
    if (!err) err = gpuS->GetGPUWorldSize(in_data->effect_ref, src_world, &src_bytes);
    if (!err) err = gpuS->GetGPUWorldData(in_data->effect_ref, out_world, &dst_mem);
    if (!err) err = gpuS->GetGPUWorldSize(in_data->effect_ref, out_world, &dst_bytes);
    if (!err && (!src_mem || !dst_mem)) err = PF_Err_BAD_CALLBACK_PARAM;

    if (!err) {
        std::lock_guard<std::mutex> lock(gpu_data->renderLock);

        // 3 descriptors: CBV + 2 UAVs. Bind order = declaration order in the
        // HLSL (src u0, then dst u1) — fixed ABI, see EdgeRepairKernels.hlsl.
        DXShaderExecution exec(gpu_data->dxContext,
                               gpu_data->psPassthrough, 3);
        const bool ok =
            exec.SetParamBuffer(const_cast<ER_GPU_PassParams*>(&p), sizeof(p)) &&
            exec.SetUnorderedAccessView(
                reinterpret_cast<ID3D12Resource*>(src_mem), (UINT)src_bytes) &&
            exec.SetUnorderedAccessView(
                reinterpret_cast<ID3D12Resource*>(dst_mem), (UINT)dst_bytes) &&
            exec.Execute((UINT)((p.outW + 15) / 16),
                         (UINT)((p.outH + 15) / 16));
        if (!ok) err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
                      kPFGPUDeviceSuiteVersion1, "");
    return err;
}

PF_Err ER_SmartRenderGPU(PF_InData *in_data, PF_OutData *out_data,
                         PF_SmartRenderExtra *extra,
                         PF_EffectWorld *src_world, PF_EffectWorld *out_world,
                         const ER_Params &prm, A_long srcOX, A_long srcOY)
{
    ER_GPU_LOG_FIRST(s_gpuRenderCount, 8,
        "[EdgeRepair] SMART_RENDER GPU what_gpu=%d out=%ldx%ld src=%ldx%ld off=(%ld,%ld)\n",
        (int)extra->input->what_gpu,
        (long)out_world->width, (long)out_world->height,
        (long)src_world->width, (long)src_world->height,
        (long)srcOX, (long)srcOY);

    ER_GPUData *gpu_data =
        static_cast<ER_GPUData*>(const_cast<void*>(extra->input->gpu_data));
    if (!gpu_data || gpu_data->framework != extra->input->what_gpu)
        return PF_Err_INTERNAL_STRUCT_DAMAGED;

    switch (gpu_data->framework) {
        case PF_GPU_Framework_CUDA:
            return ER_RenderCUDA(in_data, out_data, gpu_data,
                                 src_world, out_world, prm, srcOX, srcOY);

        case PF_GPU_Framework_DIRECTX: {
            // Passthrough only — see ER_RenderDX12's header note.
            ER_GPU_PassParams p;
            AEFX_CLR_STRUCT(p);
            p.srcPitchF = src_world->rowbytes / (A_long)sizeof(float);
            p.dstPitchF = out_world->rowbytes / (A_long)sizeof(float);
            p.srcOX = srcOX;  p.srcOY = srcOY;
            p.outW  = out_world->width;
            p.outH  = out_world->height;
            p.W     = src_world->width;
            p.H     = src_world->height;
            return ER_RenderDX12(in_data, out_data, gpu_data,
                                 src_world, out_world, p);
        }
        default:
            return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
}

#else   /* !_WIN32 — no GPU host on this platform yet */

#include "EdgeRepair.h"
#include "EdgeRepair_GPU.h"

/*  Decline stubs. These are not placeholders for a Metal host — they are the
    correct behaviour for a build with no GPU backend, and they are the same
    behaviour the Windows host gives an unsupported framework (OpenCL): accept
    the setup call, hand back no device data, and let the CPU pipeline have
    every frame. AE asks no further questions.  */

PF_Err ER_GPUDeviceSetup(PF_InData *, PF_OutData *, PF_GPUDeviceSetupExtra *extra)
{
    extra->output->gpu_data = nullptr;
    return PF_Err_NONE;
}

PF_Err ER_GPUDeviceSetdown(PF_InData *, PF_OutData *, PF_GPUDeviceSetdownExtra *)
{
    return PF_Err_NONE;
}

/* Premiere-only in the first place (guarded by appl_id == 'PrMr' at the call
   site), and Premiere on Mac would need the Metal host anyway. */
void ER_DeclareGPUPixelFormats(PF_InData *, PF_OutData *) {}

bool ER_GPUFrameworkSupported(PF_GPU_Framework) { return false; }

void ER_GPULogSmartRenderCPU(int, int, int, int, int, int) {}
void ER_GPULogPreRenderDecision(PF_GPU_Framework, const void *, bool,
                                int, int, int) {}
void ER_GPULogEvent(const char *) {}
void ER_GPULogEventF(const char *, ...) {}

/* Unreachable: ER_GPUFrameworkSupported is false, so PreRender never raises
   GPU_RENDER_POSSIBLE and AE never sends PF_Cmd_SMART_RENDER_GPU. Fail loudly
   rather than silently returning a black frame if that ever stops holding. */
PF_Err ER_SmartRenderGPU(PF_InData *, PF_OutData *, PF_SmartRenderExtra *,
                         PF_EffectWorld *, PF_EffectWorld *,
                         const ER_Params &, A_long, A_long)
{
    return PF_Err_INTERNAL_STRUCT_DAMAGED;
}

#endif  /* _WIN32 */
