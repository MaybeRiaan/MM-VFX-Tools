/*  mm_gpu_host.h — AE-facing, platform-free GPU host layer.

    Owns everything about the GPU render that is NOT a graphics API call:
    the band tiling plan (geometry + memory budget) and the render-job
    description the platform hosts execute. mm_gpu_dx.cpp (and later
    mm_gpu_metal.mm) consume an FS_GPUJob and translate it into dispatches;
    they contain no tiling arithmetic of their own.

    TILING MODEL (the usage-profile requirement: 6K+ footage, memory-bound).
    The output is cut into horizontal bands. Tiles span the FULL source-world
    width, so all HORIZONTAL blur support is inherited from PreRender's
    checkout inflation (srcOX) and needs no per-tile handling. VERTICAL
    support is the tile's problem: each band loads marginY extra source rows
    above and below, where

        marginY = (guided ? 2*guidedRy : sepRy) + smoothRy

    — exactly FS_InflateAmount's vertical term. Stages consume the margin
    progressively (sep-V eats sepRy, smooth-V eats smoothRy; guided's two
    cascaded box passes eat 2*guidedRy), so the rows a later stage reads are
    always rows an earlier stage computed with full tap coverage. Rows inside
    the margin that fall at an INTERIOR band edge get edge-narrowed taps and
    are garbage — and are never consumed. Where a band edge coincides with a
    real world edge, the narrowing IS the CPU path's zero-extension, so the
    result matches the untiled reference exactly. tests/gpu_parity_test.cpp
    proves this end to end by running the tiled pipeline against the untiled
    CPU reference on this very header's plan.

    All logic here is pure integer/float math — no D3D, no Metal, no AE
    suites — precisely so those tests can run with no GPU and no AE host.
*/

#pragma once

#include "AE_Effect.h"

#include <cstddef>
#include <vector>

namespace fsgpu {

// Bands thinner than this make the overlap overhead absurd; below it we
// refuse the GPU path (PreRender then simply doesn't offer GPU render and
// the CPU reference renders the frame — slower, never wrong).
constexpr int FS_TILE_MIN_BAND = 16;

// One horizontal band: the output rows it produces and the source rows it loads.
struct FS_TileBand {
	int outY0;      // first output row of this band
	int outH;       // output rows in this band
	int srcY0;      // first source-world row loaded into the tile
	int srcRows;    // tile height actually loaded (margin included, clipped)
	int loOY;       // output→tile row offset for compose (== outY0+srcOY-srcY0)
};

struct FS_TilePlan {
	std::vector<FS_TileBand> bands;
	int tileRowsMax = 0;   // allocation height for margin-sized tile planes
	int bandHMax    = 0;   // allocation height for output-sized planes
};

// Margin-sized ARGB tile planes a method needs:
//   Gaussian: src, tmp, lo                          = 3
//   Guided:   src, s1, s2, tmp, lo (ping-ponged)    = 5
// The guided plan reaches the "3 scratch planes, not 5" goal by scheduling —
// the A/B stage's outputs land in planes whose previous contents are dead —
// rather than by aliased in-place writes, which a single DX12 dispatch cannot
// express (a resource can't be bound SRV and UAV at once). Same memory
// footprint, no aliasing hazard for Metal to inherit.
inline int FS_TilePlaneCount(bool guided) { return guided ? 5 : 3; }

// Vertical read reach of the full cascade, in rows (see header comment).
inline int FS_TileMarginY(bool guided, int sepRy, int guidedRy, int smoothRy)
{
	return (guided ? 2 * guidedRy : sepRy) + smoothRy;
}

// Per-row byte costs, used by the budget solve below and by PreRender's
// feasibility gate (both MUST use these so gate and render agree).
inline size_t FS_TileBytesPerRow(bool guided, int srcW)
{
	return (size_t)FS_TilePlaneCount(guided) * (size_t)srcW * 4u * sizeof(float);
}
inline size_t FS_OutBytesPerRow(int outW)
{
	// ARGB compose target + single-float matte weight plane.
	return (size_t)outW * 4u * sizeof(float) + (size_t)outW * sizeof(float);
}

// Cut [0, outH) into bands such that the scratch for one band —
//   (bandH + 2*marginY) * bytesPerTileRow  +  bandH * bytesPerOutRow
// — fits budgetBytes. Bands are rebalanced to near-equal height so the last
// band isn't a sliver. Returns false when even FS_TILE_MIN_BAND doesn't fit
// (caller renders on CPU). srcOY is the recovered output→source offset;
// srcH clips the margins at real world edges.
inline bool FS_PlanTiles(int outH, int srcH, int srcOY, int marginY,
                         size_t bytesPerTileRow, size_t bytesPerOutRow,
                         size_t budgetBytes, FS_TilePlan *plan)
{
	if (outH <= 0 || srcH <= 0 || marginY < 0 || !plan) return false;

	const long long fixedCost = 2LL * marginY * (long long)bytesPerTileRow;
	const long long perBand   = (long long)bytesPerTileRow + (long long)bytesPerOutRow;
	long long bandFit = ((long long)budgetBytes - fixedCost) / perBand;
	if (bandFit < FS_TILE_MIN_BAND) return false;
	if (bandFit > outH) bandFit = outH;

	const int nBands = (int)((outH + bandFit - 1) / bandFit);
	const int bandH  = (outH + nBands - 1) / nBands;   // rebalanced, <= bandFit

	plan->bands.clear();
	plan->bands.reserve((size_t)nBands);
	plan->tileRowsMax = 0;
	plan->bandHMax    = 0;

	for (int b = 0; b < nBands; ++b) {
		FS_TileBand band;
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

// ---------------------------------------------------------------------------
// The render job: everything a platform host needs, fully resolved. Weights
// are the EXACT CPU-built kernels (blur.cpp BuildKernel, double-accumulated —
// rules §3.1); the host uploads them verbatim and never rebuilds them.
// ---------------------------------------------------------------------------
struct FS_GPUJob {
	// Geometry (pixels / rows; pitches in floats — rowbytes/4).
	int srcW = 0, srcH = 0, outW = 0, outH = 0;
	int srcOX = 0, srcOY = 0;
	int srcPitchFl = 0, outPitchFl = 0;

	// Separation.
	bool guided = false;
	int  sepRx = 0, sepRy = 0;              // Gaussian support radii
	std::vector<float> kSepX, kSepY;        // 2R+1 weights (empty when R==0)
	int   guidedRx = 0, guidedRy = 0;       // box radii
	float guidedEps = 0.f;
	float guidedInvCount = 0.f;             // 1/((2rx+1)*(2ry+1)), float like CPU

	// Low Band Smooth cascade (always Gaussian).
	int smoothRx = 0, smoothRy = 0;
	std::vector<float> kSmX, kSmY;

	// Compose.
	int   view = 0;                         // kernel-space: 0=Result 1=Low 2=High 3=Matte
	float texGain = 1.f, mix = 1.f;

	// Matte driver (fs_matte.kernel). Mirrors fs::FS_MatteField's layer-space
	// geometry; the matte WORLD's dims/pitch are read off the world itself by
	// the platform host. active=false → the weight plane is filled with 1.0.
	struct {
		bool  active = false;
		int   mOriginX = 0, mOriginY = 0;
		int   mRefW = 0, mRefH = 0;
		int   outOriginX = 0, outOriginY = 0;
		int   srcRefW = 0, srcRefH = 0;
		float dsx = 1.f, dsy = 1.f;
		int   channel = 1, fit = 1;
	} matte;

	FS_TilePlan plan;
};

// ---------------------------------------------------------------------------
// Platform hosts. Each exposes the same three calls and consumes the same
// FS_GPUJob; smart_render.cpp picks one on extra->input->what_gpu and knows
// nothing else about them. Setup returns gpu_data == null (with PF_Err_NONE)
// for frameworks it does not handle — AE then simply never offers GPU render
// on devices of that framework, and the CPU reference takes the frame.
//
// CUDA is the backend After Effects actually offers on Windows/NVIDIA and the
// one this suite maintains; DirectX serves Premiere hosts, where DX12 IS the
// Windows renderer. See ../../docs/AE_GPU_Effects.md §1.1–1.2. Metal later adds
// a third set with the same signatures.
// ---------------------------------------------------------------------------
#ifdef _WIN32
// --- CUDA (mm_gpu_cuda.cpp) — the After Effects path --------------------
PF_Err FS_CudaDeviceSetup(PF_InData *in_data, PF_OutData *out_data,
                          PF_GPUDeviceSetupExtra *extra);
PF_Err FS_CudaDeviceSetdown(PF_InData *in_data, PF_OutData *out_data,
                            PF_GPUDeviceSetdownExtra *extra);
size_t FS_CudaScratchBudget(const void *gpu_data);
PF_Err FS_CudaRender(PF_InData *in_data, PF_OutData *out_data,
                     const void *gpu_data, A_u_long device_index,
                     const FS_GPUJob &job,
                     PF_EffectWorld *srcWorld, PF_EffectWorld *outWorld,
                     PF_EffectWorld *matteWorld);

// Records the per-frame PreRender GPU decision to %TEMP%\MM_FreqSep_gpu.log —
// what AE offered, what the frame needs, and whether GPU_RENDER_POSSIBLE was
// raised. This is THE decisive signal: the GPU failure mode is silent (the
// effect renders correctly on the CPU), so the frame tells you nothing
// (../../docs/AE_GPU_Effects.md §5.1). `what_gpu=0` on every line means the
// gate is shut. No-op when logging is compiled out.
void FS_GPULogPreRenderDecision(PF_GPU_Framework what_gpu, const void *gpu_data,
                                bool raised, int outW, int outH,
                                double scratchMB, double budgetMB);

// --- DirectX 12 (mm_gpu_dx.cpp) — the Premiere path ---------------------
PF_Err FS_DXDeviceSetup(PF_InData *in_data, PF_OutData *out_data,
                        PF_GPUDeviceSetupExtra *extra);
PF_Err FS_DXDeviceSetdown(PF_InData *in_data, PF_OutData *out_data,
                          PF_GPUDeviceSetdownExtra *extra);

// Scratch budget (bytes) the tile planner may spend, from the device data
// created at setup. 0 means the device data is unusable — no GPU render.
size_t FS_DXScratchBudget(const void *gpu_data);

// matteWorld may be null (== job.matte.active false).
PF_Err FS_DXRender(PF_InData *in_data, PF_OutData *out_data,
                   const void *gpu_data, A_u_long device_index,
                   const FS_GPUJob &job,
                   PF_EffectWorld *srcWorld, PF_EffectWorld *outWorld,
                   PF_EffectWorld *matteWorld);

// --- Framework dispatch -------------------------------------------------
// The ONE place that maps a PF_GPU_Framework onto a backend. Everything else
// in smart_render.cpp asks these rather than testing frameworks itself, so
// adding Metal touches this header and nothing in the render path.
inline bool FS_FrameworkSupported(PF_GPU_Framework what_gpu)
{
	return what_gpu == PF_GPU_Framework_CUDA ||
	       what_gpu == PF_GPU_Framework_DIRECTX;
}

inline size_t FS_ScratchBudget(PF_GPU_Framework what_gpu, const void *gpu_data)
{
	if (!gpu_data) return 0;
	if (what_gpu == PF_GPU_Framework_CUDA)    return FS_CudaScratchBudget(gpu_data);
	if (what_gpu == PF_GPU_Framework_DIRECTX) return FS_DXScratchBudget(gpu_data);
	return 0;
}
#endif // _WIN32

} // namespace fsgpu
