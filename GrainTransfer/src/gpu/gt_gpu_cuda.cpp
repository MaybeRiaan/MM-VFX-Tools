/*  gt_gpu_cuda.cpp — the CUDA host for Grain Transfer.

    THE BACKEND AE ACTUALLY OFFERS (../../../docs/AE_GPU_Effects.md §1.1/§1.2).

    Shape of this file, and how it differs from its siblings: MM_Qualify's and
    MM_FreqSep's CUDA hosts are dominated by a band loop, a tile planner and a
    multi-plane scratch block, because their kernels read image neighbourhoods.
    Grain Transfer's does not. Per frame this host does exactly three things:

      1. make sure the tile bank for the CURRENT analysis is on the device,
      2. fill one argument struct,
      3. launch one kernel over the whole frame.

    There is no scratch block, no band loop, and no planner — and there should
    not be one. See gpu/kernels/gt_stamp.kernel's header.

    THE ONE PIECE OF DEVICE STATE: the tile bank. It is at most
    8 x 3 x 256 x 256 floats = 6 MB, it is IDENTICAL for every frame of a given
    source state, and re-uploading it per frame would cost more than the stamp
    it feeds. So it is cached on the per-device gpu_data and keyed on the
    analysis identity; a key change (new source, new Seed, new depth, new
    encode) re-uploads, everything else reuses. That mirrors the plugin's own
    CPU cost model, which is the whole point of the plugin.
*/

#ifdef _WIN32

#define NOMINMAX
#include <Windows.h>

#include <atomic>
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

#include "gt_gpu_host.h"
#include "gt_cuda_shim.h"
#include "cuda/gt_kernel_args.h"   // the launch structs, compiled FROM the kernel

namespace gtgpu {

// ---------------------------------------------------------------------------
// Diagnostics. The GPU failure mode is SILENT — a declined device renders on
// the CPU, correctly, with no error anywhere (AE_GPU_Effects.md §5). These logs
// are the only way to tell "GPU rendering" from "looks right".
//
// SEPARATE FILE, not the existing GrainTransfer_diag.log. That one is marked
// TEMPORARY in GrainTransfer.cpp and is meant to be deleted once the grain
// tuning settles; folding GPU lines into it would delete this diagnostic
// infrastructure along with it. %TEMP%\GrainTransfer_gpu.log matches the rest
// of the suite (MM_Qualify_gpu.log, MM_FreqSep_gpu.log).
//
// Same first-N-per-site budget as EdgeRepair's ER_GPU_LOG so a long render
// cannot fill the disk.
// ---------------------------------------------------------------------------
#define GT_GPU_LOG 1

// Visible destructive marker (§5.2). A PASSTHROUGH milestone is worthless —
// output is bit-identical whether or not the GPU ran, and the suite wasted
// several test rounds proving exactly that, twice. Set to 1 only while proving
// the path is live, then back to 0.
#define GT_GPU_VISIBLE_MARKER 0

#if GT_GPU_LOG
static void GT_GPULog(const char *fmt, ...)
{
	char path[MAX_PATH];
	const DWORD n = ::GetTempPathA(MAX_PATH, path);
	if (n == 0 || n > MAX_PATH) return;
	std::string file(path);
	file += "GrainTransfer_gpu.log";

	FILE *f = nullptr;
	if (fopen_s(&f, file.c_str(), "a") != 0 || !f) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fclose(f);
}
#define GT_GPU_LOG_FIRST(counter, limit, ...)                   \
	do { static std::atomic<int> counter{0};                    \
	     if (counter.fetch_add(1) < (limit)) GT_GPULog(__VA_ARGS__); } while (0)
#define GT_GPU_LOGF(...) GT_GPULog(__VA_ARGS__)
#else
#define GT_GPU_LOG_FIRST(counter, limit, ...) ((void)0)
#define GT_GPU_LOGF(...) ((void)0)
#endif

// ---------------------------------------------------------------------------
// The kernel table. One entry per __global__ in cuda/gt_kernels.cu; names are
// resolved by STRING through cuModuleGetFunction, so they must match the
// KERNEL_END(name) spellings in gt_stamp.kernel EXACTLY.
// ---------------------------------------------------------------------------
enum GT_CudaKernelId {
	GT_K_STAMP = 0,
	GT_K_COPY,
	GT_K_COUNT
};

static const char *const kGT_CudaKernelNames[GT_K_COUNT] = {
	"gt_stamp",
	"gt_copy",
};

bool GT_FrameworkSupported(PF_GPU_Framework what_gpu)
{
	return what_gpu == PF_GPU_Framework_CUDA;
}

// ---------------------------------------------------------------------------
// Per-device state, owned by AE via extra->output->gpu_data. Immutable after
// GPU_DEVICE_SETUP except for the tile bank, which renderLock guards.
// ---------------------------------------------------------------------------
struct GT_CudaData {
	CUcontext  cuCtx    = nullptr;             // AE's context (not owned)
	CUstream   cuStream = nullptr;             // AE's stream  (not owned)
	CUmodule   cuModule = nullptr;             // loaded PTX   (owned)
	CUfunction cuFn[GT_K_COUNT] = { nullptr };

	A_u_long   deviceIndex = 0;                // needed to free the bank at setdown

	// Serializes the enqueue sequence AND the cached bank below.
	std::mutex renderLock;
	void      *bank      = nullptr;            // device tile bank (AE-suite owned)
	size_t     bankBytes = 0;                  // allocated size (grow-only)
	uint64_t   bankKey   = 0;                  // analysis identity resident on device
};

// ---------------------------------------------------------------------------
// Asset loading. Resolve CUDA_Assets from THIS module's own path so the PTX
// always travels next to the deployed .aex, whatever the host's CWD is.
// ---------------------------------------------------------------------------
static bool GT_CudaPtxPath(std::wstring &outPath)
{
	wchar_t path[1024];
	HMODULE hm = nullptr;
	if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                          reinterpret_cast<LPCWSTR>(&GT_CudaPtxPath), &hm))
		return false;
	if (::GetModuleFileNameW(hm, path, static_cast<DWORD>(std::size(path))) == 0)
		return false;
	std::wstring dir(path);
	const size_t slash = dir.rfind(L'\\');
	if (slash == std::wstring::npos) return false;
	outPath = dir.substr(0, slash) + L"\\CUDA_Assets\\GrainTransfer_Kernels.ptx";
	return true;
}

// cuModuleLoadDataEx takes a NUL-terminated PTX text image.
static bool GT_ReadTextFile(const std::wstring &path, std::vector<char> &out)
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
// GPU_DEVICE_SETUP / SETDOWN
// ---------------------------------------------------------------------------
PF_Err GT_CudaDeviceSetup(PF_InData *in_data, PF_OutData *out_data,
                          PF_GPUDeviceSetupExtra *extra)
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
	extra->output->gpu_data = nullptr;

	GT_GPU_LOGF("[GrainTransfer] DEVICE_SETUP what_gpu=%d device_index=%u\n",
		(int)extra->input->what_gpu, (unsigned)extra->input->device_index);

	if (!GT_FrameworkSupported(extra->input->what_gpu)) {
		// Not ours. Declining is PF_Err_NONE with gpu_data left null — AE then
		// never offers GPU render for devices of this framework and the CPU
		// reference takes the frame. An error here would surface a user-visible
		// AE failure for what is a normal configuration.
		GT_GPU_LOGF("  -> not CUDA, declined (CPU renders)\n");
		return PF_Err_NONE;
	}

	const GT_CudaApi &cu = GT_CudaApi::Get();
	if (!cu.loaded) {
		GT_GPU_LOGF("  -> nvcuda.dll unavailable - CUDA declined (CPU fallback)\n");
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

		{
			GT_CudaData *data = new (std::nothrow) GT_CudaData();
			if (!data) { err = PF_Err_OUT_OF_MEMORY; goto done; }

			data->cuCtx       = reinterpret_cast<CUcontext>(info.contextPV);
			data->cuStream    = reinterpret_cast<CUstream>(info.command_queuePV);
			data->deviceIndex = extra->input->device_index;

			if (!data->cuCtx) {
				GT_GPU_LOGF("  -> AE provided no CUcontext - CUDA declined\n");
				delete data; goto done;
			}

			std::wstring ptxPath;
			std::vector<char> ptx;
			if (!GT_CudaPtxPath(ptxPath) || !GT_ReadTextFile(ptxPath, ptx)) {
				// Normal on a build machine without the CUDA toolkit: no PTX was
				// produced, so there is nothing to run. Quiet decline, CPU renders.
				GT_GPU_LOGF("  -> PTX missing/unreadable (%ls) - CUDA declined "
				            "(build with the CUDA toolkit installed to produce it)\n",
				            ptxPath.c_str());
				delete data; goto done;
			}

			{
				GT_CudaCtxGuard ctx(cu, data->cuCtx);
				if (!ctx.pushed) {
					GT_GPU_LOGF("  -> cuCtxPushCurrent FAILED - CUDA declined\n");
					delete data; goto done;
				}

				CUresult res = cu.ModuleLoadDataEx(&data->cuModule, ptx.data(),
				                                   0, nullptr, nullptr);
				if (res != GT_CUDA_SUCCESS || !data->cuModule) {
					GT_GPU_LOGF("  -> cuModuleLoadDataEx FAILED (CUresult=%d) - CUDA declined\n",
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
				for (int k = 0; k < GT_K_COUNT; ++k) {
					res = cu.ModuleGetFunction(&data->cuFn[k], data->cuModule,
					                           kGT_CudaKernelNames[k]);
					if (res != GT_CUDA_SUCCESS || !data->cuFn[k]) {
						GT_GPU_LOGF("  -> cuModuleGetFunction(%s) FAILED (CUresult=%d) - "
						            "CUDA declined (stale CUDA_Assets PTX?)\n",
						            kGT_CudaKernelNames[k], (int)res);
						allResolved = false;
						break;
					}
				}
				if (!allResolved) {
					cu.ModuleUnload(data->cuModule);
					data->cuModule = nullptr;
					delete data; goto done;
				}
			}

			extra->output->gpu_data = data;

			// Re-affirm GPU support for THIS device — a plain ASSIGNMENT, not an OR,
			// exactly as Adobe's SDK_Invert_ProcAmp does in every successful backend
			// branch. Without it AE completes GPU_DEVICE_SETUP happily and then
			// offers what_gpu = NONE at every PreRender forever, which is a perfect
			// imitation of "AE doesn't support third-party GPU effects" and is the
			// single mistake that cost the suite seven failed experiments
			// (AE_GPU_Effects.md §2.2 and §8).
			out_data->out_flags2 = PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;

			GT_GPU_LOGF("  -> OK (CUDA ready, %d kernels)\n", (int)GT_K_COUNT);
		}
	}

done:
	ERR2(AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
		kPFGPUDeviceSuiteVersion1, ""));
	return err;
}

PF_Err GT_CudaDeviceSetdown(PF_InData *in_data, PF_OutData *out_data,
                            PF_GPUDeviceSetdownExtra *extra)
{
	GT_CudaData *data = static_cast<GT_CudaData*>(extra->input->gpu_data);
	if (!data) return PF_Err_NONE;

	GT_GPU_LOGF("[GrainTransfer] DEVICE_SETDOWN gpu_data=%p\n", (void*)data);

	// Hand the tile bank back BEFORE unloading the module: the suite owns this
	// memory and outliving the device would leak it for the whole session.
	if (data->bank) {
		PF_GPUDeviceSuite1 *gpuSuite = nullptr;
		if (!AEFX_AcquireSuite(in_data, out_data, kPFGPUDeviceSuite,
				kPFGPUDeviceSuiteVersion1, "",
				reinterpret_cast<void**>(&gpuSuite)) && gpuSuite) {
			gpuSuite->FreeDeviceMemory(in_data->effect_ref, data->deviceIndex,
			                           data->bank);
			AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
			                  kPFGPUDeviceSuiteVersion1, "");
		}
		data->bank      = nullptr;
		data->bankBytes = 0;
		data->bankKey   = 0;
	}

	if (data->cuModule) {
		const GT_CudaApi &cu = GT_CudaApi::Get();
		if (cu.loaded) {
			GT_CudaCtxGuard ctx(cu, data->cuCtx);
			cu.ModuleUnload(data->cuModule);
		}
		data->cuModule = nullptr;
	}
	delete data;
	return PF_Err_NONE;
}

void GT_GPULogPreRenderDecision(PF_GPU_Framework what_gpu, const void *gpu_data,
                                bool raised, bool cached, int outW, int outH)
{
	GT_GPU_LOG_FIRST(s_prerender, 24,
		"[GrainTransfer] PRERENDER what_gpu=%d gpu_data=%p out=%dx%d "
		"analysis_cached=%s -> GPU_RENDER_POSSIBLE=%s\n",
		(int)what_gpu, gpu_data, outW, outH,
		cached ? "YES" : "no", raised ? "YES" : "no");
}

// ---------------------------------------------------------------------------
// Launcher. Every launch goes on AE'S OWN stream and is NOT synchronized here —
// AE owns stream synchronization.
// ---------------------------------------------------------------------------
struct GT_CudaLauncher {
	const GT_CudaApi &cu;
	GT_CudaData      *g;
	CUresult          res    = GT_CUDA_SUCCESS;
	int               failed = -1;

	GT_CudaLauncher(const GT_CudaApi &api, GT_CudaData *gp) : cu(api), g(gp) {}

	// One thread per pixel over a w x h grid. Block size comes from the SHARED
	// prologue constants, not a number chosen here, so the guard in the kernel
	// body and the grid the host computes can never disagree.
	template <typename ArgsT>
	void Launch2D(GT_CudaKernelId k, int w, int h, ArgsT &args)
	{
		if (res != GT_CUDA_SUCCESS || w <= 0 || h <= 0) return;
		void *params[] = { &args };
		res = cu.LaunchKernel(g->cuFn[k],
		                      (unsigned)((w + GT_TG_X - 1) / GT_TG_X),
		                      (unsigned)((h + GT_TG_Y - 1) / GT_TG_Y), 1u,
		                      (unsigned)GT_TG_X, (unsigned)GT_TG_Y, 1u,
		                      0u, g->cuStream, params, nullptr);
		if (res != GT_CUDA_SUCCESS) failed = (int)k;
	}
};

// ---------------------------------------------------------------------------
// The device-resident tile bank. Grow-only allocation, cached on gpu_data and
// re-uploaded ONLY when the analysis identity changes. Caller holds renderLock
// AND has AE's context current.
//
// Why the key rather than a pointer comparison: the host-side bank lives in
// the compute cache and a cache eviction plus re-compute can land a NEW bank at
// the SAME address. The key is derived from the analysis inputs, so it changes
// whenever the contents could have.
// ---------------------------------------------------------------------------
static bool GT_EnsureBank(PF_ProgPtr effect_ref, PF_GPUDeviceSuite1 *gpuSuite,
                          GT_CudaData *g, const GT_CudaApi &cu,
                          const GT_GPUJob &job)
{
	const size_t bytes = job.bankFloats * sizeof(float);
	if (bytes == 0 || !job.bankData) return false;

	if (g->bank && g->bankBytes >= bytes && g->bankKey == job.bankKey)
		return true;   // already resident and current

	if (!g->bank || g->bankBytes < bytes) {
		void *mem = nullptr;
		if (gpuSuite->AllocateDeviceMemory(effect_ref, g->deviceIndex, bytes, &mem) ||
		    !mem) {
			GT_GPU_LOG_FIRST(s_allocFail, 4,
				"[GrainTransfer] AllocateDeviceMemory(%zu bytes) FAILED\n", bytes);
			g->bankKey = 0;
			return false;
		}
		if (g->bank)
			gpuSuite->FreeDeviceMemory(effect_ref, g->deviceIndex, g->bank);
		g->bank      = mem;
		g->bankBytes = bytes;
		g->bankKey   = 0;   // nothing valid in the new block yet
	}

	const CUresult res = cu.MemcpyHtoDAsync(
		reinterpret_cast<CUdeviceptr>(g->bank), job.bankData, bytes, g->cuStream);
	if (res != GT_CUDA_SUCCESS) {
		GT_GPU_LOG_FIRST(s_uploadFail, 4,
			"[GrainTransfer] tile-bank upload FAILED (CUresult=%d)\n", (int)res);
		g->bankKey = 0;
		return false;
	}

	// Ordering against the stamp is already guaranteed — both are enqueued on
	// the SAME stream. The synchronize is about the HOST buffer instead: it
	// belongs to a compute-cache receipt that SmartRenderGPU checks back in as
	// soon as this returns, and an async copy still reading it after that would
	// be a use-after-free. Pageable copies are usually synchronous in practice,
	// which is exactly the kind of "usually" that fails on someone else's
	// machine. This costs one wait per ANALYSIS, not per frame.
	if (cu.StreamSynchronize)
		cu.StreamSynchronize(g->cuStream);

	g->bankKey = job.bankKey;
	GT_GPU_LOG_FIRST(s_upload, 6,
		"[GrainTransfer] tile bank uploaded: %.2f MB (key=%016llx)\n",
		(double)bytes / (1024.0 * 1024.0), (unsigned long long)job.bankKey);
	return true;
}

// ---------------------------------------------------------------------------
// Render. One dispatch over the whole frame — see the file header for why
// there is no band loop here.
// ---------------------------------------------------------------------------
PF_Err GT_CudaRender(PF_InData *in_data, PF_OutData *out_data,
                     const void *gpu_dataPV, A_u_long /*device_index*/,
                     const GT_GPUJob &job,
                     PF_EffectWorld *srcWorld, PF_EffectWorld *outWorld)
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;

	GT_CudaData *data =
		const_cast<GT_CudaData*>(static_cast<const GT_CudaData*>(gpu_dataPV));
	if (!data || !srcWorld || !outWorld) return PF_Err_BAD_CALLBACK_PARAM;

	const GT_CudaApi &cu = GT_CudaApi::Get();
	if (!cu.loaded || !data->cuFn[GT_K_STAMP] || !data->cuFn[GT_K_COPY])
		return PF_Err_INTERNAL_STRUCT_DAMAGED;

	PF_GPUDeviceSuite1 *gpuSuite = nullptr;
	ERR(AEFX_AcquireSuite(in_data, out_data, kPFGPUDeviceSuite,
		kPFGPUDeviceSuiteVersion1, "", reinterpret_cast<void**>(&gpuSuite)));
	if (err || !gpuSuite) return err ? err : PF_Err_BAD_CALLBACK_PARAM;

	{
		// --- World device pointers (for CUDA these ARE the CUdeviceptrs) ----
		void *srcPV = nullptr, *outPV = nullptr;
		ERR(gpuSuite->GetGPUWorldData(in_data->effect_ref, srcWorld, &srcPV));
		ERR(gpuSuite->GetGPUWorldData(in_data->effect_ref, outWorld, &outPV));
		if (err || !srcPV || !outPV) {
			if (!err) err = PF_Err_BAD_CALLBACK_PARAM;
			goto cleanup;
		}

		{
			// GPU worlds are PF_PixelFormat_GPU_BGRA128 — 4 floats per pixel,
			// pitched rows. The kernels index in FLOATS.
			const int srcPitch = (int)(srcWorld->rowbytes / sizeof(float));
			const int dstPitch = (int)(outWorld->rowbytes / sizeof(float));
			if (srcPitch <= 0 || dstPitch <= 0 || job.W <= 0 || job.H <= 0) {
				err = PF_Err_BAD_CALLBACK_PARAM;
				goto cleanup;
			}

			GT_GPU_LOG_FIRST(s_render, 8,
				"[GrainTransfer] SMART_RENDER GPU what_gpu=CUDA %dx%d "
				"passthrough=%d tile=%d/%d n=%d exact=%d enc=%d preview=%d "
				"amount=%.3f trim=%.3f\n",
				job.W, job.H, job.passthrough ? 1 : 0,
				job.tileIndex, job.tileN ? job.bankFloats / (size_t)(3 * job.tileN * job.tileN) : 0,
				job.tileN, job.exact, job.encode, job.preview,
				(double)job.amount, (double)job.trim);

			// The lock covers bank residency AND the enqueue sequence.
			std::lock_guard<std::mutex> lock(data->renderLock);

			// AE's context is pushed BEFORE the allocation, not just around the
			// launches: the suite's device allocator runs driver calls of its own,
			// and the discipline is that a context is current for any driver-call
			// sequence we trigger.
			GT_CudaCtxGuard ctx(cu, data->cuCtx);
			if (!ctx.pushed) { err = PF_Err_INTERNAL_STRUCT_DAMAGED; goto cleanup; }

			GT_CudaLauncher L(cu, data);

			bool stamped = false;
			if (!job.passthrough && job.bankKey != 0 && job.tileN > 0) {
				if (GT_EnsureBank(in_data->effect_ref, gpuSuite, data, cu, job)) {
					ArgsStamp a{};
					a.gGtSrc   = static_cast<const float*>(srcPV);
					a.gGtTiles = static_cast<const float*>(data->bank);
					a.gGtDst   = static_cast<float*>(outPV);
					a.gGtW = job.W;                a.gGtH = job.H;
					a.gGtSrcPitch = srcPitch;      a.gGtDstPitch = dstPitch;
					a.gGtN = job.tileN;
					a.gGtTileIndex = job.tileIndex;
					a.gGtOffX = job.offX;          a.gGtOffY = job.offY;
					a.gGtFlipX = job.flipX;        a.gGtFlipY = job.flipY;
					a.gGtExact = job.exact;
					a.gGtEncode = job.encode;
					a.gGtPreview = job.preview;
					a.gGtInvScale = job.invScale;
					a.gGtAmount = job.amount;
					a.gGtTrim = job.trim;
					a.gGtPreviewBase = job.previewBase;
					GT_StoreResponse(a, job.resp);

					L.Launch2D(GT_K_STAMP, job.W, job.H, a);
					stamped = true;
				}
				// Bank upload failed: fall through to the copy so the frame is
				// the ungrained plate rather than uninitialized device memory.
				// There is no CPU fallback here — the worlds are device memory
				// (AE_GPU_Effects.md §2.4).
			}

			if (!stamped) {
				ArgsCopy c{};
				c.gCpSrc = static_cast<const float*>(srcPV);
				c.gCpDst = static_cast<float*>(outPV);
				c.gCpW = job.W;              c.gCpH = job.H;
				c.gCpSrcPitch = srcPitch;    c.gCpDstPitch = dstPitch;
				L.Launch2D(GT_K_COPY, job.W, job.H, c);
			}

			if (L.res != GT_CUDA_SUCCESS) {
				GT_GPU_LOG_FIRST(s_launchFail, 6,
					"[GrainTransfer] launch FAILED kernel=%s CUresult=%d\n",
					(L.failed >= 0 && L.failed < GT_K_COUNT)
						? kGT_CudaKernelNames[L.failed] : "?",
					(int)L.res);
				err = PF_Err_INTERNAL_STRUCT_DAMAGED;
				goto cleanup;
			}
		}
	}

cleanup:
	ERR2(AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
		kPFGPUDeviceSuiteVersion1, ""));
	return err;
}

} // namespace gtgpu

#endif /* _WIN32 */
