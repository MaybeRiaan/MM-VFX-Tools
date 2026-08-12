/*  mm_gpu_cuda.cpp — CUDA host for the MM FreqSep GPU render.

    THE BACKEND AFTER EFFECTS ACTUALLY USES. AE 2026 on Windows/NVIDIA hands
    effects what_gpu = CUDA and never probes DIRECTX, even with
    SUPPORTS_DIRECTX_RENDERING declared (measured 2026-07-23 —
    ../../../docs/AE_GPU_Effects.md §1.1). mm_gpu_dx.cpp remains correct and
    maintained; it serves Premiere hosts, where DX12 IS the Windows renderer.

    Executes the same FS_GPUJob smart_render.cpp assembles, through the same
    band plan from mm_gpu_host.h, dispatching the same platform-free .kernel
    bodies — compiled here by nvcc into CUDA_Assets/MM_FreqSep_Kernels.ptx
    instead of by dxc into embedded DXIL. All tiling arithmetic stays in
    mm_gpu_host.h; this file is exclusively CUDA plumbing, and its pass sequence
    is a line-for-line translation of FS_DXRender's.

    CONTRACT WITH AE (../../../docs/AE_GPU_Effects.md §3):
      - PF_GPUDeviceInfo.contextPV is a CUcontext, command_queuePV a CUstream;
        both are AE's and neither is owned here.
      - PF_GPUDeviceSuite1::GetGPUWorldData returns the CUdeviceptr directly.
      - Worlds are PF_PixelFormat_GPU_BGRA128 (float BGRA, pitched rows) — NOT
        the ARGB the CPU path uses. The swizzle lives in fs_tile.kernel, shared
        with DX, not in this host.
      - Device memory comes from the suite's allocator ONLY, never cuMemAlloc.
      - Launch on AE's stream and do not synchronize; AE owns synchronization.

    ── TWO DELIBERATE DIVERGENCES FROM mm_gpu_dx.cpp ─────────────────────────
    1. SCRATCH IS CACHED PER DEVICE, not allocated per render. The DX host
       allocates and frees inside FS_DXRender, which is MFR-safe with zero
       shared state but costs a fresh allocation every frame — and FreqSep's
       scratch reaches ~1GB at 6K with radius 90, which is exactly the case this
       whole backend exists to make fast. This follows EdgeRepair instead
       (ER_EnsureScratch): one grow-only block per device, freed at
       GPU_DEVICE_SETDOWN, so a steady-state render allocates nothing.

       That makes the block shared between concurrent MFR renders, so
       renderLock covers the whole ENQUEUE sequence. Because AE's per-device
       stream is FIFO, serializing enqueue serializes execution: render A's
       passes all run before render B's touch the buffers (§4, "Multi-pass
       scratch"). Note the DX host could not use this trick as freely — it also
       had a command list to serialize — whereas here the lock is the only
       synchronization in the file and there is no stream sync at all.

    2. NO DOUBLES DEVICE GATE. The DX host refuses devices without
       DoublePrecisionFloatShaderOps because fs_guided.kernel accumulates in
       double (rules §3.7). Every CUDA-capable NVIDIA GPU supports fp64, so
       there is nothing to refuse — but see the rate warning in
       kernels/fs_prologue_cuda.h: supported is not the same as fast.
*/

#ifdef _WIN32

#define NOMINMAX
#include <Windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectPixelFormat.h"   // must precede AE_EffectGPUSuites.h (PF_PixelFormat)
#include "AE_EffectGPUSuites.h"
#include "AE_Macros.h"
#include "AEFX_SuiteHelper.h"

#include "mm_gpu_host.h"
#include "fs_cuda_shim.h"
#include "cuda/fs_kernel_args.h"   // the launch structs, compiled FROM the kernels

namespace fsgpu {

// ---------------------------------------------------------------------------
// Diagnostics. The GPU failure mode is SILENT — a declined device renders on
// the CPU, correctly, with no error anywhere (../../../docs/AE_GPU_Effects.md
// §5). These logs are the only way to tell "GPU rendering" from "looks right".
// Mirrors EdgeRepair's ER_GPU_LOG; same first-N-per-site budget so a long
// render cannot fill the disk.
// ---------------------------------------------------------------------------
#define FS_GPU_LOG 1

// Visible destructive marker (§5.2). A PASSTHROUGH milestone is worthless —
// output is bit-identical whether or not the GPU ran, and the suite wasted
// several test rounds proving exactly that, twice. This zeroes the top eighth
// of every output world: it must read TRANSPARENT (zeroing BGRA zeroes alpha)
// and must vanish under Mercury -> Software Only. Set to 1 only while proving
// the path is live, then back to 0.
#define FS_GPU_VISIBLE_MARKER 0

#if FS_GPU_LOG
#include <atomic>
static void FS_GPULog(const char *fmt, ...)
{
	char path[MAX_PATH];
	const DWORD n = ::GetTempPathA(MAX_PATH, path);
	if (n == 0 || n > MAX_PATH) return;
	std::string file(path);
	file += "MM_FreqSep_gpu.log";

	FILE *f = nullptr;
	if (fopen_s(&f, file.c_str(), "a") != 0 || !f) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fclose(f);
}
// The per-site budget keeps a long render from filling the disk, but the
// counters are PROCESS statics: a few seconds of scrubbing at default params
// exhausts them, after which the log is silent and an A/B sweep leaves no
// trace at all. Set MM_GPU_LOG_ALL=1 to lift every cap across the whole suite
// for a validation session (../../../docs/gpu-validation-checklist.md §3).
static bool FS_GPULogAll()
{
	static const bool all = [] {
		char v[8];
		const DWORD n = ::GetEnvironmentVariableA("MM_GPU_LOG_ALL", v, (DWORD)sizeof(v));
		return n > 0 && n < (DWORD)sizeof(v) && v[0] != '0';
	}();
	return all;
}
#define FS_GPU_LOG_FIRST(counter, limit, ...)                   \
	do { static std::atomic<int> counter{0};                    \
	     if (counter.fetch_add(1) < (limit) || FS_GPULogAll())  \
	         FS_GPULog(__VA_ARGS__); } while (0)
#define FS_GPU_LOGF(...) FS_GPULog(__VA_ARGS__)
#else
#define FS_GPU_LOG_FIRST(counter, limit, ...) ((void)0)
#define FS_GPU_LOGF(...) ((void)0)
#endif

// ---------------------------------------------------------------------------
// The kernel table. One entry per __global__ in cuda/fs_kernels.cu; names are
// resolved by STRING through cuModuleGetFunction, so they must match the
// KERNEL_END(name) spellings in the .kernel files EXACTLY. Order matches the
// FS_PSO_* enum in mm_gpu_dx.cpp so the two hosts read alike.
// ---------------------------------------------------------------------------
enum FS_CudaKernelId {
	FS_K_GAUSS = 0,
	FS_K_GBOX,
	FS_K_GSQUARES,
	FS_K_GAB,
	FS_K_GFINAL,
	FS_K_COMPOSE,
	FS_K_LOAD,
	FS_K_STORE,
	FS_K_FILL,
	FS_K_MATTE,
	FS_K_COUNT
};

static const char *const kFS_CudaKernelNames[FS_K_COUNT] = {
	"fs_gauss_pass",
	"fs_guided_box",
	"fs_guided_squares",
	"fs_guided_ab",
	"fs_guided_final",
	"fs_compose",
	"fs_load_tile",
	"fs_store_tile",
	"fs_fill_ones",
	"fs_matte",
};

// ---------------------------------------------------------------------------
// Per-device state, owned by AE via extra->output->gpu_data. Immutable after
// GPU_DEVICE_SETUP except for the scratch block, which renderLock guards.
// ---------------------------------------------------------------------------
struct FS_CudaData {
	CUcontext  cuCtx    = nullptr;             // AE's context (not owned)
	CUstream   cuStream = nullptr;             // AE's stream  (not owned)
	CUmodule   cuModule = nullptr;             // loaded PTX   (owned)
	CUfunction cuFn[FS_K_COUNT] = { nullptr };

	A_u_long   deviceIndex   = 0;              // needed to free scratch at setdown
	size_t     scratchBudget = 0;

	// Serializes the enqueue sequence AND the shared scratch block below.
	std::mutex renderLock;
	void      *scratch      = nullptr;
	size_t     scratchBytes = 0;
};

// ---------------------------------------------------------------------------
// Asset loading. Resolve CUDA_Assets from THIS module's own path so the PTX
// always travels next to the deployed .aex, whatever the host's CWD is.
// ---------------------------------------------------------------------------
static bool FS_CudaPtxPath(std::wstring &outPath)
{
	wchar_t path[1024];
	HMODULE hm = nullptr;
	if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                          reinterpret_cast<LPCWSTR>(&FS_CudaPtxPath), &hm))
		return false;
	if (::GetModuleFileNameW(hm, path, static_cast<DWORD>(std::size(path))) == 0)
		return false;
	std::wstring dir(path);
	const size_t slash = dir.rfind(L'\\');
	if (slash == std::wstring::npos) return false;
	outPath = dir.substr(0, slash) + L"\\CUDA_Assets\\MM_FreqSep_Kernels.ptx";
	return true;
}

// cuModuleLoadDataEx takes a NUL-terminated PTX text image.
static bool FS_ReadTextFile(const std::wstring &path, std::vector<char> &out)
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
// Scratch budget: a quarter of FREE VRAM, clamped to [256MB, 1GB] — the same
// shape as the DX host's FS_QueryScratchBudget, but measured against what is
// actually free rather than what the card has, since AE, the OS compositor and
// any other GPU effect in the stack are already holding some. The tile planner
// turns whatever this yields into a band height, so the number only moves the
// per-band overlap overhead, never correctness. Query failure -> 384MB.
// Caller must hold AE's context current.
// ---------------------------------------------------------------------------
static size_t FS_QueryCudaBudget(const FS_CudaApi &cu)
{
	const size_t kFallback = 384ull << 20;
	if (!cu.MemGetInfo) return kFallback;

	size_t freeBytes = 0, totalBytes = 0;
	if (cu.MemGetInfo(&freeBytes, &totalBytes) != FS_CUDA_SUCCESS || freeBytes == 0)
		return kFallback;

	size_t budget = freeBytes / 4;
	if (budget < (256ull << 20))  budget = 256ull << 20;
	if (budget > (1024ull << 20)) budget = 1024ull << 20;
	return budget;
}

// ---------------------------------------------------------------------------
// GPU_DEVICE_SETUP / SETDOWN
// ---------------------------------------------------------------------------
PF_Err FS_CudaDeviceSetup(PF_InData *in_data, PF_OutData *out_data,
                          PF_GPUDeviceSetupExtra *extra)
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
	extra->output->gpu_data = nullptr;

	FS_GPU_LOGF("[MM_FreqSep] DEVICE_SETUP what_gpu=%d device_index=%u\n",
		(int)extra->input->what_gpu, (unsigned)extra->input->device_index);

	if (extra->input->what_gpu != PF_GPU_Framework_CUDA) {
		// Not ours. Declining is PF_Err_NONE with gpu_data left null — AE then
		// never offers GPU render for devices of this framework and the CPU
		// reference takes the frame. An error here would surface a user-visible
		// AE failure for what is a normal configuration.
		FS_GPU_LOGF("  -> not CUDA, declined here (DX host handles DIRECTX)\n");
		return PF_Err_NONE;
	}

	const FS_CudaApi &cu = FS_CudaApi::Get();
	if (!cu.loaded) {
		FS_GPU_LOGF("  -> nvcuda.dll unavailable — CUDA declined (CPU fallback)\n");
		return PF_Err_NONE;
	}

	PF_GPUDeviceSuite1 *gpuSuite = nullptr;
	ERR(AEFX_AcquireSuite(in_data, out_data, kPFGPUDeviceSuite,
		kPFGPUDeviceSuiteVersion1, "", reinterpret_cast<void**>(&gpuSuite)));
	if (err || !gpuSuite) return err;

	{
		PF_GPUDeviceInfo info;
		AEFX_CLR_STRUCT(info);
		ERR(gpuSuite->GetDeviceInfo(in_data->effect_ref,
			extra->input->device_index, &info));
		if (err) goto done;

		FS_CudaData *data = new (std::nothrow) FS_CudaData();
		if (!data) { err = PF_Err_OUT_OF_MEMORY; goto done; }

		data->cuCtx       = reinterpret_cast<CUcontext>(info.contextPV);
		data->cuStream    = reinterpret_cast<CUstream>(info.command_queuePV);
		data->deviceIndex = extra->input->device_index;

		if (!data->cuCtx) {
			FS_GPU_LOGF("  -> AE provided no CUcontext — CUDA declined\n");
			delete data; goto done;
		}

		std::wstring ptxPath;
		std::vector<char> ptx;
		if (!FS_CudaPtxPath(ptxPath) || !FS_ReadTextFile(ptxPath, ptx)) {
			// Normal on a build machine without the CUDA toolkit: no PTX was
			// produced, so there is nothing to run. Quiet decline, CPU renders.
			FS_GPU_LOGF("  -> PTX missing/unreadable (%ls) — CUDA declined "
			            "(build with the CUDA toolkit installed to produce it)\n",
			            ptxPath.c_str());
			delete data; goto done;
		}

		{
			FS_CudaCtxGuard ctx(cu, data->cuCtx);
			if (!ctx.pushed) {
				FS_GPU_LOGF("  -> cuCtxPushCurrent FAILED — CUDA declined\n");
				delete data; goto done;
			}

			CUresult res = cu.ModuleLoadDataEx(&data->cuModule, ptx.data(),
			                                   0, nullptr, nullptr);
			if (res != FS_CUDA_SUCCESS || !data->cuModule) {
				FS_GPU_LOGF("  -> cuModuleLoadDataEx FAILED (CUresult=%d) — CUDA declined\n",
				            (int)res);
				data->cuModule = nullptr;
				delete data; goto done;
			}

			// Resolve every entry point NOW. A missing one means the deployed
			// PTX is stale against this binary — the classic half-finished
			// deploy. Decline the whole backend rather than discover it
			// mid-render, where there is no CPU fallback because the worlds are
			// device memory.
			bool allResolved = true;
			for (int k = 0; k < FS_K_COUNT; ++k) {
				res = cu.ModuleGetFunction(&data->cuFn[k], data->cuModule,
				                           kFS_CudaKernelNames[k]);
				if (res != FS_CUDA_SUCCESS || !data->cuFn[k]) {
					FS_GPU_LOGF("  -> cuModuleGetFunction(%s) FAILED (CUresult=%d) — "
					            "CUDA declined (stale CUDA_Assets PTX?)\n",
					            kFS_CudaKernelNames[k], (int)res);
					allResolved = false;
					break;
				}
			}
			if (!allResolved) {
				cu.ModuleUnload(data->cuModule);
				data->cuModule = nullptr;
				delete data; goto done;
			}

			data->scratchBudget = FS_QueryCudaBudget(cu);
		}

		extra->output->gpu_data = data;

		// Re-affirm GPU support for THIS device — a plain ASSIGNMENT, not an OR,
		// exactly as Adobe's SDK_Invert_ProcAmp does in every successful backend
		// branch. Without it AE completes GPU_DEVICE_SETUP happily and then
		// offers what_gpu = NONE at every PreRender forever, which is a perfect
		// imitation of "AE doesn't support third-party GPU effects" and is the
		// single mistake that cost the suite seven failed experiments
		// (../../../docs/AE_GPU_Effects.md §2.2 and §8).
		out_data->out_flags2 = PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;

		FS_GPU_LOGF("  -> OK (CUDA ready, %d kernels, scratch budget %.0f MB)\n",
			(int)FS_K_COUNT, (double)data->scratchBudget / (1024.0 * 1024.0));
	}

done:
	ERR2(AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
		kPFGPUDeviceSuiteVersion1, ""));
	return err;
}

PF_Err FS_CudaDeviceSetdown(PF_InData *in_data, PF_OutData *out_data,
                            PF_GPUDeviceSetdownExtra *extra)
{
	FS_CudaData *data = static_cast<FS_CudaData*>(extra->input->gpu_data);
	if (!data) return PF_Err_NONE;

	FS_GPU_LOGF("[MM_FreqSep] DEVICE_SETDOWN gpu_data=%p\n", (void*)data);

	// Hand the scratch back BEFORE unloading the module: the suite owns this
	// memory and outliving the device would leak it for the whole session.
	if (data->scratch) {
		PF_GPUDeviceSuite1 *gpuSuite = nullptr;
		if (!AEFX_AcquireSuite(in_data, out_data, kPFGPUDeviceSuite,
				kPFGPUDeviceSuiteVersion1, "",
				reinterpret_cast<void**>(&gpuSuite)) && gpuSuite) {
			gpuSuite->FreeDeviceMemory(in_data->effect_ref, data->deviceIndex,
			                           data->scratch);
			AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
			                  kPFGPUDeviceSuiteVersion1, "");
		}
		data->scratch      = nullptr;
		data->scratchBytes = 0;
	}

	if (data->cuModule) {
		const FS_CudaApi &cu = FS_CudaApi::Get();
		if (cu.loaded) {
			FS_CudaCtxGuard ctx(cu, data->cuCtx);
			cu.ModuleUnload(data->cuModule);
		}
		data->cuModule = nullptr;
	}
	delete data;
	return PF_Err_NONE;
}

// Re-measured on every call rather than returning the setup-time reading.
//
// A device is set up when AE's frame cache is COLD, which is the one moment in
// a session that free VRAM is least representative. Cache that answer and a
// card which happened to be busy at startup keeps a floored budget for hours,
// long after the condition cleared — an entire work session banding harder (or
// declining outright) on a reading that stopped being true a moment later.
// The error is also self-perpetuating in the bad direction: a budget low enough
// to decline frames means no GPU render ever runs, so nothing would refresh it.
// cuMemGetInfo is a driver query — no sync, microseconds against a render.
// ModernEdgeBlur made this call first; MM_Qualify and this host follow it.
//
// Nothing is current at either call site — PreRender's gate and the tile plan
// both run outside the render's own context guard — so this pushes AE's context
// itself. When that fails the setup-time reading is a better fallback than a
// hardcoded constant, so it is kept for exactly that.
//
// CONSEQUENCE, deliberate: the gate and the render may now see different
// budgets for the same frame if VRAM moves between them. That is safe — the
// number only picks a band height, never affects correctness, and a render
// whose planner refuses after the gate said yes falls back to the CPU and says
// so in the log (SMART_RENDER CPU).
size_t FS_CudaScratchBudget(const void *gpu_data)
{
	const FS_CudaData *data = static_cast<const FS_CudaData*>(gpu_data);
	if (!data) return 0;

	const FS_CudaApi &cu = FS_CudaApi::Get();
	if (cu.loaded && data->cuCtx) {
		FS_CudaCtxGuard ctx(cu, data->cuCtx);
		if (ctx.pushed) return FS_QueryCudaBudget(cu);
	}
	return data->scratchBudget;   // setup-time reading, better than a constant
}

void FS_GPULogSmartRenderCPU(int srcW, int srcH, int outW, int outH,
                             int srcOX, int srcOY, int view)
{
	// The counterpart to the SMART_RENDER GPU line. Without it a frame that
	// falls back to the CPU is INVISIBLE — you can only infer it from a
	// missing GPU line, which the per-site caps make unreliable.
	FS_GPU_LOG_FIRST(s_cpu, 6,
		"[MM_FreqSep] SMART_RENDER CPU src=%dx%d out=%dx%d off=(%d,%d) view=%d\n",
		srcW, srcH, outW, outH, srcOX, srcOY, view);
}

void FS_GPULogPreRenderDecision(PF_GPU_Framework what_gpu, const void *gpu_data,
                                bool raised, int outW, int outH,
                                double scratchMB, double budgetMB)
{
	FS_GPU_LOG_FIRST(s_prerender, 24,
		"[MM_FreqSep] PRERENDER what_gpu=%d gpu_data=%p out=%dx%d "
		"need=%.1fMB budget=%.1fMB -> GPU_RENDER_POSSIBLE=%s\n",
		(int)what_gpu, gpu_data, outW, outH, scratchMB, budgetMB,
		raised ? "YES" : "no");
}

// ---------------------------------------------------------------------------
// Launcher. Every launch goes on AE'S OWN stream and is NOT synchronized here —
// AE owns stream synchronization. First failure wins and every later launch is
// skipped, so a broken pass cannot cascade into running kernels over
// half-written buffers.
//
// WHERE THE DX HOST'S UAV BARRIERS WENT: nowhere, and none are needed. This
// pipeline is one dependency chain — every pass reads what the previous pass
// wrote — and launches issued to a single CUDA stream execute in issue order
// with an implicit dependency between them. D3D12 has no such guarantee for
// UAV writes, which is why mm_gpu_dx.cpp barriers after every dispatch. Do not
// "optimize" by moving passes onto a second stream without re-deriving the
// hazards: the plane ping-pong in the guided schedule reuses buffers precisely
// because it can assume serial execution.
// ---------------------------------------------------------------------------
struct FS_CudaLauncher {
	const FS_CudaApi &cu;
	FS_CudaData      *g;
	CUresult          res    = FS_CUDA_SUCCESS;
	int               failed = -1;

	FS_CudaLauncher(const FS_CudaApi &api, FS_CudaData *gp) : cu(api), g(gp) {}

	// One thread per pixel over a w x h grid. Block size comes from the SHARED
	// prologue constants, not a number chosen here, so the guard in every kernel
	// body and the grid the host computes can never disagree. (Retuning is a
	// prologue edit — see kernels/fs_prologue_cuda.h.)
	template <typename ArgsT>
	void Launch2D(FS_CudaKernelId k, int w, int h, ArgsT &args)
	{
		if (res != FS_CUDA_SUCCESS || w <= 0 || h <= 0) return;
		void *params[] = { &args };
		res = cu.LaunchKernel(g->cuFn[k],
		                      (unsigned)((w + FS_TG_X - 1) / FS_TG_X),
		                      (unsigned)((h + FS_TG_Y - 1) / FS_TG_Y), 1u,
		                      (unsigned)FS_TG_X, (unsigned)FS_TG_Y, 1u,
		                      0u, g->cuStream, params, nullptr);
		if (res != FS_CUDA_SUCCESS) failed = (int)k;
	}
};

// ---------------------------------------------------------------------------
// Grow-only device scratch, cached on the per-device gpu_data. Caller holds
// renderLock AND has AE's context current. Leaves the old block intact on
// failure. Mirrors EdgeRepair's ER_EnsureScratch.
// ---------------------------------------------------------------------------
static bool FS_EnsureScratch(PF_ProgPtr effect_ref, PF_GPUDeviceSuite1 *gpuSuite,
                             FS_CudaData *g, size_t bytes)
{
	if (g->scratch && g->scratchBytes >= bytes) return true;

	void *mem = nullptr;
	if (gpuSuite->AllocateDeviceMemory(effect_ref, g->deviceIndex, bytes, &mem) ||
	    !mem) {
		FS_GPU_LOG_FIRST(s_allocFail, 4,
			"[MM_FreqSep] AllocateDeviceMemory(%zu bytes) FAILED\n", bytes);
		return false;
	}
	if (g->scratch)
		gpuSuite->FreeDeviceMemory(effect_ref, g->deviceIndex, g->scratch);
	g->scratch      = mem;
	g->scratchBytes = bytes;
	FS_GPU_LOG_FIRST(s_allocOk, 4,
		"[MM_FreqSep] GPU scratch grown to %.1f MB\n",
		(double)bytes / (1024.0 * 1024.0));
	return true;
}

static size_t FS_Align256(size_t v) { return (v + 255u) & ~(size_t)255u; }

// ---------------------------------------------------------------------------
// Render — the pass sequence of FS_DXRender, translated. Read the two side by
// side when changing either: the band loop, the guided ping-pong schedule and
// the plane roles are meant to stay identical, and any divergence between them
// is a bug in one of the two hosts, not a backend difference.
// ---------------------------------------------------------------------------
PF_Err FS_CudaRender(PF_InData *in_data, PF_OutData *out_data,
                     const void *gpu_dataPV, A_u_long device_index,
                     const FS_GPUJob &job,
                     PF_EffectWorld *srcWorld, PF_EffectWorld *outWorld,
                     PF_EffectWorld *matteWorld)
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;

	FS_CudaData *data =
		const_cast<FS_CudaData*>(static_cast<const FS_CudaData*>(gpu_dataPV));
	if (!data || job.plan.bands.empty()) return PF_Err_BAD_CALLBACK_PARAM;

	const FS_CudaApi &cu = FS_CudaApi::Get();
	if (!cu.loaded || !data->cuFn[FS_K_COMPOSE]) return PF_Err_INTERNAL_STRUCT_DAMAGED;

	PF_GPUDeviceSuite1 *gpuSuite = nullptr;
	ERR(AEFX_AcquireSuite(in_data, out_data, kPFGPUDeviceSuite,
		kPFGPUDeviceSuiteVersion1, "", reinterpret_cast<void**>(&gpuSuite)));
	if (err || !gpuSuite) return err ? err : PF_Err_BAD_CALLBACK_PARAM;

	{
		// --- World device pointers (for CUDA these ARE the CUdeviceptrs) ----
		void *srcPV = nullptr, *outPV = nullptr, *mattePV = nullptr;
		ERR(gpuSuite->GetGPUWorldData(in_data->effect_ref, srcWorld, &srcPV));
		ERR(gpuSuite->GetGPUWorldData(in_data->effect_ref, outWorld, &outPV));
		if (!err && matteWorld)
			ERR(gpuSuite->GetGPUWorldData(in_data->effect_ref, matteWorld, &mattePV));
		if (err || !srcPV || !outPV || (matteWorld && !mattePV)) {
			if (!err) err = PF_Err_BAD_CALLBACK_PARAM;
			goto cleanup;
		}

		{
			const float *dSrcWorld   = static_cast<const float*>(srcPV);
			float       *dOutWorld   = static_cast<float*>(outPV);
			const float *dMatteWorld = static_cast<const float*>(mattePV);

			// --- Weight layout (16-float aligned, as on DX) ------------------
			auto alignF = [](size_t n) { return (n + 15u) & ~(size_t)15u; };
			const size_t offSepX = 0;
			const size_t offSepY = offSepX + alignF(job.kSepX.size());
			const size_t offSmX  = offSepY + alignF(job.kSepY.size());
			const size_t offSmY  = offSmX  + alignF(job.kSmX.size());
			const size_t weightFloats = offSmY + alignF(job.kSmY.size());

			// --- One scratch block: planes | out | matte | weights -----------
			const int    nPlanes    = FS_TilePlaneCount(job.guided);
			const size_t planeBytes = FS_Align256((size_t)job.plan.tileRowsMax * job.srcW * 4 * sizeof(float));
			const size_t outBytes   = FS_Align256((size_t)job.plan.bandHMax * job.outW * 4 * sizeof(float));
			const size_t matteBytes = FS_Align256((size_t)job.plan.bandHMax * job.outW * sizeof(float));
			const size_t weightBytes= FS_Align256(weightFloats * sizeof(float));
			const size_t scratchBytes = (size_t)nPlanes * planeBytes
			                          + outBytes + matteBytes + weightBytes;

			// Tagged "SMART_RENDER GPU" so one grep answers "did AE actually
			// dispatch to the GPU?" across every plugin in the suite.
			//
			// view= is the POPUP value (1-based: 1=Result 2=Low 3=High
			// 4=Matte), not job.view, which is the 0-based kernel index.
			// The CPU line logs the same 1-based value. They have to agree:
			// an A/B is read by comparing these two lines, and while they
			// differed by one a matched pair looked like a param mismatch.
			FS_GPU_LOG_FIRST(s_plan, 6,
				"[MM_FreqSep] SMART_RENDER GPU what_gpu=CUDA "
				"src=%dx%d out=%dx%d off=(%d,%d) guided=%d "
				"sepR=(%d,%d) guidedR=(%d,%d) smoothR=(%d,%d) view=%d matte=%d "
				"bands=%d scratch=%.1fMB\n",
				job.srcW, job.srcH, job.outW, job.outH, job.srcOX, job.srcOY,
				job.guided ? 1 : 0, job.sepRx, job.sepRy, job.guidedRx, job.guidedRy,
				job.smoothRx, job.smoothRy, job.view + 1, job.matte.active ? 1 : 0,
				(int)job.plan.bands.size(),
				(double)scratchBytes / (1024.0 * 1024.0));

			// The lock covers scratch growth AND the whole enqueue sequence —
			// see the header note on why enqueue order is sufficient.
			std::lock_guard<std::mutex> lock(data->renderLock);

			// AE's context is pushed BEFORE the allocation, not just around the
			// launches: the suite's device allocator runs driver calls of its
			// own, and the discipline is that a context is current for any
			// driver-call sequence we trigger.
			FS_CudaCtxGuard ctx(cu, data->cuCtx);
			if (!ctx.pushed) { err = PF_Err_INTERNAL_STRUCT_DAMAGED; goto cleanup; }

			if (!FS_EnsureScratch(in_data->effect_ref, gpuSuite, data, scratchBytes)) {
				err = PF_Err_OUT_OF_MEMORY; goto cleanup;
			}

			char *const base = static_cast<char*>(data->scratch);
			float *plane[5] = {};
			for (int i = 0; i < nPlanes; ++i)
				plane[i] = reinterpret_cast<float*>(base + (size_t)i * planeBytes);

			float *const pA      = plane[0];
			float *const pL      = plane[nPlanes - 1];
			float *const pOut    = reinterpret_cast<float*>(base + (size_t)nPlanes * planeBytes);
			float *const pMatte  = reinterpret_cast<float*>(base + (size_t)nPlanes * planeBytes + outBytes);
			float *const pWeight = reinterpret_cast<float*>(base + (size_t)nPlanes * planeBytes + outBytes + matteBytes);

			// --- Weights: the EXACT CPU-built kernels, uploaded verbatim -----
			// Built in double by fs::BuildGaussianKernel and narrowed once
			// (rules §3.1). A GPU-side float rebuild would diverge in the last
			// bits and turn the parity check into a coin flip.
			if (weightFloats > 0) {
				std::vector<float> w(weightFloats, 0.0f);
				if (!job.kSepX.empty()) std::memcpy(w.data() + offSepX, job.kSepX.data(), job.kSepX.size() * sizeof(float));
				if (!job.kSepY.empty()) std::memcpy(w.data() + offSepY, job.kSepY.data(), job.kSepY.size() * sizeof(float));
				if (!job.kSmX.empty())  std::memcpy(w.data() + offSmX,  job.kSmX.data(),  job.kSmX.size()  * sizeof(float));
				if (!job.kSmY.empty())  std::memcpy(w.data() + offSmY,  job.kSmY.data(),  job.kSmY.size()  * sizeof(float));
				// Pageable source: the driver stages it before returning, so w
				// may die at the end of this scope. Stream-ordered, so every
				// kernel below sees the finished upload.
				if (cu.MemcpyHtoDAsync(reinterpret_cast<CUdeviceptr>(pWeight),
						w.data(), weightFloats * sizeof(float),
						data->cuStream) != FS_CUDA_SUCCESS) {
					err = PF_Err_INTERNAL_STRUCT_DAMAGED; goto cleanup;
				}
			}

			FS_CudaLauncher L(cu, data);

			// Inactive matte: fill the whole weight plane with 1.0 once, exactly
			// as fs::SampleMatte behaves with no matte layer connected.
			if (!job.matte.active) {
				ArgsFill a{};
				a.gFillDst = pMatte;
				a.gFillW   = job.outW;
				a.gFillH   = job.plan.bandHMax;
				L.Launch2D(FS_K_FILL, job.outW, job.plan.bandHMax, a);
			}

			// ---------------- Per band ----------------
			for (const FS_TileBand &band : job.plan.bands) {
				const int rows = band.srcRows;

				// Load source rows (BGRA world -> packed ARGB tile A).
				{
					ArgsLoad a{};
					a.gLdWorld  = dSrcWorld;
					a.gLdTile   = pA;
					a.gLdW      = job.srcW;
					a.gLdH      = rows;
					a.gLdWorldY0= band.srcY0;
					a.gLdPitch  = job.srcPitchFl;
					L.Launch2D(FS_K_LOAD, job.srcW, rows, a);
				}

				float *pLo = pA;   // no separation -> lo == src

				if (job.guided && (job.guidedRx > 0 || job.guidedRy > 0)) {
					// Guided: squares -> box(sq) -> box(src) -> ab -> box(sa)/
					// box(sb) -> final. Ping-pong schedule from mm_gpu_host.h:
					// each stage writes into planes whose prior contents are
					// dead, which is what gets guided into 3 scratch planes
					// instead of 5 without aliasing a single dispatch.
					float *const P1 = plane[1];
					float *const P2 = plane[2];
					float *const P3 = plane[3];

					auto box = [&](const float *in, float *out, int R, int horiz) {
						ArgsBox a{};
						a.gBoxSrc     = in;
						a.gBoxDst     = out;
						a.gW          = job.srcW;
						a.gH          = rows;
						a.gR          = R;
						a.gHorizontal = horiz;
						L.Launch2D(FS_K_GBOX, job.srcW, rows, a);
					};

					{	// squares: A -> L  (L is dead until final writes it)
						ArgsSquares a{};
						a.gSqSrc = pA;
						a.gSqDst = pL;
						a.gSqW   = job.srcW;
						a.gSqH   = rows;
						L.Launch2D(FS_K_GSQUARES, job.srcW, rows, a);
					}
					box(pL, P3, job.guidedRx, 1);   // box(sq) H
					box(P3, P2, job.guidedRy, 0);   //         V -> P2 = s2
					box(pA, P3, job.guidedRx, 1);   // box(src) H
					box(P3, P1, job.guidedRy, 0);   //          V -> P1 = s1

					{	// ab: (A, s1, s2) -> (P3 = sa, L = sb)
						ArgsAB a{};
						a.gABSrc = pA;
						a.gABm1  = P1;
						a.gABm2  = P2;
						a.gABsa  = P3;
						a.gABsb  = pL;
						a.gABW   = job.srcW;
						a.gABH   = rows;
						a.gEps   = job.guidedEps;
						L.Launch2D(FS_K_GAB, job.srcW, rows, a);
					}
					box(P3, P1, job.guidedRx, 1);   // box(sa) H
					box(P1, P2, job.guidedRy, 0);   //         V -> P2 = boxed sa
					box(pL, P1, job.guidedRx, 1);   // box(sb) H
					box(P1, P3, job.guidedRy, 0);   //         V -> P3 = boxed sb

					{	// final: (A, boxed sa, boxed sb) -> L = lo
						ArgsFinal a{};
						a.gFinSrc   = pA;
						a.gFinSa    = P2;
						a.gFinSb    = P3;
						a.gFinDst   = pL;
						a.gFinW     = job.srcW;
						a.gFinH     = rows;
						a.gInvCount = job.guidedInvCount;
						L.Launch2D(FS_K_GFINAL, job.srcW, rows, a);
					}
					pLo = pL;
				} else if (!job.guided && (job.sepRx > 0 || job.sepRy > 0)) {
					// Gaussian separation. Planes: A, T (= plane[1]), L.
					float *const pT = plane[1];
					auto gauss = [&](const float *in, size_t kOff, float *out,
					                 int R, int horiz) {
						ArgsGauss a{};
						a.gSrc        = in;
						a.gKernel     = pWeight + kOff;
						a.gDst        = out;
						a.gW          = job.srcW;
						a.gH          = rows;
						a.gR          = R;
						a.gHorizontal = horiz;
						L.Launch2D(FS_K_GAUSS, job.srcW, rows, a);
					};
					if (job.sepRx > 0 && job.sepRy > 0) {
						gauss(pA, offSepX, pT, job.sepRx, 1);
						gauss(pT, offSepY, pL, job.sepRy, 0);
					} else if (job.sepRx > 0) {
						gauss(pA, offSepX, pL, job.sepRx, 1);
					} else {
						gauss(pA, offSepY, pL, job.sepRy, 0);
					}
					pLo = pL;
				}

				// Low Band Smooth cascade (always Gaussian, both methods).
				// Tmp plane: plane[1] is dead by now in either schedule.
				if (job.smoothRx > 0 || job.smoothRy > 0) {
					float *const pT = plane[1];
					auto gauss = [&](const float *in, size_t kOff, float *out,
					                 int R, int horiz) {
						ArgsGauss a{};
						a.gSrc        = in;
						a.gKernel     = pWeight + kOff;
						a.gDst        = out;
						a.gW          = job.srcW;
						a.gH          = rows;
						a.gR          = R;
						a.gHorizontal = horiz;
						L.Launch2D(FS_K_GAUSS, job.srcW, rows, a);
					};
					if (job.smoothRx > 0 && job.smoothRy > 0) {
						gauss(pLo, offSmX, pT, job.smoothRx, 1);
						gauss(pT,  offSmY, pL, job.smoothRy, 0);
						pLo = pL;
					} else if (job.smoothRx > 0) {
						gauss(pLo, offSmX, pT, job.smoothRx, 1);
						pLo = pT;
					} else {
						gauss(pLo, offSmY, pT, job.smoothRy, 0);
						pLo = pT;
					}
				}

				// Matte weights for this band (active matte only — inactive was
				// filled once above).
				if (job.matte.active && dMatteWorld) {
					ArgsMatte a{};
					a.gMtWorld      = dMatteWorld;
					a.gMtDst        = pMatte;
					a.gMtOutW       = job.outW;
					a.gMtOutH       = band.outH;
					a.gMtOutY0      = band.outY0;
					a.gMtW          = matteWorld->width;
					a.gMtH          = matteWorld->height;
					a.gMtPitch      = (int)(matteWorld->rowbytes / sizeof(float));
					a.gMtOriginX    = job.matte.mOriginX;
					a.gMtOriginY    = job.matte.mOriginY;
					a.gMtRefW       = job.matte.mRefW;
					a.gMtRefH       = job.matte.mRefH;
					a.gMtOutOriginX = job.matte.outOriginX;
					a.gMtOutOriginY = job.matte.outOriginY;
					a.gMtSrcRefW    = job.matte.srcRefW;
					a.gMtSrcRefH    = job.matte.srcRefH;
					a.gMtDsx        = job.matte.dsx;
					a.gMtDsy        = job.matte.dsy;
					a.gMtChannel    = job.matte.channel;
					a.gMtFit        = job.matte.fit;
					L.Launch2D(FS_K_MATTE, job.outW, band.outH, a);
				}

				// Compose the band (reads A + lo through the local offset).
				{
					ArgsCompose a{};
					a.gSrc      = pA;
					a.gLo       = pLo;
					a.gMatte    = pMatte;
					a.gDst      = pOut;
					a.gOutW     = job.outW;
					a.gOutH     = band.outH;
					a.gSrcW     = job.srcW;
					a.gSrcOX    = job.srcOX;
					a.gSrcOY    = band.loOY;
					a.gView     = job.view;
					a.gTexGain  = job.texGain;
					a.gMix      = job.mix;
					L.Launch2D(FS_K_COMPOSE, job.outW, band.outH, a);
				}

				// Store the band (packed ARGB tile -> BGRA world rows).
				{
					ArgsStore a{};
					a.gStTile   = pOut;
					a.gStWorld  = dOutWorld;
					a.gStW      = job.outW;
					a.gStH      = band.outH;
					a.gStWorldY0= band.outY0;
					a.gStPitch  = job.outPitchFl;
					L.Launch2D(FS_K_STORE, job.outW, band.outH, a);
				}
			}

#if FS_GPU_VISIBLE_MARKER
			// Zero the top eighth of the output world. Reads TRANSPARENT (BGRA
			// zero == alpha zero) and must disappear under Software Only.
			if (cu.MemsetD8Async && L.res == FS_CUDA_SUCCESS) {
				cu.MemsetD8Async(reinterpret_cast<CUdeviceptr>(dOutWorld), 0,
					(size_t)outWorld->rowbytes * (size_t)(job.outH / 8),
					data->cuStream);
			}
#endif

			if (L.res != FS_CUDA_SUCCESS) {
				// No CPU fallback is possible from here: the worlds are device
				// memory and CPU code cannot address them.
				FS_GPU_LOG_FIRST(s_launchFail, 8,
					"[MM_FreqSep] launch FAILED at kernel %s (CUresult=%d)\n",
					(L.failed >= 0 && L.failed < FS_K_COUNT)
						? kFS_CudaKernelNames[L.failed] : "?", (int)L.res);
				err = PF_Err_INTERNAL_STRUCT_DAMAGED;
			}
		}
	}

cleanup:
	ERR2(AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
		kPFGPUDeviceSuiteVersion1, ""));
	return err;
}

} // namespace fsgpu

#endif // _WIN32
