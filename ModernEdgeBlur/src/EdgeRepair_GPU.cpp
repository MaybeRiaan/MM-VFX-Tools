/*  EdgeRepair_GPU.cpp — Stage 6 GPU path implementation (CUDA + DirectX 12).

    See EdgeRepair_GPU.h for the backend policy. MEASURED 2026-07-23 (see the
    diagnostics below + docs/EdgeRepair.md): After Effects 2026 on Windows/
    NVIDIA hands effects what_gpu = CUDA — it never probes DIRECTX even with
    SUPPORTS_DIRECTX_RENDERING set (that flag serves AE-SDK effects hosted in
    Premiere Pro, whose Windows GPU renderer is DX12). So CUDA is the backend
    AE actually exercises on the test box; the DX12 backend remains for
    Premiere hosts and is kept fully working.

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

#define NOMINMAX
#include "EdgeRepair.h"          // SDK includes, ER_* enums
#include "EdgeRepair_GPU.h"

#include "AE_EffectGPUSuites.h"
#include "AEFX_SuiteHelper.h"
#include "PrSDKAESupport.h"      // PF_PixelFormatSuite (MediaCore-side formats)
#include "PrSDKPixelFormat.h"    // PrPixelFormat_* labels

#include "DirectXUtils.h"        // DXContext / DXShaderExecution / GetShaderPath
#include "gpu/ER_CudaShim.h"     // runtime nvcuda.dll driver-API surface

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

// TEMPORARY visual proof that the GPU path executed. The Stage 6 milestone
// kernel is a passthrough, so GPU output is bit-identical to CPU output and no
// amount of looking at the frame can tell you whether the GPU ran. With this
// on, the top eighth of the frame is zeroed on the GPU path only. Set to 0 (or
// delete both this and the block in ER_RenderCUDA) once real kernels land.
#define ER_GPU_VISIBLE_MARKER 1

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
#define ER_GPU_LOG_FIRST(counter, limit, ...) \
    do { static std::atomic<int> counter(0); \
         if (counter.fetch_add(1) < (limit)) ER_GPULog(__VA_ARGS__); } while (0)
#define ER_GPU_LOGF(...) ER_GPULog(__VA_ARGS__)
#else
#define ER_GPU_LOG_FIRST(counter, limit, ...) ((void)0)
#define ER_GPU_LOGF(...) ((void)0)
#endif

// ---------------------------------------------------------------------------
// Per-device GPU state, owned by AE via extra->output->gpu_data.
// ---------------------------------------------------------------------------
struct ER_GPUData {
    PF_GPU_Framework framework = PF_GPU_Framework_NONE;

    // DIRECTX backend
    DXContextPtr     dxContext;        // wraps AE's device + queue
    ShaderObjectPtr  psPassthrough;    // EdgeRepair_Passthrough.cso/.rs
    std::mutex       renderLock;       // serialize command-list use per device

    // CUDA backend (driver API via ER_CudaShim; objects immutable post-setup)
    CUcontext        cuCtx        = nullptr;   // AE's context (not owned)
    CUstream         cuStream     = nullptr;   // AE's stream (not owned)
    CUmodule         cuModule     = nullptr;   // loaded PTX (owned)
    CUfunction       cuPassthrough= nullptr;   // ER_Passthrough entry
};

// Kernel launch params — must match cbuffer ERPassParams in
// EdgeRepairKernels.hlsl and ER_PassParamsCu in EdgeRepairKernels.cu (and the
// .metal twin) field for field. 32 bytes; DXShaderExecution::SetParamBuffer
// requires < 256.
struct ER_GPU_PassParams {
    A_long srcPitchF;   // source row pitch in FLOATS (rowbytes / 4)
    A_long dstPitchF;   // dest   row pitch in FLOATS
    A_long srcOX;       // output (0,0) sits at source (srcOX, srcOY)
    A_long srcOY;
    A_long outW;
    A_long outH;
    A_long pad0;
    A_long pad1;
};

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

    res = cu.ModuleGetFunction(&gpu_data->cuPassthrough, gpu_data->cuModule,
                               "ER_Passthrough");
    if (res != ER_CUDA_SUCCESS || !gpu_data->cuPassthrough) {
        ER_GPU_LOGF("  -> cuModuleGetFunction(ER_Passthrough) FAILED "
                    "(CUresult=%d) — CUDA declined\n", (int)res);
        cu.ModuleUnload(gpu_data->cuModule);
        gpu_data->cuModule = nullptr;
        return false;
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
        gpu_data->framework = extra->input->what_gpu;
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
    (void)in_data; (void)out_data;
    ER_GPU_LOGF("[EdgeRepair] DEVICE_SETDOWN what_gpu=%d gpu_data=%p\n",
                (int)extra->input->what_gpu, extra->input->gpu_data);

    ER_GPUData *gpu_data = static_cast<ER_GPUData*>(extra->input->gpu_data);
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
// GPU SmartRender — passthrough milestone, per-backend dispatch
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

static PF_Err ER_RenderCUDA(PF_InData *in_data, PF_OutData *out_data,
                            ER_GPUData *gpu_data,
                            PF_EffectWorld *src_world, PF_EffectWorld *out_world,
                            const ER_GPU_PassParams &p)
{
    const ER_CudaApi &cu = ER_CudaApi::Get();
    if (!cu.loaded || !gpu_data->cuPassthrough)
        return PF_Err_INTERNAL_STRUCT_DAMAGED;

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

    if (!err) {
        ER_CudaCtxGuard ctx(cu, gpu_data->cuCtx);
        if (!ctx.pushed) {
            err = PF_Err_INTERNAL_STRUCT_DAMAGED;
        } else {
            // Matches EdgeRepairKernels.cu: (const float* src, float* dst,
            // ER_PassParamsCu p). Struct passed by value through the kernel
            // param block. Launch on AE'S stream; AE owns synchronization —
            // do NOT sync here (the stream also serializes concurrent MFR
            // launches, which is why no lock is needed on this path).
            CUdeviceptr srcp = reinterpret_cast<CUdeviceptr>(src_mem);
            CUdeviceptr dstp = reinterpret_cast<CUdeviceptr>(dst_mem);
            void *args[3] = { &srcp, &dstp,
                              const_cast<ER_GPU_PassParams*>(&p) };
            const CUresult res = cu.LaunchKernel(
                gpu_data->cuPassthrough,
                (unsigned)((p.outW + 15) / 16),
                (unsigned)((p.outH + 15) / 16), 1u,
                16u, 16u, 1u,
                0u, gpu_data->cuStream, args, nullptr);
            if (res != ER_CUDA_SUCCESS) {
                ER_GPU_LOG_FIRST(s_launchFail, 4,
                    "[EdgeRepair] cuLaunchKernel FAILED (CUresult=%d)\n",
                    (int)res);
                err = PF_Err_INTERNAL_STRUCT_DAMAGED;
            }
#if ER_GPU_VISIBLE_MARKER
            // TEMPORARY — delete when the real kernels land.
            // The milestone kernel is a passthrough, so its output is
            // bit-identical to the CPU path and the Mercury CUDA-vs-Software
            // A/B cannot see it (this is exactly why several earlier "same
            // result" tests were uninformative). Zero the top eighth of the
            // OUTPUT world so "the GPU path ran" is unmistakable on screen.
            // Also proves device-pointer + pitch addressing is correct, which
            // the kernel port depends on.
            if (res == ER_CUDA_SUCCESS && cu.MemsetD8Async) {
                const size_t rowBytes = (size_t)p.dstPitchF * sizeof(float);
                const size_t band     = (size_t)(p.outH / 8);
                if (band > 0) {
                    cu.MemsetD8Async(dstp, 0, rowBytes * band,
                                     gpu_data->cuStream);
                }
            }
#endif
        }
    }

    AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
                      kPFGPUDeviceSuiteVersion1, "");
    return err;
}

PF_Err ER_SmartRenderGPU(PF_InData *in_data, PF_OutData *out_data,
                         PF_SmartRenderExtra *extra,
                         PF_EffectWorld *src_world, PF_EffectWorld *out_world,
                         A_long srcOX, A_long srcOY)
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

    // GPU worlds are BGRA128: 16 bytes/px, rowbytes always float-aligned.
    ER_GPU_PassParams p;
    p.srcPitchF = src_world->rowbytes / (A_long)sizeof(float);
    p.dstPitchF = out_world->rowbytes / (A_long)sizeof(float);
    p.srcOX = srcOX;
    p.srcOY = srcOY;
    p.outW  = out_world->width;
    p.outH  = out_world->height;
    p.pad0  = 0;
    p.pad1  = 0;

    switch (gpu_data->framework) {
        case PF_GPU_Framework_CUDA:
            return ER_RenderCUDA(in_data, out_data, gpu_data,
                                 src_world, out_world, p);
        case PF_GPU_Framework_DIRECTX:
            return ER_RenderDX12(in_data, out_data, gpu_data,
                                 src_world, out_world, p);
        default:
            return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
}
