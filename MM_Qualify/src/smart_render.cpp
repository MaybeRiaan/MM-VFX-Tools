/*  smart_render.cpp — MM Qualify SmartFX render path (CPU reference + GPU
    job assembly).

    PIPELINE (all math 32-bit float regardless of input depth; 8/16bpc are
    promoted on unpack and clamped ONLY on the final pack):

        S  = unpack(input)                          // premult ARGB, pristine
        Q  = denoise ? gaussARGB(S, sigma_dn) : S   // QUALIFICATION input only
        G  = garbage layer weights (1.0 when none)  // sampled once, current time
        per pixel:  raw = QK_RawMatte(straight(Q)) * G
                    M   = QK_CleanClip(raw)         // clean B/W + clip levels
        temporal (optional): warm-started asymmetric IIR across the previous
            QK_TEMPORAL_WINDOW source frames — each history frame runs the
            SAME denoise+gates+cleanclip, then S_state advances oldest->now
        Mb = gaussF1(M, sigma_blur)                 // matte blur, edge-neutral
        per output pixel: m = QK_InOutRatio(Mb); QK_Compose(mode, m, ...)

    CHAIN ORDER NOTE (deliberate deviation from plan §2's "temporal last"):
    the IIR runs after clean/clip but BEFORE Blur Radius / In-Out Ratio.
    Blur is the expensive stage; temporal-last would re-blur every history
    frame (~7x the blur cost at window 6) for no stability gain — blurring a
    temporally-stable matte is itself stable, and the blur then also smooths
    any residual temporal artifacts. Perf is an explicit project goal.

    RECT LOGIC: every output pixel reads input up to inflate = dnSupport +
    blurSupport away per axis (cascaded supports ADD: the matte blur reads
    matte pixels whose gates read denoised pixels). PreRender expands the
    checkout by that; AE clamps to real layer data; SmartRender recovers the
    actual offset from the returned world size. Garbage is checked out at
    blurSupport inflation only (it feeds the matte, not the denoise).
    History frames use the input's full inflated rect.

    MFR-safe: everything a render needs lives on the stack or in the
    pre_render_data block AE owns; no globals, no statics with state.
*/

#define NOMINMAX
#include "params.h"
#include "blur.h"
#include "garbage.h"
#include "common/MM_Common.h"
#include "gpu/qk_gpu_host.h"

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <new>

// ---------------------------------------------------------------------------
// Param checkout — one snapshot per render call, never cached.
// ---------------------------------------------------------------------------
PF_Err
QK_ReadParams(PF_InData *in_data, QK_Params *p)
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
	PF_ParamDef def;

	#define QK_GET(IDX, EXPR) \
		AEFX_CLR_STRUCT(def); \
		ERR(PF_CHECKOUT_PARAM(in_data, IDX, in_data->current_time, \
			in_data->time_step, in_data->time_scale, &def)); \
		if (!err) { EXPR; } \
		ERR2(PF_CHECKIN_PARAM(in_data, &def));

	QK_GET(QK_HUE_ENABLE,   p->hueEnL = def.u.bd.value)
	QK_GET(QK_HUE_CENTER,   p->hueCenterF = (PF_FpLong)def.u.ad.value / 65536.0)
	QK_GET(QK_HUE_WIDTH,    p->hueWidthF = def.u.fs_d.value)
	QK_GET(QK_HUE_IN_SOFT,  p->hueInSoftF = def.u.fs_d.value)
	QK_GET(QK_HUE_OUT_SOFT, p->hueOutSoftF = def.u.fs_d.value)
	QK_GET(QK_SAT_ENABLE,   p->satEnL = def.u.bd.value)
	QK_GET(QK_SAT_IN,       p->satInF = def.u.fs_d.value)
	QK_GET(QK_SAT_OUT,      p->satOutF = def.u.fs_d.value)
	QK_GET(QK_SAT_IN_SOFT,  p->satInSoftF = def.u.fs_d.value)
	QK_GET(QK_SAT_OUT_SOFT, p->satOutSoftF = def.u.fs_d.value)
	QK_GET(QK_LUM_ENABLE,   p->lumEnL = def.u.bd.value)
	QK_GET(QK_LUM_IN,       p->lumInF = def.u.fs_d.value)
	QK_GET(QK_LUM_OUT,      p->lumOutF = def.u.fs_d.value)
	QK_GET(QK_LUM_IN_SOFT,  p->lumInSoftF = def.u.fs_d.value)
	QK_GET(QK_LUM_OUT_SOFT, p->lumOutSoftF = def.u.fs_d.value)
	QK_GET(QK_QIN,          p->qinL = def.u.pd.value)
	QK_GET(QK_LUMA_WEIGHTS, p->lumaWL = def.u.pd.value)
	QK_GET(QK_OUTPUT,       p->outputL = def.u.pd.value)
	QK_GET(QK_DENOISE,      p->denoiseF = def.u.fs_d.value)
	QK_GET(QK_CLEAN_BLACK,  p->cleanBlackF = def.u.fs_d.value)
	QK_GET(QK_CLEAN_WHITE,  p->cleanWhiteF = def.u.fs_d.value)
	QK_GET(QK_BLACK_CLIP,   p->blackClipF = def.u.fs_d.value)
	QK_GET(QK_WHITE_CLIP,   p->whiteClipF = def.u.fs_d.value)
	QK_GET(QK_BLUR,         p->blurF = def.u.fs_d.value)
	QK_GET(QK_RATIO,        p->ratioF = def.u.fs_d.value)
	QK_GET(QK_TEMPORAL,     p->temporalL = def.u.bd.value)
	QK_GET(QK_ATTACK,       p->attackF = def.u.fs_d.value)
	QK_GET(QK_RELEASE,      p->releaseF = def.u.fs_d.value)
	QK_GET(QK_GARBAGE_CHAN, p->garbageChanL = def.u.pd.value)

	#undef QK_GET
	return err;
}

// ---------------------------------------------------------------------------
// Effective sigmas: Denoise / Blur Radius ARE sigma at full resolution;
// scale by the downsample ratio per axis (num/den <= 1) so previews at
// half/quarter res match full-res renders.
// ---------------------------------------------------------------------------
static void
QK_EffectiveSigmas(PF_InData *in_data, const QK_Params *p,
				   double *dnX, double *dnY, double *blX, double *blY)
{
	const double dsx = (in_data->downsample_x.den != 0)
		? (double)in_data->downsample_x.num / (double)in_data->downsample_x.den : 1.0;
	const double dsy = (in_data->downsample_y.den != 0)
		? (double)in_data->downsample_y.num / (double)in_data->downsample_y.den : 1.0;

	const double dn = (p->denoiseF > 0.0) ? p->denoiseF : 0.0;
	const double bl = (p->blurF    > 0.0) ? p->blurF    : 0.0;

	*dnX = dn * dsx;  *dnY = dn * dsy;
	*blX = bl * dsx;  *blY = bl * dsy;
}

// Per-axis inflation: denoise read reach + matte blur read reach (cascaded
// supports ADD — the blur reads matte pixels whose gates read denoised px).
static void
QK_InflateAmount(PF_InData *in_data, const QK_Params *p, A_long *ix, A_long *iy)
{
	double dnX, dnY, blX, blY;
	QK_EffectiveSigmas(in_data, p, &dnX, &dnY, &blX, &blY);
	*ix = qk::GaussianSupport(dnX) + qk::GaussianSupport(blX);
	*iy = qk::GaussianSupport(dnY) + qk::GaussianSupport(blY);
}

// Stashed by PreRender, read by SmartRender. Scalars only — the source
// origin offset is recovered from the checked-out world's actual size.
typedef struct {
	A_long  inflateX;
	A_long  inflateY;
	A_long  srcRefW;           // source LAYER frame (full-res) — Fit base
	A_long  srcRefH;
	A_long  garbRefW;          // garbage LAYER frame (full-res)
	A_long  garbRefH;
	A_long  histOKMask;        // bit i-1 set = history checkout t-i succeeded
	A_short garbCheckedOut;
	A_short histRequested;     // QK_TEMPORAL_WINDOW when temporal, else 0
} QK_PreRenderData;

extern "C" void
QK_DeletePreRenderData(void *pre_render_dataPV)
{
	if (pre_render_dataPV) free(pre_render_dataPV);
}

// ---------------------------------------------------------------------------
// PreRender
// ---------------------------------------------------------------------------
PF_Err
QK_PreRender(PF_InData *in_data, PF_OutData *out_data, PF_PreRenderExtra *extra)
{
	PF_Err err = PF_Err_NONE;

	QK_Params p; AEFX_CLR_STRUCT(p);
	ERR(QK_ReadParams(in_data, &p));

	A_long inflateX = 0, inflateY = 0;
	if (!err) QK_InflateAmount(in_data, &p, &inflateX, &inflateY);

	double dnX = 0, dnY = 0, blX = 0, blY = 0;
	if (!err) QK_EffectiveSigmas(in_data, &p, &dnX, &dnY, &blX, &blY);
	const A_long blSupX = qk::GaussianSupport(blX);
	const A_long blSupY = qk::GaussianSupport(blY);

	QK_PreRenderData *prd = static_cast<QK_PreRenderData*>(
		malloc(sizeof(QK_PreRenderData)));
	if (prd) {
		memset(prd, 0, sizeof(*prd));
		prd->inflateX = inflateX;
		prd->inflateY = inflateY;
		extra->output->pre_render_data = prd;
		extra->output->delete_pre_render_data_func = QK_DeletePreRenderData;
	}

	PF_RenderRequest req = extra->input->output_request;

	// RECT DECISION: expand the request by the cascade support on all sides
	// so every output pixel has full kernel coverage of real data. AE clamps
	// against actual layer bounds; at a true layer edge we simply receive
	// less, and the blur's boundary rule is then exactly the truth.
	if (inflateX > 0) { req.rect.left -= inflateX; req.rect.right  += inflateX; }
	if (inflateY > 0) { req.rect.top  -= inflateY; req.rect.bottom += inflateY; }

	PF_CheckoutResult in_result;
	ERR(extra->cb->checkout_layer(in_data->effect_ref,
		QK_INPUT, QK_INPUT, &req,
		in_data->current_time, in_data->time_step, in_data->time_scale,
		&in_result));

	if (prd && !err) {
		prd->srcRefW = in_result.ref_width;
		prd->srcRefH = in_result.ref_height;
	}

	UnionLRect(&in_result.result_rect,     &extra->output->result_rect);
	UnionLRect(&in_result.max_result_rect, &extra->output->max_result_rect);

	// RECT DECISION: intersect the result rect back to the ORIGINAL request —
	// the inflated read must not grow our advertised output extent.
	const PF_LRect &out_req = extra->input->output_request.rect;
	if (extra->output->result_rect.left   < out_req.left)   extra->output->result_rect.left   = out_req.left;
	if (extra->output->result_rect.top    < out_req.top)    extra->output->result_rect.top    = out_req.top;
	if (extra->output->result_rect.right  > out_req.right)  extra->output->result_rect.right  = out_req.right;
	if (extra->output->result_rect.bottom > out_req.bottom) extra->output->result_rect.bottom = out_req.bottom;

	// GARBAGE LAYER checkout — at blur-support inflation only (it multiplies
	// the raw matte, which is then blurred; it never passes through the
	// denoise). Deliberately does NOT contribute to result rects: a garbage
	// matte can only shrink coverage, never extend it. None selected /
	// failed checkout -> degrade to "no garbage" (weights 1.0).
	{
		PF_RenderRequest greq = extra->input->output_request;
		if (blSupX > 0) { greq.rect.left -= blSupX; greq.rect.right  += blSupX; }
		if (blSupY > 0) { greq.rect.top  -= blSupY; greq.rect.bottom += blSupY; }
		PF_CheckoutResult gres;
		AEFX_CLR_STRUCT(gres);
		PF_Err gerr = extra->cb->checkout_layer(in_data->effect_ref,
			QK_GARBAGE_LAYER, QK_CHECKOUT_GARBAGE, &greq,
			in_data->current_time, in_data->time_step, in_data->time_scale,
			&gres);
		const bool have = (gerr == PF_Err_NONE) &&
			(gres.result_rect.right  > gres.result_rect.left) &&
			(gres.result_rect.bottom > gres.result_rect.top);
		if (prd) {
			prd->garbCheckedOut = have ? 1 : 0;
			prd->garbRefW = gres.ref_width;
			prd->garbRefH = gres.ref_height;
		}
	}

	// HISTORY checkouts for temporal stabilize: the SOURCE at t-1 .. t-N,
	// same inflated rect (each history frame runs the full denoise+gates).
	// Stills (time_step 0) can't stabilize — skip. Frames before the layer
	// start come back empty; their bits stay clear and the IIR warm-starts
	// from the oldest frame that exists.
	if (!err && p.temporalL && in_data->time_step != 0) {
		if (prd) prd->histRequested = QK_TEMPORAL_WINDOW;
		for (A_long i = 1; i <= QK_TEMPORAL_WINDOW; ++i) {
			PF_RenderRequest hreq = req;   // input's inflated request
			PF_CheckoutResult hres;
			AEFX_CLR_STRUCT(hres);
			PF_Err herr = extra->cb->checkout_layer(in_data->effect_ref,
				QK_INPUT, QK_CHECKOUT_HIST0 + i - 1, &hreq,
				in_data->current_time - i * in_data->time_step,
				in_data->time_step, in_data->time_scale, &hres);
			const bool have = (herr == PF_Err_NONE) &&
				(hres.result_rect.right  > hres.result_rect.left) &&
				(hres.result_rect.bottom > hres.result_rect.top);
			if (have && prd) prd->histOKMask |= (1L << (i - 1));
		}
	}

	// GPU render offer: only when AE drives the DirectX pipeline, device
	// setup succeeded, and the WORST-CASE tile plan for this request fits
	// the device scratch budget — the same planner the render runs, so gate
	// and render cannot disagree. Anything else renders on the CPU path:
	// slower, never wrong.
#ifdef _WIN32
	if (!err &&
		extra->input->what_gpu == PF_GPU_Framework_DIRECTX &&
		extra->input->gpu_data) {
		const PF_LRect &rq = extra->input->output_request.rect;
		const int outW = rq.right - rq.left;
		const int outH = rq.bottom - rq.top;
		const size_t budget = qkgpu::QK_DXScratchBudget(extra->input->gpu_data);
		if (outW > 0 && outH > 0 && budget > 0) {
			const bool denoise  = qk::GaussianSupport(dnX) > 0 || qk::GaussianSupport(dnY) > 0;
			const bool temporal = (p.temporalL != 0);
			const int srcW = outW + 2 * inflateX;
			const int srcH = outH + 2 * inflateY;
			qkgpu::QK_TilePlan plan;
			if (qkgpu::QK_PlanTiles(outH, srcH, inflateY, inflateY,
					qkgpu::QK_TileBytesPerRow(denoise, temporal, srcW),
					qkgpu::QK_OutBytesPerRow(outW), budget, &plan)) {
				extra->output->flags |= PF_RenderOutputFlag_GPU_RENDER_POSSIBLE;
			}
		}
	}
#endif

	return err;
}

// ---------------------------------------------------------------------------
// Row workers
// ---------------------------------------------------------------------------
struct QK_UnpackRefcon {
	const PF_EffectWorld *world;
	PF_PixelFormat        fmt;
	float                *dst;
	A_long                W;
};

struct QK_GarbageRefcon {
	const qk::QK_GarbageField *field;
	float   *dst;          // srcW*srcH f1 plane
	A_long   srcW;
	A_long   srcOX, srcOY; // plane pixel -> output pixel offset
};

struct QK_MatteRefcon {
	const float *qual;     // premult ARGB qualification input (denoised)
	const float *g;        // f1 garbage weights, may be null (== 1.0)
	float       *dst;      // f1 matte plane
	A_long       W;
	QK_Gates     gates;
	float        cleanB, cleanW, clipB, clipW;
};

struct QK_IIRRefcon {
	float       *state;    // f1, read+write
	const float *m;        // f1 incoming matte
	A_long       W;
	float        kA, kR;
	A_long       first;    // 1 = state := m
};

struct QK_ComposeRefcon {
	const float *src;      // pristine premult ARGB (current frame)
	const float *matte;    // f1 final (blurred/stabilized) matte
	const float *qual;     // premult ARGB qualification input (for Raw view)
	const float *g;        // f1 garbage weights, may be null
	A_long       srcW;
	A_long       srcOX, srcOY;
	A_long       outW;
	A_long       mode;     // QK_OUT_* (0-based)
	float        ratio;    // -1..1
	QK_Gates     gates;
	PF_EffectWorld *out_world;
	PF_PixelFormat  fmt;
};

extern "C" {

static PF_Err
QK_UnpackRow(void *refconPV, A_long, A_long y, A_long)
{
	const QK_UnpackRefcon *rc = static_cast<const QK_UnpackRefcon*>(refconPV);
	const void *srcRow = reinterpret_cast<const char*>(rc->world->data)
		+ (size_t)y * rc->world->rowbytes;
	mm::UnpackRow(srcRow, rc->dst + (size_t)y * rc->W * 4, rc->W, rc->fmt);
	return PF_Err_NONE;
}

static PF_Err
QK_GarbageRow(void *refconPV, A_long, A_long y, A_long)
{
	const QK_GarbageRefcon *rc = static_cast<const QK_GarbageRefcon*>(refconPV);
	float *dst = rc->dst + (size_t)y * rc->srcW;
	const int oy = (int)(y - rc->srcOY);
	for (A_long x = 0; x < rc->srcW; ++x)
		dst[x] = qk::SampleGarbage(*rc->field, (int)(x - rc->srcOX), oy);
	return PF_Err_NONE;
}

static PF_Err
QK_MatteRow(void *refconPV, A_long, A_long y, A_long)
{
	const QK_MatteRefcon *rc = static_cast<const QK_MatteRefcon*>(refconPV);
	const size_t off = (size_t)y * rc->W;
	const float *q = rc->qual + off * 4;
	const float *g = rc->g ? rc->g + off : nullptr;
	float *dst = rc->dst + off;
	for (A_long x = 0; x < rc->W; ++x, q += 4) {
		float r, gg, b;
		QK_Straight(q[0], q[1], q[2], q[3], r, gg, b);
		float m = QK_RawMatte(r, gg, b, rc->gates);
		if (g) m *= g[x];
		dst[x] = QK_CleanClip(m, rc->cleanB, rc->cleanW, rc->clipB, rc->clipW);
	}
	return PF_Err_NONE;
}

static PF_Err
QK_IIRRow(void *refconPV, A_long, A_long y, A_long)
{
	const QK_IIRRefcon *rc = static_cast<const QK_IIRRefcon*>(refconPV);
	const size_t off = (size_t)y * rc->W;
	float *s = rc->state + off;
	const float *m = rc->m + off;
	if (rc->first) {
		for (A_long x = 0; x < rc->W; ++x) s[x] = m[x];
	} else {
		for (A_long x = 0; x < rc->W; ++x)
			s[x] = QK_IIRStep(s[x], m[x], rc->kA, rc->kR);
	}
	return PF_Err_NONE;
}

static PF_Err
QK_ComposeRow(void *refconPV, A_long, A_long oy, A_long)
{
	const QK_ComposeRefcon *rc = static_cast<const QK_ComposeRefcon*>(refconPV);
	const A_long W = rc->outW;
	const size_t so = ((size_t)(oy + rc->srcOY) * rc->srcW + rc->srcOX);
	const float *s = rc->src + so * 4;
	const float *q = rc->qual + so * 4;
	const float *mt = rc->matte + so;
	const float *g = rc->g ? rc->g + so : nullptr;

	thread_local std::vector<float> rowv;
	if ((A_long)rowv.size() < W * 4) rowv.resize((size_t)W * 4);
	float *dst = rowv.data();

	const int mode = (int)rc->mode;
	for (A_long ox = 0; ox < W; ++ox, s += 4, q += 4, dst += 4) {
		const float m = QK_InOutRatio(mt[ox], rc->ratio);

		float rawM = 0.f;
		if (mode == QK_OUT_RAW) {
			float r, gg, b;
			QK_Straight(q[0], q[1], q[2], q[3], r, gg, b);
			rawM = QK_RawMatte(r, gg, b, rc->gates);
			if (g) rawM *= g[ox];
		}

		QK_Compose(mode, m, rawM, s[0], s[1], s[2], s[3],
		           dst[0], dst[1], dst[2], dst[3]);
	}

	void *outRow = reinterpret_cast<char*>(rc->out_world->data)
		+ (size_t)oy * rc->out_world->rowbytes;
	mm::PackRow(rowv.data(), outRow, W, rc->fmt);
	return PF_Err_NONE;
}

} // extern "C"

// ---------------------------------------------------------------------------
// SmartRender (CPU)
// ---------------------------------------------------------------------------
PF_Err
QK_SmartRender(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra)
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;

	PF_WorldSuite2 *wsP = nullptr;
	ERR(AEFX_AcquireSuite(in_data, out_data, kPFWorldSuite, kPFWorldSuiteVersion2,
		"", reinterpret_cast<void**>(&wsP)));

	PF_EffectWorld *src_world = nullptr;
	PF_EffectWorld *out_world = nullptr;
	ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, QK_INPUT, &src_world));
	ERR(extra->cb->checkout_output(in_data->effect_ref, &out_world));

	if (err || !wsP || !src_world || !out_world) goto cleanup;

	{
		PF_PixelFormat fmt = PF_PixelFormat_INVALID;
		ERR(wsP->PF_GetPixelFormat(out_world, &fmt));
		if (err) goto cleanup;
		if (mm::PixelBytes(fmt) == 0) { err = PF_Err_BAD_CALLBACK_PARAM; goto cleanup; }

		QK_Params p; AEFX_CLR_STRUCT(p);
		ERR(QK_ReadParams(in_data, &p));
		if (err) goto cleanup;

		const QK_PreRenderData *prd =
			static_cast<const QK_PreRenderData*>(extra->input->pre_render_data);
		const A_long inflateX = prd ? prd->inflateX : 0;
		const A_long inflateY = prd ? prd->inflateY : 0;

		const A_long srcW = src_world->width;
		const A_long srcH = src_world->height;
		const A_long outW = out_world->width;
		const A_long outH = out_world->height;

		// Output -> source origin offset (clamped by how much inflation the
		// layer edge actually allowed — recovered from the returned size).
		A_long srcOX = inflateX, srcOY = inflateY;
		if (srcOX > (srcW - outW)) srcOX = srcW - outW;
		if (srcOY > (srcH - outH)) srcOY = srcH - outH;
		if (srcOX < 0) srcOX = 0;
		if (srcOY < 0) srcOY = 0;

		double dnX, dnY, blX, blY;
		QK_EffectiveSigmas(in_data, &p, &dnX, &dnY, &blX, &blY);
		const bool denoise = qk::GaussianSupport(dnX) > 0 || qk::GaussianSupport(dnY) > 0;
		const bool doBlur  = qk::GaussianSupport(blX) > 0 || qk::GaussianSupport(blY) > 0;
		const bool temporal = (p.temporalL != 0) && prd && prd->histRequested > 0;

		QK_Gates gates;
		QK_BuildGates(p, &gates);
		const float cleanB = (float)(p.cleanBlackF / 100.0);
		const float cleanW = (float)(p.cleanWhiteF / 100.0);
		const float clipB  = (float)(p.blackClipF  / 100.0);
		const float clipW  = (float)(p.whiteClipF  / 100.0);

		const size_t srcN4 = (size_t)srcW * srcH * 4;
		const size_t srcN1 = (size_t)srcW * srcH;

		std::vector<float> srcBuf, qBuf, tBuf, histBuf, mBuf, btBuf, mhBuf, ssBuf, gBuf, garbBuf;
		try {
			srcBuf.resize(srcN4);
			mBuf.resize(srcN1);
			if (denoise) { qBuf.resize(srcN4); tBuf.resize(srcN4); }
			if (temporal) { histBuf.resize(srcN4); mhBuf.resize(srcN1); ssBuf.resize(srcN1); }
			if (doBlur) btBuf.resize(srcN1);
		} catch (const std::bad_alloc &) {
			err = PF_Err_OUT_OF_MEMORY;
			goto cleanup;
		}

		{
			QK_UnpackRefcon urc{ src_world, fmt, srcBuf.data(), srcW };
			mm::ForEachRow(in_data, srcH, &urc, QK_UnpackRow);
		}

		// Denoise the QUALIFICATION copy (image untouched).
		const float *qual = srcBuf.data();
		if (denoise) {
			qk::GaussianBlurARGB(in_data, srcBuf.data(), tBuf.data(), qBuf.data(),
								 srcW, srcH, dnX, dnY);
			qual = qBuf.data();
		}

		// GARBAGE weights, sampled once at current time over the source-plane
		// footprint (margins extrapolate the linear Fit mapping; reads clamp).
		qk::QK_GarbageField garb;   // active=false -> SampleGarbage()==1
		if (prd && prd->garbCheckedOut) {
			PF_EffectWorld *garb_world = nullptr;
			PF_Err gerr = extra->cb->checkout_layer_pixels(
				in_data->effect_ref, QK_CHECKOUT_GARBAGE, &garb_world);
			if (gerr == PF_Err_NONE && garb_world &&
				garb_world->width > 0 && garb_world->height > 0) {
				PF_PixelFormat gfmt = PF_PixelFormat_INVALID;
				gerr = wsP->PF_GetPixelFormat(garb_world, &gfmt);
				if (gerr == PF_Err_NONE && mm::PixelBytes(gfmt) != 0) {
					const A_long gW = garb_world->width;
					const A_long gH = garb_world->height;
					try {
						garbBuf.resize((size_t)gW * gH * 4);
						gBuf.resize(srcN1);
						QK_UnpackRefcon grc{ garb_world, gfmt, garbBuf.data(), gW };
						mm::ForEachRow(in_data, gH, &grc, QK_UnpackRow);

						const double dsx = (in_data->downsample_x.den != 0)
							? (double)in_data->downsample_x.num / (double)in_data->downsample_x.den : 1.0;
						const double dsy = (in_data->downsample_y.den != 0)
							? (double)in_data->downsample_y.num / (double)in_data->downsample_y.den : 1.0;

						garb.data = garbBuf.data();
						garb.W = (int)gW;  garb.H = (int)gH;
						garb.mOriginX = (int)garb_world->origin_x;
						garb.mOriginY = (int)garb_world->origin_y;
						garb.mRefW = prd->garbRefW > 0 ? (int)prd->garbRefW : (int)(gW / (dsx > 0 ? dsx : 1.0));
						garb.mRefH = prd->garbRefH > 0 ? (int)prd->garbRefH : (int)(gH / (dsy > 0 ? dsy : 1.0));
						garb.outOriginX = (int)out_world->origin_x;
						garb.outOriginY = (int)out_world->origin_y;
						garb.srcRefW = prd->srcRefW > 0 ? (int)prd->srcRefW : (int)(outW / (dsx > 0 ? dsx : 1.0));
						garb.srcRefH = prd->srcRefH > 0 ? (int)prd->srcRefH : (int)(outH / (dsy > 0 ? dsy : 1.0));
						garb.dsx = (float)dsx;  garb.dsy = (float)dsy;
						garb.channel = (int)p.garbageChanL;
						garb.active = true;

						QK_GarbageRefcon grefc{ &garb, gBuf.data(), srcW, srcOX, srcOY };
						mm::ForEachRow(in_data, srcH, &grefc, QK_GarbageRow);
					} catch (const std::bad_alloc &) {
						garbBuf.clear(); gBuf.clear();
						garb.active = false;   // degrade: garbage = 1.0
					}
				}
			}
		}
		const float *gPlane = garb.active && !gBuf.empty() ? gBuf.data() : nullptr;

		// Raw matte -> clean/clip for the CURRENT frame.
		{
			QK_MatteRefcon mrc{ qual, gPlane, mBuf.data(), srcW,
			                    gates, cleanB, cleanW, clipB, clipW };
			mm::ForEachRow(in_data, srcH, &mrc, QK_MatteRow);
		}

		// TEMPORAL STABILIZE: warm-started asymmetric IIR, oldest -> current.
		// Each available history frame runs the same denoise+gates+cleanclip
		// chain (with the current-time garbage weights); frames that came
		// back missing or geometry-mismatched are skipped — the IIR then
		// simply warm-starts later. No history at all == identity.
		float *finalMatte = mBuf.data();
		if (temporal) {
			const float kA = QK_SmoothK((float)p.attackF);
			const float kR = QK_SmoothK((float)p.releaseF);
			bool haveState = false;
			for (A_long j = QK_TEMPORAL_WINDOW; j >= 1; --j) {
				if (!(prd->histOKMask & (1L << (j - 1)))) continue;
				PF_EffectWorld *hw = nullptr;
				PF_Err herr = extra->cb->checkout_layer_pixels(
					in_data->effect_ref, QK_CHECKOUT_HIST0 + j - 1, &hw);
				if (herr != PF_Err_NONE || !hw) continue;
				if (hw->width != srcW || hw->height != srcH) continue;   // geometry rule

				PF_PixelFormat hfmt = PF_PixelFormat_INVALID;
				if (wsP->PF_GetPixelFormat(hw, &hfmt) != PF_Err_NONE ||
					mm::PixelBytes(hfmt) == 0) continue;

				QK_UnpackRefcon hrc{ hw, hfmt, histBuf.data(), srcW };
				mm::ForEachRow(in_data, srcH, &hrc, QK_UnpackRow);
				if (denoise) {
					qk::GaussianBlurARGB(in_data, histBuf.data(), tBuf.data(),
										 histBuf.data(), srcW, srcH, dnX, dnY);
				}
				QK_MatteRefcon hmrc{ histBuf.data(), gPlane, mhBuf.data(), srcW,
				                     gates, cleanB, cleanW, clipB, clipW };
				mm::ForEachRow(in_data, srcH, &hmrc, QK_MatteRow);

				QK_IIRRefcon irc{ ssBuf.data(), mhBuf.data(), srcW, kA, kR,
				                  haveState ? 0 : 1 };
				mm::ForEachRow(in_data, srcH, &irc, QK_IIRRow);
				haveState = true;
			}
			if (haveState) {
				QK_IIRRefcon irc{ ssBuf.data(), mBuf.data(), srcW, kA, kR, 0 };
				mm::ForEachRow(in_data, srcH, &irc, QK_IIRRow);
				finalMatte = ssBuf.data();
			}
		}

		// Matte blur (edge-neutral), in place on the final matte plane.
		if (doBlur) {
			qk::GaussianBlurF1(in_data, finalMatte, btBuf.data(), finalMatte,
							   srcW, srcH, blX, blY);
		}

		// Ratio + output-mode compose.
		{
			QK_ComposeRefcon rc;
			rc.src = srcBuf.data();
			rc.matte = finalMatte;
			rc.qual = qual;
			rc.g = gPlane;
			rc.srcW = srcW; rc.srcOX = srcOX; rc.srcOY = srcOY;
			rc.outW = outW;
			rc.mode = p.outputL - 1;                    // 1-based -> QK_OUT_*
			rc.ratio = (float)(p.ratioF / 100.0);
			rc.gates = gates;
			rc.out_world = out_world; rc.fmt = fmt;
			mm::ForEachRow(in_data, outH, &rc, QK_ComposeRow);
		}
	}

cleanup:
	if (wsP) ERR2(AEFX_ReleaseSuite(in_data, out_data, kPFWorldSuite, kPFWorldSuiteVersion2, ""));
	return err;
}

// ---------------------------------------------------------------------------
// SmartRender — GPU (PF_Cmd_SMART_RENDER_GPU). Assembles the platform-free
// QK_GPUJob — geometry, CPU-built Gaussian weights, gate block, tile plan,
// garbage mapping, history worlds — and hands it to the DX12 host. Every
// DECISION here is the same code the CPU render runs, so the paths can only
// diverge inside kernel execution.
// ---------------------------------------------------------------------------
PF_Err
QK_SmartRenderGPU(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra)
{
#ifndef _WIN32
	return PF_Err_UNRECOGNIZED_PARAM_TYPE;
#else
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;

	if (extra->input->what_gpu != PF_GPU_Framework_DIRECTX ||
		!extra->input->gpu_data)
		return PF_Err_UNRECOGNIZED_PARAM_TYPE;

	PF_WorldSuite2 *wsP = nullptr;
	ERR(AEFX_AcquireSuite(in_data, out_data, kPFWorldSuite, kPFWorldSuiteVersion2,
		"", reinterpret_cast<void**>(&wsP)));

	PF_EffectWorld *src_world = nullptr;
	PF_EffectWorld *out_world = nullptr;
	ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, QK_INPUT, &src_world));
	ERR(extra->cb->checkout_output(in_data->effect_ref, &out_world));

	if (err || !wsP || !src_world || !out_world) goto cleanup;

	{
		PF_PixelFormat fmt = PF_PixelFormat_INVALID;
		ERR(wsP->PF_GetPixelFormat(out_world, &fmt));
		if (err) goto cleanup;
		if (fmt != PF_PixelFormat_GPU_BGRA128) {
			err = PF_Err_UNRECOGNIZED_PARAM_TYPE;
			goto cleanup;
		}

		QK_Params p; AEFX_CLR_STRUCT(p);
		ERR(QK_ReadParams(in_data, &p));
		if (err) goto cleanup;

		const QK_PreRenderData *prd =
			static_cast<const QK_PreRenderData*>(extra->input->pre_render_data);
		const A_long inflateX = prd ? prd->inflateX : 0;
		const A_long inflateY = prd ? prd->inflateY : 0;

		qkgpu::QK_GPUJob job;
		job.srcW = (int)src_world->width;
		job.srcH = (int)src_world->height;
		job.outW = (int)out_world->width;
		job.outH = (int)out_world->height;
		job.srcPitchFl = (int)(src_world->rowbytes / (A_long)sizeof(float));
		job.outPitchFl = (int)(out_world->rowbytes / (A_long)sizeof(float));

		A_long srcOX = inflateX, srcOY = inflateY;
		if (srcOX > (job.srcW - job.outW)) srcOX = job.srcW - job.outW;
		if (srcOY > (job.srcH - job.outH)) srcOY = job.srcH - job.outH;
		if (srcOX < 0) srcOX = 0;
		if (srcOY < 0) srcOY = 0;
		job.srcOX = (int)srcOX;
		job.srcOY = (int)srcOY;

		double dnX, dnY, blX, blY;
		QK_EffectiveSigmas(in_data, &p, &dnX, &dnY, &blX, &blY);
		job.dnRx = (int)qk::GaussianSupport(dnX);
		job.dnRy = (int)qk::GaussianSupport(dnY);
		if (job.dnRx > 0) qk::BuildGaussianKernel(dnX, job.dnRx, job.kDnX);
		if (job.dnRy > 0) qk::BuildGaussianKernel(dnY, job.dnRy, job.kDnY);
		job.blRx = (int)qk::GaussianSupport(blX);
		job.blRy = (int)qk::GaussianSupport(blY);
		if (job.blRx > 0) qk::BuildGaussianKernel(blX, job.blRx, job.kBlX);
		if (job.blRy > 0) qk::BuildGaussianKernel(blY, job.blRy, job.kBlY);

		QK_BuildGates(p, &job.gates);
		job.cleanB = (float)(p.cleanBlackF / 100.0);
		job.cleanW = (float)(p.cleanWhiteF / 100.0);
		job.clipB  = (float)(p.blackClipF  / 100.0);
		job.clipW  = (float)(p.whiteClipF  / 100.0);
		job.ratio  = (float)(p.ratioF / 100.0);
		job.mode   = (int)p.outputL - 1;

		job.temporal = (p.temporalL != 0) && prd && prd->histRequested > 0;
		job.kAttack  = QK_SmoothK((float)p.attackF);
		job.kRelease = QK_SmoothK((float)p.releaseF);

		// Tile plan — same planner and budget the PreRender gate ran.
		const bool denoise = (job.dnRx > 0 || job.dnRy > 0);
		const int marginY = qkgpu::QK_TileMarginY(job.dnRy, job.blRy);
		const size_t budget = qkgpu::QK_DXScratchBudget(extra->input->gpu_data);
		if (!qkgpu::QK_PlanTiles(job.outH, job.srcH, job.srcOY, marginY,
				qkgpu::QK_TileBytesPerRow(denoise, job.temporal, job.srcW),
				qkgpu::QK_OutBytesPerRow(job.outW), budget, &job.plan)) {
			err = PF_Err_OUT_OF_MEMORY;
			goto cleanup;
		}

		// GARBAGE — same layer-space geometry and degrade-to-inactive
		// fallbacks as the CPU path; pixels stay on the GPU.
		PF_EffectWorld *garb_world = nullptr;
		if (prd && prd->garbCheckedOut) {
			PF_Err gerr = extra->cb->checkout_layer_pixels(
				in_data->effect_ref, QK_CHECKOUT_GARBAGE, &garb_world);
			if (gerr == PF_Err_NONE && garb_world &&
				garb_world->width > 0 && garb_world->height > 0) {
				const double dsx = (in_data->downsample_x.den != 0)
					? (double)in_data->downsample_x.num / (double)in_data->downsample_x.den : 1.0;
				const double dsy = (in_data->downsample_y.den != 0)
					? (double)in_data->downsample_y.num / (double)in_data->downsample_y.den : 1.0;
				job.garbage.active   = true;
				job.garbage.mOriginX = (int)garb_world->origin_x;
				job.garbage.mOriginY = (int)garb_world->origin_y;
				job.garbage.mRefW = prd->garbRefW > 0 ? (int)prd->garbRefW
					: (int)(garb_world->width  / (dsx > 0 ? dsx : 1.0));
				job.garbage.mRefH = prd->garbRefH > 0 ? (int)prd->garbRefH
					: (int)(garb_world->height / (dsy > 0 ? dsy : 1.0));
				job.garbage.outOriginX = (int)out_world->origin_x;
				job.garbage.outOriginY = (int)out_world->origin_y;
				job.garbage.srcRefW = prd->srcRefW > 0 ? (int)prd->srcRefW
					: (int)(job.outW / (dsx > 0 ? dsx : 1.0));
				job.garbage.srcRefH = prd->srcRefH > 0 ? (int)prd->srcRefH
					: (int)(job.outH / (dsy > 0 ? dsy : 1.0));
				job.garbage.dsx = (float)dsx;
				job.garbage.dsy = (float)dsy;
				job.garbage.channel = (int)p.garbageChanL;
			} else {
				garb_world = nullptr;
			}
		}

		// HISTORY worlds, oldest-usable first is the HOST's job; here we just
		// collect them in t-N .. t-1 order (index 0 = oldest). Absent or
		// geometry-mismatched frames stay null — same rule as the CPU path.
		std::vector<PF_EffectWorld*> histWorlds;
		if (job.temporal) {
			histWorlds.resize(QK_TEMPORAL_WINDOW, nullptr);
			for (A_long j = QK_TEMPORAL_WINDOW; j >= 1; --j) {
				if (!(prd->histOKMask & (1L << (j - 1)))) continue;
				PF_EffectWorld *hw = nullptr;
				PF_Err herr = extra->cb->checkout_layer_pixels(
					in_data->effect_ref, QK_CHECKOUT_HIST0 + j - 1, &hw);
				if (herr != PF_Err_NONE || !hw) continue;
				if (hw->width != job.srcW || hw->height != job.srcH) continue;
				histWorlds[(size_t)(QK_TEMPORAL_WINDOW - j)] = hw;
			}
		}

		ERR(qkgpu::QK_DXRender(in_data, out_data, extra->input->gpu_data,
			extra->input->device_index, job, src_world, out_world,
			garb_world, histWorlds));
	}

cleanup:
	if (wsP) ERR2(AEFX_ReleaseSuite(in_data, out_data, kPFWorldSuite, kPFWorldSuiteVersion2, ""));
	return err;
#endif // _WIN32
}

// ---------------------------------------------------------------------------
// GPU device setup/setdown — platform dispatch. Frameworks without a host
// get gpu_data == null with PF_Err_NONE: AE then never offers this effect
// GPU render and the CPU reference carries the frame.
// ---------------------------------------------------------------------------
PF_Err
QK_GPUDeviceSetup(PF_InData *in_data, PF_OutData *out_data, PF_GPUDeviceSetupExtra *extra)
{
#ifdef _WIN32
	if (extra->input->what_gpu == PF_GPU_Framework_DIRECTX)
		return qkgpu::QK_DXDeviceSetup(in_data, out_data, extra);
#endif
	extra->output->gpu_data = nullptr;
	return PF_Err_NONE;
}

PF_Err
QK_GPUDeviceSetdown(PF_InData *in_data, PF_OutData *out_data, PF_GPUDeviceSetdownExtra *extra)
{
#ifdef _WIN32
	if (extra->input->what_gpu == PF_GPU_Framework_DIRECTX)
		return qkgpu::QK_DXDeviceSetdown(in_data, out_data, extra);
#endif
	return PF_Err_NONE;
}
