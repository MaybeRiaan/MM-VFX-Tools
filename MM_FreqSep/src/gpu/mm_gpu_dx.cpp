/*  mm_gpu_dx.cpp — DirectX 12 host for the MM FreqSep GPU render.

    Executes the FS_GPUJob that smart_render.cpp assembles: per band of the
    tile plan (mm_gpu_host.h), load source rows from AE's GPU world, run the
    platform-free kernels (separation → smooth → matte → compose), store the
    band back. All tiling arithmetic lives in mm_gpu_host.h; this file is
    exclusively D3D12 plumbing.

    CONTRACT WITH AE (established by the SDK's SDK_Invert_ProcAmp DX path):
      - PF_GPUDeviceInfo.devicePV / command_queuePV are ID3D12Device* /
        ID3D12CommandQueue*; the queue accepts COMPUTE-type command lists.
      - PF_GPUDeviceSuite1::GetGPUWorldData / AllocateDeviceMemory return
        ID3D12Resource* (committed buffers).
      - Worlds are PF_PixelFormat_GPU_BGRA128, premultiplied, pitched
        (world->rowbytes is valid).
      - Full-GPU entry points hold exclusive device access implicitly; no
        Acquire/ReleaseExclusiveDeviceAccess needed (AE_EffectGPUSuites.h).

    RESOURCE-STATE POLICY (why there are no state transitions here):
    every buffer is bound as a UAV via root descriptors, always. D3D12 buffers
    implicitly promote COMMON→UNORDERED_ACCESS on first use and decay back to
    COMMON when a submission completes, so binding uniformly as UAV means the
    promotion is the only state change any AE-owned resource ever undergoes.
    The one exception is our own weights buffer, which is copy-uploaded first
    and explicitly transitioned COPY_DEST→UNORDERED_ACCESS — a resource we
    created, whose state we know. Write-after-write/read hazards between
    dispatches are ordered with global UAV barriers.

    MFR: everything per-render (allocator, list, fence, scratch) lives on the
    stack of FS_DXRender; the FS_DXData created at GPU_DEVICE_SETUP is
    immutable after setup. Two render threads may execute concurrently — the
    shared ID3D12CommandQueue is free-threaded by API contract.
*/

#ifdef _WIN32

#define NOMINMAX
#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl.h>

#include <cstring>
#include <new>
#include <vector>

#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectPixelFormat.h"   // must precede AE_EffectGPUSuites.h (PF_PixelFormat)
#include "AE_EffectGPUSuites.h"
#include "AE_Macros.h"
#include "AEFX_SuiteHelper.h"

#include "mm_gpu_host.h"

// DXIL blobs generated at build time by dxc (see CMakeLists.txt): one header
// per entry point, each defining const unsigned char kFS_DXIL_<entry>[].
#include "fs_dxil/fs_gauss_pass.h"
#include "fs_dxil/fs_guided_box.h"
#include "fs_dxil/fs_guided_squares.h"
#include "fs_dxil/fs_guided_ab.h"
#include "fs_dxil/fs_guided_final.h"
#include "fs_dxil/fs_compose.h"
#include "fs_dxil/fs_load_tile.h"
#include "fs_dxil/fs_store_tile.h"
#include "fs_dxil/fs_fill_ones.h"
#include "fs_dxil/fs_matte.h"

using Microsoft::WRL::ComPtr;

namespace fsgpu {

// ---------------------------------------------------------------------------
// Pipeline states, one per kernel entry point.
// ---------------------------------------------------------------------------
enum {
	FS_PSO_GAUSS = 0,
	FS_PSO_GBOX,
	FS_PSO_GSQUARES,
	FS_PSO_GAB,
	FS_PSO_GFINAL,
	FS_PSO_COMPOSE,
	FS_PSO_LOAD,
	FS_PSO_STORE,
	FS_PSO_FILL,
	FS_PSO_MATTE,
	FS_PSO_COUNT
};

static const struct { const unsigned char *code; size_t size; } kFS_PSOCode[FS_PSO_COUNT] = {
	{ kFS_DXIL_fs_gauss_pass,     sizeof(kFS_DXIL_fs_gauss_pass)     },
	{ kFS_DXIL_fs_guided_box,     sizeof(kFS_DXIL_fs_guided_box)     },
	{ kFS_DXIL_fs_guided_squares, sizeof(kFS_DXIL_fs_guided_squares) },
	{ kFS_DXIL_fs_guided_ab,      sizeof(kFS_DXIL_fs_guided_ab)      },
	{ kFS_DXIL_fs_guided_final,   sizeof(kFS_DXIL_fs_guided_final)   },
	{ kFS_DXIL_fs_compose,        sizeof(kFS_DXIL_fs_compose)        },
	{ kFS_DXIL_fs_load_tile,      sizeof(kFS_DXIL_fs_load_tile)      },
	{ kFS_DXIL_fs_store_tile,     sizeof(kFS_DXIL_fs_store_tile)     },
	{ kFS_DXIL_fs_fill_ones,      sizeof(kFS_DXIL_fs_fill_ones)      },
	{ kFS_DXIL_fs_matte,          sizeof(kFS_DXIL_fs_matte)          },
};

// Root signature layout — must mirror the prologue's register map:
//   param 0     : root constants b0 (FS_ROOT_CONSTS DWORDs; largest cbuffer
//                 is fs_matte's 18 — see the P* structs below)
//   params 1..4 : root UAV descriptors u0..u3  (BUF_IN slots 0..3)
//   params 5..8 : root UAV descriptors u4..u7  (BUF_OUT slots 0..3)
enum { FS_ROOT_CONSTS = 24 };

// The effect-owned gpu_data. Immutable after GPU_DEVICE_SETUP.
struct FS_DXData {
	ComPtr<ID3D12Device>        device;
	ComPtr<ID3D12CommandQueue>  queue;
	ComPtr<ID3D12RootSignature> rootSig;
	ComPtr<ID3D12PipelineState> pso[FS_PSO_COUNT];
	size_t                      scratchBudget = 0;
};

// ---------------------------------------------------------------------------
// Param structs — layout must match the kernels' PARAM_BLOCKs (all 4-byte
// scalars, sequential; HLSL packs contiguous scalars tightly, so a plain C
// struct of int32/float maps 1:1 onto the root constants).
// ---------------------------------------------------------------------------
struct PPass    { INT32 W, H, R, horiz; };                       // gauss + guided box
struct PDims    { INT32 W, H; };                                 // fs_guided_squares
struct PAB      { INT32 W, H; float eps; };                      // fs_guided_ab
struct PFinal   { INT32 W, H; float invCount; };                 // fs_guided_final
struct PCompose { INT32 outW, outH, srcW, srcOX, srcOY, view; float texGain, mix; };
struct PMove    { INT32 W, H, worldY0, pitch; };                 // load / store
struct PFill    { INT32 W, H; };
struct PMatte   {
	INT32 outW, outH, outY0;
	INT32 mtW, mtH, mtPitch;
	INT32 mOriginX, mOriginY, mRefW, mRefH;
	INT32 outOriginX, outOriginY, srcRefW, srcRefH;
	float dsx, dsy;
	INT32 channel, fit;
};
static_assert(sizeof(PMatte) == 18 * 4, "PMatte must stay 18 DWORDs (root constants)");

// ---------------------------------------------------------------------------
// GPU_DEVICE_SETUP — build root signature + PSOs once per device.
// ---------------------------------------------------------------------------

// VRAM-derived scratch budget: a quarter of dedicated VRAM, clamped to
// [256MB, 1GB]. AE and other effects share the card; the tile planner turns
// whatever this yields into a band height, so the exact number only moves the
// overlap overhead, never correctness. Query failure falls back to 384MB.
static size_t FS_QueryScratchBudget(ID3D12Device *device)
{
	const size_t kFallback = 384ull << 20;
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return kFallback;

	const LUID luid = device->GetAdapterLuid();
	for (UINT i = 0;; ++i) {
		ComPtr<IDXGIAdapter1> adapter;
		if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
		DXGI_ADAPTER_DESC1 desc;
		if (FAILED(adapter->GetDesc1(&desc))) continue;
		if (desc.AdapterLuid.LowPart == luid.LowPart &&
			desc.AdapterLuid.HighPart == luid.HighPart) {
			size_t budget = (size_t)(desc.DedicatedVideoMemory / 4);
			if (budget < (256ull << 20)) budget = 256ull << 20;
			if (budget > (1024ull << 20)) budget = 1024ull << 20;
			return budget;
		}
	}
	return kFallback;
}

PF_Err FS_DXDeviceSetup(PF_InData *in_data, PF_OutData *out_data,
                        PF_GPUDeviceSetupExtra *extra)
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
	extra->output->gpu_data = nullptr;

	if (extra->input->what_gpu != PF_GPU_Framework_DIRECTX)
		return PF_Err_NONE;   // no gpu_data → AE keeps this effect on the CPU

	PF_GPUDeviceSuite1 *gpuSuite = nullptr;
	ERR(AEFX_AcquireSuite(in_data, out_data, kPFGPUDeviceSuite,
		kPFGPUDeviceSuiteVersion1, "", reinterpret_cast<void**>(&gpuSuite)));
	if (err || !gpuSuite) return err;

	{
		PF_GPUDeviceInfo info;
		AEFX_CLR_STRUCT(info);
		ERR(gpuSuite->GetDeviceInfo(in_data->effect_ref,
			extra->input->device_index, &info));

		ID3D12Device       *device = static_cast<ID3D12Device*>(info.devicePV);
		ID3D12CommandQueue *queue  = static_cast<ID3D12CommandQueue*>(info.command_queuePV);
		if (err || !device || !queue) goto done;

		// fs_guided_box accumulates in double (rules §3.7). No doubles on this
		// device → no GPU path at all: silently correct (CPU) beats subtly
		// wrong. Every desktop-class GPU of the last decade supports this.
		{
			D3D12_FEATURE_DATA_D3D12_OPTIONS opts = {};
			if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,
					&opts, sizeof(opts))) ||
				!opts.DoublePrecisionFloatShaderOps)
				goto done;
		}

		FS_DXData *data = new (std::nothrow) FS_DXData();
		if (!data) { err = PF_Err_OUT_OF_MEMORY; goto done; }
		data->device = device;   // ComPtr AddRef — keeps AE's device alive vs our PSOs
		data->queue  = queue;

		// Root signature (see layout note above).
		{
			D3D12_ROOT_PARAMETER rp[9] = {};
			rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			rp[0].Constants.ShaderRegister = 0;
			rp[0].Constants.RegisterSpace  = 0;
			rp[0].Constants.Num32BitValues = FS_ROOT_CONSTS;
			rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			for (int i = 0; i < 8; ++i) {
				rp[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
				rp[1 + i].Descriptor.ShaderRegister = (UINT)i;   // u0..u7
				rp[1 + i].Descriptor.RegisterSpace  = 0;
				rp[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			}
			D3D12_ROOT_SIGNATURE_DESC desc = {};
			desc.NumParameters = 9;
			desc.pParameters   = rp;
			desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

			ComPtr<ID3DBlob> blob, errBlob;
			if (FAILED(D3D12SerializeRootSignature(&desc,
					D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errBlob)) ||
				FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(),
					blob->GetBufferSize(), IID_PPV_ARGS(&data->rootSig)))) {
				delete data; goto done;
			}
		}

		// One compute PSO per kernel entry point.
		for (int i = 0; i < FS_PSO_COUNT; ++i) {
			D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
			pd.pRootSignature    = data->rootSig.Get();
			pd.CS.pShaderBytecode = kFS_PSOCode[i].code;
			pd.CS.BytecodeLength  = kFS_PSOCode[i].size;
			if (FAILED(device->CreateComputePipelineState(&pd,
					IID_PPV_ARGS(&data->pso[i])))) {
				delete data; goto done;
			}
		}

		data->scratchBudget = FS_QueryScratchBudget(device);
		extra->output->gpu_data = data;
	}

done:
	ERR2(AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
		kPFGPUDeviceSuiteVersion1, ""));
	return err;
}

PF_Err FS_DXDeviceSetdown(PF_InData * /*in_data*/, PF_OutData * /*out_data*/,
                          PF_GPUDeviceSetdownExtra *extra)
{
	delete static_cast<FS_DXData*>(extra->input->gpu_data);
	return PF_Err_NONE;
}

size_t FS_DXScratchBudget(const void *gpu_data)
{
	const FS_DXData *data = static_cast<const FS_DXData*>(gpu_data);
	return data ? data->scratchBudget : 0;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

// Small RAII wrappers so error paths need no goto over C++ initializations.
struct FS_ScratchFree {
	PF_GPUDeviceSuite1 *suite = nullptr;
	PF_ProgPtr          ref   = nullptr;
	A_u_long            idx   = 0;
	void               *mem   = nullptr;
	~FS_ScratchFree() { if (suite && mem) suite->FreeDeviceMemory(ref, idx, mem); }
};
struct FS_EventHandle {
	HANDLE h = nullptr;
	~FS_EventHandle() { if (h) ::CloseHandle(h); }
};

// Record one dispatch: constants, input UAVs (u0..), output UAVs (u4..),
// ceil-divided 8x8 grid, then a global UAV barrier — every dispatch in this
// pipeline feeds the next, so ordering all UAV work is both correct and free
// of before-state bookkeeping.
static void FS_Dispatch(ID3D12GraphicsCommandList *cl, ID3D12PipelineState *pso,
                        const void *consts, UINT nConsts,
                        const D3D12_GPU_VIRTUAL_ADDRESS *ins, UINT nIns,
                        const D3D12_GPU_VIRTUAL_ADDRESS *outs, UINT nOuts,
                        int W, int H)
{
	cl->SetPipelineState(pso);
	cl->SetComputeRoot32BitConstants(0, nConsts, consts, 0);
	for (UINT i = 0; i < nIns; ++i)
		cl->SetComputeRootUnorderedAccessView(1 + i, ins[i]);
	for (UINT i = 0; i < nOuts; ++i)
		cl->SetComputeRootUnorderedAccessView(5 + i, outs[i]);
	cl->Dispatch((UINT)((W + 7) / 8), (UINT)((H + 7) / 8), 1);

	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	b.UAV.pResource = nullptr;   // all UAV accesses
	cl->ResourceBarrier(1, &b);
}

PF_Err FS_DXRender(PF_InData *in_data, PF_OutData *out_data,
                   const void *gpu_dataPV, A_u_long device_index,
                   const FS_GPUJob &job,
                   PF_EffectWorld *srcWorld, PF_EffectWorld *outWorld,
                   PF_EffectWorld *matteWorld)
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;

	const FS_DXData *data = static_cast<const FS_DXData*>(gpu_dataPV);
	if (!data || job.plan.bands.empty()) return PF_Err_BAD_CALLBACK_PARAM;

	PF_GPUDeviceSuite1 *gpuSuite = nullptr;
	ERR(AEFX_AcquireSuite(in_data, out_data, kPFGPUDeviceSuite,
		kPFGPUDeviceSuiteVersion1, "", reinterpret_cast<void**>(&gpuSuite)));
	if (err || !gpuSuite) return err ? err : PF_Err_BAD_CALLBACK_PARAM;

	{
		ID3D12Device *device = data->device.Get();

		// --- World buffers (ID3D12Resource*, see contract note) -------------
		void *srcPV = nullptr, *outPV = nullptr, *mattePV = nullptr;
		ERR(gpuSuite->GetGPUWorldData(in_data->effect_ref, srcWorld, &srcPV));
		ERR(gpuSuite->GetGPUWorldData(in_data->effect_ref, outWorld, &outPV));
		if (!err && matteWorld)
			ERR(gpuSuite->GetGPUWorldData(in_data->effect_ref, matteWorld, &mattePV));
		if (err || !srcPV || !outPV || (matteWorld && !mattePV)) goto cleanup;

		{
			ID3D12Resource *srcRes   = static_cast<ID3D12Resource*>(srcPV);
			ID3D12Resource *outRes   = static_cast<ID3D12Resource*>(outPV);
			ID3D12Resource *matteRes = static_cast<ID3D12Resource*>(mattePV);

			const D3D12_GPU_VIRTUAL_ADDRESS srcVA = srcRes->GetGPUVirtualAddress();
			const D3D12_GPU_VIRTUAL_ADDRESS outVA = outRes->GetGPUVirtualAddress();
			const D3D12_GPU_VIRTUAL_ADDRESS matteVA =
				matteRes ? matteRes->GetGPUVirtualAddress() : 0;

			// --- Weights: one upload + one device buffer, offsets per kernel
			// (16-float aligned so every root descriptor address is aligned).
			auto alignF = [](size_t n) { return (n + 15u) & ~(size_t)15u; };
			const size_t offSepX = 0;
			const size_t offSepY = offSepX + alignF(job.kSepX.size());
			const size_t offSmX  = offSepY + alignF(job.kSepY.size());
			const size_t offSmY  = offSmX  + alignF(job.kSmX.size());
			const size_t weightFloats = offSmY + alignF(job.kSmY.size());

			ComPtr<ID3D12Resource> wUpload, wDevice;
			if (weightFloats > 0) {
				D3D12_RESOURCE_DESC bd = {};
				bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
				bd.Width            = weightFloats * sizeof(float);
				bd.Height           = 1;
				bd.DepthOrArraySize = 1;
				bd.MipLevels        = 1;
				bd.SampleDesc.Count = 1;
				bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

				D3D12_HEAP_PROPERTIES up = {}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
				D3D12_HEAP_PROPERTIES df = {}; df.Type = D3D12_HEAP_TYPE_DEFAULT;

				if (FAILED(device->CreateCommittedResource(&up,
						D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
						nullptr, IID_PPV_ARGS(&wUpload))) ||
					FAILED(device->CreateCommittedResource(&df,
						D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
						nullptr, IID_PPV_ARGS(&wDevice)))) {
					err = PF_Err_OUT_OF_MEMORY; goto cleanup;
				}

				void *mapped = nullptr;
				if (FAILED(wUpload->Map(0, nullptr, &mapped))) {
					err = PF_Err_INTERNAL_STRUCT_DAMAGED; goto cleanup;
				}
				std::memset(mapped, 0, weightFloats * sizeof(float));
				float *w = static_cast<float*>(mapped);
				if (!job.kSepX.empty()) std::memcpy(w + offSepX, job.kSepX.data(), job.kSepX.size() * sizeof(float));
				if (!job.kSepY.empty()) std::memcpy(w + offSepY, job.kSepY.data(), job.kSepY.size() * sizeof(float));
				if (!job.kSmX.empty())  std::memcpy(w + offSmX,  job.kSmX.data(),  job.kSmX.size()  * sizeof(float));
				if (!job.kSmY.empty())  std::memcpy(w + offSmY,  job.kSmY.data(),  job.kSmY.size()  * sizeof(float));
				wUpload->Unmap(0, nullptr);
			}

			// --- Scratch: one device allocation, planes at 256B offsets -----
			auto align256 = [](size_t n) { return (n + 255u) & ~(size_t)255u; };
			const int    nPlanes    = FS_TilePlaneCount(job.guided);
			const size_t planeBytes = align256((size_t)job.plan.tileRowsMax * job.srcW * 4 * sizeof(float));
			const size_t outBytes   = align256((size_t)job.plan.bandHMax * job.outW * 4 * sizeof(float));
			const size_t matteBytes = align256((size_t)job.plan.bandHMax * job.outW * sizeof(float));
			const size_t scratchBytes = (size_t)nPlanes * planeBytes + outBytes + matteBytes;

			FS_ScratchFree scratch;
			scratch.suite = gpuSuite;
			scratch.ref   = in_data->effect_ref;
			scratch.idx   = device_index;
			ERR(gpuSuite->AllocateDeviceMemory(in_data->effect_ref, device_index,
				scratchBytes, &scratch.mem));
			if (err || !scratch.mem) { if (!err) err = PF_Err_OUT_OF_MEMORY; goto cleanup; }

			ID3D12Resource *scratchRes = static_cast<ID3D12Resource*>(scratch.mem);
			const D3D12_GPU_VIRTUAL_ADDRESS scratchVA = scratchRes->GetGPUVirtualAddress();

			// Plane VAs. Both methods: plane 0 = tile source A, last = lo L.
			// Gaussian: A, T, L. Guided: A, P1, P2, P3, L (see mm_gpu_host.h).
			D3D12_GPU_VIRTUAL_ADDRESS plane[5] = {};
			for (int i = 0; i < nPlanes; ++i)
				plane[i] = scratchVA + (size_t)i * planeBytes;
			const D3D12_GPU_VIRTUAL_ADDRESS vaA    = plane[0];
			const D3D12_GPU_VIRTUAL_ADDRESS vaL    = plane[nPlanes - 1];
			const D3D12_GPU_VIRTUAL_ADDRESS vaOut   = scratchVA + (size_t)nPlanes * planeBytes;
			const D3D12_GPU_VIRTUAL_ADDRESS vaMatte = vaOut + outBytes;
			const D3D12_GPU_VIRTUAL_ADDRESS vaWeights = wDevice ? wDevice->GetGPUVirtualAddress() : 0;

			// --- Per-render command objects (MFR-safe: nothing shared) ------
			const D3D12_COMMAND_LIST_TYPE listType = data->queue->GetDesc().Type;
			ComPtr<ID3D12CommandAllocator>    alloc;
			ComPtr<ID3D12GraphicsCommandList> cl;
			ComPtr<ID3D12Fence>               fence;
			FS_EventHandle                    evt;
			if (FAILED(device->CreateCommandAllocator(listType, IID_PPV_ARGS(&alloc))) ||
				FAILED(device->CreateCommandList(0, listType, alloc.Get(), nullptr, IID_PPV_ARGS(&cl))) ||
				FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
				(evt.h = ::CreateEventW(nullptr, FALSE, FALSE, nullptr)) == nullptr) {
				err = PF_Err_INTERNAL_STRUCT_DAMAGED; goto cleanup;
			}

			cl->SetComputeRootSignature(data->rootSig.Get());

			// Weights upload: the only explicit state transition in the file.
			if (wDevice) {
				cl->CopyBufferRegion(wDevice.Get(), 0, wUpload.Get(), 0,
					weightFloats * sizeof(float));
				D3D12_RESOURCE_BARRIER tb = {};
				tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				tb.Transition.pResource   = wDevice.Get();
				tb.Transition.Subresource = 0;
				tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
				tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
				cl->ResourceBarrier(1, &tb);
			}

			// Inactive matte: fill the whole weight plane with 1.0 once.
			if (!job.matte.active) {
				PFill p = { job.outW, job.plan.bandHMax };
				FS_Dispatch(cl.Get(), data->pso[FS_PSO_FILL].Get(), &p, 2,
					nullptr, 0, &vaMatte, 1, job.outW, job.plan.bandHMax);
			}

			// ---------------- Per band ----------------
			for (const FS_TileBand &band : job.plan.bands) {
				const int rows = band.srcRows;

				// Load source rows (BGRA world → packed ARGB tile A).
				{
					PMove p = { job.srcW, rows, band.srcY0, job.srcPitchFl };
					FS_Dispatch(cl.Get(), data->pso[FS_PSO_LOAD].Get(), &p, 4,
						&srcVA, 1, &vaA, 1, job.srcW, rows);
				}

				D3D12_GPU_VIRTUAL_ADDRESS vaLo = vaA;   // no separation → lo == src

				if (job.guided && (job.guidedRx > 0 || job.guidedRy > 0)) {
					// Guided: squares → box(sq) → box(src) → ab → box(sa)/box(sb)
					// → final. Ping-pong schedule from mm_gpu_host.h.
					const D3D12_GPU_VIRTUAL_ADDRESS P1 = plane[1], P2 = plane[2], P3 = plane[3];
					auto box = [&](D3D12_GPU_VIRTUAL_ADDRESS in,
					               D3D12_GPU_VIRTUAL_ADDRESS out, int R, int horiz) {
						PPass p = { job.srcW, rows, R, horiz };
						FS_Dispatch(cl.Get(), data->pso[FS_PSO_GBOX].Get(), &p, 4,
							&in, 1, &out, 1, job.srcW, rows);
					};

					{	// squares: A → L  (L is dead until final writes it)
						PDims p = { job.srcW, rows };
						FS_Dispatch(cl.Get(), data->pso[FS_PSO_GSQUARES].Get(), &p, 2,
							&vaA, 1, &vaL, 1, job.srcW, rows);
					}
					box(vaL, P3, job.guidedRx, 1);   // box(sq) H
					box(P3, P2, job.guidedRy, 0);    //          V → P2 = s2
					box(vaA, P3, job.guidedRx, 1);   // box(src) H
					box(P3, P1, job.guidedRy, 0);    //          V → P1 = s1

					{	// ab: (A, s1, s2) → (P3 = sa, L = sb)
						PAB p = { job.srcW, rows, job.guidedEps };
						const D3D12_GPU_VIRTUAL_ADDRESS ins[3]  = { vaA, P1, P2 };
						const D3D12_GPU_VIRTUAL_ADDRESS outs[2] = { P3, vaL };
						FS_Dispatch(cl.Get(), data->pso[FS_PSO_GAB].Get(), &p, 3,
							ins, 3, outs, 2, job.srcW, rows);
					}
					box(P3, P1, job.guidedRx, 1);    // box(sa) H
					box(P1, P2, job.guidedRy, 0);    //         V → P2 = boxed sa
					box(vaL, P1, job.guidedRx, 1);   // box(sb) H
					box(P1, P3, job.guidedRy, 0);    //         V → P3 = boxed sb

					{	// final: (A, boxed sa, boxed sb) → L = lo
						PFinal p = { job.srcW, rows, job.guidedInvCount };
						const D3D12_GPU_VIRTUAL_ADDRESS ins[3] = { vaA, P2, P3 };
						FS_Dispatch(cl.Get(), data->pso[FS_PSO_GFINAL].Get(), &p, 3,
							ins, 3, &vaL, 1, job.srcW, rows);
					}
					vaLo = vaL;
				} else if (!job.guided && (job.sepRx > 0 || job.sepRy > 0)) {
					// Gaussian separation. Planes: A, T (=plane[1]), L.
					const D3D12_GPU_VIRTUAL_ADDRESS vaT = plane[1];
					auto gauss = [&](D3D12_GPU_VIRTUAL_ADDRESS in, size_t kOff,
					                 D3D12_GPU_VIRTUAL_ADDRESS out, int R, int horiz) {
						PPass p = { job.srcW, rows, R, horiz };
						const D3D12_GPU_VIRTUAL_ADDRESS ins[2] =
							{ in, vaWeights + kOff * sizeof(float) };
						FS_Dispatch(cl.Get(), data->pso[FS_PSO_GAUSS].Get(), &p, 4,
							ins, 2, &out, 1, job.srcW, rows);
					};
					if (job.sepRx > 0 && job.sepRy > 0) {
						gauss(vaA, offSepX, vaT, job.sepRx, 1);
						gauss(vaT, offSepY, vaL, job.sepRy, 0);
					} else if (job.sepRx > 0) {
						gauss(vaA, offSepX, vaL, job.sepRx, 1);
					} else {
						gauss(vaA, offSepY, vaL, job.sepRy, 0);
					}
					vaLo = vaL;
				}

				// Low Band Smooth cascade (always Gaussian, both methods).
				// Tmp plane: plane[1] is dead by now in either schedule.
				if (job.smoothRx > 0 || job.smoothRy > 0) {
					const D3D12_GPU_VIRTUAL_ADDRESS vaT = plane[1];
					auto gauss = [&](D3D12_GPU_VIRTUAL_ADDRESS in, size_t kOff,
					                 D3D12_GPU_VIRTUAL_ADDRESS out, int R, int horiz) {
						PPass p = { job.srcW, rows, R, horiz };
						const D3D12_GPU_VIRTUAL_ADDRESS ins[2] =
							{ in, vaWeights + kOff * sizeof(float) };
						FS_Dispatch(cl.Get(), data->pso[FS_PSO_GAUSS].Get(), &p, 4,
							ins, 2, &out, 1, job.srcW, rows);
					};
					if (job.smoothRx > 0 && job.smoothRy > 0) {
						gauss(vaLo, offSmX, vaT, job.smoothRx, 1);
						gauss(vaT, offSmY, vaL, job.smoothRy, 0);
						vaLo = vaL;
					} else if (job.smoothRx > 0) {
						gauss(vaLo, offSmX, vaT, job.smoothRx, 1);
						vaLo = vaT;
					} else {
						gauss(vaLo, offSmY, vaT, job.smoothRy, 0);
						vaLo = vaT;
					}
				}

				// Matte weights for this band (active matte only — inactive
				// was filled once above).
				if (job.matte.active && matteRes) {
					PMatte p = {};
					p.outW = job.outW; p.outH = band.outH; p.outY0 = band.outY0;
					p.mtW = matteWorld->width; p.mtH = matteWorld->height;
					p.mtPitch = (INT32)(matteWorld->rowbytes / sizeof(float));
					p.mOriginX = job.matte.mOriginX;   p.mOriginY = job.matte.mOriginY;
					p.mRefW = job.matte.mRefW;         p.mRefH = job.matte.mRefH;
					p.outOriginX = job.matte.outOriginX; p.outOriginY = job.matte.outOriginY;
					p.srcRefW = job.matte.srcRefW;     p.srcRefH = job.matte.srcRefH;
					p.dsx = job.matte.dsx;             p.dsy = job.matte.dsy;
					p.channel = job.matte.channel;     p.fit = job.matte.fit;
					FS_Dispatch(cl.Get(), data->pso[FS_PSO_MATTE].Get(), &p, 18,
						&matteVA, 1, &vaMatte, 1, job.outW, band.outH);
				}

				// Compose the band (reads A + lo through the local offset).
				{
					PCompose p = { job.outW, band.outH, job.srcW,
					               job.srcOX, band.loOY, job.view,
					               job.texGain, job.mix };
					const D3D12_GPU_VIRTUAL_ADDRESS ins[3] = { vaA, vaLo, vaMatte };
					FS_Dispatch(cl.Get(), data->pso[FS_PSO_COMPOSE].Get(), &p, 8,
						ins, 3, &vaOut, 1, job.outW, band.outH);
				}

				// Store the band (packed ARGB tile → BGRA world rows).
				{
					PMove p = { job.outW, band.outH, band.outY0, job.outPitchFl };
					FS_Dispatch(cl.Get(), data->pso[FS_PSO_STORE].Get(), &p, 4,
						&vaOut, 1, &outVA, 1, job.outW, band.outH);
				}
			}

			// Submit and wait; scratch must outlive GPU execution (RAII frees
			// it after this block).
			if (FAILED(cl->Close())) { err = PF_Err_INTERNAL_STRUCT_DAMAGED; goto cleanup; }
			ID3D12CommandList *lists[] = { cl.Get() };
			data->queue->ExecuteCommandLists(1, lists);
			if (FAILED(data->queue->Signal(fence.Get(), 1)) ||
				FAILED(fence->SetEventOnCompletion(1, evt.h))) {
				err = PF_Err_INTERNAL_STRUCT_DAMAGED; goto cleanup;
			}
			::WaitForSingleObject(evt.h, INFINITE);
		}
	}

cleanup:
	ERR2(AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
		kPFGPUDeviceSuiteVersion1, ""));
	return err;
}

} // namespace fsgpu

#endif // _WIN32
