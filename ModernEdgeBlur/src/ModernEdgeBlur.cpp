/*	ModernEdgeBlur.cpp	*/

#define NOMINMAX			// prevent Windows.h from defining min/max macros
#include "ModernEdgeBlur.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <vector>

#ifndef MODERN_EDGE_BLUR_THREADED
#define MODERN_EDGE_BLUR_THREADED 1
#endif

// Per-render data passed to iterate callbacks via refcon. Stack-allocated
// inside SmartRender so each frame thread gets its own — MFR-safe.
//
// As of the Phase 4 corrective rewrite, the pipeline is unified on packed-float
// regardless of bit depth: the input is unpacked once, optionally cleaned via
// BuildCleanSource, then colour-bleed-blurred. The callbacks read sharp from
// clean_rgba and blurred from blurred_rgba and quantise to native on writeback.
struct EdgeBlurRenderInfo {
	EdgeBlurInfo		params;
	A_long				input_width;
	A_long				input_height;
	const float			*edge_weight_map;	// [H * W], row-major, 0..1
	const float			*clean_rgba;		// [H * W * 4], ARGB float — sharp side of compose
	const float			*blurred_rgba;		// [H * W * 4], ARGB float — blurred side of compose
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
	PF_ADD_FLOAT_SLIDERX(	"Edge Extend",
							EDGE_BLUR_DILATE_AMT_MIN,
							EDGE_BLUR_DILATE_AMT_MAX,
							EDGE_BLUR_DILATE_AMT_MIN,
							EDGE_BLUR_DILATE_AMT_MAX,
							EDGE_BLUR_DILATE_AMT_DFLT,
							PF_Precision_HUNDREDTHS,
							0, 0,
							DILATE_AMT_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX(	"Sample Distance",
							EDGE_BLUR_SAMPLE_DIST_MIN,
							EDGE_BLUR_SAMPLE_DIST_MAX,
							EDGE_BLUR_SAMPLE_DIST_MIN,
							EDGE_BLUR_SAMPLE_DIST_MAX,
							EDGE_BLUR_SAMPLE_DIST_DFLT,
							PF_Precision_HUNDREDTHS,
							0, 0,
							SAMPLE_DIST_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_CHECKBOXX(	"Sharp Matte Edge",
						FALSE,
						0,
						SHARP_ALPHA_DISK_ID);

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

// Morphological min-filter refcon and callbacks. Mirrors the Gauss layout
// (per-thread scratch slot keyed by thread_indexL) but uses a windowed min
// instead of a weighted sum. Used for true erosion of the alpha channel (Phase 4
// Sample Distance) — produces hard, predictable inward chokes without the
// "two-way smear" of Gaussian+threshold.

struct MEB_MinRefcon {
	float       *buf;
	A_long       W;
	A_long       H;
	int          radius;            // half-window width in pixels
	float       *scratch_base;      // n_workers * stride floats
	A_long       stride;
};

extern "C" {

static PF_Err
MEB_HMinRow(void *refconPV, A_long thread_indexL, A_long i, A_long /*iterationsL*/)
{
	const MEB_MinRefcon *rc = static_cast<const MEB_MinRefcon*>(refconPV);
	const A_long W      = rc->W;
	const int    radius = rc->radius;
	float       *row    = rc->buf + i * W;
	float       *tmp    = rc->scratch_base + thread_indexL * rc->stride;

	for (A_long x = 0; x < W; ++x) {
		float m = row[x];
		const A_long lo = std::max((A_long)0, x - (A_long)radius);
		const A_long hi = std::min(W - 1, x + (A_long)radius);
		for (A_long s = lo; s <= hi; ++s) {
			if (row[s] < m) m = row[s];
		}
		tmp[x] = m;
	}
	for (A_long x = 0; x < W; ++x) row[x] = tmp[x];
	return PF_Err_NONE;
}

static PF_Err
MEB_VMinCol(void *refconPV, A_long thread_indexL, A_long i, A_long /*iterationsL*/)
{
	const MEB_MinRefcon *rc = static_cast<const MEB_MinRefcon*>(refconPV);
	const A_long W      = rc->W;
	const A_long H      = rc->H;
	const int    radius = rc->radius;
	float       *tmp    = rc->scratch_base + thread_indexL * rc->stride;

	for (A_long y = 0; y < H; ++y) tmp[y] = rc->buf[y * W + i];
	for (A_long y = 0; y < H; ++y) {
		float m = tmp[y];
		const A_long lo = std::max((A_long)0, y - (A_long)radius);
		const A_long hi = std::min(H - 1, y + (A_long)radius);
		for (A_long s = lo; s <= hi; ++s) {
			if (tmp[s] < m) m = tmp[s];
		}
		rc->buf[y * W + i] = m;
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

// Separable morphological min-filter on a float plane. Each pixel becomes the
// minimum value within `radius` pixels along each axis (clamp-to-edge). True
// erosion — no rounding of internal corners, no two-way smear like Gaussian
// approaches. Threaded identically to GaussBlurFloat. Naive O(radius) per
// pixel; fine for our typical Sample Distance values (0..50).
static void
MinFilterFloat(float *buf, A_long W, A_long H, int radius, PF_InData *in_data)
{
	if (radius <= 0 || W <= 0 || H <= 0) return;

#if MODERN_EDGE_BLUR_THREADED
	if (in_data && in_data->utils && in_data->utils->iterate_generic) {
		MEB_ProbeRefcon probe;
		probe.n_workers.store(0);
		PF_Err perr = in_data->utils->iterate_generic(
			PF_Iterations_ONCE_PER_PROCESSOR, &probe, MEB_ProbeCB);

		A_long n_workers = probe.n_workers.load();
		if (perr == PF_Err_NONE && n_workers > 0) {
			if (n_workers > 256) n_workers = 256;

			const A_long stride = std::max(W, H);
			std::vector<float> scratch((size_t)n_workers * (size_t)stride);

			MEB_MinRefcon rc;
			rc.buf          = buf;
			rc.W            = W;
			rc.H            = H;
			rc.radius       = radius;
			rc.scratch_base = scratch.data();
			rc.stride       = stride;

			PF_Err herr = in_data->utils->iterate_generic(H, &rc, MEB_HMinRow);
			if (herr == PF_Err_NONE) {
				PF_Err verr = in_data->utils->iterate_generic(W, &rc, MEB_VMinCol);
				if (verr == PF_Err_NONE) return;
			}
		}
	}
#endif

	// Serial fallback.
	std::vector<float> tmp(std::max(W, H));

	// Horizontal pass
	for (A_long y = 0; y < H; ++y) {
		float *row = buf + y * W;
		for (A_long x = 0; x < W; ++x) {
			float m = row[x];
			const A_long lo = std::max((A_long)0, x - (A_long)radius);
			const A_long hi = std::min(W - 1, x + (A_long)radius);
			for (A_long s = lo; s <= hi; ++s) {
				if (row[s] < m) m = row[s];
			}
			tmp[x] = m;
		}
		for (A_long x = 0; x < W; ++x) row[x] = tmp[x];
	}

	// Vertical pass
	for (A_long x = 0; x < W; ++x) {
		for (A_long y = 0; y < H; ++y) tmp[y] = buf[y * W + x];
		for (A_long y = 0; y < H; ++y) {
			float m = tmp[y];
			const A_long lo = std::max((A_long)0, y - (A_long)radius);
			const A_long hi = std::min(H - 1, y + (A_long)radius);
			for (A_long s = lo; s <= hi; ++s) {
				if (tmp[s] < m) m = tmp[s];
			}
			buf[y * W + x] = m;
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
// Pure colour-bleed blur — dilation is handled separately in BuildCleanSource.
static void
GaussBlurRGBA(float *buf, A_long W, A_long H,
              const std::vector<float> &k, PF_InData *in_data)
{
	if (k.empty() || W <= 0 || H <= 0) return;
	const A_long N = W * H;
	float *plane = new (std::nothrow) float[N];
	if (!plane) return;

	for (int c = 0; c < 4; ++c) {
		for (A_long i = 0; i < N; ++i) plane[i] = buf[i * 4 + c];
		GaussBlurFloat(plane, W, H, k, in_data);
		for (A_long i = 0; i < N; ++i) buf[i * 4 + c] = plane[i];
	}
	delete[] plane;
}

// Build a "clean source" packed-float ARGB buffer in-place from the unpacked
// input. Implements the Phase 4 dilation pipeline as a source preprocessing
// step. See Architecture.md §4.2 and the plan file for the algorithm.
//
// When dilate_en is false (or dilate_k is empty), leaves buf unchanged.
// Otherwise produces a "clean source" where:
//   - Inside the original matte (input_alpha > EPS): RGB is untouched. So
//     enabling dilation never softens the interior; only the edge changes.
//   - Outside the original matte: RGB is filled by the alpha-weighted-
//     neighbourhood divide (the "dilation" math), sourced from the eroded
//     interior (Sample Distance hard-chokes the sampling silhouette inward).
//   - Output alpha is either input_alpha (Sharp Matte OFF — compose will swap
//     in the blurred alpha to give outward edge bleed) OR the input_alpha
//     hard-grown outward by Edge Extend pixels (Sharp Matte ON).
//
// Sample Distance is decoupled from Edge Extend: the grown matte (Sharp Matte
// ON) is always measured from the INPUT alpha, never from the eroded one.
static void
BuildCleanSource(float *buf, A_long W, A_long H,
                 const EdgeBlurInfo &p,
                 const std::vector<float> &dilate_k,
                 PF_InData *in_data)
{
	if (!p.dilate_en || dilate_k.empty() || W <= 0 || H <= 0) return;
	const A_long N = W * H;

	// Snapshot input alpha — we'll need it after we overwrite buf's alpha channel.
	std::vector<float> input_alpha(N);
	for (A_long i = 0; i < N; ++i) input_alpha[i] = buf[i * 4 + 0];

	constexpr float EPS = 1e-4f;

	// Stage 1: build a hard 0/1 sample-zone mask via min-filter erosion of the
	// input alpha. mask[i] == 1 only where every neighbour within sample_radius
	// is fully opaque (or near-fully — we threshold at EPS to be tolerant of
	// near-1 alpha values from typical mattes). Pixels outside the eroded zone
	// contribute nothing to the dilation's colour-sampling sum.
	const int sample_radius = (int)std::ceil(p.sample_distF);
	const bool do_sample = sample_radius > 0;
	std::vector<float> mask;
	if (do_sample) {
		mask.resize(N);
		for (A_long i = 0; i < N; ++i) mask[i] = input_alpha[i];
		MinFilterFloat(mask.data(), W, H, sample_radius, in_data);
		// Threshold to 0/1 so masked-out pixels contribute exactly zero to the
		// dilation Gaussian (no soft-leak of contaminated edge alpha into the
		// blurred sum).
		for (A_long i = 0; i < N; ++i) mask[i] = (mask[i] > EPS) ? 1.0f : 0.0f;
	}

	// Stage 2: alpha-weighted-neighbourhood divide on the eroded source.
	// blur_a is the divisor for the colour push: Gauss of the masked alpha, so
	// it's only non-zero in the dilation reach of confident interior pixels.
	std::vector<float> blur_a(N);
	for (A_long i = 0; i < N; ++i)
		blur_a[i] = do_sample ? input_alpha[i] * mask[i] : input_alpha[i];
	GaussBlurFloat(blur_a.data(), W, H, dilate_k, in_data);

	// Stage 3: choose output alpha (independent of Sample Distance — always
	// measured from INPUT alpha, never from eroded). For Sharp Matte ON, the
	// grown alpha is a hard threshold of a Gaussian-of-input — same "any
	// neighbour-reach counts" semantics, but driven by the un-eroded source so
	// Edge Extend's reach is unaffected by Sample Distance.
	std::vector<float> out_alpha(N);
	if (p.sharp_alpha) {
		// Need an UN-eroded Gauss of input alpha for the grow measurement.
		std::vector<float> grow_a(N);
		for (A_long i = 0; i < N; ++i) grow_a[i] = input_alpha[i];
		GaussBlurFloat(grow_a.data(), W, H, dilate_k, in_data);
		for (A_long i = 0; i < N; ++i) {
			const float grown = (grow_a[i] > EPS) ? 1.0f : 0.0f;
			out_alpha[i] = std::max(input_alpha[i], grown);
		}
	} else {
		for (A_long i = 0; i < N; ++i) out_alpha[i] = input_alpha[i];
	}

	// Stage 4: per-channel dilation + compose. The compose rule: inside the
	// original matte keep input RGB untouched; outside, fill with the dilation
	// result re-premultiplied by the output alpha.
	std::vector<float> plane(N);
	for (int c = 1; c <= 3; ++c) {
		// Eroded premult RGB → Gauss → straight RGB via divide.
		for (A_long i = 0; i < N; ++i)
			plane[i] = do_sample ? buf[i * 4 + c] * mask[i] : buf[i * 4 + c];
		GaussBlurFloat(plane.data(), W, H, dilate_k, in_data);

		for (A_long i = 0; i < N; ++i) {
			if (input_alpha[i] > EPS) {
				// Inside original matte: untouched. (buf[i*4+c] already holds
				// the input value here.)
				continue;
			}
			const float straight = plane[i] / std::max(blur_a[i], EPS);
			buf[i * 4 + c] = straight * out_alpha[i];   // premult fill
		}
	}

	// Stage 5: write the output alpha. Must be last — Stage 4's "inside
	// original matte" test reads input_alpha indirectly (via the snapshot),
	// not buf's alpha, so order is actually independent. Keeping last for
	// symmetry with the previous structure.
	for (A_long i = 0; i < N; ++i) buf[i * 4 + 0] = out_alpha[i];
}


// ---------------------------------------------------------------------------
// Iterate callbacks — per-pixel compose. Output alpha comes from the blurred
// base (so the matte itself softens), and output RGB is lerp(blurred, sharp, w)
// where w = pow(blurred_alpha, 3) was prebuilt in SmartRender. w≈1 inside the
// matte (sharp wins), w→0 at the eroded edge (blurred wins).
// ---------------------------------------------------------------------------

// All three callbacks compose from the unified packed-float pipeline:
//   sharp side  = info->clean_rgba   (the dilation-preprocessed input)
//   blurred side= info->blurred_rgba (clean_rgba run through the colour-bleed blur)
//   output alpha= clean alpha (Sharp Matte Edge / dilation handled upstream)
// inP is unused; we only need the (xL, yL) index. The 8/16 callbacks quantise
// to native on writeback; the float callback writes the float values directly.

static PF_Err
EdgeBlurFunc8(
	void		*refcon,
	A_long		xL,
	A_long		yL,
	PF_Pixel8	* /*inP*/,
	PF_Pixel8	*outP)
{
	const EdgeBlurRenderInfo *info = static_cast<const EdgeBlurRenderInfo*>(refcon);
	const A_long idx  = yL * info->input_width + xL;
	const float  w    = info->edge_weight_map[idx];
	const float  inv  = 1.0f - w;
	const float *s    = info->clean_rgba   + idx * 4;
	const float *b    = info->blurred_rgba + idx * 4;
	const float  out_a = info->params.sharp_alpha ? s[0] : b[0];
	constexpr float M = (float)PF_MAX_CHAN8;
	outP->alpha = (A_u_char)(std::min(1.0f, std::max(0.0f, out_a))             * M + 0.5f);
	outP->red   = (A_u_char)(std::min(1.0f, std::max(0.0f, s[1]*w + b[1]*inv)) * M + 0.5f);
	outP->green = (A_u_char)(std::min(1.0f, std::max(0.0f, s[2]*w + b[2]*inv)) * M + 0.5f);
	outP->blue  = (A_u_char)(std::min(1.0f, std::max(0.0f, s[3]*w + b[3]*inv)) * M + 0.5f);
	return PF_Err_NONE;
}


static PF_Err
EdgeBlurFunc16(
	void		*refcon,
	A_long		xL,
	A_long		yL,
	PF_Pixel16	* /*inP*/,
	PF_Pixel16	*outP)
{
	const EdgeBlurRenderInfo *info = static_cast<const EdgeBlurRenderInfo*>(refcon);
	const A_long idx  = yL * info->input_width + xL;
	const float  w    = info->edge_weight_map[idx];
	const float  inv  = 1.0f - w;
	const float *s    = info->clean_rgba   + idx * 4;
	const float *b    = info->blurred_rgba + idx * 4;
	const float  out_a = info->params.sharp_alpha ? s[0] : b[0];
	constexpr float M = (float)PF_MAX_CHAN16;
	outP->alpha = (A_u_short)(std::min(1.0f, std::max(0.0f, out_a))            * M + 0.5f);
	outP->red   = (A_u_short)(std::min(1.0f, std::max(0.0f, s[1]*w + b[1]*inv)) * M + 0.5f);
	outP->green = (A_u_short)(std::min(1.0f, std::max(0.0f, s[2]*w + b[2]*inv)) * M + 0.5f);
	outP->blue  = (A_u_short)(std::min(1.0f, std::max(0.0f, s[3]*w + b[3]*inv)) * M + 0.5f);
	return PF_Err_NONE;
}


static PF_Err
EdgeBlurFuncFloat(
	void			*refcon,
	A_long			xL,
	A_long			yL,
	PF_PixelFloat	* /*inP*/,
	PF_PixelFloat	*outP)
{
	const EdgeBlurRenderInfo *info = static_cast<const EdgeBlurRenderInfo*>(refcon);
	const A_long idx = yL * info->input_width + xL;
	const float  w   = info->edge_weight_map[idx];
	const float  inv = 1.0f - w;
	const float *s   = info->clean_rgba   + idx * 4;
	const float *b   = info->blurred_rgba + idx * 4;
	outP->alpha = info->params.sharp_alpha ? s[0] : b[0];
	outP->red   = s[1] * w + b[1] * inv;
	outP->green = s[2] * w + b[2] * inv;
	outP->blue  = s[3] * w + b[3] * inv;
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
	CHECKOUT_FLOAT(EDGE_BLUR_SAMPLE_DIST, sample_distF);
	CHECKOUT_BOOL (EDGE_BLUR_SHARP_ALPHA, sharp_alpha);

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
	// Slider units are pixels. Inflate by edge_radius + blur_amount so both
	// convolutions have valid source at the output boundary; when dilation is
	// on, also add the dilation radius and the sample-distance erosion so
	// neither reads outside the checked-out region. +1 for rounding slop.
	PF_FpLong total = p.edge_radiusF + p.blur_amountF;
	if (p.dilate_en) {
		total += p.dilate_amtF + p.sample_distF;
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

	// Unified packed-float pipeline (Phase 4 corrective rewrite):
	//   Stage A. UnpackWorldToFloat → BuildCleanSource (Phase 3/4 dilation)
	//   Stage B. erosion weight map from clean alpha (Phase 1)
	//   Stage C. colour-bleed blur on a copy of the clean source (Phase 2)
	//   Stage D. iterate compose, quantising to native on writeback
	const std::vector<float> blur_k = (info.params.blur_amountF > 1e-4f)
		? MakeGaussianKernel((float)info.params.blur_amountF)
		: std::vector<float>{};

	const std::vector<float> dilate_k = (info.params.dilate_en && info.params.dilate_amtF > 1e-4f)
		? MakeGaussianKernel((float)info.params.dilate_amtF)
		: std::vector<float>{};

	// Sample Distance now uses a morphological min-filter (true erosion) instead
	// of a Gaussian + threshold. The pixel radius is read directly from params
	// inside BuildCleanSource — no kernel allocation needed.

	float *clean_rgba   = nullptr;
	float *blurred_rgba = nullptr;
	float *edge_map     = nullptr;

	if (!err && input_world) {
		const A_long W = input_world->width;
		const A_long H = input_world->height;
		const A_long N = W * H;

		// Stage A.
		clean_rgba = UnpackWorldToFloat(input_world, format);
		if (!clean_rgba) err = PF_Err_OUT_OF_MEMORY;

		if (!err) BuildCleanSource(clean_rgba, W, H, info.params, dilate_k, in_data);

		// Stage B. Erosion weight map from clean alpha (so Sharp Matte Edge's
		// hard-grown alpha is what we erode against). Cube the blur for the
		// characteristic falloff-inside-matte shape.
		if (!err) {
			edge_map = new (std::nothrow) float[N];
			if (!edge_map) {
				err = PF_Err_OUT_OF_MEMORY;
			} else {
				for (A_long i = 0; i < N; ++i) edge_map[i] = clean_rgba[i * 4 + 0];
				if (info.params.edge_radiusF > 1e-4f) {
					const std::vector<float> k = MakeGaussianKernel((float)info.params.edge_radiusF);
					GaussBlurFloat(edge_map, W, H, k, in_data);
					for (A_long i = 0; i < N; ++i)
						edge_map[i] = std::pow(std::min(1.0f, std::max(0.0f, edge_map[i])), 3.0f);
				} else {
					for (A_long i = 0; i < N; ++i)
						edge_map[i] = std::min(1.0f, std::max(0.0f, edge_map[i]));
				}
			}
		}

		// Stage C. Colour-bleed blur of clean source. Copy so clean_rgba stays
		// available as the "sharp" side of the lerp at compose time.
		if (!err) {
			blurred_rgba = new (std::nothrow) float[N * 4];
			if (!blurred_rgba) {
				err = PF_Err_OUT_OF_MEMORY;
			} else {
				std::memcpy(blurred_rgba, clean_rgba, N * 4 * sizeof(float));
				if (!blur_k.empty())
					GaussBlurRGBA(blurred_rgba, W, H, blur_k, in_data);
			}
		}
	}

	info.clean_rgba      = clean_rgba;
	info.blurred_rgba    = blurred_rgba;
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
	delete[] clean_rgba;

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
