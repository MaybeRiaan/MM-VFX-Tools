/*  qk_gpu_host.h — AE-facing, platform-free GPU host layer for MM Qualify.

    Owns everything about the GPU render that is NOT a graphics API call:
    the band tiling plan (geometry + memory budget) and the render-job
    description the platform hosts execute. mm_gpu_dx.cpp consumes a
    QK_GPUJob and translates it into dispatches; it contains no tiling
    arithmetic of its own. Same architecture as MM FreqSep's mm_gpu_host.h
    (see that header + ../docs/gpu-kernel-rules.md for the full rationale).

    TILING MODEL (the usage-profile requirement: 6K+ footage, memory-bound).
    The output is cut into horizontal bands spanning the FULL source width;
    horizontal blur support is inherited from PreRender's checkout inflation.
    Each band loads marginY extra source rows above and below:

        marginY = dnRy + blRy         (denoise support + matte-blur support)

    Stages consume the margin progressively (denoise-V eats dnRy, matte
    blur-V eats blRy), so rows a later stage reads were computed with full
    tap coverage. Margin rows at INTERIOR band edges are garbage and never
    consumed; at real world edges the tap narrowing IS the CPU path's
    boundary rule, so the result matches the untiled reference exactly —
    tests/pipeline_test.cpp proves this end to end on this very planner.

    Per-band plane set (tileRows tall, srcW wide):
        RGBA: A (frame tile — history frames stream through it, current
              frame loads LAST so compose reads it), + T, Q when denoise on.
        f1:   M (matte), B1 (blur tmp), G (garbage weights, filled 1.0 when
              inactive), + S (IIR state) when temporal on.
    plus one output-sized RGBA compose tile.
*/

#pragma once

#include "AE_Effect.h"

#include "../qk_cpu_prologue.h"
#include "../qk_core.h"

#include <cstddef>
#include <vector>

namespace qkgpu {

// Bands thinner than this make the overlap overhead absurd; below it we
// refuse the GPU path (the CPU reference renders the frame instead).
constexpr int QK_TILE_MIN_BAND = 16;

struct QK_TileBand {
	int outY0;      // first output row of this band
	int outH;       // output rows in this band
	int srcY0;      // first source-world row loaded into the tile
	int srcRows;    // tile height actually loaded (margin included, clipped)
	int loOY;       // output→tile row offset for compose (== outY0+srcOY-srcY0)
};

struct QK_TilePlan {
	std::vector<QK_TileBand> bands;
	int tileRowsMax = 0;   // allocation height for margin-sized tile planes
	int bandHMax    = 0;   // allocation height for the output-sized plane
};

inline int QK_TileMarginY(int dnRy, int blRy) { return dnRy + blRy; }

inline int QK_RGBAPlanes(bool denoise)  { return denoise ? 3 : 1; }
inline int QK_F1Planes(bool temporal)   { return temporal ? 4 : 3; }

// Per-row byte costs, used by the budget solve below and by PreRender's
// feasibility gate (both MUST use these so gate and render agree).
inline size_t QK_TileBytesPerRow(bool denoise, bool temporal, int srcW)
{
	return ((size_t)QK_RGBAPlanes(denoise) * 4u + (size_t)QK_F1Planes(temporal))
		* (size_t)srcW * sizeof(float);
}
inline size_t QK_OutBytesPerRow(int outW)
{
	return (size_t)outW * 4u * sizeof(float);
}

// Cut [0, outH) into bands such that the scratch for one band fits
// budgetBytes; bands rebalanced to near-equal height. Returns false when
// even QK_TILE_MIN_BAND doesn't fit (caller renders on CPU). Identical
// algorithm to MM FreqSep's FS_PlanTiles (proven by its parity suite).
inline bool QK_PlanTiles(int outH, int srcH, int srcOY, int marginY,
                         size_t bytesPerTileRow, size_t bytesPerOutRow,
                         size_t budgetBytes, QK_TilePlan *plan)
{
	if (outH <= 0 || srcH <= 0 || marginY < 0 || !plan) return false;

	const long long fixedCost = 2LL * marginY * (long long)bytesPerTileRow;
	const long long perBand   = (long long)bytesPerTileRow + (long long)bytesPerOutRow;
	long long bandFit = ((long long)budgetBytes - fixedCost) / perBand;
	if (bandFit < QK_TILE_MIN_BAND) return false;
	if (bandFit > outH) bandFit = outH;

	const int nBands = (int)((outH + bandFit - 1) / bandFit);
	const int bandH  = (outH + nBands - 1) / nBands;   // rebalanced, <= bandFit

	plan->bands.clear();
	plan->bands.reserve((size_t)nBands);
	plan->tileRowsMax = 0;
	plan->bandHMax    = 0;

	for (int b = 0; b < nBands; ++b) {
		QK_TileBand band;
		band.outY0 = b * bandH;
		band.outH  = (band.outY0 + bandH <= outH) ? bandH : (outH - band.outY0);

		int y0 = band.outY0 + srcOY - marginY;
		int y1 = band.outY0 + srcOY + band.outH + marginY;
		if (y0 < 0)    y0 = 0;
		if (y1 > srcH) y1 = srcH;

		band.srcY0   = y0;
		band.srcRows = y1 - y0;
		band.loOY    = band.outY0 + srcOY - y0;

		if (band.srcRows <= 0) return false;   // degenerate world — CPU path
		if (band.srcRows > plan->tileRowsMax) plan->tileRowsMax = band.srcRows;
		if (band.outH    > plan->bandHMax)    plan->bandHMax    = band.outH;
		plan->bands.push_back(band);
	}
	return true;
}

// Pack the gate block in the QK_GATES_DECL order — the binding contract
// with the kernels' PARAM_BLOCKs (see qk_core.h).
inline void QK_PackGates(const QK_Gates &g, float *dst)
{
	dst[0]  = g.hueEn;     dst[1]  = g.hueIn;      dst[2]  = g.hueOut;
	dst[3]  = g.hueInSoft; dst[4]  = g.hueOutSoft;
	dst[5]  = g.satEn;     dst[6]  = g.satIn;      dst[7]  = g.satOut;
	dst[8]  = g.satInSoft; dst[9]  = g.satOutSoft;
	dst[10] = g.lumEn;     dst[11] = g.lumIn;      dst[12] = g.lumOut;
	dst[13] = g.lumInSoft; dst[14] = g.lumOutSoft;
	dst[15] = g.lumR;      dst[16] = g.lumG;       dst[17] = g.lumB;
	dst[18] = g.model;     dst[19] = g.qin;
}

// ---------------------------------------------------------------------------
// The render job: everything a platform host needs, fully resolved. Weights
// are the EXACT CPU-built kernels (double-accumulated, rules §3.1); the
// host uploads them verbatim and never rebuilds them.
// ---------------------------------------------------------------------------
struct QK_GPUJob {
	// Geometry (pixels / rows; pitches in floats — rowbytes/4).
	int srcW = 0, srcH = 0, outW = 0, outH = 0;
	int srcOX = 0, srcOY = 0;
	int srcPitchFl = 0, outPitchFl = 0;

	// Denoise pre-filter (qualification input only).
	int dnRx = 0, dnRy = 0;
	std::vector<float> kDnX, kDnY;

	// Matte blur.
	int blRx = 0, blRy = 0;
	std::vector<float> kBlX, kBlY;

	// Gates + per-pixel finesse.
	QK_Gates gates = {};
	float clipB = 0.f, clipW = 1.f;
	float ratio = 0.f;
	int   mode = 0;                 // QK_OUT_* (0-based)

	// Temporal stabilize.
	bool  temporal = false;
	float kAttack = 1.f, kRelease = 1.f;

	// Garbage matte (qk_garbage.kernel). Mirrors qk::QK_GarbageField's
	// layer-space geometry; the world's dims/pitch are read off the world
	// by the platform host. active=false -> the G plane is filled with 1.0.
	struct {
		bool  active = false;
		int   mOriginX = 0, mOriginY = 0;
		int   mRefW = 0, mRefH = 0;
		int   outOriginX = 0, outOriginY = 0;
		int   srcRefW = 0, srcRefH = 0;
		float dsx = 1.f, dsy = 1.f;
		int   channel = 1;
	} garbage;

	QK_TilePlan plan;
};

// ---------------------------------------------------------------------------
// Platform hosts.
//
// CUDA is the backend After Effects actually offers on Windows/NVIDIA and the
// one this suite maintains (../../docs/AE_GPU_Effects.md §1.1-§1.2). The DX12
// host is kept and kept building — it is the Premiere path, where DX12 IS the
// Windows renderer — but AE never reaches it on the test box. A Metal host
// adds the matching three calls.
//
// Both hosts execute the SAME QK_GPUJob through the SAME QK_PlanTiles plan and
// dispatch the SAME .kernel bodies; the pass sequences are meant to stay
// line-for-line identical, and tests/pipeline_test.cpp runs that sequence on
// the CPU so "identical" is checkable without a GPU.
// ---------------------------------------------------------------------------
#ifdef _WIN32

// garbWorld may be null (== job.garbage.active false). histWorlds is in
// oldest-first order (t-N .. t-1); null entries are skipped — the IIR
// warm-starts at the first non-null world, exactly like the CPU path.
//
// TEMPORAL LIFETIME (this plugin's own problem — no other effect in the suite
// has one): the IIR state plane lives for exactly ONE render call. PreRender
// checks out t-1..t-N, SmartRender folds them oldest->newest inside that call,
// and every output frame is therefore a pure function of its own checked-out
// frames. There is no ordering relationship between renders for Multi-Frame
// Rendering to violate. See smart_render.cpp's TEMPORAL STABILIZE block (the
// CPU original) and qk_gpu_cuda.cpp's header note (why a cached, shared state
// plane is still safe).

// ---- CUDA (the AE path) ----
PF_Err QK_CudaDeviceSetup(PF_InData *in_data, PF_OutData *out_data,
                          PF_GPUDeviceSetupExtra *extra);
PF_Err QK_CudaDeviceSetdown(PF_InData *in_data, PF_OutData *out_data,
                            PF_GPUDeviceSetdownExtra *extra);
size_t QK_CudaScratchBudget(const void *gpu_data);
PF_Err QK_CudaRender(PF_InData *in_data, PF_OutData *out_data,
                     const void *gpu_data, A_u_long device_index,
                     const QK_GPUJob &job,
                     PF_EffectWorld *srcWorld, PF_EffectWorld *outWorld,
                     PF_EffectWorld *garbWorld,
                     const std::vector<PF_EffectWorld*> &histWorlds);

// Records the per-frame PreRender GPU decision to
// %TEMP%\MM_Qualify_gpu.log — what AE offered, what the frame needs, and
// whether GPU_RENDER_POSSIBLE was raised. The GPU failure mode is SILENT (the
// effect renders correctly on the CPU), so this log is the decisive signal
// (AE_GPU_Effects.md §5.1). No-op when logging is compiled out.
void QK_GPULogPreRenderDecision(PF_GPU_Framework what_gpu, const void *gpu_data,
                                bool raised, int outW, int outH,
                                double scratchMB, double budgetMB);

// ---- DirectX 12 (the Premiere path; parked for AE, still built) ----
PF_Err QK_DXDeviceSetup(PF_InData *in_data, PF_OutData *out_data,
                        PF_GPUDeviceSetupExtra *extra);
PF_Err QK_DXDeviceSetdown(PF_InData *in_data, PF_OutData *out_data,
                          PF_GPUDeviceSetdownExtra *extra);

// Scratch budget (bytes) the tile planner may spend, from the device data
// created at setup. 0 means the device data is unusable — no GPU render.
size_t QK_DXScratchBudget(const void *gpu_data);

PF_Err QK_DXRender(PF_InData *in_data, PF_OutData *out_data,
                   const void *gpu_data, A_u_long device_index,
                   const QK_GPUJob &job,
                   PF_EffectWorld *srcWorld, PF_EffectWorld *outWorld,
                   PF_EffectWorld *garbWorld,
                   const std::vector<PF_EffectWorld*> &histWorlds);

// ---- framework dispatch ----------------------------------------------------
// The one place that answers "do we have a host for this framework?". Keep
// PreRender, GPU_DEVICE_SETUP/SETDOWN and SMART_RENDER_GPU all asking THIS,
// so a framework can never be accepted at setup and refused at render.
inline bool QK_FrameworkSupported(PF_GPU_Framework what_gpu)
{
	return what_gpu == PF_GPU_Framework_CUDA ||
	       what_gpu == PF_GPU_Framework_DIRECTX;
}

inline size_t QK_ScratchBudget(PF_GPU_Framework what_gpu, const void *gpu_data)
{
	if (!gpu_data) return 0;
	if (what_gpu == PF_GPU_Framework_CUDA)    return QK_CudaScratchBudget(gpu_data);
	if (what_gpu == PF_GPU_Framework_DIRECTX) return QK_DXScratchBudget(gpu_data);
	return 0;
}
#endif // _WIN32

} // namespace qkgpu
