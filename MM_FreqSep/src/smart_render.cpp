/*  smart_render.cpp — MM FreqSep SmartFX render path (Phase 2: CPU reference).

    Pipeline (all math in 32-bit float regardless of input depth; 8/16bpc
    are promoted on unpack and clamped ONLY on the final pack):

        src_pm  = unpack(input)                  // premultiplied ARGB float
        lo_pm   = gauss(src_pm, sigma_sep)       // alpha-weighted: pm planes
        lo_pm   = gauss(lo_pm,  sigma_smooth)    // Low Band Smooth cascade
        per output pixel, in STRAIGHT color (renormalize by blurred alpha):
            src_st = src_rgb / src_a
            lo_st  = lo_rgb  / lo_a              // == weighted avg of straight color
            hi_st  = src_st - lo_st
            proc   = lo_st + texture_gain * hi_st
            out_st = lerp(src_st, proc, matte * mix)     // matte stubs to 1
            out    = { src_a, out_st * src_a }           // alpha untouched
        pack(out)

    Renormalizing by the BLURRED alpha is what makes the blur alpha-weighted:
    transparent neighbors contribute no color, so feathered mask edges and
    layer boundaries don't fringe. Identity: Texture=100 / Smooth=0 / Mix=100
    → proc == src_st → out == src (to float rounding, ~2 ulp).

    RECT LOGIC (the #1 regression risk — every decision commented):
    every output pixel reads input up to inflate = ceil(3*sigma_sep) +
    ceil(3*sigma_smooth) away PER AXIS (cascaded Gaussians: the smooth pass
    reads lo pixels which themselves read src pixels — supports ADD).
    PreRender expands the checkout request by that amount; AE clamps the
    request to real layer data, and SmartRender recovers the actual offset
    from the returned world's size. The output result rect is intersected
    back to the original request so our alpha extent never grows.

    Sigmas are the PARAM values scaled by downsample (num/den ≤ 1) — the
    plan's effective_radius rule — with X and Y scaled independently so
    non-square pixel-aspect previews stay correct.

    MFR-safe: everything a render needs lives on the stack or in the
    pre_render_data block AE owns; no globals, no statics with state.
*/

#define NOMINMAX
#include "params.h"
#include "blur.h"
#include "guided.h"
#include "matte.h"
#include "curve.h"
#include "fs_core.h"
#include "fs_common.h"
#include "gpu/mm_gpu_host.h"

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <new>

// ---------------------------------------------------------------------------
// Param checkout — one snapshot per render call, never cached.
// ---------------------------------------------------------------------------
static PF_Err
ReadParams(PF_InData *in_data, FS_Params *p)
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
	PF_ParamDef def;

	#define FS_GET_FLOAT(IDX, FLD) \
		AEFX_CLR_STRUCT(def); \
		ERR(PF_CHECKOUT_PARAM(in_data, IDX, in_data->current_time, \
			in_data->time_step, in_data->time_scale, &def)); \
		p->FLD = def.u.fs_d.value; \
		ERR2(PF_CHECKIN_PARAM(in_data, &def));

	#define FS_GET_POPUP(IDX, FLD) \
		AEFX_CLR_STRUCT(def); \
		ERR(PF_CHECKOUT_PARAM(in_data, IDX, in_data->current_time, \
			in_data->time_step, in_data->time_scale, &def)); \
		p->FLD = def.u.pd.value; \
		ERR2(PF_CHECKIN_PARAM(in_data, &def));

	FS_GET_FLOAT(FS_RADIUS,     radiusF)
	FS_GET_FLOAT(FS_LO_SMOOTH,  smoothF)
	FS_GET_FLOAT(FS_HI_TEXTURE, textureF)
	FS_GET_FLOAT(FS_MIX,        mixF)
	FS_GET_FLOAT(FS_EDGE,       edgeF)
	FS_GET_POPUP(FS_MATTE_CHANNEL, matteChanL)
	FS_GET_POPUP(FS_MATTE_FIT,     matteFitL)
	FS_GET_POPUP(FS_METHOD,        methodL)
	FS_GET_POPUP(FS_VIEW,          viewL)
	FS_GET_POPUP(FS_CURVE,         curveL)

	// The Matte Layer param itself is checked out via checkout_layer in
	// PreRender (not here) — a layer param needs the render-graph checkout.

	#undef FS_GET_FLOAT
	#undef FS_GET_POPUP
	return err;
}

// ---------------------------------------------------------------------------
// Effective sigmas. Separation Radius IS sigma at full resolution; scale by
// the downsample ratio per axis (num/den ≤ 1) so previews at half/quarter
// res match full-res renders. Low Band Smooth maps to a second sigma:
// sigma_smooth = (smooth% / 100) * sigma_sep — 0 keeps the band untouched.
// ---------------------------------------------------------------------------
static void
FS_EffectiveSigmas(PF_InData *in_data, const FS_Params *p,
				   double *sepX, double *sepY, double *smX, double *smY)
{
	const double dsx = (in_data->downsample_x.den != 0)
		? (double)in_data->downsample_x.num / (double)in_data->downsample_x.den : 1.0;
	const double dsy = (in_data->downsample_y.den != 0)
		? (double)in_data->downsample_y.num / (double)in_data->downsample_y.den : 1.0;

	const double sep = (p->radiusF > 0.0) ? p->radiusF : 0.0;
	const double sm  = sep * (p->smoothF / 100.0);

	*sepX = sep * dsx;  *sepY = sep * dsy;
	*smX  = sm  * dsx;  *smY  = sm  * dsy;
}

// Per-axis inflation: the SEPARATION read reach plus the Low Band Smooth read
// reach (the smooth pass reads lo pixels which themselves read src pixels, so
// the supports ADD). The separation reach depends on Method — Gaussian is
// ceil(3*sigma), Guided is 2r (two cascaded box windows, see GuidedSupport).
// Smooth is always a Gaussian on the band, whatever the separation method.
static void
FS_InflateAmount(PF_InData *in_data, const FS_Params *p, A_long *ix, A_long *iy)
{
	double sepX, sepY, smX, smY;
	FS_EffectiveSigmas(in_data, p, &sepX, &sepY, &smX, &smY);

	A_long sepInfX, sepInfY;
	if (p->methodL == FS_METHOD_GUIDED) {
		sepInfX = fs::GuidedSupport(fs::GuidedRadius(sepX));
		sepInfY = fs::GuidedSupport(fs::GuidedRadius(sepY));
	} else {
		sepInfX = fs::GaussianSupport(sepX);
		sepInfY = fs::GaussianSupport(sepY);
	}
	*ix = sepInfX + fs::GaussianSupport(smX);
	*iy = sepInfY + fs::GaussianSupport(smY);
}

// checkout_layer IDs. The input reuses its param index (FS_INPUT=0); the
// matte layer needs its OWN unique, non-negative id distinct from that.
enum {
	FS_CHECKOUT_INPUT = 0,   // == FS_INPUT
	FS_CHECKOUT_MATTE = 1
};

// Stashed by PreRender, read by SmartRender. Scalars only — the source
// origin offset is recovered from the checked-out world's actual size.
typedef struct {
	A_long  inflateX;
	A_long  inflateY;
	A_long  srcRefW;           // source LAYER frame size (res-independent), from
	A_long  srcRefH;           // the input checkout — the Fit normalization base
	A_long  matteRefW;         // matte LAYER frame size (res-independent), from
	A_long  matteRefH;         // the matte checkout — the Fit target frame
	A_short matteCheckedOut;   // 1 if the matte layer checkout succeeded
} FS_PreRenderData;

extern "C" void
FS_DeletePreRenderData(void *pre_render_dataPV)
{
	if (pre_render_dataPV) free(pre_render_dataPV);
}

// ---------------------------------------------------------------------------
// PreRender
// ---------------------------------------------------------------------------
PF_Err
FS_PreRender(PF_InData *in_data, PF_OutData *out_data, PF_PreRenderExtra *extra)
{
	PF_Err err = PF_Err_NONE;

	FS_Params p; AEFX_CLR_STRUCT(p);
	ERR(ReadParams(in_data, &p));

	A_long inflateX = 0, inflateY = 0;
	if (!err) FS_InflateAmount(in_data, &p, &inflateX, &inflateY);

	// Stash for SmartRender. AE owns the pointer once assigned and frees it
	// through our delete func. Inflation is param-only (independent of View)
	// so cached pre-render decisions stay valid when only View changes.
	FS_PreRenderData *prd = static_cast<FS_PreRenderData*>(
		malloc(sizeof(FS_PreRenderData)));
	if (prd) {
		prd->inflateX = inflateX;
		prd->inflateY = inflateY;
		prd->srcRefW = 0;           // filled from the input checkout below
		prd->srcRefH = 0;
		prd->matteRefW = 0;         // filled from the matte checkout below
		prd->matteRefH = 0;
		prd->matteCheckedOut = 0;   // set below if the matte checkout succeeds
		extra->output->pre_render_data = prd;
		extra->output->delete_pre_render_data_func = FS_DeletePreRenderData;
	}

	PF_RenderRequest req = extra->input->output_request;

	// RECT DECISION: expand the request by the blur support on all sides so
	// every output pixel has full kernel coverage of real data. AE
	// intersects this against the actual layer bounds during checkout — at
	// a true layer edge we simply receive less, and the blur's zero-extend
	// boundary is then EXACTLY the transparent-beyond-the-layer truth.
	if (inflateX > 0) { req.rect.left -= inflateX; req.rect.right  += inflateX; }
	if (inflateY > 0) { req.rect.top  -= inflateY; req.rect.bottom += inflateY; }

	PF_CheckoutResult in_result;
	ERR(extra->cb->checkout_layer(in_data->effect_ref,
		FS_INPUT, FS_INPUT, &req,
		in_data->current_time, in_data->time_step, in_data->time_scale,
		&in_result));

	// Record the source LAYER frame (ref_width/ref_height = pre-effect layer
	// size, WITHOUT downsample). This is the resolution-independent rectangle
	// Fit-to-Layer normalizes against, so previews at any resolution map the
	// matte identically (fixes the half↔third-res sub-pixel jump).
	if (prd && !err) {
		prd->srcRefW = in_result.ref_width;
		prd->srcRefH = in_result.ref_height;
	}

	// RECT DECISION: union what the checkout can actually produce into our
	// result/max-result rects — the standard SmartFX contract.
	UnionLRect(&in_result.result_rect,     &extra->output->result_rect);
	UnionLRect(&in_result.max_result_rect, &extra->output->max_result_rect);

	// RECT DECISION: intersect the result rect back to the ORIGINAL request.
	// The inflated read must not grow our advertised output extent — this
	// effect never produces pixels outside what was asked of it (alpha
	// extent unchanged; Mix/View never extend coverage either).
	const PF_LRect &out_req = extra->input->output_request.rect;
	if (extra->output->result_rect.left   < out_req.left)   extra->output->result_rect.left   = out_req.left;
	if (extra->output->result_rect.top    < out_req.top)    extra->output->result_rect.top    = out_req.top;
	if (extra->output->result_rect.right  > out_req.right)  extra->output->result_rect.right  = out_req.right;
	if (extra->output->result_rect.bottom > out_req.bottom) extra->output->result_rect.bottom = out_req.bottom;

	// MATTE LAYER checkout. Requested at the ORIGINAL output rect (NOT the
	// inflated one — the matte modulates the final blend per output pixel, it
	// is never blurred, so it needs no support border). Uses a distinct
	// checkout id so its pixels are addressable separately in SmartRender.
	//
	// RECT DECISION: the matte checkout deliberately does NOT contribute to
	// our result/max_result rects. Those describe where THIS effect produces
	// output, which the matte only attenuates — it can shrink coverage but
	// never extend it, and a matte layer that happens to be larger than the
	// source must not grow our output. Leaving it out of the union is correct.
	//
	// When Matte Layer = None, checkout_layer returns an empty/error result;
	// we swallow it and leave matteCheckedOut = 0 so SmartRender treats the
	// matte as 1.0 everywhere. A real checkout failure is likewise non-fatal:
	// degrade to "no matte" rather than failing the whole render.
	{
		PF_RenderRequest matte_req = extra->input->output_request;   // original rect
		PF_CheckoutResult matte_result;
		AEFX_CLR_STRUCT(matte_result);
		PF_Err matte_err = extra->cb->checkout_layer(in_data->effect_ref,
			FS_MATTE_LAYER, FS_CHECKOUT_MATTE, &matte_req,
			in_data->current_time, in_data->time_step, in_data->time_scale,
			&matte_result);

		// A non-empty result rect means a layer is actually connected.
		const bool haveMatte = (matte_err == PF_Err_NONE) &&
			(matte_result.result_rect.right  > matte_result.result_rect.left) &&
			(matte_result.result_rect.bottom > matte_result.result_rect.top);
		if (prd) {
			prd->matteCheckedOut = haveMatte ? 1 : 0;
			// Matte LAYER frame — the res-independent rectangle Fit stretches
			// ACROSS. Paired with srcRefW/H this makes Fit a pure layer-space
			// (source frame → matte frame) mapping, independent of the current
			// render resolution or either buffer's tiling.
			prd->matteRefW = matte_result.ref_width;
			prd->matteRefH = matte_result.ref_height;
		}
	}

	// GPU render offer (../../docs/AE_GPU_Effects.md §2.3). Raised on EVERY
	// frame except one this host can PROVE will not fit: a worst-case tile plan
	// that does not fit the device scratch budget, run through the SAME
	// FS_PlanTiles the render runs so gate and render cannot disagree.
	// Declining here costs a CPU render — slower, never wrong. Discovering it
	// at SMART_RENDER_GPU costs the frame, because by then the worlds are
	// device memory and there is nothing to fall back to. (Pathological case
	// that trips it: radius near the 250 max on 6K+ footage, small-VRAM card.)
	//
	// THE GATE NEVER FIRES ON A FRAME IT COULD NOT SIZE — no framework offered
	// yet, no gpu_data, no budget reading, degenerate rect. Withholding the
	// flag out of ignorance is a DEADLOCK, not caution: AE offers
	// what_gpu = NONE with gpu_data = null at every PreRender until it has SEEN
	// this flag at least once, so a gate conditioned on a known device never
	// raises it and is therefore never offered a device.
	//
	// This is measured, not reasoned. EdgeRepair — the only plugin AE has ever
	// GPU-rendered — records it verbatim at EdgeRepair.cpp:859: "The original
	// gate (what_gpu != NONE && gpu_data) never fired because AE offered
	// what_gpu=NONE at every prerender", and raises unconditionally instead.
	// THIS FILE CARRIED EXACTLY THAT GATE until 2026-08-04, which is why the
	// CUDA host added the day before was correct and unreachable: AE rendered
	// the effect on the CPU, silently and correctly, exactly as it had when the
	// only host was DX12. MM_Qualify had the identical bug, fixed the same day.
	// See ModernEdgeBlur's MEB_GPUBudgetAllows for the shape a budget gate must
	// have.
#ifdef _WIN32
	if (!err) {
		const PF_LRect &rq = extra->input->output_request.rect;
		const int outW = rq.right - rq.left;
		const int outH = rq.bottom - rq.top;

		bool   fits     = true;          // cannot size it -> do NOT withhold
		double needMB   = 0.0;
		double budgetMB = 0.0;

		const size_t budget = fsgpu::FS_ScratchBudget(extra->input->what_gpu,
		                                              extra->input->gpu_data);
		if (fsgpu::FS_FrameworkSupported(extra->input->what_gpu) &&
			extra->input->gpu_data && budget > 0 && outW > 0 && outH > 0) {
			budgetMB = (double)budget / (1024.0 * 1024.0);

			// Worst case: the layer honors the full inflation on both axes
			// (real checkouts can only come back smaller). inflateY is exactly
			// FS_TileMarginY's value — both derive from the same per-axis
			// support terms in FS_InflateAmount.
			const bool guided = (p.methodL == FS_METHOD_GUIDED);
			const int srcW = outW + 2 * inflateX;
			const int srcH = outH + 2 * inflateY;

			const size_t bytesTile = fsgpu::FS_TileBytesPerRow(guided, srcW);
			const size_t bytesOut  = fsgpu::FS_OutBytesPerRow(outW);

			fsgpu::FS_TilePlan plan;
			fits = fsgpu::FS_PlanTiles(outH, srcH, inflateY, inflateY,
			                           bytesTile, bytesOut, budget, &plan);
			if (fits)
				needMB = (double)((size_t)plan.tileRowsMax * bytesTile +
				                  (size_t)plan.bandHMax * bytesOut)
				         / (1024.0 * 1024.0);
		}

		if (fits)
			extra->output->flags |= PF_RenderOutputFlag_GPU_RENDER_POSSIBLE;

		fsgpu::FS_GPULogPreRenderDecision(extra->input->what_gpu,
			extra->input->gpu_data, fits, outW, outH, needMB, budgetMB);
	}
#endif

	return err;
}

// ---------------------------------------------------------------------------
// Unpack: whole inflated source world → contiguous premultiplied ARGB float.
// This is the 8/16→32f promotion point for deep-color inputs.
// ---------------------------------------------------------------------------
struct FS_UnpackRefcon {
	const PF_EffectWorld *world;
	PF_PixelFormat        fmt;
	float                *dst;    // srcW*srcH*4
	A_long                srcW;
};

extern "C" {

static PF_Err
FS_UnpackRow(void *refconPV, A_long /*thread_indexL*/, A_long y, A_long /*iterationsL*/)
{
	const FS_UnpackRefcon *rc = static_cast<const FS_UnpackRefcon*>(refconPV);
	const void *srcRow = reinterpret_cast<const char*>(rc->world->data)
		+ (size_t)y * rc->world->rowbytes;
	fs::UnpackRow(srcRow, rc->dst + (size_t)y * rc->srcW * 4, rc->srcW, rc->fmt);
	return PF_Err_NONE;
}

} // extern "C"

// ---------------------------------------------------------------------------
// Compose: one threaded pass over OUTPUT rows. Reads the inflated src/lo
// buffers through the srcOX/srcOY origin offset, writes the output world.
// All intermediates unclamped; fs::PackRow clamps 8/16bpc on write only.
// ---------------------------------------------------------------------------
struct FS_ComposeRefcon {
	const float         *src;     // inflated premult ARGB float
	const float         *lo;      // blurred (and smoothed) premult ARGB float
	A_long               srcW;
	A_long               srcOX, srcOY;
	float                texGain; // High Band Texture / 100
	float                mix;     // Mix / 100
	A_long               viewL;   // FS_VIEW_*
	A_long               curveL;  // FS_CURVE_* (identity stub this session)
	const fs::FS_MatteField *matte; // matte sampler (inactive → 1.0 everywhere)
	PF_EffectWorld      *out_world;
	PF_PixelFormat       fmt;
	A_long               outW;
};

extern "C" {

static PF_Err
FS_ComposeRow(void *refconPV, A_long /*thread_indexL*/, A_long oy, A_long /*iterationsL*/)
{
	const FS_ComposeRefcon *rc = static_cast<const FS_ComposeRefcon*>(refconPV);
	const A_long W = rc->outW;
	const float g   = rc->texGain;
	const float mix = rc->mix;
	const float EPS = 1e-8f;

	const size_t off = ((size_t)(oy + rc->srcOY) * rc->srcW + rc->srcOX) * 4;
	const float *s = rc->src + off;
	const float *l = rc->lo  + off;

	thread_local std::vector<float> rowv;
	if ((A_long)rowv.size() < W * 4) rowv.resize((size_t)W * 4);
	float *dst = rowv.data();

	for (A_long ox = 0; ox < W; ++ox, s += 4, l += 4, dst += 4) {
		const float a  = s[0];
		const float la = l[0];

		// Straight colors. lo renormalized by BLURRED alpha = alpha-weighted
		// average of straight color (the anti-fringing step). Curve hooks:
		// a real Working Curve encodes src_st/lo_st here and decodes proc —
		// identity while curve.cpp is a stub, so skipped entirely.
		float src_st[3], lo_st[3];
		if (a > EPS) {
			const float inva = 1.f / a;
			src_st[0] = s[1] * inva; src_st[1] = s[2] * inva; src_st[2] = s[3] * inva;
		} else {
			src_st[0] = src_st[1] = src_st[2] = 0.f;
		}
		if (la > EPS) {
			const float invla = 1.f / la;
			lo_st[0] = l[1] * invla; lo_st[1] = l[2] * invla; lo_st[2] = l[3] * invla;
		} else {
			// No coverage anywhere in the kernel → no meaningful low band;
			// fall back to the source color so hi degenerates to 0.
			lo_st[0] = src_st[0]; lo_st[1] = src_st[1]; lo_st[2] = src_st[2];
		}

		// Matte weight for this output pixel, shared by the Matte view and the
		// Result recombine. No matte layer selected → SampleMatte returns 1.
		const float matteW = fs::SampleMatte(*rc->matte, (int)ox, (int)oy);

		switch (rc->viewL) {
			case FS_VIEW_LOW:
				// Tone band re-premultiplied by the SOURCE alpha: the band
				// carries the layer's own coverage, so it composites like
				// the layer would.
				dst[0] = a;
				dst[1] = lo_st[0] * a; dst[2] = lo_st[1] * a; dst[3] = lo_st[2] * a;
				break;

			case FS_VIEW_HIGH:
				// Detail band displayed as texGain*hi + 0.5, opaque, straight —
				// the PS-style inspection view, showing the band as the Result
				// recombine will use it (Texture affects this view). Legitimately
				// exceeds [0,1]; stays unclamped until an 8/16bpc pack.
				dst[0] = 1.f;
				dst[1] = fs::HighBandDisplay(src_st[0], lo_st[0], g);
				dst[2] = fs::HighBandDisplay(src_st[1], lo_st[1], g);
				dst[3] = fs::HighBandDisplay(src_st[2], lo_st[2], g);
				break;

			case FS_VIEW_MATTE:
				// The matte weight as an opaque greyscale field — the FS
				// workflow's "where is the effect acting?" view. No matte and
				// no layer-masks → matteW == 1 → solid white.
				dst[0] = 1.f; dst[1] = matteW; dst[2] = matteW; dst[3] = matteW;
				break;

			case FS_VIEW_RESULT:
			default: {
				if (a > EPS) {
					// proc = lo + g*hi; out = lerp(src, proc, matte*mix).
					// fs::RecombineChannel is written so Texture=100/Mix=100/
					// matte=1 collapses to src within float rounding (identity).
					const float m = matteW * mix;
					const float o0 = fs::RecombineChannel(src_st[0], lo_st[0], g, m);
					const float o1 = fs::RecombineChannel(src_st[1], lo_st[1], g, m);
					const float o2 = fs::RecombineChannel(src_st[2], lo_st[2], g, m);
					dst[0] = a;
					dst[1] = o0 * a; dst[2] = o1 * a; dst[3] = o2 * a;
				} else {
					// Fully transparent source pixel: exact passthrough of
					// the (premultiplied ≈ 0) source values.
					dst[0] = a; dst[1] = s[1]; dst[2] = s[2]; dst[3] = s[3];
				}
				break;
			}
		}
	}

	void *outRow = reinterpret_cast<char*>(rc->out_world->data)
		+ (size_t)oy * rc->out_world->rowbytes;
	fs::PackRow(rowv.data(), outRow, W, rc->fmt);
	return PF_Err_NONE;
}

} // extern "C"

// ---------------------------------------------------------------------------
// SmartRender
// ---------------------------------------------------------------------------
PF_Err
FS_SmartRender(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra)
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;

	PF_WorldSuite2 *wsP = nullptr;
	ERR(AEFX_AcquireSuite(in_data, out_data, kPFWorldSuite, kPFWorldSuiteVersion2,
		"", reinterpret_cast<void**>(&wsP)));

	PF_EffectWorld *src_world = nullptr;
	PF_EffectWorld *out_world = nullptr;
	ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, FS_INPUT, &src_world));
	ERR(extra->cb->checkout_output(in_data->effect_ref, &out_world));

	if (err || !wsP || !src_world || !out_world) goto cleanup;

	{
		PF_PixelFormat fmt = PF_PixelFormat_INVALID;
		ERR(wsP->PF_GetPixelFormat(out_world, &fmt));
		if (err) goto cleanup;
		if (fs::PixelBytes(fmt) == 0) { err = PF_Err_BAD_CALLBACK_PARAM; goto cleanup; }

		FS_Params p; AEFX_CLR_STRUCT(p);
		ERR(ReadParams(in_data, &p));
		if (err) goto cleanup;

		const FS_PreRenderData *prd =
			static_cast<const FS_PreRenderData*>(extra->input->pre_render_data);
		const A_long inflateX = prd ? prd->inflateX : 0;
		const A_long inflateY = prd ? prd->inflateY : 0;

		const A_long srcW = src_world->width;
		const A_long srcH = src_world->height;
		const A_long outW = out_world->width;
		const A_long outH = out_world->height;

		// RECT DECISION: output→source origin offset. The output's top-left
		// sits `inflate` px inside the source's — but only as far as the
		// layer edge let the inflation actually happen (AE clamps the
		// request). Recover the real offset from how much bigger the source
		// came back, capped at the requested inflate, floored at 0. This is
		// what keeps tiles at the layer boundary addressed correctly.
		A_long srcOX = inflateX, srcOY = inflateY;
		if (srcOX > (srcW - outW)) srcOX = srcW - outW;
		if (srcOY > (srcH - outH)) srcOY = srcH - outH;
		if (srcOX < 0) srcOX = 0;
		if (srcOY < 0) srcOY = 0;

		// Working buffers: pristine source, scratch, low band. ~48 bytes/px
		// of inflated tile — the CPU-reference tradeoff, revisited in Phase 5.
		const size_t srcN4 = (size_t)srcW * srcH * 4;
		std::vector<float> srcBuf, tmpBuf, loBuf;
		try {
			srcBuf.resize(srcN4);
			tmpBuf.resize(srcN4);
			loBuf.resize(srcN4);
		} catch (const std::bad_alloc &) {
			err = PF_Err_OUT_OF_MEMORY;
			goto cleanup;
		}

		{
			FS_UnpackRefcon urc{ src_world, fmt, srcBuf.data(), srcW };
			fs::ForEachRow(in_data, srcH, &urc, FS_UnpackRow);
		}

		// Frequency split on the FULL inflated buffer so lo is correct right
		// up to the output edge. Separation Method: Gaussian (creamy/soft) or
		// Guided Filter (edge-aware, no halos). Both deliver the low band in
		// the same premultiplied "blurred-alpha" form the compose pass
		// renormalizes, so nothing downstream changes with the method.
		double sepX, sepY, smX, smY;
		FS_EffectiveSigmas(in_data, &p, &sepX, &sepY, &smX, &smY);

		if (p.methodL == FS_METHOD_GUIDED) {
			const A_long rx = fs::GuidedRadius(sepX);
			const A_long ry = fs::GuidedRadius(sepY);
			const float eps = (float)FS_EdgeToEps(p.edgeF);
			ERR(fs::GuidedFilterARGB(in_data, srcBuf.data(), loBuf.data(),
									 srcW, srcH, rx, ry, eps));
			if (err) goto cleanup;   // OUT_OF_MEMORY from scratch alloc
		} else {
			fs::GaussianBlurARGB(in_data, srcBuf.data(), tmpBuf.data(), loBuf.data(),
								 srcW, srcH, sepX, sepY);
		}

		// Low Band Smooth: cascaded second Gaussian on the band itself
		// (in place — blur.h guarantees dst-aliases-src safety).
		if (fs::GaussianSupport(smX) > 0 || fs::GaussianSupport(smY) > 0) {
			fs::GaussianBlurARGB(in_data, loBuf.data(), tmpBuf.data(), loBuf.data(),
								 srcW, srcH, smX, smY);
		}

		// MATTE LAYER pixels. Checked out in PreRender under FS_CHECKOUT_MATTE;
		// pull the world now (only if that checkout succeeded — matteCheckedOut).
		// Unpack it to its own premultiplied ARGB float plane and build the
		// sampler. Any failure degrades to an inactive matte (1.0 everywhere)
		// rather than failing the render. matteBuf must outlive the compose
		// pass, so it lives at this scope.
		fs::FS_MatteField matte;   // active=false by default → SampleMatte()==1
		std::vector<float> matteBuf;
		if (prd && prd->matteCheckedOut) {
			PF_EffectWorld *matte_world = nullptr;
			PF_Err merr = extra->cb->checkout_layer_pixels(
				in_data->effect_ref, FS_CHECKOUT_MATTE, &matte_world);
			if (merr == PF_Err_NONE && matte_world &&
				matte_world->width > 0 && matte_world->height > 0) {
				PF_PixelFormat mfmt = PF_PixelFormat_INVALID;
				merr = wsP->PF_GetPixelFormat(matte_world, &mfmt);
				if (merr == PF_Err_NONE && fs::PixelBytes(mfmt) != 0) {
					const A_long mW = matte_world->width;
					const A_long mH = matte_world->height;
					try {
						matteBuf.resize((size_t)mW * mH * 4);
						FS_UnpackRefcon murc{ matte_world, mfmt, matteBuf.data(), mW };
						fs::ForEachRow(in_data, mH, &murc, FS_UnpackRow);

						// Downsample ratio (num/den ≤ 1). Buffer coords below are
						// downsampled; ref frames are full-res — dsx/dsy reconcile
						// them (see matte.h). This is the fix for the res-jump.
						const double dsx = (in_data->downsample_x.den != 0)
							? (double)in_data->downsample_x.num / (double)in_data->downsample_x.den : 1.0;
						const double dsy = (in_data->downsample_y.den != 0)
							? (double)in_data->downsample_y.num / (double)in_data->downsample_y.den : 1.0;

						matte.data    = matteBuf.data();
						matte.W       = (int)mW;
						matte.H       = (int)mH;
						// LAYER-SPACE geometry. Both worlds report origin_x/y =
						// buffer top-left in their (downsampled) layer coords;
						// ref_w/h (stashed in PreRender) are the FULL-res layer
						// frames. Fit maps source-frame → matte-frame in full-res
						// layer space; dsx/dsy bridge to the downsampled buffers.
						matte.mOriginX   = (int)matte_world->origin_x;
						matte.mOriginY   = (int)matte_world->origin_y;
						// Ref frames should always be reported; fall back to the
						// full-res-equivalent buffer size (buffer / ds) if a host
						// ever returns 0, so Fit never divides by a bogus frame.
						matte.mRefW      = prd->matteRefW > 0 ? (int)prd->matteRefW : (int)(mW  / (dsx > 0 ? dsx : 1.0));
						matte.mRefH      = prd->matteRefH > 0 ? (int)prd->matteRefH : (int)(mH  / (dsy > 0 ? dsy : 1.0));
						matte.outOriginX = (int)out_world->origin_x;
						matte.outOriginY = (int)out_world->origin_y;
						matte.srcRefW    = prd->srcRefW > 0 ? (int)prd->srcRefW : (int)(outW / (dsx > 0 ? dsx : 1.0));
						matte.srcRefH    = prd->srcRefH > 0 ? (int)prd->srcRefH : (int)(outH / (dsy > 0 ? dsy : 1.0));
						matte.dsx     = (float)dsx;
						matte.dsy     = (float)dsy;
						matte.channel = (int)p.matteChanL;
						matte.fit     = (int)p.matteFitL;
						matte.active  = true;
					} catch (const std::bad_alloc &) {
						matteBuf.clear();   // leave matte inactive → 1.0
					}
				}
			}
		}

		{
			FS_ComposeRefcon rc;
			rc.src = srcBuf.data(); rc.lo = loBuf.data();
			rc.srcW = srcW; rc.srcOX = srcOX; rc.srcOY = srcOY;
			rc.texGain = (float)(p.textureF / 100.0);
			rc.mix     = (float)(p.mixF / 100.0);
			rc.viewL   = p.viewL;
			rc.curveL  = p.curveL;
			rc.matte   = &matte;
			rc.out_world = out_world; rc.fmt = fmt; rc.outW = outW;
			fs::ForEachRow(in_data, outH, &rc, FS_ComposeRow);
		}
	}

cleanup:
	if (wsP) ERR2(AEFX_ReleaseSuite(in_data, out_data, kPFWorldSuite, kPFWorldSuiteVersion2, ""));
	return err;
}

// ---------------------------------------------------------------------------
// SmartRender — GPU (PF_Cmd_SMART_RENDER_GPU).
//
// Assembles the platform-free FS_GPUJob — geometry, CPU-built Gaussian
// weights (rules §3.1), tile plan, matte mapping — and hands it to the DX12
// host. Every DECISION here (sigma scaling, srcOX/srcOY recovery, matte
// geometry and its fallbacks, eps floor) is the same code the CPU render
// runs, so the two paths can only diverge inside kernel execution — which is
// exactly what the View-popup validation isolates per band.
// ---------------------------------------------------------------------------
PF_Err
FS_SmartRenderGPU(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra)
{
#ifndef _WIN32
	return PF_Err_UNRECOGNIZED_PARAM_TYPE;   // Metal host not yet written
#else
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;

	if (!fsgpu::FS_FrameworkSupported(extra->input->what_gpu) ||
		!extra->input->gpu_data)
		return PF_Err_UNRECOGNIZED_PARAM_TYPE;

	PF_WorldSuite2 *wsP = nullptr;
	ERR(AEFX_AcquireSuite(in_data, out_data, kPFWorldSuite, kPFWorldSuiteVersion2,
		"", reinterpret_cast<void**>(&wsP)));

	PF_EffectWorld *src_world = nullptr;
	PF_EffectWorld *out_world = nullptr;
	ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, FS_INPUT, &src_world));
	ERR(extra->cb->checkout_output(in_data->effect_ref, &out_world));

	if (err || !wsP || !src_world || !out_world) goto cleanup;

	{
		// GPU worlds are 32f BGRA on every framework AE offers (rules §1) —
		// anything else means the contract changed under us; refuse rather
		// than misrender.
		PF_PixelFormat fmt = PF_PixelFormat_INVALID;
		ERR(wsP->PF_GetPixelFormat(out_world, &fmt));
		if (err) goto cleanup;
		if (fmt != PF_PixelFormat_GPU_BGRA128) {
			err = PF_Err_UNRECOGNIZED_PARAM_TYPE;
			goto cleanup;
		}

		FS_Params p; AEFX_CLR_STRUCT(p);
		ERR(ReadParams(in_data, &p));
		if (err) goto cleanup;

		const FS_PreRenderData *prd =
			static_cast<const FS_PreRenderData*>(extra->input->pre_render_data);
		const A_long inflateX = prd ? prd->inflateX : 0;
		const A_long inflateY = prd ? prd->inflateY : 0;

		fsgpu::FS_GPUJob job;
		job.srcW = (int)src_world->width;
		job.srcH = (int)src_world->height;
		job.outW = (int)out_world->width;
		job.outH = (int)out_world->height;
		job.srcPitchFl = (int)(src_world->rowbytes / (A_long)sizeof(float));
		job.outPitchFl = (int)(out_world->rowbytes / (A_long)sizeof(float));

		// Output→source origin offset — identical recovery to the CPU path.
		A_long srcOX = inflateX, srcOY = inflateY;
		if (srcOX > (job.srcW - job.outW)) srcOX = job.srcW - job.outW;
		if (srcOY > (job.srcH - job.outH)) srcOY = job.srcH - job.outH;
		if (srcOX < 0) srcOX = 0;
		if (srcOY < 0) srcOY = 0;
		job.srcOX = (int)srcOX;
		job.srcOY = (int)srcOY;

		// Separation + smooth, same sigma pipeline as the CPU render. The
		// Gaussian weights are the EXACT CPU-built kernels (BuildGaussianKernel
		// accumulates in double) — the GPU never rebuilds them.
		double sepX, sepY, smX, smY;
		FS_EffectiveSigmas(in_data, &p, &sepX, &sepY, &smX, &smY);

		job.guided = (p.methodL == FS_METHOD_GUIDED);
		if (job.guided) {
			job.guidedRx = (int)fs::GuidedRadius(sepX);
			job.guidedRy = (int)fs::GuidedRadius(sepY);
			float eps = (float)FS_EdgeToEps(p.edgeF);
			if (eps < fs::FS_GUIDED_EPS_MIN) eps = fs::FS_GUIDED_EPS_MIN;
			job.guidedEps = eps;   // kernel takes the FLOORED eps (guided.cpp floors internally)
			job.guidedInvCount = 1.0f /
				(float)((2 * job.guidedRx + 1) * (2 * job.guidedRy + 1));
		} else {
			job.sepRx = (int)fs::GaussianSupport(sepX);
			job.sepRy = (int)fs::GaussianSupport(sepY);
			if (job.sepRx > 0) fs::BuildGaussianKernel(sepX, job.sepRx, job.kSepX);
			if (job.sepRy > 0) fs::BuildGaussianKernel(sepY, job.sepRy, job.kSepY);
		}
		job.smoothRx = (int)fs::GaussianSupport(smX);
		job.smoothRy = (int)fs::GaussianSupport(smY);
		if (job.smoothRx > 0) fs::BuildGaussianKernel(smX, job.smoothRx, job.kSmX);
		if (job.smoothRy > 0) fs::BuildGaussianKernel(smY, job.smoothRy, job.kSmY);

		job.view    = (int)p.viewL - 1;   // popup is 1-based; kernel is 0-based
		job.texGain = (float)(p.textureF / 100.0);
		job.mix     = (float)(p.mixF / 100.0);

		// Tile plan — same planner and budget PreRender's gate ran, on the
		// real (≤ worst-case) geometry, so this cannot newly fail.
		const int marginY = fsgpu::FS_TileMarginY(job.guided, job.sepRy,
			job.guidedRy, job.smoothRy);
		const size_t budget = fsgpu::FS_ScratchBudget(extra->input->what_gpu,
		                                              extra->input->gpu_data);
		if (!fsgpu::FS_PlanTiles(job.outH, job.srcH, job.srcOY, marginY,
				fsgpu::FS_TileBytesPerRow(job.guided, job.srcW),
				fsgpu::FS_OutBytesPerRow(job.outW), budget, &job.plan)) {
			err = PF_Err_OUT_OF_MEMORY;
			goto cleanup;
		}

		// MATTE LAYER — same checkout id, same layer-space geometry and the
		// same degrade-to-inactive fallbacks as the CPU path. The pixels stay
		// on the GPU; fs_matte.kernel samples the world directly.
		PF_EffectWorld *matte_world = nullptr;
		if (prd && prd->matteCheckedOut) {
			PF_Err merr = extra->cb->checkout_layer_pixels(
				in_data->effect_ref, FS_CHECKOUT_MATTE, &matte_world);
			if (merr == PF_Err_NONE && matte_world &&
				matte_world->width > 0 && matte_world->height > 0) {
				const double dsx = (in_data->downsample_x.den != 0)
					? (double)in_data->downsample_x.num / (double)in_data->downsample_x.den : 1.0;
				const double dsy = (in_data->downsample_y.den != 0)
					? (double)in_data->downsample_y.num / (double)in_data->downsample_y.den : 1.0;

				job.matte.active     = true;
				job.matte.mOriginX   = (int)matte_world->origin_x;
				job.matte.mOriginY   = (int)matte_world->origin_y;
				job.matte.mRefW      = prd->matteRefW > 0 ? (int)prd->matteRefW
					: (int)(matte_world->width  / (dsx > 0 ? dsx : 1.0));
				job.matte.mRefH      = prd->matteRefH > 0 ? (int)prd->matteRefH
					: (int)(matte_world->height / (dsy > 0 ? dsy : 1.0));
				job.matte.outOriginX = (int)out_world->origin_x;
				job.matte.outOriginY = (int)out_world->origin_y;
				job.matte.srcRefW    = prd->srcRefW > 0 ? (int)prd->srcRefW
					: (int)(job.outW / (dsx > 0 ? dsx : 1.0));
				job.matte.srcRefH    = prd->srcRefH > 0 ? (int)prd->srcRefH
					: (int)(job.outH / (dsy > 0 ? dsy : 1.0));
				job.matte.dsx        = (float)dsx;
				job.matte.dsy        = (float)dsy;
				job.matte.channel    = (int)p.matteChanL;
				job.matte.fit        = (int)p.matteFitL;
			} else {
				matte_world = nullptr;   // degrade to inactive (matte == 1.0)
			}
		}

		// One job, one plan, one backend call. The hosts take identical
		// arguments precisely so this stays a dispatch and never grows
		// framework-specific setup of its own.
		if (extra->input->what_gpu == PF_GPU_Framework_CUDA) {
			ERR(fsgpu::FS_CudaRender(in_data, out_data, extra->input->gpu_data,
				extra->input->device_index, job, src_world, out_world, matte_world));
		} else {
			ERR(fsgpu::FS_DXRender(in_data, out_data, extra->input->gpu_data,
				extra->input->device_index, job, src_world, out_world, matte_world));
		}
	}

cleanup:
	if (wsP) ERR2(AEFX_ReleaseSuite(in_data, out_data, kPFWorldSuite, kPFWorldSuiteVersion2, ""));
	return err;
#endif // _WIN32
}

// ---------------------------------------------------------------------------
// GPU device setup/setdown — platform dispatch. CUDA is what After Effects
// offers on Windows/NVIDIA and DirectX is what Premiere offers there; both are
// hosted. Frameworks without a host (OpenCL, Metal-for-now) get gpu_data ==
// null with PF_Err_NONE: AE then simply never offers this effect GPU render on
// such a device, and the CPU reference carries the frame. So does a CUDA device
// on a machine with no NVIDIA driver or no deployed PTX — those decline inside
// FS_CudaDeviceSetup for the same reason and with the same result.
// Metal's entry lands in this switch when mm_gpu_metal.mm is written.
// ---------------------------------------------------------------------------
PF_Err
FS_GPUDeviceSetup(PF_InData *in_data, PF_OutData *out_data, PF_GPUDeviceSetupExtra *extra)
{
#ifdef _WIN32
	if (extra->input->what_gpu == PF_GPU_Framework_CUDA)
		return fsgpu::FS_CudaDeviceSetup(in_data, out_data, extra);
	if (extra->input->what_gpu == PF_GPU_Framework_DIRECTX)
		return fsgpu::FS_DXDeviceSetup(in_data, out_data, extra);
#endif
	extra->output->gpu_data = nullptr;
	return PF_Err_NONE;
}

PF_Err
FS_GPUDeviceSetdown(PF_InData *in_data, PF_OutData *out_data, PF_GPUDeviceSetdownExtra *extra)
{
#ifdef _WIN32
	if (extra->input->what_gpu == PF_GPU_Framework_CUDA)
		return fsgpu::FS_CudaDeviceSetdown(in_data, out_data, extra);
	if (extra->input->what_gpu == PF_GPU_Framework_DIRECTX)
		return fsgpu::FS_DXDeviceSetdown(in_data, out_data, extra);
#endif
	return PF_Err_NONE;
}
