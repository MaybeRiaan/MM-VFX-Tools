/*  qk_gpu_cuda.cpp — CUDA host for the MM Qualify GPU render.

    THE BACKEND AFTER EFFECTS ACTUALLY USES. AE 2026 on Windows/NVIDIA hands
    effects what_gpu = CUDA and never probes DIRECTX, even with
    SUPPORTS_DIRECTX_RENDERING declared (measured 2026-07-23 —
    ../../../docs/AE_GPU_Effects.md §1.1). mm_gpu_dx.cpp remains correct and
    maintained; it serves Premiere hosts, where DX12 IS the Windows renderer.

    Executes the same QK_GPUJob smart_render.cpp assembles, through the same
    band plan from qk_gpu_host.h, dispatching the same platform-free .kernel
    bodies — compiled here by nvcc into CUDA_Assets/MM_Qualify_Kernels.ptx
    instead of by dxc into embedded DXIL. All tiling arithmetic stays in
    qk_gpu_host.h; this file is exclusively CUDA plumbing, and its pass
    sequence is a line-for-line translation of QK_DXRender's.

    CONTRACT WITH AE (../../../docs/AE_GPU_Effects.md §3):
      - PF_GPUDeviceInfo.contextPV is a CUcontext, command_queuePV a CUstream;
        both are AE's and neither is owned here.
      - PF_GPUDeviceSuite1::GetGPUWorldData returns the CUdeviceptr directly.
      - Worlds are PF_PixelFormat_GPU_BGRA128 (float BGRA, pitched rows) — NOT
        the ARGB the CPU path uses. The swizzle lives in qk_tile.kernel, shared
        with DX, not in this host.
      - Device memory comes from the suite's allocator ONLY, never cuMemAlloc.
      - Launch on AE's stream and do not synchronize; AE owns synchronization.

    ── THREE DELIBERATE DIVERGENCES FROM mm_gpu_dx.cpp ───────────────────────
    1. SCRATCH IS CACHED PER DEVICE, not allocated per render. The DX host
       allocates and frees inside QK_DXRender, which is MFR-safe with zero
       shared state but costs a fresh allocation every frame — and this
       plugin's usage profile is 6K+ footage, which is exactly the case the
       whole backend exists to make fast. This follows EdgeRepair and
       MM_FreqSep instead: one grow-only block per device, freed at
       GPU_DEVICE_SETDOWN, so a steady-state render allocates nothing.

       That makes the block shared between concurrent MFR renders, so
       renderLock covers the whole ENQUEUE sequence. Because AE's per-device
       stream is FIFO, serializing enqueue serializes execution: render A's
       passes all run before render B's touch the buffers (§4, "Multi-pass
       scratch"). See the temporal note below for why that is sufficient even
       for the one plane that carries state.

    2. NO UAV BARRIERS. The DX host issues a global UAV barrier after every
       dispatch; none are needed here. This pipeline is one dependency chain —
       every pass reads what the previous pass wrote — and launches issued to a
       single CUDA stream execute in issue order with an implicit dependency
       between them. Do not "optimize" onto a second stream without
       re-deriving the hazards: the matte-blur ping-pong reuses planes
       precisely because it can assume serial execution.

    3. NO DOUBLES DEVICE GATE — and there is none on DX either. MM Qualify has
       no box sums, so nothing accumulates in double (gpu-kernel-rules §3.7
       does not apply to this plugin). MM_FreqSep's DoublePrecisionFloatShader-
       Ops device gate deliberately does NOT carry over.

    ── THE TEMPORAL IIR, AND WHY MFR IS NOT A PROBLEM ────────────────────────
    qk_iir.kernel carries a state plane across history frames, and an IIR is
    order-dependent while Multi-Frame Rendering is not ordered. No other plugin
    in this suite has faced this, so it is worth stating exactly where the
    story is settled rather than assuming it.

    It is settled in the CPU path, and this host inherits it unchanged:

      - PreRender checks out the SOURCE at t-1 .. t-QK_TEMPORAL_WINDOW
        (smart_render.cpp, "HISTORY checkouts"), one checkout ID per offset.
      - SmartRender then walks those frames oldest -> newest INSIDE THE ONE
        RENDER CALL, seeding the state from the oldest usable frame and folding
        forward to the current one (smart_render.cpp, "TEMPORAL STABILIZE").
        The CPU's state buffer `ssBuf` is a local std::vector on the render
        stack.

    So the state plane's lifetime is ONE RENDER CALL, and every output frame is
    a pure function of its own N+1 checked-out source frames. There is no
    ordering relationship between renders for MFR to violate — which is what
    that file's header means by "everything a render needs lives on the stack
    or in the pre_render_data block AE owns; no globals, no statics with
    state". The price is recomputation (the warm-start window is re-derived per
    frame); the payment buys determinism and MFR safety.

    The one thing THIS host adds is that the S plane lives in the shared,
    cached scratch block rather than on a stack. That stays safe because:
      (a) renderLock + a FIFO stream means render A's whole band sequence
          executes before render B touches the planes, and
      (b) within each band the seed launch (gFirst = 1) WRITES S before
          anything reads it, so no residue from a previous render can be
          observed. tests/kernel_test.cpp asserts that by seeding qk_iir from a
          deliberately poisoned state plane, and tests/pipeline_test.cpp runs
          the whole band schedule with S pre-filled with garbage.

    Timing/threading refinement is a later pass driven by real logs; this file
    is the architecture, and the architecture is the CPU path's, verbatim.
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

#include "qk_gpu_host.h"
#include "qk_cuda_shim.h"
#include "cuda/qk_kernel_args.h"   // the launch structs, compiled FROM the kernels

namespace qkgpu {

// ---------------------------------------------------------------------------
// Diagnostics. The GPU failure mode is SILENT — a declined device renders on
// the CPU, correctly, with no error anywhere (../../../docs/AE_GPU_Effects.md
// §5). These logs are the only way to tell "GPU rendering" from "looks right".
// Same first-N-per-site budget as EdgeRepair's ER_GPU_LOG so a long render
// cannot fill the disk.
// ---------------------------------------------------------------------------
#define QK_GPU_LOG 1

// Visible destructive marker (§5.2). A PASSTHROUGH milestone is worthless —
// output is bit-identical whether or not the GPU ran, and the suite wasted
// several test rounds proving exactly that, twice. This zeroes the top eighth
// of every output world: it must read TRANSPARENT (zeroing BGRA zeroes alpha)
// and must vanish under Mercury -> Software Only. Set to 1 only while proving
// the path is live, then back to 0.
#define QK_GPU_VISIBLE_MARKER 0

#if QK_GPU_LOG
#include <atomic>
static void QK_GPULog(const char *fmt, ...)
{
	char path[MAX_PATH];
	const DWORD n = ::GetTempPathA(MAX_PATH, path);
	if (n == 0 || n > MAX_PATH) return;
	std::string file(path);
	file += "MM_Qualify_gpu.log";

	FILE *f = nullptr;
	if (fopen_s(&f, file.c_str(), "a") != 0 || !f) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fclose(f);
}
#define QK_GPU_LOG_FIRST(counter, limit, ...)                   \
	do { static std::atomic<int> counter{0};                    \
	     if (counter.fetch_add(1) < (limit)) QK_GPULog(__VA_ARGS__); } while (0)
#define QK_GPU_LOGF(...) QK_GPULog(__VA_ARGS__)
#else
#define QK_GPU_LOG_FIRST(counter, limit, ...) ((void)0)
#define QK_GPU_LOGF(...) ((void)0)
#endif

// ---------------------------------------------------------------------------
// The kernel table. One entry per __global__ in cuda/qk_kernels.cu; names are
// resolved by STRING through cuModuleGetFunction, so they must match the
// KERNEL_END(name) spellings in the .kernel files EXACTLY. Order matches the
// QK_PSO_* enum in mm_gpu_dx.cpp so the two hosts read alike.
// ---------------------------------------------------------------------------
enum QK_CudaKernelId {
	QK_K_LOAD = 0,
	QK_K_STORE,
	QK_K_FILL,
	QK_K_GAUSS_RGBA,
	QK_K_GAUSS_F1,
	QK_K_GARBAGE,
	QK_K_MATTE,
	QK_K_IIR,
	QK_K_COMPOSE,
	QK_K_COUNT
};

static const char *const kQK_CudaKernelNames[QK_K_COUNT] = {
	"qk_load_tile",
	"qk_store_tile",
	"qk_fill_ones",
	"qk_gauss_rgba",
	"qk_gauss_f1",
	"qk_garbage",
	"qk_matte",
	"qk_iir",
	"qk_compose",
};

// ---------------------------------------------------------------------------
// Per-device state, owned by AE via extra->output->gpu_data. Immutable after
// GPU_DEVICE_SETUP except for the scratch block, which renderLock guards.
// ---------------------------------------------------------------------------
struct QK_CudaData {
	CUcontext  cuCtx    = nullptr;             // AE's context (not owned)
	CUstream   cuStream = nullptr;             // AE's stream  (not owned)
	CUmodule   cuModule = nullptr;             // loaded PTX   (owned)
	CUfunction cuFn[QK_K_COUNT] = { nullptr };

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
static bool QK_CudaPtxPath(std::wstring &outPath)
{
	wchar_t path[1024];
	HMODULE hm = nullptr;
	if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                          reinterpret_cast<LPCWSTR>(&QK_CudaPtxPath), &hm))
		return false;
	if (::GetModuleFileNameW(hm, path, static_cast<DWORD>(std::size(path))) == 0)
		return false;
	std::wstring dir(path);
	const size_t slash = dir.rfind(L'\\');
	if (slash == std::wstring::npos) return false;
	outPath = dir.substr(0, slash) + L"\\CUDA_Assets\\MM_Qualify_Kernels.ptx";
	return true;
}

// cuModuleLoadDataEx takes a NUL-terminated PTX text image.
static bool QK_ReadTextFile(const std::wstring &path, std::vector<char> &out)
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
// shape as the DX host's QK_QueryScratchBudget, but measured against what is
// actually free rather than what the card has, since AE, the OS compositor and
// any other GPU effect in the stack are already holding some. The tile planner
// turns whatever this yields into a band height, so the number only moves the
// per-band overlap overhead, never correctness — the floor is therefore safe
// to apply unconditionally here (unlike ModernEdgeBlur, whose allocation is
// all-or-nothing). Query failure -> 384MB.
// Caller must hold AE's context current.
// ---------------------------------------------------------------------------
static size_t QK_QueryCudaBudget(const QK_CudaApi &cu)
{
	const size_t kFallback = 384ull << 20;
	if (!cu.MemGetInfo) return kFallback;

	size_t freeBytes = 0, totalBytes = 0;
	if (cu.MemGetInfo(&freeBytes, &totalBytes) != QK_CUDA_SUCCESS || freeBytes == 0)
		return kFallback;

	size_t budget = freeBytes / 4;
	if (budget < (256ull << 20))  budget = 256ull << 20;
	if (budget > (1024ull << 20)) budget = 1024ull << 20;
	return budget;
}

// ---------------------------------------------------------------------------
// GPU_DEVICE_SETUP / SETDOWN
// ---------------------------------------------------------------------------
PF_Err QK_CudaDeviceSetup(PF_InData *in_data, PF_OutData *out_data,
                          PF_GPUDeviceSetupExtra *extra)
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
	extra->output->gpu_data = nullptr;

	QK_GPU_LOGF("[MM_Qualify] DEVICE_SETUP what_gpu=%d device_index=%u\n",
		(int)extra->input->what_gpu, (unsigned)extra->input->device_index);

	if (extra->input->what_gpu != PF_GPU_Framework_CUDA) {
		// Not ours. Declining is PF_Err_NONE with gpu_data left null — AE then
		// never offers GPU render for devices of this framework and the CPU
		// reference takes the frame. An error here would surface a user-visible
		// AE failure for what is a normal configuration.
		QK_GPU_LOGF("  -> not CUDA, declined here (DX host handles DIRECTX)\n");
		return PF_Err_NONE;
	}

	const QK_CudaApi &cu = QK_CudaApi::Get();
	if (!cu.loaded) {
		QK_GPU_LOGF("  -> nvcuda.dll unavailable - CUDA declined (CPU fallback)\n");
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

		QK_CudaData *data = new (std::nothrow) QK_CudaData();
		if (!data) { err = PF_Err_OUT_OF_MEMORY; goto done; }

		data->cuCtx       = reinterpret_cast<CUcontext>(info.contextPV);
		data->cuStream    = reinterpret_cast<CUstream>(info.command_queuePV);
		data->deviceIndex = extra->input->device_index;

		if (!data->cuCtx) {
			QK_GPU_LOGF("  -> AE provided no CUcontext - CUDA declined\n");
			delete data; goto done;
		}

		std::wstring ptxPath;
		std::vector<char> ptx;
		if (!QK_CudaPtxPath(ptxPath) || !QK_ReadTextFile(ptxPath, ptx)) {
			// Normal on a build machine without the CUDA toolkit: no PTX was
			// produced, so there is nothing to run. Quiet decline, CPU renders.
			QK_GPU_LOGF("  -> PTX missing/unreadable (%ls) - CUDA declined "
			            "(build with the CUDA toolkit installed to produce it)\n",
			            ptxPath.c_str());
			delete data; goto done;
		}

		{
			QK_CudaCtxGuard ctx(cu, data->cuCtx);
			if (!ctx.pushed) {
				QK_GPU_LOGF("  -> cuCtxPushCurrent FAILED - CUDA declined\n");
				delete data; goto done;
			}

			CUresult res = cu.ModuleLoadDataEx(&data->cuModule, ptx.data(),
			                                   0, nullptr, nullptr);
			if (res != QK_CUDA_SUCCESS || !data->cuModule) {
				QK_GPU_LOGF("  -> cuModuleLoadDataEx FAILED (CUresult=%d) - CUDA declined\n",
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
			for (int k = 0; k < QK_K_COUNT; ++k) {
				res = cu.ModuleGetFunction(&data->cuFn[k], data->cuModule,
				                           kQK_CudaKernelNames[k]);
				if (res != QK_CUDA_SUCCESS || !data->cuFn[k]) {
					QK_GPU_LOGF("  -> cuModuleGetFunction(%s) FAILED (CUresult=%d) - "
					            "CUDA declined (stale CUDA_Assets PTX?)\n",
					            kQK_CudaKernelNames[k], (int)res);
					allResolved = false;
					break;
				}
			}
			if (!allResolved) {
				cu.ModuleUnload(data->cuModule);
				data->cuModule = nullptr;
				delete data; goto done;
			}

			data->scratchBudget = QK_QueryCudaBudget(cu);
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

		QK_GPU_LOGF("  -> OK (CUDA ready, %d kernels, scratch budget %.0f MB)\n",
			(int)QK_K_COUNT, (double)data->scratchBudget / (1024.0 * 1024.0));
	}

done:
	ERR2(AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
		kPFGPUDeviceSuiteVersion1, ""));
	return err;
}

PF_Err QK_CudaDeviceSetdown(PF_InData *in_data, PF_OutData *out_data,
                            PF_GPUDeviceSetdownExtra *extra)
{
	QK_CudaData *data = static_cast<QK_CudaData*>(extra->input->gpu_data);
	if (!data) return PF_Err_NONE;

	QK_GPU_LOGF("[MM_Qualify] DEVICE_SETDOWN gpu_data=%p\n", (void*)data);

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
		const QK_CudaApi &cu = QK_CudaApi::Get();
		if (cu.loaded) {
			QK_CudaCtxGuard ctx(cu, data->cuCtx);
			cu.ModuleUnload(data->cuModule);
		}
		data->cuModule = nullptr;
	}
	delete data;
	return PF_Err_NONE;
}

size_t QK_CudaScratchBudget(const void *gpu_data)
{
	const QK_CudaData *data = static_cast<const QK_CudaData*>(gpu_data);
	return data ? data->scratchBudget : 0;
}

void QK_GPULogPreRenderDecision(PF_GPU_Framework what_gpu, const void *gpu_data,
                                bool raised, int outW, int outH,
                                double scratchMB, double budgetMB)
{
	QK_GPU_LOG_FIRST(s_prerender, 24,
		"[MM_Qualify] PRERENDER what_gpu=%d gpu_data=%p out=%dx%d "
		"need=%.1fMB budget=%.1fMB -> GPU_RENDER_POSSIBLE=%s\n",
		(int)what_gpu, gpu_data, outW, outH, scratchMB, budgetMB,
		raised ? "YES" : "no");
}

// ---------------------------------------------------------------------------
// Launcher. Every launch goes on AE'S OWN stream and is NOT synchronized here —
// AE owns stream synchronization. First failure wins and every later launch is
// skipped, so a broken pass cannot cascade into running kernels over
// half-written buffers.
// ---------------------------------------------------------------------------
struct QK_CudaLauncher {
	const QK_CudaApi &cu;
	QK_CudaData      *g;
	CUresult          res    = QK_CUDA_SUCCESS;
	int               failed = -1;

	QK_CudaLauncher(const QK_CudaApi &api, QK_CudaData *gp) : cu(api), g(gp) {}

	// One thread per pixel over a w x h grid. Block size comes from the SHARED
	// prologue constants, not a number chosen here, so the guard in every kernel
	// body and the grid the host computes can never disagree. (Retuning is a
	// prologue edit — see kernels/qk_prologue_cuda.h.)
	template <typename ArgsT>
	void Launch2D(QK_CudaKernelId k, int w, int h, ArgsT &args)
	{
		if (res != QK_CUDA_SUCCESS || w <= 0 || h <= 0) return;
		void *params[] = { &args };
		res = cu.LaunchKernel(g->cuFn[k],
		                      (unsigned)((w + QK_TG_X - 1) / QK_TG_X),
		                      (unsigned)((h + QK_TG_Y - 1) / QK_TG_Y), 1u,
		                      (unsigned)QK_TG_X, (unsigned)QK_TG_Y, 1u,
		                      0u, g->cuStream, params, nullptr);
		if (res != QK_CUDA_SUCCESS) failed = (int)k;
	}
};

// ---------------------------------------------------------------------------
// Grow-only device scratch, cached on the per-device gpu_data. Caller holds
// renderLock AND has AE's context current. Leaves the old block intact on
// failure. Mirrors EdgeRepair's ER_EnsureScratch.
// ---------------------------------------------------------------------------
static bool QK_EnsureScratch(PF_ProgPtr effect_ref, PF_GPUDeviceSuite1 *gpuSuite,
                             QK_CudaData *g, size_t bytes)
{
	if (g->scratch && g->scratchBytes >= bytes) return true;

	void *mem = nullptr;
	if (gpuSuite->AllocateDeviceMemory(effect_ref, g->deviceIndex, bytes, &mem) ||
	    !mem) {
		QK_GPU_LOG_FIRST(s_allocFail, 4,
			"[MM_Qualify] AllocateDeviceMemory(%zu bytes) FAILED\n", bytes);
		return false;
	}
	if (g->scratch)
		gpuSuite->FreeDeviceMemory(effect_ref, g->deviceIndex, g->scratch);
	g->scratch      = mem;
	g->scratchBytes = bytes;
	QK_GPU_LOG_FIRST(s_allocOk, 4,
		"[MM_Qualify] GPU scratch grown to %.1f MB\n",
		(double)bytes / (1024.0 * 1024.0));
	return true;
}

static size_t QK_Align256(size_t v) { return (v + 255u) & ~(size_t)255u; }

// ---------------------------------------------------------------------------
// Render — the pass sequence of QK_DXRender, translated. Read the two side by
// side when changing either: the band loop, the plane roles, the temporal
// stream and the matte-blur ping-pong are meant to stay identical, and any
// divergence between them is a bug in one of the two hosts, not a backend
// difference. tests/pipeline_test.cpp runs this same sequence on the CPU with
// the same kernels, which is what makes "identical" checkable.
// ---------------------------------------------------------------------------
PF_Err QK_CudaRender(PF_InData *in_data, PF_OutData *out_data,
                     const void *gpu_dataPV, A_u_long /*device_index*/,
                     const QK_GPUJob &job,
                     PF_EffectWorld *srcWorld, PF_EffectWorld *outWorld,
                     PF_EffectWorld *garbWorld,
                     const std::vector<PF_EffectWorld*> &histWorlds)
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;

	QK_CudaData *data =
		const_cast<QK_CudaData*>(static_cast<const QK_CudaData*>(gpu_dataPV));
	if (!data || job.plan.bands.empty()) return PF_Err_BAD_CALLBACK_PARAM;

	const QK_CudaApi &cu = QK_CudaApi::Get();
	if (!cu.loaded || !data->cuFn[QK_K_COMPOSE]) return PF_Err_INTERNAL_STRUCT_DAMAGED;

	PF_GPUDeviceSuite1 *gpuSuite = nullptr;
	ERR(AEFX_AcquireSuite(in_data, out_data, kPFGPUDeviceSuite,
		kPFGPUDeviceSuiteVersion1, "", reinterpret_cast<void**>(&gpuSuite)));
	if (err || !gpuSuite) return err ? err : PF_Err_BAD_CALLBACK_PARAM;

	{
		// --- World device pointers (for CUDA these ARE the CUdeviceptrs) ----
		void *srcPV = nullptr, *outPV = nullptr, *garbPV = nullptr;
		ERR(gpuSuite->GetGPUWorldData(in_data->effect_ref, srcWorld, &srcPV));
		ERR(gpuSuite->GetGPUWorldData(in_data->effect_ref, outWorld, &outPV));
		if (!err && garbWorld)
			ERR(gpuSuite->GetGPUWorldData(in_data->effect_ref, garbWorld, &garbPV));
		if (err || !srcPV || !outPV || (garbWorld && !garbPV)) {
			if (!err) err = PF_Err_BAD_CALLBACK_PARAM;
			goto cleanup;
		}

		{
			const float *dSrcWorld  = static_cast<const float*>(srcPV);
			float       *dOutWorld  = static_cast<float*>(outPV);
			const float *dGarbWorld = static_cast<const float*>(garbPV);

			// History world pointers, in the oldest-first order the caller
			// built. Null entries are skipped — the IIR then warm-starts at the
			// first frame that exists, exactly like the CPU path.
			std::vector<const float*> histPtr(histWorlds.size(), nullptr);
			for (size_t i = 0; i < histWorlds.size(); ++i) {
				if (!histWorlds[i]) continue;
				void *hpv = nullptr;
				if (gpuSuite->GetGPUWorldData(in_data->effect_ref, histWorlds[i], &hpv)
						== PF_Err_NONE && hpv)
					histPtr[i] = static_cast<const float*>(hpv);
			}

			// --- Weight layout (16-float aligned, as on DX) ------------------
			auto alignF = [](size_t n) { return (n + 15u) & ~(size_t)15u; };
			const size_t offDnX = 0;
			const size_t offDnY = offDnX + alignF(job.kDnX.size());
			const size_t offBlX = offDnY + alignF(job.kDnY.size());
			const size_t offBlY = offBlX + alignF(job.kBlX.size());
			const size_t weightFloats = offBlY + alignF(job.kBlY.size());

			// --- One scratch block: RGBA planes | f1 planes | out | weights --
			// Plane roles mirror mm_gpu_dx.cpp exactly:
			//   RGBA: A [, T, Q]        (T/Q only when denoise is on; else Q==A)
			//   f1:   M, B1, G [, S]    (S only when temporal is on)
			const bool denoise = (job.dnRx > 0 || job.dnRy > 0);
			const int nRGBA = QK_RGBAPlanes(denoise);
			const int nF1   = QK_F1Planes(job.temporal);
			const size_t rgbaBytes  = QK_Align256((size_t)job.plan.tileRowsMax * job.srcW * 4 * sizeof(float));
			const size_t f1Bytes    = QK_Align256((size_t)job.plan.tileRowsMax * job.srcW * sizeof(float));
			const size_t outBytes   = QK_Align256((size_t)job.plan.bandHMax * job.outW * 4 * sizeof(float));
			const size_t weightBytes= QK_Align256(weightFloats * sizeof(float));
			const size_t scratchBytes = (size_t)nRGBA * rgbaBytes
			                          + (size_t)nF1 * f1Bytes + outBytes + weightBytes;

			QK_GPU_LOG_FIRST(s_plan, 6,
				"[MM_Qualify] SMART_RENDER GPU what_gpu=CUDA src=%dx%d out=%dx%d "
				"off=(%d,%d) denoise=%d dnR=(%d,%d) blR=(%d,%d) temporal=%d hist=%d "
				"garbage=%d mode=%d bands=%d scratch=%.1fMB\n",
				job.srcW, job.srcH, job.outW, job.outH, job.srcOX, job.srcOY,
				denoise ? 1 : 0, job.dnRx, job.dnRy, job.blRx, job.blRy,
				job.temporal ? 1 : 0, (int)histWorlds.size(),
				job.garbage.active ? 1 : 0, job.mode,
				(int)job.plan.bands.size(),
				(double)scratchBytes / (1024.0 * 1024.0));

			// The lock covers scratch growth AND the whole enqueue sequence —
			// see the header note on why enqueue order is sufficient, including
			// for the temporal state plane.
			std::lock_guard<std::mutex> lock(data->renderLock);

			// AE's context is pushed BEFORE the allocation, not just around the
			// launches: the suite's device allocator runs driver calls of its
			// own, and the discipline is that a context is current for any
			// driver-call sequence we trigger.
			QK_CudaCtxGuard ctx(cu, data->cuCtx);
			if (!ctx.pushed) { err = PF_Err_INTERNAL_STRUCT_DAMAGED; goto cleanup; }

			if (!QK_EnsureScratch(in_data->effect_ref, gpuSuite, data, scratchBytes)) {
				err = PF_Err_OUT_OF_MEMORY; goto cleanup;
			}

			{
				char *const base = static_cast<char*>(data->scratch);
				float *const pA = reinterpret_cast<float*>(base);
				float *const pT = denoise ? reinterpret_cast<float*>(base + rgbaBytes) : pA;
				float *const pQ = denoise ? reinterpret_cast<float*>(base + 2 * rgbaBytes) : pA;
				char *const f1base = base + (size_t)nRGBA * rgbaBytes;
				float *const pM  = reinterpret_cast<float*>(f1base);
				float *const pB1 = reinterpret_cast<float*>(f1base + f1Bytes);
				float *const pG  = reinterpret_cast<float*>(f1base + 2 * f1Bytes);
				float *const pS  = job.temporal
					? reinterpret_cast<float*>(f1base + 3 * f1Bytes) : nullptr;
				float *const pOut = reinterpret_cast<float*>(f1base + (size_t)nF1 * f1Bytes);
				float *const pWeight = reinterpret_cast<float*>(
					f1base + (size_t)nF1 * f1Bytes + outBytes);

				// --- Weights: the EXACT CPU-built kernels, uploaded verbatim --
				// Built in double by qk::BuildGaussianKernel and narrowed once
				// (rules §3.1). A GPU-side float rebuild would diverge in the
				// last bits and turn the parity check into a coin flip.
				if (weightFloats > 0) {
					std::vector<float> w(weightFloats, 0.0f);
					if (!job.kDnX.empty()) std::memcpy(w.data() + offDnX, job.kDnX.data(), job.kDnX.size() * sizeof(float));
					if (!job.kDnY.empty()) std::memcpy(w.data() + offDnY, job.kDnY.data(), job.kDnY.size() * sizeof(float));
					if (!job.kBlX.empty()) std::memcpy(w.data() + offBlX, job.kBlX.data(), job.kBlX.size() * sizeof(float));
					if (!job.kBlY.empty()) std::memcpy(w.data() + offBlY, job.kBlY.data(), job.kBlY.size() * sizeof(float));
					// Pageable source: the driver stages it before returning, so
					// w may die at the end of this scope. Stream-ordered, so
					// every kernel below sees the finished upload.
					if (cu.MemcpyHtoDAsync(reinterpret_cast<CUdeviceptr>(pWeight),
							w.data(), weightFloats * sizeof(float),
							data->cuStream) != QK_CUDA_SUCCESS) {
						err = PF_Err_INTERNAL_STRUCT_DAMAGED; goto cleanup;
					}
				}

				// Gate block, assembled once — identical for every launch.
				float gatePacked[QK_GATES_FLOATS];
				QK_PackGates(job.gates, gatePacked);

				QK_CudaLauncher L(cu, data);

				// Denoise cascade helper: X into tmp, then Y into out (single
				// axis lands directly in out). Same schedule as QK_DXRender's
				// gaussRGBA lambda.
				auto gaussRGBA = [&](const float *in, float *tmp, float *out, int rows) {
					const float *cur = in;
					if (job.dnRx > 0) {
						float *dst = (job.dnRy > 0) ? tmp : out;
						ArgsGaussRGBA a{};
						a.gRgSrc = cur; a.gRgKern = pWeight + offDnX; a.gRgDst = dst;
						a.gRgW = job.srcW; a.gRgRows = rows;
						a.gRgR = job.dnRx; a.gRgHoriz = 1;
						L.Launch2D(QK_K_GAUSS_RGBA, job.srcW, rows, a);
						cur = dst;
					}
					if (job.dnRy > 0) {
						ArgsGaussRGBA a{};
						a.gRgSrc = cur; a.gRgKern = pWeight + offDnY; a.gRgDst = out;
						a.gRgW = job.srcW; a.gRgRows = rows;
						a.gRgR = job.dnRy; a.gRgHoriz = 0;
						L.Launch2D(QK_K_GAUSS_RGBA, job.srcW, rows, a);
					}
				};

				auto loadTile = [&](const float *world, int y0, int rows, float *tile) {
					ArgsLoad a{};
					a.gLdWorld = world; a.gLdTile = tile;
					a.gLdW = job.srcW; a.gLdRows = rows;
					a.gLdWorldY0 = y0; a.gLdPitch = job.srcPitchFl;
					L.Launch2D(QK_K_LOAD, job.srcW, rows, a);
				};

				auto matteKernel = [&](const float *qual, int rows) {
					ArgsMatte a{};
					a.gQual = qual; a.gGIn = pG; a.gMOut = pM;
					a.gW = job.srcW; a.gRows = rows;
					QK_StoreGates(a, gatePacked);
					a.gClipB = job.clipB; a.gClipW = job.clipW;
					L.Launch2D(QK_K_MATTE, job.srcW, rows, a);
				};

				// ---------------- Per band ----------------
				for (const QK_TileBand &band : job.plan.bands) {
					const int rows = band.srcRows;

					// Garbage weights for the tile (active) or fill 1.0.
					if (job.garbage.active && dGarbWorld && garbWorld) {
						ArgsGarbage a{};
						a.gGarbWorld = dGarbWorld; a.gGPlane = pG;
						a.gW = job.srcW; a.gRows = rows; a.gSrcY0 = band.srcY0;
						a.gSrcOX = job.srcOX; a.gSrcOY = job.srcOY;
						a.gMtW = (int)garbWorld->width;
						a.gMtH = (int)garbWorld->height;
						a.gMtPitch = (int)(garbWorld->rowbytes / (A_long)sizeof(float));
						a.gMOriginX = job.garbage.mOriginX; a.gMOriginY = job.garbage.mOriginY;
						a.gMRefW = job.garbage.mRefW; a.gMRefH = job.garbage.mRefH;
						a.gOutOriginX = job.garbage.outOriginX;
						a.gOutOriginY = job.garbage.outOriginY;
						a.gSrcRefW = job.garbage.srcRefW; a.gSrcRefH = job.garbage.srcRefH;
						a.gDsx = job.garbage.dsx; a.gDsy = job.garbage.dsy;
						a.gChannel = job.garbage.channel;
						L.Launch2D(QK_K_GARBAGE, job.srcW, rows, a);
					} else {
						ArgsFill a{};
						a.gFillDst = pG; a.gFW = job.srcW; a.gFRows = rows;
						L.Launch2D(QK_K_FILL, job.srcW, rows, a);
					}

					// TEMPORAL: stream history frames oldest-first through the
					// IIR. Each runs the SAME denoise + gates + clip chain the
					// current frame will, which is what the CPU path does.
					bool haveState = false;
					if (job.temporal && pS) {
						for (size_t hi = 0; hi < histPtr.size(); ++hi) {
							if (!histPtr[hi]) continue;
							loadTile(histPtr[hi], band.srcY0, rows, pA);
							if (denoise) gaussRGBA(pA, pT, pQ, rows);
							matteKernel(pQ, rows);
							ArgsIIR a{};
							a.gMIn = pM; a.gState = pS;
							a.gW = job.srcW; a.gRows = rows;
							a.gFirst = haveState ? 0 : 1;   // seed WRITES, never reads
							a.gKAttack = job.kAttack; a.gKRelease = job.kRelease;
							L.Launch2D(QK_K_IIR, job.srcW, rows, a);
							haveState = true;
						}
					}

					// Current frame -> A (loaded LAST so compose reads it).
					loadTile(dSrcWorld, band.srcY0, rows, pA);
					if (denoise) gaussRGBA(pA, pT, pQ, rows);
					matteKernel(pQ, rows);

					// Final IIR fold: state <- current matte; matte := state.
					float *matte = pM;
					if (job.temporal && pS && haveState) {
						ArgsIIR a{};
						a.gMIn = pM; a.gState = pS;
						a.gW = job.srcW; a.gRows = rows; a.gFirst = 0;
						a.gKAttack = job.kAttack; a.gKRelease = job.kRelease;
						L.Launch2D(QK_K_IIR, job.srcW, rows, a);
						matte = pS;
					}

					// Matte blur (edge-neutral f1). A separable pass reads
					// neighbours, so a launch's input and output must never be
					// the same plane. Ping-pong between the incoming matte plane
					// and B1: X reads matte writes B1; Y reads B1 writes matte
					// (free once X has consumed it). A single-axis blur lands in
					// B1. No third plane needed — in != out on every launch.
					float *finalMatte = matte;
					if (job.blRx > 0 || job.blRy > 0) {
						float *cur = matte;
						if (job.blRx > 0) {
							ArgsGaussF1 a{};
							a.gF1Src = cur; a.gF1Kern = pWeight + offBlX; a.gF1Dst = pB1;
							a.gF1W = job.srcW; a.gF1Rows = rows;
							a.gF1R = job.blRx; a.gF1Horiz = 1;
							L.Launch2D(QK_K_GAUSS_F1, job.srcW, rows, a);
							cur = pB1;
						}
						if (job.blRy > 0) {
							// out = matte when it is free (X ran); with Y-only,
							// cur == matte so we must land in B1.
							float *dst = (cur == matte) ? pB1 : matte;
							ArgsGaussF1 a{};
							a.gF1Src = cur; a.gF1Kern = pWeight + offBlY; a.gF1Dst = dst;
							a.gF1W = job.srcW; a.gF1Rows = rows;
							a.gF1R = job.blRy; a.gF1Horiz = 0;
							L.Launch2D(QK_K_GAUSS_F1, job.srcW, rows, a);
							cur = dst;
						}
						finalMatte = cur;
					}

					// Ratio + compose -> output tile, through the band's loOY.
					{
						ArgsCompose a{};
						a.gCSrc = pA; a.gCMatte = finalMatte; a.gCQual = pQ; a.gCG = pG;
						a.gCOut = pOut;
						a.gOutW = job.outW; a.gOutH = band.outH; a.gSrcW = job.srcW;
						a.gSrcOX = job.srcOX; a.gSrcOY = band.loOY;
						a.gMode = job.mode; a.gRatio = job.ratio;
						QK_StoreGates(a, gatePacked);
						L.Launch2D(QK_K_COMPOSE, job.outW, band.outH, a);
					}

					// Store the band into the output world.
					{
						ArgsStore a{};
						a.gStTile = pOut; a.gStWorld = dOutWorld;
						a.gStW = job.outW; a.gStRows = band.outH;
						a.gStWorldY0 = band.outY0; a.gStPitch = job.outPitchFl;
						L.Launch2D(QK_K_STORE, job.outW, band.outH, a);
					}
				}

#if QK_GPU_VISIBLE_MARKER
				// Zero the top eighth of the output world. Reads TRANSPARENT
				// (BGRA zero == alpha zero) and must disappear under Software
				// Only. A passthrough milestone would prove nothing.
				if (cu.MemsetD8Async && L.res == QK_CUDA_SUCCESS) {
					cu.MemsetD8Async(reinterpret_cast<CUdeviceptr>(dOutWorld), 0,
						(size_t)outWorld->rowbytes * (size_t)(job.outH / 8),
						data->cuStream);
				}
#endif

				if (L.res != QK_CUDA_SUCCESS) {
					// No CPU fallback is possible from here: the worlds are
					// device memory and CPU code cannot address them.
					QK_GPU_LOG_FIRST(s_launchFail, 8,
						"[MM_Qualify] launch FAILED at kernel %s (CUresult=%d)\n",
						(L.failed >= 0 && L.failed < QK_K_COUNT)
							? kQK_CudaKernelNames[L.failed] : "?", (int)L.res);
					err = PF_Err_INTERNAL_STRUCT_DAMAGED;
				}
			}
		}
	}

cleanup:
	ERR2(AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
		kPFGPUDeviceSuiteVersion1, ""));
	return err;
}

} // namespace qkgpu

#endif // _WIN32
