/*  mm_gpu_dx.cpp — DirectX 12 host for the MM Qualify GPU render.

    Executes the QK_GPUJob that smart_render.cpp assembles: per band of the
    tile plan (qk_gpu_host.h), stream the history frames through the temporal
    IIR, load the current source rows, denoise the qualification copy, run
    the gate/cleanup kernel, matte-blur, then ratio+compose and store the
    band. All tiling arithmetic lives in qk_gpu_host.h; this file is
    exclusively D3D12 plumbing. Architecture and resource-state policy mirror
    MM FreqSep's mm_gpu_dx.cpp — see that file + docs/gpu-kernel-rules.md §6.

    RESOURCE-STATE POLICY: every buffer binds as a UAV via root descriptors,
    always. D3D12 buffers promote COMMON->UNORDERED_ACCESS on first use and
    decay on submission completion, so uniform UAV binding means the promotion
    is the only state change any AE-owned resource undergoes. Our own weights
    buffer is the one exception (COPY_DEST->UNORDERED_ACCESS after upload).
    Dispatch hazards are ordered with global UAV barriers.

    MFR: per-render allocator/list/fence/scratch on the stack of QK_DXRender;
    the QK_DXData built at GPU_DEVICE_SETUP is immutable after setup. AE's
    queue is free-threaded by API contract.
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
#include "AE_EffectPixelFormat.h"   // must precede AE_EffectGPUSuites.h
#include "AE_EffectGPUSuites.h"
#include "AE_Macros.h"
#include "AEFX_SuiteHelper.h"

#include "qk_gpu_host.h"

// DXIL blobs generated at build time by dxc (see CMakeLists.txt).
#include "qk_dxil/qk_load_tile.h"
#include "qk_dxil/qk_store_tile.h"
#include "qk_dxil/qk_fill_ones.h"
#include "qk_dxil/qk_gauss_rgba.h"
#include "qk_dxil/qk_gauss_f1.h"
#include "qk_dxil/qk_garbage.h"
#include "qk_dxil/qk_matte.h"
#include "qk_dxil/qk_iir.h"
#include "qk_dxil/qk_compose.h"

using Microsoft::WRL::ComPtr;

namespace qkgpu {

enum {
	QK_PSO_LOAD = 0,
	QK_PSO_STORE,
	QK_PSO_FILL,
	QK_PSO_GAUSS_RGBA,
	QK_PSO_GAUSS_F1,
	QK_PSO_GARBAGE,
	QK_PSO_MATTE,
	QK_PSO_IIR,
	QK_PSO_COMPOSE,
	QK_PSO_COUNT
};

static const struct { const unsigned char *code; size_t size; } kQK_PSOCode[QK_PSO_COUNT] = {
	{ kQK_DXIL_qk_load_tile,   sizeof(kQK_DXIL_qk_load_tile)   },
	{ kQK_DXIL_qk_store_tile,  sizeof(kQK_DXIL_qk_store_tile)  },
	{ kQK_DXIL_qk_fill_ones,   sizeof(kQK_DXIL_qk_fill_ones)   },
	{ kQK_DXIL_qk_gauss_rgba,  sizeof(kQK_DXIL_qk_gauss_rgba)  },
	{ kQK_DXIL_qk_gauss_f1,    sizeof(kQK_DXIL_qk_gauss_f1)    },
	{ kQK_DXIL_qk_garbage,     sizeof(kQK_DXIL_qk_garbage)     },
	{ kQK_DXIL_qk_matte,       sizeof(kQK_DXIL_qk_matte)       },
	{ kQK_DXIL_qk_iir,         sizeof(kQK_DXIL_qk_iir)         },
	{ kQK_DXIL_qk_compose,     sizeof(kQK_DXIL_qk_compose)     },
};

// Root signature: root constants b0 + eight root UAV descriptors u0..u7.
// Num32BitValues must cover the largest PARAM_BLOCK: qk_compose = 7 scalars +
// 20 gate floats = 27; qk_matte = 2 + 20 + 2 = 24; qk_garbage = 19. 30 is a
// safe ceiling for all of them.
enum { QK_ROOT_CONSTS = 30 };

struct QK_DXData {
	ComPtr<ID3D12Device>        device;
	ComPtr<ID3D12CommandQueue>  queue;
	ComPtr<ID3D12RootSignature> rootSig;
	ComPtr<ID3D12PipelineState> pso[QK_PSO_COUNT];
	size_t                      scratchBudget = 0;
};

// Param structs — layout must match the kernels' PARAM_BLOCKs (contiguous
// 4-byte scalars; HLSL packs them tightly onto the root constants).
struct PMove    { INT32 W, Rows, worldY0, pitch; };
struct PFill    { INT32 W, Rows; };
struct PGauss   { INT32 W, Rows, R, horiz; };
struct PMatte {
	INT32 W, Rows;
	float gates[QK_GATES_FLOATS];
	float clipB, clipW;
};
static_assert(sizeof(PMatte) == (2 + QK_GATES_FLOATS + 2) * 4, "PMatte layout");
struct PIIR     { INT32 W, Rows, first; float kA, kR; };
struct PCompose {
	INT32 outW, outH, srcW, srcOX, srcOY, mode; float ratio;
	float gates[QK_GATES_FLOATS];
};
static_assert(sizeof(PCompose) == (7 + QK_GATES_FLOATS) * 4, "PCompose layout");
struct PGarbage {
	INT32 W, Rows, srcY0, srcOX, srcOY;
	INT32 mtW, mtH, mtPitch;
	INT32 mOriginX, mOriginY, mRefW, mRefH;
	INT32 outOriginX, outOriginY, srcRefW, srcRefH;
	float dsx, dsy;
	INT32 channel;
};
static_assert(sizeof(PGarbage) == 19 * 4, "PGarbage layout");

// ---------------------------------------------------------------------------
// GPU_DEVICE_SETUP
// ---------------------------------------------------------------------------
static size_t QK_QueryScratchBudget(ID3D12Device *device)
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

PF_Err QK_DXDeviceSetup(PF_InData *in_data, PF_OutData *out_data,
                        PF_GPUDeviceSetupExtra *extra)
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
	extra->output->gpu_data = nullptr;

	if (extra->input->what_gpu != PF_GPU_Framework_DIRECTX)
		return PF_Err_NONE;

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

		// No shader-doubles requirement in this plugin (no box sums) — every
		// DX12 device qualifies. (MM FreqSep gates on doubles; MM Qualify
		// does not, deliberately.)

		{
			QK_DXData *data = new (std::nothrow) QK_DXData();
			if (!data) { err = PF_Err_OUT_OF_MEMORY; goto done; }
			data->device = device;
			data->queue  = queue;

			{
				D3D12_ROOT_PARAMETER rp[9] = {};
				rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
				rp[0].Constants.ShaderRegister = 0;
				rp[0].Constants.RegisterSpace  = 0;
				rp[0].Constants.Num32BitValues = QK_ROOT_CONSTS;
				rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
				for (int i = 0; i < 8; ++i) {
					rp[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
					rp[1 + i].Descriptor.ShaderRegister = (UINT)i;
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

			for (int i = 0; i < QK_PSO_COUNT; ++i) {
				D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
				pd.pRootSignature     = data->rootSig.Get();
				pd.CS.pShaderBytecode = kQK_PSOCode[i].code;
				pd.CS.BytecodeLength  = kQK_PSOCode[i].size;
				if (FAILED(device->CreateComputePipelineState(&pd,
						IID_PPV_ARGS(&data->pso[i])))) {
					delete data; goto done;
				}
			}

			data->scratchBudget = QK_QueryScratchBudget(device);
			extra->output->gpu_data = data;
		}
	}

done:
	ERR2(AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
		kPFGPUDeviceSuiteVersion1, ""));
	return err;
}

PF_Err QK_DXDeviceSetdown(PF_InData * /*in_data*/, PF_OutData * /*out_data*/,
                          PF_GPUDeviceSetdownExtra *extra)
{
	delete static_cast<QK_DXData*>(extra->input->gpu_data);
	return PF_Err_NONE;
}

size_t QK_DXScratchBudget(const void *gpu_data)
{
	const QK_DXData *data = static_cast<const QK_DXData*>(gpu_data);
	return data ? data->scratchBudget : 0;
}

// ---------------------------------------------------------------------------
// Render helpers
// ---------------------------------------------------------------------------
struct QK_ScratchFree {
	PF_GPUDeviceSuite1 *suite = nullptr;
	PF_ProgPtr          ref   = nullptr;
	A_u_long            idx   = 0;
	void               *mem   = nullptr;
	~QK_ScratchFree() { if (suite && mem) suite->FreeDeviceMemory(ref, idx, mem); }
};
struct QK_EventHandle {
	HANDLE h = nullptr;
	~QK_EventHandle() { if (h) ::CloseHandle(h); }
};

static void QK_Dispatch(ID3D12GraphicsCommandList *cl, ID3D12PipelineState *pso,
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
	b.UAV.pResource = nullptr;
	cl->ResourceBarrier(1, &b);
}

PF_Err QK_DXRender(PF_InData *in_data, PF_OutData *out_data,
                   const void *gpu_dataPV, A_u_long device_index,
                   const QK_GPUJob &job,
                   PF_EffectWorld *srcWorld, PF_EffectWorld *outWorld,
                   PF_EffectWorld *garbWorld,
                   const std::vector<PF_EffectWorld*> &histWorlds)
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;

	const QK_DXData *data = static_cast<const QK_DXData*>(gpu_dataPV);
	if (!data || job.plan.bands.empty()) return PF_Err_BAD_CALLBACK_PARAM;

	PF_GPUDeviceSuite1 *gpuSuite = nullptr;
	ERR(AEFX_AcquireSuite(in_data, out_data, kPFGPUDeviceSuite,
		kPFGPUDeviceSuiteVersion1, "", reinterpret_cast<void**>(&gpuSuite)));
	if (err || !gpuSuite) return err ? err : PF_Err_BAD_CALLBACK_PARAM;

	{
		ID3D12Device *device = data->device.Get();

		void *srcPV = nullptr, *outPV = nullptr, *garbPV = nullptr;
		ERR(gpuSuite->GetGPUWorldData(in_data->effect_ref, srcWorld, &srcPV));
		ERR(gpuSuite->GetGPUWorldData(in_data->effect_ref, outWorld, &outPV));
		if (!err && garbWorld)
			ERR(gpuSuite->GetGPUWorldData(in_data->effect_ref, garbWorld, &garbPV));
		if (err || !srcPV || !outPV || (garbWorld && !garbPV)) goto cleanup;

		{
			const D3D12_GPU_VIRTUAL_ADDRESS srcVA =
				static_cast<ID3D12Resource*>(srcPV)->GetGPUVirtualAddress();
			const D3D12_GPU_VIRTUAL_ADDRESS outVA =
				static_cast<ID3D12Resource*>(outPV)->GetGPUVirtualAddress();
			const D3D12_GPU_VIRTUAL_ADDRESS garbVA =
				garbPV ? static_cast<ID3D12Resource*>(garbPV)->GetGPUVirtualAddress() : 0;

			// History world VAs, in the oldest-first order the host built.
			std::vector<D3D12_GPU_VIRTUAL_ADDRESS> histVA(histWorlds.size(), 0);
			bool anyHist = false;
			for (size_t i = 0; i < histWorlds.size(); ++i) {
				if (!histWorlds[i]) continue;
				void *hpv = nullptr;
				if (gpuSuite->GetGPUWorldData(in_data->effect_ref, histWorlds[i], &hpv)
						== PF_Err_NONE && hpv) {
					histVA[i] = static_cast<ID3D12Resource*>(hpv)->GetGPUVirtualAddress();
					anyHist = true;
				}
			}

			// --- Weights: one upload + one device buffer, 16-float aligned.
			auto alignF = [](size_t n) { return (n + 15u) & ~(size_t)15u; };
			const size_t offDnX = 0;
			const size_t offDnY = offDnX + alignF(job.kDnX.size());
			const size_t offBlX = offDnY + alignF(job.kDnY.size());
			const size_t offBlY = offBlX + alignF(job.kBlX.size());
			const size_t weightFloats = offBlY + alignF(job.kBlY.size());

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
				if (!job.kDnX.empty()) std::memcpy(w + offDnX, job.kDnX.data(), job.kDnX.size() * sizeof(float));
				if (!job.kDnY.empty()) std::memcpy(w + offDnY, job.kDnY.data(), job.kDnY.size() * sizeof(float));
				if (!job.kBlX.empty()) std::memcpy(w + offBlX, job.kBlX.data(), job.kBlX.size() * sizeof(float));
				if (!job.kBlY.empty()) std::memcpy(w + offBlY, job.kBlY.data(), job.kBlY.size() * sizeof(float));
				wUpload->Unmap(0, nullptr);
			}

			// --- Scratch: RGBA tile planes + f1 planes + output tile. -----
			auto align256 = [](size_t n) { return (n + 255u) & ~(size_t)255u; };
			const bool denoise = (job.dnRx > 0 || job.dnRy > 0);
			const int nRGBA = QK_RGBAPlanes(denoise);   // A(+T,Q)
			const int nF1   = QK_F1Planes(job.temporal); // M,B1,G(+S)
			const size_t rgbaBytes = align256((size_t)job.plan.tileRowsMax * job.srcW * 4 * sizeof(float));
			const size_t f1Bytes   = align256((size_t)job.plan.tileRowsMax * job.srcW * sizeof(float));
			const size_t outBytes  = align256((size_t)job.plan.bandHMax * job.outW * 4 * sizeof(float));
			const size_t scratchBytes = (size_t)nRGBA * rgbaBytes + (size_t)nF1 * f1Bytes + outBytes;

			QK_ScratchFree scratch;
			scratch.suite = gpuSuite;
			scratch.ref   = in_data->effect_ref;
			scratch.idx   = device_index;
			ERR(gpuSuite->AllocateDeviceMemory(in_data->effect_ref, device_index,
				scratchBytes, &scratch.mem));
			if (err || !scratch.mem) { if (!err) err = PF_Err_OUT_OF_MEMORY; goto cleanup; }

			ID3D12Resource *scratchRes = static_cast<ID3D12Resource*>(scratch.mem);
			const D3D12_GPU_VIRTUAL_ADDRESS base = scratchRes->GetGPUVirtualAddress();

			// Plane VAs. RGBA: A [,T,Q]. f1: M, B1, G [,S].
			const D3D12_GPU_VIRTUAL_ADDRESS vaA = base;
			const D3D12_GPU_VIRTUAL_ADDRESS vaT = denoise ? base + rgbaBytes : 0;
			const D3D12_GPU_VIRTUAL_ADDRESS vaQ = denoise ? base + 2 * rgbaBytes : vaA;
			const D3D12_GPU_VIRTUAL_ADDRESS f1base = base + (size_t)nRGBA * rgbaBytes;
			const D3D12_GPU_VIRTUAL_ADDRESS vaM  = f1base;
			const D3D12_GPU_VIRTUAL_ADDRESS vaB1 = f1base + f1Bytes;
			const D3D12_GPU_VIRTUAL_ADDRESS vaG  = f1base + 2 * f1Bytes;
			const D3D12_GPU_VIRTUAL_ADDRESS vaS  = job.temporal ? f1base + 3 * f1Bytes : 0;
			const D3D12_GPU_VIRTUAL_ADDRESS vaOut = f1base + (size_t)nF1 * f1Bytes;
			const D3D12_GPU_VIRTUAL_ADDRESS vaW   = wDevice ? wDevice->GetGPUVirtualAddress() : 0;

			// --- Per-render command objects (MFR-safe) --------------------
			const D3D12_COMMAND_LIST_TYPE listType = data->queue->GetDesc().Type;
			ComPtr<ID3D12CommandAllocator>    alloc;
			ComPtr<ID3D12GraphicsCommandList> cl;
			ComPtr<ID3D12Fence>               fence;
			QK_EventHandle                    evt;
			if (FAILED(device->CreateCommandAllocator(listType, IID_PPV_ARGS(&alloc))) ||
				FAILED(device->CreateCommandList(0, listType, alloc.Get(), nullptr, IID_PPV_ARGS(&cl))) ||
				FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
				(evt.h = ::CreateEventW(nullptr, FALSE, FALSE, nullptr)) == nullptr) {
				err = PF_Err_INTERNAL_STRUCT_DAMAGED; goto cleanup;
			}

			cl->SetComputeRootSignature(data->rootSig.Get());

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

			// Gaussian helper (RGBA or f1) — in/tmp/out planes, X then Y.
			auto gaussRGBA = [&](D3D12_GPU_VIRTUAL_ADDRESS in, D3D12_GPU_VIRTUAL_ADDRESS tmp,
			                     D3D12_GPU_VIRTUAL_ADDRESS out, int rows,
			                     int rx, int ry, size_t koX, size_t koY) {
				D3D12_GPU_VIRTUAL_ADDRESS cur = in;
				if (rx > 0) {
					PGauss p = { job.srcW, rows, rx, 1 };
					const D3D12_GPU_VIRTUAL_ADDRESS ins[2] = { cur, vaW + koX * sizeof(float) };
					D3D12_GPU_VIRTUAL_ADDRESS dst = (ry > 0) ? tmp : out;
					QK_Dispatch(cl.Get(), data->pso[QK_PSO_GAUSS_RGBA].Get(), &p, 4,
						ins, 2, &dst, 1, job.srcW, rows);
					cur = dst;
				}
				if (ry > 0) {
					PGauss p = { job.srcW, rows, ry, 0 };
					const D3D12_GPU_VIRTUAL_ADDRESS ins[2] = { cur, vaW + koY * sizeof(float) };
					QK_Dispatch(cl.Get(), data->pso[QK_PSO_GAUSS_RGBA].Get(), &p, 4,
						ins, 2, &out, 1, job.srcW, rows);
				} else if (rx == 0 && out != in) {
					// no-op copy handled by caller choosing out==in; nothing here
				}
			};

			// Assemble the gate block once (identical for every dispatch).
			float gatePacked[QK_GATES_FLOATS];
			QK_PackGates(job.gates, gatePacked);

			// ---------------- Per band ----------------
			for (const QK_TileBand &band : job.plan.bands) {
				const int rows = band.srcRows;

				// Garbage weights for the tile (active) or fill 1.0 (inactive).
				if (job.garbage.active && garbVA) {
					PGarbage p = {};
					p.W = job.srcW; p.Rows = rows; p.srcY0 = band.srcY0;
					p.srcOX = job.srcOX; p.srcOY = job.srcOY;
					p.mtW = garbWorld->width; p.mtH = garbWorld->height;
					p.mtPitch = (INT32)(garbWorld->rowbytes / sizeof(float));
					p.mOriginX = job.garbage.mOriginX; p.mOriginY = job.garbage.mOriginY;
					p.mRefW = job.garbage.mRefW; p.mRefH = job.garbage.mRefH;
					p.outOriginX = job.garbage.outOriginX; p.outOriginY = job.garbage.outOriginY;
					p.srcRefW = job.garbage.srcRefW; p.srcRefH = job.garbage.srcRefH;
					p.dsx = job.garbage.dsx; p.dsy = job.garbage.dsy;
					p.channel = job.garbage.channel;
					QK_Dispatch(cl.Get(), data->pso[QK_PSO_GARBAGE].Get(), &p, 19,
						&garbVA, 1, &vaG, 1, job.srcW, rows);
				} else {
					PFill p = { job.srcW, rows };
					QK_Dispatch(cl.Get(), data->pso[QK_PSO_FILL].Get(), &p, 2,
						nullptr, 0, &vaG, 1, job.srcW, rows);
				}

				// TEMPORAL: stream history frames oldest-first through the IIR.
				bool haveState = false;
				if (job.temporal) {
					for (size_t hi = 0; hi < histVA.size(); ++hi) {
						if (!histVA[hi]) continue;
						// Load history frame -> A.
						{
							PMove p = { job.srcW, rows, band.srcY0, job.srcPitchFl };
							QK_Dispatch(cl.Get(), data->pso[QK_PSO_LOAD].Get(), &p, 4,
								&histVA[hi], 1, &vaA, 1, job.srcW, rows);
						}
						// Denoise -> Q (or Q == A).
						if (denoise)
							gaussRGBA(vaA, vaT, vaQ, rows, job.dnRx, job.dnRy, offDnX, offDnY);
						// Gates + cleanup -> M.
						{
							PMatte p = {};
							p.W = job.srcW; p.Rows = rows;
							std::memcpy(p.gates, gatePacked, sizeof(gatePacked));
							p.clipB = job.clipB; p.clipW = job.clipW;
							const D3D12_GPU_VIRTUAL_ADDRESS ins[2] = { vaQ, vaG };
							QK_Dispatch(cl.Get(), data->pso[QK_PSO_MATTE].Get(), &p,
								sizeof(PMatte) / 4, ins, 2, &vaM, 1, job.srcW, rows);
						}
						// IIR step: state S <- M.
						{
							PIIR p = { job.srcW, rows, haveState ? 0 : 1, job.kAttack, job.kRelease };
							QK_Dispatch(cl.Get(), data->pso[QK_PSO_IIR].Get(), &p, 5,
								&vaM, 1, &vaS, 1, job.srcW, rows);
						}
						haveState = true;
					}
				}

				// Current frame -> A (loaded LAST so compose reads it).
				{
					PMove p = { job.srcW, rows, band.srcY0, job.srcPitchFl };
					QK_Dispatch(cl.Get(), data->pso[QK_PSO_LOAD].Get(), &p, 4,
						&srcVA, 1, &vaA, 1, job.srcW, rows);
				}
				if (denoise)
					gaussRGBA(vaA, vaT, vaQ, rows, job.dnRx, job.dnRy, offDnX, offDnY);

				// Current-frame matte -> M.
				{
					PMatte p = {};
					p.W = job.srcW; p.Rows = rows;
					std::memcpy(p.gates, gatePacked, sizeof(gatePacked));
					p.clipB = job.clipB; p.clipW = job.clipW;
					const D3D12_GPU_VIRTUAL_ADDRESS ins[2] = { vaQ, vaG };
					QK_Dispatch(cl.Get(), data->pso[QK_PSO_MATTE].Get(), &p,
						sizeof(PMatte) / 4, ins, 2, &vaM, 1, job.srcW, rows);
				}

				// Final IIR fold: state <- current matte; then M := state.
				D3D12_GPU_VIRTUAL_ADDRESS matteVA = vaM;
				if (job.temporal && haveState) {
					PIIR p = { job.srcW, rows, 0, job.kAttack, job.kRelease };
					QK_Dispatch(cl.Get(), data->pso[QK_PSO_IIR].Get(), &p, 5,
						&vaM, 1, &vaS, 1, job.srcW, rows);
					matteVA = vaS;
				}

				// Matte blur (edge-neutral f1). A separable pass reads neighbors,
				// so a dispatch's input and output must never be the same plane.
				// Ping-pong between the incoming matte plane (matteVA) and vaB1:
				// X reads matteVA writes vaB1; Y reads vaB1 writes matteVA (free
				// once X has consumed it). A single-axis blur lands in vaB1. This
				// needs no third plane — in != out holds on every dispatch.
				D3D12_GPU_VIRTUAL_ADDRESS finalMatte = matteVA;
				if (job.blRx > 0 || job.blRy > 0) {
					D3D12_GPU_VIRTUAL_ADDRESS cur = matteVA;
					if (job.blRx > 0) {
						PGauss p = { job.srcW, rows, job.blRx, 1 };
						const D3D12_GPU_VIRTUAL_ADDRESS ins[2] = { cur, vaW + offBlX * sizeof(float) };
						QK_Dispatch(cl.Get(), data->pso[QK_PSO_GAUSS_F1].Get(), &p, 4,
							ins, 2, &vaB1, 1, job.srcW, rows);
						cur = vaB1;
					}
					if (job.blRy > 0) {
						// out = matteVA when it is free (X ran, or matteVA != cur);
						// with Y-only, cur == matteVA so we must land in vaB1.
						D3D12_GPU_VIRTUAL_ADDRESS dst = (cur == matteVA) ? vaB1 : matteVA;
						PGauss p = { job.srcW, rows, job.blRy, 0 };
						const D3D12_GPU_VIRTUAL_ADDRESS ins[2] = { cur, vaW + offBlY * sizeof(float) };
						QK_Dispatch(cl.Get(), data->pso[QK_PSO_GAUSS_F1].Get(), &p, 4,
							ins, 2, &dst, 1, job.srcW, rows);
						cur = dst;
					}
					finalMatte = cur;
				}

				// Ratio + compose -> output tile, through the band's loOY.
				{
					PCompose p = {};
					p.outW = job.outW; p.outH = band.outH; p.srcW = job.srcW;
					p.srcOX = job.srcOX; p.srcOY = band.loOY; p.mode = job.mode;
					p.ratio = job.ratio;
					std::memcpy(p.gates, gatePacked, sizeof(gatePacked));
					const D3D12_GPU_VIRTUAL_ADDRESS ins[4] = { vaA, finalMatte, vaQ, vaG };
					QK_Dispatch(cl.Get(), data->pso[QK_PSO_COMPOSE].Get(), &p,
						sizeof(PCompose) / 4, ins, 4, &vaOut, 1, job.outW, band.outH);
				}

				// Store the band.
				{
					PMove p = { job.outW, band.outH, band.outY0, job.outPitchFl };
					QK_Dispatch(cl.Get(), data->pso[QK_PSO_STORE].Get(), &p, 4,
						&vaOut, 1, &outVA, 1, job.outW, band.outH);
				}
			}

			if (FAILED(cl->Close())) { err = PF_Err_INTERNAL_STRUCT_DAMAGED; goto cleanup; }
			ID3D12CommandList *lists[] = { cl.Get() };
			data->queue->ExecuteCommandLists(1, lists);
			if (FAILED(data->queue->Signal(fence.Get(), 1)) ||
				FAILED(fence->SetEventOnCompletion(1, evt.h))) {
				err = PF_Err_INTERNAL_STRUCT_DAMAGED; goto cleanup;
			}
			::WaitForSingleObject(evt.h, INFINITE);
			(void)anyHist;
		}
	}

cleanup:
	ERR2(AEFX_ReleaseSuite(in_data, out_data, kPFGPUDeviceSuite,
		kPFGPUDeviceSuiteVersion1, ""));
	return err;
}

} // namespace qkgpu

#endif // _WIN32
