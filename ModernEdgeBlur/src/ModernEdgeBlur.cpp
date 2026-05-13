/*	ModernEdgeBlur.cpp	*/

#define NOMINMAX			// prevent Windows.h from defining min/max macros
#include "ModernEdgeBlur.h"
#include <cmath>
#include <algorithm>
#include <vector>

// Per-render data passed to iterate callbacks via refcon. Stack-allocated
// inside SmartRender so each frame thread gets its own — MFR-safe.
struct EdgeBlurRenderInfo {
	EdgeBlurInfo	params;
	A_long			input_width;
	A_long			input_height;
	const float		*edge_weight_map;	// [H * W], row-major, 0..1
	const float		*blurred_rgba;		// [H * W * 4], ARGB float, box-blurred input
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
// Phase 1: Sobel-gradient edge weight map
//
// Returns a heap-allocated float array [height * width] (caller must delete[]).
// Values are in [0,1]: 1 = strong edge, 0 = flat interior or flat transparent.
// Edge Radius controls the spread (box blur kernel half-width = edge_radius px).
// Edge Offset shifts the threshold after spreading: positive → outward (into
// transparent), negative → inward (into opaque).
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


// Single-pass separable box blur — internal, called by BoxBlurFloat3.
static void
BoxBlurFloatOnce(float *buf, A_long width, A_long height, A_long kernel_radius)
{
	if (kernel_radius <= 0 || width <= 0 || height <= 0) return;

	std::vector<float> tmp(width > height ? width : height);

	// Horizontal pass
	for (A_long y = 0; y < height; ++y) {
		float *row = buf + y * width;
		// Initialise window for x=0: samples [0-kernel_radius .. 0+kernel_radius], clamped.
		double sum = 0.0;
		A_long count = 2 * kernel_radius + 1;
		for (A_long k = -kernel_radius; k <= kernel_radius; ++k) {
			A_long sx = std::max((A_long)0, std::min(width - 1, k));  // x=0, so sample at k
			sum += row[sx];
		}
		for (A_long x = 0; x < width; ++x) {
			tmp[x] = (float)(sum / count);
			// Slide window: remove left sample, add right sample (both clamped).
			A_long xl = x - kernel_radius;
			A_long xr = x + kernel_radius + 1;
			sum -= row[std::max((A_long)0, xl)];
			sum += row[std::min(width - 1, xr)];
		}
		for (A_long x = 0; x < width; ++x) row[x] = tmp[x];
	}

	// Vertical pass
	for (A_long x = 0; x < width; ++x) {
		for (A_long y = 0; y < height; ++y) tmp[y] = buf[y * width + x];

		double sum = 0.0;
		A_long count = 2 * kernel_radius + 1;
		for (A_long k = -kernel_radius; k <= kernel_radius; ++k) {
			A_long sy = std::max((A_long)0, std::min(height - 1, k));  // y=0
			sum += tmp[sy];
		}
		for (A_long y = 0; y < height; ++y) {
			buf[y * width + x] = (float)(sum / count);
			A_long yt = y - kernel_radius;
			A_long yb = y + kernel_radius + 1;
			sum -= tmp[std::max((A_long)0, yt)];
			sum += tmp[std::min(height - 1, yb)];
		}
	}
}


// 3-pass box blur approximating a Gaussian. Each pass uses radius/3 so the
// combined spread matches the requested radius with circular-kernel quality.
static void
BoxBlurFloat(float *buf, A_long width, A_long height, A_long kernel_radius)
{
	if (kernel_radius <= 0) return;
	// Distribute radius across 3 passes: r0=r1=(radius/3), r2=remainder.
	const A_long r0 = kernel_radius / 3;
	const A_long r2 = kernel_radius - 2 * r0;
	BoxBlurFloatOnce(buf, width, height, r0);
	BoxBlurFloatOnce(buf, width, height, r0);
	BoxBlurFloatOnce(buf, width, height, r2);
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

// Box-blur RGB channels only (skip alpha=ch0) of a packed ARGB float buffer.
// Single-pass box blur — sufficient for the blurred-colour base, 3x faster than
// the 3-pass Gaussian approximation used for the weight map.
static void
BoxBlurRGBA(float *buf, A_long W, A_long H, A_long kernel_radius)
{
	if (kernel_radius <= 0) return;
	float *plane = new (std::nothrow) float[W * H];
	if (!plane) return;

	for (int c = 0; c < 4; ++c) {  // all channels including alpha so colour bleeds outward
		for (A_long i = 0; i < W * H; ++i) plane[i] = buf[i * 4 + c];
		BoxBlurFloatOnce(plane, W, H, kernel_radius);  // single pass, not 3-pass
		for (A_long i = 0; i < W * H; ++i) buf[i * 4 + c] = plane[i];
	}
	delete[] plane;
}

// Build edge weight map: extract alpha, box-blur by edge_radius to create a
// soft gradient zone around any hard or soft alpha edge. Result is [0..1]
// where 1 = fully opaque interior (sharp), 0 = transparent exterior (blurred).
// Caller owns the returned array (delete[]).
static float*
BuildEdgeWeightMap(
	const PF_EffectWorld	*input_world,
	PF_PixelFormat			 format,
	PF_FpLong				 edge_radius)
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

	// Box-blur the alpha by edge_radius — spreads the 0→1 transition zone.
	// Fractional blend avoids integer snapping.
	const A_long kr_lo = (A_long)std::floor(edge_radius);
	const A_long kr_hi = kr_lo + 1;
	const float  frac  = (float)(edge_radius - std::floor(edge_radius));

	if (frac < 1e-4f) {
		BoxBlurFloat(map, W, H, kr_lo);
	} else {
		float *map_hi = new (std::nothrow) float[W * H];
		if (!map_hi) { delete[] map; return nullptr; }
		std::copy(map, map + W * H, map_hi);

		BoxBlurFloat(map,    W, H, kr_lo);
		BoxBlurFloat(map_hi, W, H, kr_hi);

		for (A_long i = 0; i < W * H; ++i)
			map[i] = map[i] * (1.0f - frac) + map_hi[i] * frac;

		delete[] map_hi;
	}

	// Clamp — blurred alpha is already 0 outside, 1 inside, soft at edge.
	// Used directly as sharp weight: lerp(blurred, sharp, w).
	for (A_long i = 0; i < W * H; ++i)
		map[i] = std::min(1.0f, std::max(0.0f, map[i]));

	return map;
}


// ---------------------------------------------------------------------------
// Iterate callbacks — lerp(input, blurred, edge_weight) per pixel
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
	// w=1 deep inside (sharp original), w=0 at edge (blurred base shows through).
	const A_long idx = yL * info->input_width + xL;
	const float  w   = info->edge_weight_map[idx];
	const float  inv = 1.0f - w;
	const float *b   = info->blurred_rgba + idx * 4;
	outP->alpha = (A_u_char)(b[0] * PF_MAX_CHAN8 + 0.5f);
	outP->red   = (A_u_char)((inP->red   / (float)PF_MAX_CHAN8 * w + b[1] * inv) * PF_MAX_CHAN8 + 0.5f);
	outP->green = (A_u_char)((inP->green / (float)PF_MAX_CHAN8 * w + b[2] * inv) * PF_MAX_CHAN8 + 0.5f);
	outP->blue  = (A_u_char)((inP->blue  / (float)PF_MAX_CHAN8 * w + b[3] * inv) * PF_MAX_CHAN8 + 0.5f);
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
	const float *b   = info->blurred_rgba + idx * 4;
	outP->alpha = (A_u_short)(b[0] * PF_MAX_CHAN16 + 0.5f);
	outP->red   = (A_u_short)((inP->red   / (float)PF_MAX_CHAN16 * w + b[1] * inv) * PF_MAX_CHAN16 + 0.5f);
	outP->green = (A_u_short)((inP->green / (float)PF_MAX_CHAN16 * w + b[2] * inv) * PF_MAX_CHAN16 + 0.5f);
	outP->blue  = (A_u_short)((inP->blue  / (float)PF_MAX_CHAN16 * w + b[3] * inv) * PF_MAX_CHAN16 + 0.5f);
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
	// Edge_radius and blur_amount are slider units (interpreted as pixels for
	// now). Dilation also reads neighbors. Add 1 for safety against rounding.
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

	// Step 1: Unpack and box-blur the full RGBA by blur_amount.
	// Alpha blurs outward naturally, RGB colour bleeds with it.
	float *blurred_rgba = nullptr;
	if (!err && input_world) {
		blurred_rgba = UnpackWorldToFloat(input_world, format);
		if (!blurred_rgba) {
			err = PF_Err_OUT_OF_MEMORY;
		} else if (info.params.blur_amountF > 1e-4f) {
			const A_long W  = input_world->width;
			const A_long H  = input_world->height;
			const A_long kr = std::max((A_long)1, (A_long)std::round(info.params.blur_amountF));

			BoxBlurRGBA(blurred_rgba, W, H, kr);
		}
	}
	info.blurred_rgba = blurred_rgba;

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

			// Blur by edge_radius to spread the alpha transition inward.
			// Then raise to power >1 to push the falloff further inside the matte,
			// creating a visible zone where w < 1 but original alpha > 0 — this is
			// where the blurred RGB shows through beneath the eroded sharp original.
			if (info.params.edge_radiusF > 1e-4f) {
				const A_long kr = std::max((A_long)1, (A_long)std::round(info.params.edge_radiusF));
				BoxBlurFloat(edge_map, W, H, kr);
				// Power curve pulls transition inward — higher = more erosion inward.
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
