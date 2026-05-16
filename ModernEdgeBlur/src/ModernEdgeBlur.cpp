/*	ModernEdgeBlur.cpp	*/

#define NOMINMAX			// prevent Windows.h from defining min/max macros
#include "ModernEdgeBlur.h"
#include <cmath>
#include <algorithm>
#include <atomic>
#include <vector>

#ifndef MODERN_EDGE_BLUR_THREADED
#define MODERN_EDGE_BLUR_THREADED 1
#endif

// Per-render data passed to iterate callbacks via refcon. Stack-allocated
// inside SmartRender so each frame thread gets its own — MFR-safe.
struct EdgeBlurRenderInfo {
	EdgeBlurInfo		params;
	A_long				input_width;
	A_long				input_height;
	const float			*edge_weight_map;	// [H * W], row-major, 0..1
	const float			*blurred_rgba;		// [H * W * 4], ARGB float — 32-bit path only
	const PF_EffectWorld *blurred_world8;	// native 8-bit blurred world — 8-bit path only
	const PF_EffectWorld *blurred_world16;	// native 16-bit blurred world — 16-bit path only
};


static PF_Err
About(
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output)
{
	AEGP_SuiteHandler suites(in_data->pica_basicP);

	suites.ANSICallbacksSuite1()->sprintf(
		out_data->return_msg,
		"%s v%d.%d.%d.%d\r%s",
		STR(StrID_Name),
		MAJOR_VERSION,
		MINOR_VERSION,
		BUG_VERSION,
		BUILD_VERSION,
		STR(StrID_Description));

	return PF_Err_NONE;
}


static PF_Err
GlobalSetup(
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output)
{
	out_data->my_version = PF_VERSION(	MAJOR_VERSION,
										MINOR_VERSION,
										BUG_VERSION,
										STAGE_VERSION,
										BUILD_VERSION);

	out_data->out_flags  =	PF_OutFlag_DEEP_COLOR_AWARE		|
							PF_OutFlag_PIX_INDEPENDENT		|
							PF_OutFlag_USE_OUTPUT_EXTENT;

	out_data->out_flags2 =	PF_OutFlag2_FLOAT_COLOR_AWARE			|
							PF_OutFlag2_SUPPORTS_SMART_RENDER		|
							PF_OutFlag2_SUPPORTS_THREADED_RENDERING;

	return PF_Err_NONE;
}


static PF_Err
ParamsSetup(
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output)
{
	PF_Err		err = PF_Err_NONE;
	PF_ParamDef	def;

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX(	"Edge Radius",
							EDGE_BLUR_EDGE_RADIUS_MIN,
							EDGE_BLUR_EDGE_RADIUS_MAX,
							EDGE_BLUR_EDGE_RADIUS_MIN,
							EDGE_BLUR_EDGE_RADIUS_MAX,
							EDGE_BLUR_EDGE_RADIUS_DFLT,
							PF_Precision_HUNDREDTHS,
							0, 0,
							EDGE_RADIUS_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX(	"Blur Amount",
							EDGE_BLUR_BLUR_AMOUNT_MIN,
							EDGE_BLUR_BLUR_AMOUNT_MAX,
							EDGE_BLUR_BLUR_AMOUNT_MIN,
							EDGE_BLUR_BLUR_AMOUNT_MAX,
							EDGE_BLUR_BLUR_AMOUNT_DFLT,
							PF_Precision_HUNDREDTHS,
							0, 0,
							BLUR_AMOUNT_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_CHECKBOXX(	"Enable Color Dilation",
						FALSE,
						0,
						DILATE_EN_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX(	"Dilation Amount",
							EDGE_BLUR_DILATE_AMT_MIN,
							EDGE_BLUR_DILATE_AMT_MAX,
							EDGE_BLUR_DILATE_AMT_MIN,
							EDGE_BLUR_DILATE_AMT_MAX,
							EDGE_BLUR_DILATE_AMT_DFLT,
							PF_Precision_HUNDREDTHS,
							0, 0,
							DILATE_AMT_DISK_ID);

	out_data->num_params = EDGE_BLUR_NUM_PARAMS;

	return err;
}


// ---------------------------------------------------------------------------
// Alpha samplers + Gaussian kernel construction shared by erosion weight map
// and RGBA blur base. Erosion weight is built inline in SmartRender as
// pow(GaussBlur(alpha, edge_radius), 3) — see Architecture.md §4.
// ---------------------------------------------------------------------------

// Integer pixel alpha samplers (clamp-to-edge).
static inline float
SampleAlpha8(const PF_EffectWorld *world, A_long x, A_long y)
{
	x = std::max((A_long)0, std::min(world->width  - 1, x));
	y = std::max((A_long)0, std::min(world->height - 1, y));
	const PF_Pixel8 *row = reinterpret_cast<const PF_Pixel8*>(
		reinterpret_cast<const char*>(world->data) + y * world->rowbytes);
	return row[x].alpha / (float)PF_MAX_CHAN8;
}

static inline float
SampleAlpha16(const PF_EffectWorld *world, A_long x, A_long y)
{
	x = std::max((A_long)0, std::min(world->width  - 1, x));
	y = std::max((A_long)0, std::min(world->height - 1, y));
	const PF_Pixel16 *row = reinterpret_cast<const PF_Pixel16*>(
		reinterpret_cast<const char*>(world->data) + y * world->rowbytes);
	return row[x].alpha / (float)PF_MAX_CHAN16;
}

static inline float
SampleAlphaFloat(const PF_EffectWorld *world, A_long x, A_long y)
{
	x = std::max((A_long)0, std::min(world->width  - 1, x));
	y = std::max((A_long)0, std::min(world->height - 1, y));
	const PF_PixelFloat *row = reinterpret_cast<const PF_PixelFloat*>(
		reinterpret_cast<const char*>(world->data) + y * world->rowbytes);
	return std::min(1.0f, std::max(0.0f, row[x].alpha));
}


// Build a normalized 1D Gaussian kernel for the given radius.
// sigma = radius / 2.0 so that ~2 sigma covers the full radius.
// All three bit-depth paths call this so the kernel shape is identical.
static std::vector<float>
MakeGaussianKernel(float radius)
{
	const float sigma  = std::max(radius / 2.0f, 0.5f);
	const int   half   = (int)std::ceil(radius);
	const int   size   = 2 * half + 1;
	std::vector<float> k(size);
	float sum = 0.0f;
	for (int i = 0; i < size; ++i) {
		float x = (float)(i - half);
		k[i] = std::exp(-(x * x) / (2.0f * sigma * sigma));
		sum += k[i];
	}
	for (float &v : k) v /= sum;
	return k;
}


// ---------------------------------------------------------------------------
// iterate_generic infrastructure for parallel separable blur.
//
// Two passes (H, V) share one POD refcon and one scratch pool. Each AE worker
// gets a `stride`-sized float slot indexed by thread_indexL. stride = max(W,H)
// so the same slot works for either pass. H-pass blocks before V-pass begins,
// so no aliasing.
//
// Callbacks are extern "C" + static: C linkage guarantees __cdecl under MSVC
// even if project-wide convention flags ever change; static keeps them out of
// the export table.
// ---------------------------------------------------------------------------

struct MEB_BlurRefcon {
	float       *buf;
	A_long       W;
	A_long       H;
	const float *kdata;
	int          half;
	float       *scratch_base;   // n_workers * stride floats
	A_long       stride;         // = max(W, H)
};

struct MEB_ProbeRefcon {
	std::atomic<A_long> n_workers;
};

extern "C" {

static PF_Err
MEB_ProbeCB(void *refconPV, A_long thread_indexL, A_long /*i*/, A_long iterationsL)
{
	// AE substitutes the real worker count in `iterationsL` when called with
	// PF_Iterations_ONCE_PER_PROCESSOR. Record once from thread 0.
	if (thread_indexL == 0) {
		static_cast<MEB_ProbeRefcon*>(refconPV)->n_workers.store(iterationsL);
	}
	return PF_Err_NONE;
}

static PF_Err
MEB_HBlurRow(void *refconPV, A_long thread_indexL, A_long i, A_long /*iterationsL*/)
{
	const MEB_BlurRefcon *rc = static_cast<const MEB_BlurRefcon*>(refconPV);
	const A_long W    = rc->W;
	const int    half = rc->half;
	float       *row  = rc->buf + i * W;
	float       *tmp  = rc->scratch_base + thread_indexL * rc->stride;

	for (A_long x = 0; x < W; ++x) {
		float acc = 0.0f;
		for (int t = -half; t <= half; ++t) {
			A_long sx = std::min(W - 1, std::max((A_long)0, x + (A_long)t));
			acc += row[sx] * rc->kdata[t + half];
		}
		tmp[x] = acc;
	}
	for (A_long x = 0; x < W; ++x) row[x] = tmp[x];
	return PF_Err_NONE;
}

static PF_Err
MEB_VBlurCol(void *refconPV, A_long thread_indexL, A_long i, A_long /*iterationsL*/)
{
	const MEB_BlurRefcon *rc = static_cast<const MEB_BlurRefcon*>(refconPV);
	const A_long W    = rc->W;
	const A_long H    = rc->H;
	const int    half = rc->half;
	float       *tmp  = rc->scratch_base + thread_indexL * rc->stride;

	for (A_long y = 0; y < H; ++y) tmp[y] = rc->buf[y * W + i];
	for (A_long y = 0; y < H; ++y) {
		float acc = 0.0f;
		for (int t = -half; t <= half; ++t) {
			A_long sy = std::min(H - 1, std::max((A_long)0, y + (A_long)t));
			acc += tmp[sy] * rc->kdata[t + half];
		}
		rc->buf[y * W + i] = acc;
	}
	return PF_Err_NONE;
}

} // extern "C"


// Separable Gaussian blur on a float plane (one channel, row-major).
// Threaded via iterate_generic when MODERN_EDGE_BLUR_THREADED=1 and the AE
// callback is available; falls back to the serial implementation otherwise.
static void
GaussBlurFloat(float *buf, A_long W, A_long H, const std::vector<float> &k, PF_InData *in_data)
{
	if (k.empty() || W <= 0 || H <= 0) return;
	const int half = (int)(k.size() / 2);

#if MODERN_EDGE_BLUR_THREADED
	if (in_data && in_data->utils && in_data->utils->iterate_generic) {
		// Probe worker count. AE swaps PF_Iterations_ONCE_PER_PROCESSOR for the
		// real worker count in the callback's iterationsL.
		MEB_ProbeRefcon probe;
		probe.n_workers.store(0);
		PF_Err perr = in_data->utils->iterate_generic(
			PF_Iterations_ONCE_PER_PROCESSOR, &probe, MEB_ProbeCB);

		A_long n_workers = probe.n_workers.load();
		if (perr == PF_Err_NONE && n_workers > 0) {
			if (n_workers > 256) n_workers = 256;   // defensive clamp

			const A_long stride = std::max(W, H);
			std::vector<float> scratch((size_t)n_workers * (size_t)stride);

			MEB_BlurRefcon rc;
			rc.buf          = buf;
			rc.W            = W;
			rc.H            = H;
			rc.kdata        = k.data();
			rc.half         = half;
			rc.scratch_base = scratch.data();
			rc.stride       = stride;

			PF_Err herr = in_data->utils->iterate_generic(H, &rc, MEB_HBlurRow);
			if (herr == PF_Err_NONE) {
				PF_Err verr = in_data->utils->iterate_generic(W, &rc, MEB_VBlurCol);
				if (verr == PF_Err_NONE) return;
			}
			// On failure of either pass, fall through to serial — buffer may be
			// partially blurred; the serial path overwrites it cleanly.
		}
	}
#endif

	// Serial fallback (original implementation).
	std::vector<float> tmp(std::max(W, H));

	// Horizontal pass
	for (A_long y = 0; y < H; ++y) {
		float *row = buf + y * W;
		for (A_long x = 0; x < W; ++x) {
			float acc = 0.0f;
			for (int t = -half; t <= half; ++t) {
				A_long sx = std::min(W - 1, std::max((A_long)0, x + (A_long)t));
				acc += row[sx] * k[t + half];
			}
			tmp[x] = acc;
		}
		for (A_long x = 0; x < W; ++x) row[x] = tmp[x];
	}

	// Vertical pass
	for (A_long x = 0; x < W; ++x) {
		for (A_long y = 0; y < H; ++y) tmp[y] = buf[y * W + x];
		for (A_long y = 0; y < H; ++y) {
			float acc = 0.0f;
			for (int t = -half; t <= half; ++t) {
				A_long sy = std::min(H - 1, std::max((A_long)0, y + (A_long)t));
				acc += tmp[sy] * k[t + half];
			}
			buf[y * W + x] = acc;
		}
	}
}

// Unpack an EffectWorld into a flat ARGB float buffer [H * W * 4], row-major,
// channel order A=0 R=1 G=2 B=3. Caller owns the returned array (delete[]).
static float*
UnpackWorldToFloat(const PF_EffectWorld *world, PF_PixelFormat format)
{
	const A_long W = world->width;
	const A_long H = world->height;
	float *buf = new (std::nothrow) float[H * W * 4];
	if (!buf) return nullptr;

	for (A_long y = 0; y < H; ++y) {
		const char *src = reinterpret_cast<const char*>(world->data) + y * world->rowbytes;
		float *dst = buf + y * W * 4;
		switch (format) {
			case PF_PixelFormat_ARGB128: {
				const PF_PixelFloat *p = reinterpret_cast<const PF_PixelFloat*>(src);
				for (A_long x = 0; x < W; ++x, ++p, dst += 4) {
					dst[0] = p->alpha; dst[1] = p->red; dst[2] = p->green; dst[3] = p->blue;
				}
				break;
			}
			case PF_PixelFormat_ARGB64: {
				const PF_Pixel16 *p = reinterpret_cast<const PF_Pixel16*>(src);
				const float s = 1.0f / PF_MAX_CHAN16;
				for (A_long x = 0; x < W; ++x, ++p, dst += 4) {
					dst[0] = p->alpha * s; dst[1] = p->red * s; dst[2] = p->green * s; dst[3] = p->blue * s;
				}
				break;
			}
			default: {
				const PF_Pixel8 *p = reinterpret_cast<const PF_Pixel8*>(src);
				const float s = 1.0f / PF_MAX_CHAN8;
				for (A_long x = 0; x < W; ++x, ++p, dst += 4) {
					dst[0] = p->alpha * s; dst[1] = p->red * s; dst[2] = p->green * s; dst[3] = p->blue * s;
				}
				break;
			}
		}
	}
	return buf;
}

// Gaussian blur all 4 channels of a packed ARGB float buffer in-place.
// Deinterleaves each channel into a temp plane, blurs, then reinterleaves.
static void
GaussBlurRGBA(float *buf, A_long W, A_long H, const std::vector<float> &k, PF_InData *in_data)
{
	if (k.empty() || W <= 0 || H <= 0) return;
	float *plane = new (std::nothrow) float[W * H];
	if (!plane) return;

	for (int c = 0; c < 4; ++c) {
		for (A_long i = 0; i < W * H; ++i) plane[i] = buf[i * 4 + c];
		GaussBlurFloat(plane, W, H, k, in_data);
		for (A_long i = 0; i < W * H; ++i) buf[i * 4 + c] = plane[i];
	}
	delete[] plane;
}

// Separable Gaussian blur on a native 8/16-bit PF_EffectWorld in-place.
// T must be PF_Pixel8 or PF_Pixel16. Deinterleaves each channel to a float
// plane, runs GaussBlurFloat (which is iterate_generic-threaded), reinterleaves
// back with clamp-to-native. PixelTraits<T>::max_value provides the
// normalisation constant so one template covers both bit depths.
template <typename T>
static void
GaussBlurWorld(PF_EffectWorld *world, const std::vector<float> &k, PF_InData *in_data)
{
	if (k.empty() || !world) return;
	const A_long W    = world->width;
	const A_long H    = world->height;
	const float  maxv = (float)PixelTraits<T>::max_value;
	const float  invv = 1.0f / maxv;

	std::vector<float> plane((size_t)W * H);

	auto getRow = [&](A_long y) -> T* {
		return reinterpret_cast<T*>(
			reinterpret_cast<char*>(world->data) + y * world->rowbytes);
	};

	for (int c = 0; c < 4; ++c) {
		// Deinterleave channel c into float plane.
		for (A_long y = 0; y < H; ++y) {
			const T *row = getRow(y);
			float   *dst = plane.data() + y * W;
			for (A_long x = 0; x < W; ++x) {
				switch (c) {
					case 0: dst[x] = row[x].alpha * invv; break;
					case 1: dst[x] = row[x].red   * invv; break;
					case 2: dst[x] = row[x].green * invv; break;
					case 3: dst[x] = row[x].blue  * invv; break;
				}
			}
		}

		GaussBlurFloat(plane.data(), W, H, k, in_data);

		// Reinterleave back, clamping to native range.
		for (A_long y = 0; y < H; ++y) {
			T           *row = getRow(y);
			const float *src = plane.data() + y * W;
			for (A_long x = 0; x < W; ++x) {
				float v = std::min(1.0f, std::max(0.0f, src[x])) * maxv + 0.5f;
				typename PixelTraits<T>::DataType vt = (typename PixelTraits<T>::DataType)v;
				switch (c) {
					case 0: row[x].alpha = vt; break;
					case 1: row[x].red   = vt; break;
					case 2: row[x].green = vt; break;
					case 3: row[x].blue  = vt; break;
				}
			}
		}
	}
}


// Build edge weight map: extract alpha, Gaussian-blur by edge_radius to create
// a soft gradient zone around any hard or soft alpha edge. Result is [0..1]
// where 1 = fully opaque interior (sharp), 0 = transparent exterior (blurred).
// Caller owns the returned array (delete[]). NOTE: currently unused —
// SmartRender builds its erosion map inline so it can apply the pow(., 3)
// shaping. Kept here as a reusable building block.
static float*
BuildEdgeWeightMap(
	const PF_EffectWorld	*input_world,
	PF_PixelFormat			 format,
	PF_FpLong				 edge_radius,
	PF_InData				*in_data)
{
	const A_long W = input_world->width;
	const A_long H = input_world->height;
	if (W <= 0 || H <= 0) return nullptr;

	float *map = new (std::nothrow) float[W * H];
	if (!map) return nullptr;

	// Extract alpha channel as float [0..1].
	for (A_long y = 0; y < H; ++y) {
		for (A_long x = 0; x < W; ++x) {
			switch (format) {
				case PF_PixelFormat_ARGB128:
					map[y * W + x] = SampleAlphaFloat(input_world, x, y); break;
				case PF_PixelFormat_ARGB64:
					map[y * W + x] = SampleAlpha16(input_world, x, y); break;
				default:
					map[y * W + x] = SampleAlpha8(input_world, x, y); break;
			}
		}
	}

	// With no edge radius, output all-ones (sharp everywhere, no blend zone).
	if (edge_radius < 1e-4f) {
		for (A_long i = 0; i < W * H; ++i) map[i] = 1.0f;
		return map;
	}

	const std::vector<float> k = MakeGaussianKernel((float)edge_radius);
	GaussBlurFloat(map, W, H, k, in_data);

	// Clamp — blurred alpha is already 0 outside, 1 inside, soft at edge.
	// Used directly as sharp weight: lerp(blurred, sharp, w).
	for (A_long i = 0; i < W * H; ++i)
		map[i] = std::min(1.0f, std::max(0.0f, map[i]));

	return map;
}


// ---------------------------------------------------------------------------
// Iterate callbacks — per-pixel compose. Output alpha comes from the blurred
// base (so the matte itself softens), and output RGB is lerp(blurred, sharp, w)
// where w = pow(blurred_alpha, 3) was prebuilt in SmartRender. w≈1 inside the
// matte (sharp wins), w→0 at the eroded edge (blurred wins).
// ---------------------------------------------------------------------------

static PF_Err
EdgeBlurFunc8(
	void		*refcon,
	A_long		xL,
	A_long		yL,
	PF_Pixel8	*inP,
	PF_Pixel8	*outP)
{
	const EdgeBlurRenderInfo *info = static_cast<const EdgeBlurRenderInfo*>(refcon);
	const A_long idx = yL * info->input_width + xL;
	const float  w   = info->edge_weight_map[idx];
	const float  inv = 1.0f - w;
	const PF_Pixel8 *b = reinterpret_cast<const PF_Pixel8*>(
		reinterpret_cast<const char*>(info->blurred_world8->data) + yL * info->blurred_world8->rowbytes) + xL;
	outP->alpha = b->alpha;
	outP->red   = (A_u_char)((inP->red   * w + b->red   * inv) + 0.5f);
	outP->green = (A_u_char)((inP->green * w + b->green * inv) + 0.5f);
	outP->blue  = (A_u_char)((inP->blue  * w + b->blue  * inv) + 0.5f);
	return PF_Err_NONE;
}


static PF_Err
EdgeBlurFunc16(
	void		*refcon,
	A_long		xL,
	A_long		yL,
	PF_Pixel16	*inP,
	PF_Pixel16	*outP)
{
	const EdgeBlurRenderInfo *info = static_cast<const EdgeBlurRenderInfo*>(refcon);
	const A_long idx = yL * info->input_width + xL;
	const float  w   = info->edge_weight_map[idx];
	const float  inv = 1.0f - w;
	const PF_Pixel16 *b = reinterpret_cast<const PF_Pixel16*>(
		reinterpret_cast<const char*>(info->blurred_world16->data) + yL * info->blurred_world16->rowbytes) + xL;
	outP->alpha = b->alpha;
	outP->red   = (A_u_short)((inP->red   * w + b->red   * inv) + 0.5f);
	outP->green = (A_u_short)((inP->green * w + b->green * inv) + 0.5f);
	outP->blue  = (A_u_short)((inP->blue  * w + b->blue  * inv) + 0.5f);
	return PF_Err_NONE;
}


static PF_Err
EdgeBlurFuncFloat(
	void			*refcon,
	A_long			xL,
	A_long			yL,
	PF_PixelFloat	*inP,
	PF_PixelFloat	*outP)
{
	const EdgeBlurRenderInfo *info = static_cast<const EdgeBlurRenderInfo*>(refcon);
	const A_long idx = yL * info->input_width + xL;
	const float  w   = info->edge_weight_map[idx];
	const float  inv = 1.0f - w;
	const float *b   = info->blurred_rgba + idx * 4;
	outP->alpha = b[0];
	outP->red   = inP->red   * w + b[1] * inv;
	outP->green = inP->green * w + b[2] * inv;
	outP->blue  = inP->blue  * w + b[3] * inv;
	return PF_Err_NONE;
}


// Read all params at the given time into an EdgeBlurInfo. Called from both
// PreRender (to size the input region) and SmartRender (to drive the iterate).
static PF_Err
ReadParams(
	PF_InData		*in_data,
	EdgeBlurInfo	*out_params)
{
	PF_Err		err = PF_Err_NONE;
	PF_ParamDef	def;

	AEFX_CLR_STRUCT(def);

	#define CHECKOUT_FLOAT(IDX, FIELD)											\
		AEFX_CLR_STRUCT(def);													\
		ERR(PF_CHECKOUT_PARAM(in_data, IDX,										\
			in_data->current_time, in_data->time_step, in_data->time_scale,		\
			&def));																\
		out_params->FIELD = def.u.fs_d.value;									\
		ERR2(PF_CHECKIN_PARAM(in_data, &def));

	#define CHECKOUT_BOOL(IDX, FIELD)											\
		AEFX_CLR_STRUCT(def);													\
		ERR(PF_CHECKOUT_PARAM(in_data, IDX,										\
			in_data->current_time, in_data->time_step, in_data->time_scale,		\
			&def));																\
		out_params->FIELD = def.u.bd.value;										\
		ERR2(PF_CHECKIN_PARAM(in_data, &def));

	PF_Err err2 = PF_Err_NONE;
	CHECKOUT_FLOAT(EDGE_BLUR_EDGE_RADIUS, edge_radiusF);
	CHECKOUT_FLOAT(EDGE_BLUR_BLUR_AMOUNT, blur_amountF);
	CHECKOUT_BOOL (EDGE_BLUR_DILATE_EN,   dilate_en);
	CHECKOUT_FLOAT(EDGE_BLUR_DILATE_AMT,  dilate_amtF);

	#undef CHECKOUT_FLOAT
	#undef CHECKOUT_POPUP
	#undef CHECKOUT_BOOL

	return err;
}


// How many pixels of input we need beyond each side of the requested output
// region, so the blur convolution has the source it needs at the boundary.
static A_long
InflationPixels(const EdgeBlurInfo &p)
{
	// Slider units are pixels (1 px = 1 px). Inflate by edge_radius + blur_amount
	// so both convolutions have valid source at the output boundary; add the
	// dilation radius too when the (not-yet-implemented) dilation pass is on,
	// so render-region math will already be correct when it lands. +1 for slop.
	PF_FpLong total = p.edge_radiusF + p.blur_amountF;
	if (p.dilate_en) {
		total += p.dilate_amtF;
	}
	return (A_long)std::ceil(total) + 1;
}


static PF_Err
PreRender(
	PF_InData			*in_data,
	PF_OutData			*out_data,
	PF_PreRenderExtra	*extra)
{
	PF_Err				err = PF_Err_NONE;
	EdgeBlurInfo		params;
	AEFX_CLR_STRUCT(params);

	ERR(ReadParams(in_data, &params));

	const A_long inflate = InflationPixels(params);

	PF_RenderRequest req = extra->input->output_request;
	req.rect.left   -= inflate;
	req.rect.top    -= inflate;
	req.rect.right  += inflate;
	req.rect.bottom += inflate;
	req.preserve_rgb_of_zero_alpha = TRUE;

	PF_CheckoutResult in_result;
	ERR(extra->cb->checkout_layer(	in_data->effect_ref,
									EDGE_BLUR_INPUT,
									EDGE_BLUR_INPUT,
									&req,
									in_data->current_time,
									in_data->time_step,
									in_data->time_scale,
									&in_result));

	UnionLRect(&in_result.result_rect,     &extra->output->result_rect);
	UnionLRect(&in_result.max_result_rect, &extra->output->max_result_rect);

	// Clamp result_rect to the requested output rect — AE errors if we exceed it.
	const PF_LRect &out_req = extra->input->output_request.rect;
	extra->output->result_rect.left   = std::max(extra->output->result_rect.left,   out_req.left);
	extra->output->result_rect.top    = std::max(extra->output->result_rect.top,    out_req.top);
	extra->output->result_rect.right  = std::min(extra->output->result_rect.right,  out_req.right);
	extra->output->result_rect.bottom = std::min(extra->output->result_rect.bottom, out_req.bottom);

	return err;
}


static PF_Err
SmartRender(
	PF_InData				*in_data,
	PF_OutData				*out_data,
	PF_SmartRenderExtra		*extra)
{
	PF_Err		err  = PF_Err_NONE;
	PF_Err		err2 = PF_Err_NONE;

	PF_EffectWorld	*input_world  = NULL;
	PF_EffectWorld	*output_world = NULL;

	AEGP_SuiteHandler	suites(in_data->pica_basicP);
	PF_WorldSuite2		*wsP = NULL;

	ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, EDGE_BLUR_INPUT, &input_world));
	ERR(extra->cb->checkout_output(in_data->effect_ref, &output_world));

	ERR(AEFX_AcquireSuite(	in_data,
							out_data,
							kPFWorldSuite,
							kPFWorldSuiteVersion2,
							"Couldn't load PFWorldSuite2.",
							(void**)&wsP));

	EdgeBlurRenderInfo	info;
	AEFX_CLR_STRUCT(info);
	ERR(ReadParams(in_data, &info.params));

	PF_PixelFormat format = PF_PixelFormat_INVALID;
	if (!err && wsP && input_world && output_world) {
		info.input_width  = input_world->width;
		info.input_height = input_world->height;
		ERR(wsP->PF_GetPixelFormat(output_world, &format));
	}

	// Step 1: Build a blurred copy of the input for the colour-bleed base.
	// 32-bit: unpack to float[], blur there (no loss of precision).
	// 8/16-bit: copy to a native scratch world and blur in-place (no float expansion).
	float        *blurred_rgba   = nullptr;
	PF_EffectWorld blurred_world8  = {};
	PF_EffectWorld blurred_world16 = {};
	bool           world8_valid    = false;
	bool           world16_valid   = false;

	const std::vector<float> blur_k = (info.params.blur_amountF > 1e-4f)
		? MakeGaussianKernel((float)info.params.blur_amountF)
		: std::vector<float>{};

	if (!err && input_world) {
		const A_long W = input_world->width;
		const A_long H = input_world->height;

		if (format == PF_PixelFormat_ARGB128) {
			blurred_rgba = UnpackWorldToFloat(input_world, format);
			if (!blurred_rgba) {
				err = PF_Err_OUT_OF_MEMORY;
			} else if (!blur_k.empty()) {
				GaussBlurRGBA(blurred_rgba, W, H, blur_k, in_data);
			}
			info.blurred_rgba   = blurred_rgba;
			info.blurred_world8  = nullptr;
			info.blurred_world16 = nullptr;
		} else if (format == PF_PixelFormat_ARGB64) {
			ERR(wsP->PF_NewWorld(in_data->effect_ref, W, H,
				TRUE, PF_PixelFormat_ARGB64, &blurred_world16));
			if (!err) {
				world16_valid = true;
				ERR(in_data->utils->copy(in_data->effect_ref,
					input_world, &blurred_world16, nullptr, nullptr));
			}
			if (!err && !blur_k.empty())
				GaussBlurWorld<PF_Pixel16>(&blurred_world16, blur_k, in_data);
			info.blurred_rgba    = nullptr;
			info.blurred_world8  = nullptr;
			info.blurred_world16 = world16_valid ? &blurred_world16 : nullptr;
		} else {
			ERR(wsP->PF_NewWorld(in_data->effect_ref, W, H,
				TRUE, PF_PixelFormat_ARGB32, &blurred_world8));
			if (!err) {
				world8_valid = true;
				ERR(in_data->utils->copy(in_data->effect_ref,
					input_world, &blurred_world8, nullptr, nullptr));
			}
			if (!err && !blur_k.empty())
				GaussBlurWorld<PF_Pixel8>(&blurred_world8, blur_k, in_data);
			info.blurred_rgba    = nullptr;
			info.blurred_world8  = world8_valid ? &blurred_world8 : nullptr;
			info.blurred_world16 = nullptr;
		}
	}

	// Step 2: Build erosion weight map from the ORIGINAL alpha, blurred by edge_radius.
	// Result is 1 deep inside, 0 outside, with a soft gradient at the edge.
	// Used as sharp weight: lerp(blurred, sharp, w).
	float *edge_map = nullptr;
	if (!err && input_world) {
		const A_long W = input_world->width;
		const A_long H = input_world->height;
		edge_map = new (std::nothrow) float[W * H];
		if (!edge_map) {
			err = PF_Err_OUT_OF_MEMORY;
		} else {
			// Extract original alpha directly.
			for (A_long y = 0; y < H; ++y) {
				for (A_long x = 0; x < W; ++x) {
					switch (format) {
						case PF_PixelFormat_ARGB128:
							edge_map[y*W+x] = SampleAlphaFloat(input_world, x, y); break;
						case PF_PixelFormat_ARGB64:
							edge_map[y*W+x] = SampleAlpha16(input_world, x, y); break;
						default:
							edge_map[y*W+x] = SampleAlpha8(input_world, x, y); break;
					}
				}
			}

			// Gaussian-blur the alpha by edge_radius to spread the transition inward.
			// Power curve pulls the falloff further inside the matte so blurred RGB
			// shows through beneath the eroded sharp original.
			if (info.params.edge_radiusF > 1e-4f) {
				const std::vector<float> k = MakeGaussianKernel((float)info.params.edge_radiusF);
				GaussBlurFloat(edge_map, W, H, k, in_data);
				for (A_long i = 0; i < W * H; ++i)
					edge_map[i] = std::pow(std::min(1.0f, std::max(0.0f, edge_map[i])), 3.0f);
			} else {
				for (A_long i = 0; i < W * H; ++i)
					edge_map[i] = std::min(1.0f, std::max(0.0f, edge_map[i]));
			}
		}
	}
	info.edge_weight_map = edge_map;

	if (!err) {
		switch (format) {
			case PF_PixelFormat_ARGB128:
				ERR(suites.IterateFloatSuite2()->iterate(	in_data,
															0,
															output_world->height,
															input_world,
															NULL,
															(void*)&info,
															EdgeBlurFuncFloat,
															output_world));
				break;

			case PF_PixelFormat_ARGB64:
				ERR(suites.Iterate16Suite2()->iterate(		in_data,
															0,
															output_world->height,
															input_world,
															NULL,
															(void*)&info,
															EdgeBlurFunc16,
															output_world));
				break;

			case PF_PixelFormat_ARGB32:
				ERR(suites.Iterate8Suite2()->iterate(		in_data,
															0,
															output_world->height,
															input_world,
															NULL,
															(void*)&info,
															EdgeBlurFunc8,
															output_world));
				break;

			default:
				err = PF_Err_BAD_CALLBACK_PARAM;
				break;
		}
	}

	delete[] edge_map;
	delete[] blurred_rgba;
	if (world8_valid && wsP)
		wsP->PF_DisposeWorld(in_data->effect_ref, &blurred_world8);
	if (world16_valid && wsP)
		wsP->PF_DisposeWorld(in_data->effect_ref, &blurred_world16);

	if (wsP) {
		ERR2(AEFX_ReleaseSuite(	in_data,
								out_data,
								kPFWorldSuite,
								kPFWorldSuiteVersion2,
								"Couldn't release PFWorldSuite2."));
	}

	return err;
}


extern "C" DllExport
PF_Err PluginDataEntryFunction2(
	PF_PluginDataPtr inPtr,
	PF_PluginDataCB2 inPluginDataCallBackPtr,
	SPBasicSuite* inSPBasicSuitePtr,
	const char* inHostName,
	const char* inHostVersion)
{
	PF_Err result = PF_Err_INVALID_CALLBACK;

	result = PF_REGISTER_EFFECT_EXT2(
		inPtr,
		inPluginDataCallBackPtr,
		"Modern Edge Blur",
		"MM Modern Edge Blur",
		"MM",
		AE_RESERVED_INFO,
		"EffectMain",
		"");

	return result;
}


PF_Err
EffectMain(
	PF_Cmd			cmd,
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output,
	void			*extra)
{
	PF_Err err = PF_Err_NONE;

	try {
		switch (cmd) {
			case PF_Cmd_ABOUT:
				err = About(in_data, out_data, params, output);
				break;

			case PF_Cmd_GLOBAL_SETUP:
				err = GlobalSetup(in_data, out_data, params, output);
				break;

			case PF_Cmd_PARAMS_SETUP:
				err = ParamsSetup(in_data, out_data, params, output);
				break;

			case PF_Cmd_SMART_PRE_RENDER:
				err = PreRender(in_data, out_data, (PF_PreRenderExtra*)extra);
				break;

			case PF_Cmd_SMART_RENDER:
				err = SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra);
				break;
		}
	} catch (PF_Err &thrown_err) {
		err = thrown_err;
	}
	return err;
}
