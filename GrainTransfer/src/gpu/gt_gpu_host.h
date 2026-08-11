/*  gt_gpu_host.h — the GPU host interface GrainTransfer.cpp talks to.

    ONE backend: CUDA. After Effects on Windows/NVIDIA offers effects
    what_gpu = CUDA and never probes DIRECTX (../../../docs/AE_GPU_Effects.md
    §1.1/§1.2), and unlike MM_Qualify and MM_FreqSep this plugin was ported
    after that was measured, so it was never aimed at the wrong one. An HLSL
    prologue still exists so one entry point keeps type-checking through dxc
    (§4 item 5) — that is a compile-time guard, not a second host.

    Everything here is Windows-only and compiled out elsewhere.
*/

#pragma once

#ifdef _WIN32

#include <cstdint>

#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectPixelFormat.h"   // must precede AE_EffectGPUSuites.h (PF_PixelFormat)
#include "AE_EffectGPUSuites.h"

#include "../gt_cpu_prologue.h"
#include "../gt_core.h"             // GT_RESP_FLOATS

namespace gtgpu {

// ---------------------------------------------------------------------------
// Everything one GPU frame needs. Deliberately POD and self-contained: it is
// assembled in SmartRender, where the analysis is already in hand, and the
// host below never reaches back into AE state to fill a gap.
//
// NOTE what is NOT here: any notion of tiles, bands or margins. The stamp
// reads no image neighbourhood, so the whole frame is one dispatch. See
// gpu/kernels/gt_stamp.kernel's header.
// ---------------------------------------------------------------------------
struct GT_GPUJob {
	int      W = 0, H = 0;          // intersection of src and out worlds

	// --- tile bank (device-cached; uploaded only when bankKey changes) ---
	uint64_t bankKey    = 0;        // analysis identity — 0 means "no bank"
	const float *bankData = nullptr; // host-side bank, count x 3 x n x n floats
	size_t   bankFloats = 0;
	int      tileN      = 0;

	// --- per-frame stamp state (rides in the kernel argument struct) ---
	int      tileIndex = 0, offX = 0, offY = 0;
	int      flipX = 0, flipY = 0, exact = 1;
	int      encode = 0, preview = 0;
	float    invScale = 1.f, amount = 1.f, trim = 1.f, previewBase = 0.5f;
	float    resp[GT_RESP_FLOATS] = { 0 };

	// True when this frame has nothing to stamp (no analysis, or the effect is
	// a no-op) and the output must simply carry the input across. On the CPU
	// that is a no-op in place; here the output world is a different
	// allocation, so it is a real copy — hence gt_copy.
	bool     passthrough = true;
};

// Does this build host the framework AE is driving? CUDA only.
bool GT_FrameworkSupported(PF_GPU_Framework what_gpu);

PF_Err GT_CudaDeviceSetup(PF_InData *in_data, PF_OutData *out_data,
                          PF_GPUDeviceSetupExtra *extra);
PF_Err GT_CudaDeviceSetdown(PF_InData *in_data, PF_OutData *out_data,
                            PF_GPUDeviceSetdownExtra *extra);

PF_Err GT_CudaRender(PF_InData *in_data, PF_OutData *out_data,
                     const void *gpu_dataPV, A_u_long device_index,
                     const GT_GPUJob &job,
                     PF_EffectWorld *srcWorld, PF_EffectWorld *outWorld);

// PreRender's decision, logged. Grain Transfer's gate is the ANALYSIS CACHE,
// not a memory budget — see GrainTransfer.cpp's PreRender for why that is the
// one legitimate withholding condition in this suite.
// Records a CPU SmartRender into MM_GPU's own log, so one file answers "did
// the GPU take this frame". A CPU frame here is frequently correct — see §4.
void GT_GPULogSmartRenderCPU(int outW, int outH, int fullRes, int cached);

void GT_GPULogPreRenderDecision(PF_GPU_Framework what_gpu, const void *gpu_data,
                                bool raised, bool cached, int outW, int outH);

} // namespace gtgpu

#endif /* _WIN32 */
