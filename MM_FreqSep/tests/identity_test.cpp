/*  identity_test.cpp — AE-free unit tests for the MM FreqSep Phase 2 core.

    Verifies the two properties the plan gates on, WITHOUT After Effects:

      1. IDENTITY  — Texture=100% (gain 1) / Smooth=0 / Mix=100% (m 1) / no
         matte → recombine returns the source per channel within 1e-6. Tested
         directly against fs::RecombineChannel, the exact function the render
         path calls (src/fs_core.h is shared, not duplicated).

      2. HI + LO == SRC — the frequency split is lossless in float: for any lo,
         (src - lo) + lo reproduces src within float rounding.

      3. ALPHA-WEIGHTED EDGE — a hard opaque/transparent boundary blurred as
         premultiplied planes and renormalized by the blurred alpha yields the
         opaque region's straight color at the edge (no fringe toward black),
         which is the anti-fringing invariant of the render path. This drives
         the REAL blur (src/blur.cpp) via a minimal fs::ForEachRow shim so the
         separable Gaussian under test is the shipped kernel.

    Build/run: tests/run_tests.ps1 (compiles this + blur.cpp with cl, runs it).
    Exit code 0 = all pass, 1 = a failure (printed).
*/

#include "../src/fs_core.h"
#include "../src/blur.h"
#include "../src/guided.h"
#include "../src/matte.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

// ---- Minimal AE shims so blur.cpp links without the SDK ------------------
// blur.cpp calls fs::ForEachRow and fs::GaussianSupport lives in blur.cpp
// itself. ForEachRow just needs to run the row callback over [0,rows). The
// AE type A_long is a long; PF_InData is opaque to blur.cpp's row loop, so a
// null in_data with a serial fallback is exactly what the shipped serial path
// does.
// Bring in the AE types the same way src/params.h does, so AE_Effect.h takes
// its Windows branch (needs AE_OS_WIN + Windows.h in scope). blur.cpp expects
// the identical setup.
#include "AEConfig.h"
#ifdef AE_OS_WIN
	#include <Windows.h>
#endif
#include "AE_Effect.h"   // PF_Err, PF_InData, A_long (header-only types)
namespace fs {
void ForEachRow(PF_InData * /*in_data*/, A_long rows, void *refcon,
				PF_Err (*cb)(void *, A_long, A_long, A_long))
{
	for (A_long y = 0; y < rows; ++y) cb(refcon, 0, y, rows);
}
} // namespace fs

// ---- Test harness --------------------------------------------------------
static int g_failures = 0;

static void check(bool cond, const char *name, double detail)
{
	if (cond) {
		std::printf("  PASS  %s\n", name);
	} else {
		std::printf("  FAIL  %s   (worst=%.3e)\n", name, detail);
		++g_failures;
	}
}

// Property 1: identity recombine.
static void test_identity()
{
	const float samples[] = { 0.f, 0.001f, 0.25f, 0.5f, 0.9973f, 1.f, 4.2f, -0.3f };
	const int N = (int)(sizeof(samples) / sizeof(samples[0]));
	double worst = 0.0;
	for (int i = 0; i < N; ++i) {
		for (int j = 0; j < N; ++j) {
			const float src = samples[i];
			const float lo  = samples[j];       // any low band
			// gain=1 (Texture 100%), m=1 (Mix 100%, matte 1)
			const float out = fs::RecombineChannel(src, lo, 1.0f, 1.0f);
			worst = std::fmax(worst, std::fabs((double)out - (double)src));
		}
	}
	check(worst <= 1e-6, "identity: gain=1,m=1 -> out==src (per channel <=1e-6)", worst);
}

// Property 2: split is lossless.
static void test_split_lossless()
{
	const float src[] = { 0.f, 0.5f, 1.f, 3.7f, -0.2f, 0.123456f };
	const float lo[]  = { 0.f, 0.4f, 0.9f, 2.0f, 0.05f, 0.100000f };
	double worst = 0.0;
	for (int i = 0; i < 6; ++i) {
		const float hi   = src[i] - lo[i];
		const float back = hi + lo[i];
		worst = std::fmax(worst, std::fabs((double)back - (double)src[i]));
	}
	check(worst <= 1e-6, "split: (src-lo)+lo == src (<=1e-6)", worst);
}

// Property 3: alpha-weighted blur has no edge fringe.
// A 1-row image: left half opaque mid-grey (0.6), right half transparent. In
// premultiplied form the transparent side is (a=0, rgb=0). After the real
// separable Gaussian and renormalization by blurred alpha, the STRAIGHT color
// at the last opaque pixel must stay ~0.6 (its own color), NOT be pulled
// toward 0 by the transparent neighbors — the classic fringe a naive
// (non-alpha-weighted) blur produces.
static void test_alpha_weighted_edge()
{
	const A_long W = 64, H = 1;
	const float COLOR = 0.6f;
	const A_long edge = 32;   // [0,edge) opaque, [edge,W) transparent

	std::vector<float> src((size_t)W * H * 4);
	for (A_long x = 0; x < W; ++x) {
		float *p = &src[(size_t)x * 4];
		if (x < edge) { p[0] = 1.f; p[1] = COLOR; p[2] = COLOR; p[3] = COLOR; } // premult, a=1
		else          { p[0] = 0.f; p[1] = 0.f;   p[2] = 0.f;   p[3] = 0.f;   } // transparent
	}

	std::vector<float> tmp((size_t)W * H * 4), lo((size_t)W * H * 4);
	fs::GaussianBlurARGB(nullptr, src.data(), tmp.data(), lo.data(), W, H, 4.0, 0.0);

	// Renormalize by blurred alpha at the last opaque pixel (x=edge-1).
	const float *l = &lo[(size_t)(edge - 1) * 4];
	const float la = l[0];
	const float straight = (la > 1e-8f) ? (l[1] / la) : 0.f;
	const double err = std::fabs((double)straight - (double)COLOR);
	// Alpha-weighted: straight color stays the opaque value. A naive blur
	// (no renorm) would give ~COLOR*blurred_alpha < COLOR — a dark fringe.
	check(err <= 1e-3, "alpha-weighted edge: straight color ~= opaque value (no fringe)", err);

	// And the naive (fringing) value must be materially different, proving the
	// test actually discriminates: premult color / 1.0 (no renorm) is darker.
	const double naive = (double)l[1];       // premultiplied color, un-renormalized
	check(naive < COLOR - 0.05, "control: un-renormalized value IS a fringe (darker)", naive);
}

// Property 4: High Band view honors Texture gain (Option A).
// At texGain 1 the view is hi + 0.5; at texGain 0 it flattens to 0.5 grey;
// at texGain 2 the deviation from 0.5 doubles. Same fs::HighBandDisplay the
// render path calls.
static void test_high_view_gain()
{
	const float src = 0.72f, lo = 0.60f;      // hi = 0.12
	const float v1 = fs::HighBandDisplay(src, lo, 1.0f);
	const float v0 = fs::HighBandDisplay(src, lo, 0.0f);
	const float v2 = fs::HighBandDisplay(src, lo, 2.0f);
	const double e1 = std::fabs((double)v1 - 0.62);   // 0.12 + 0.5
	const double e0 = std::fabs((double)v0 - 0.50);   // flat grey
	const double e2 = std::fabs((double)v2 - 0.74);   // 0.24 + 0.5
	const double worst = std::fmax(e1, std::fmax(e0, e2));
	check(worst <= 1e-6, "high view: texGain scales the displayed detail band", worst);
}

// Property 5: matte channel extraction (all four modes). RGB is the delivered
// COLOR, `a` the alpha; Luma is luma(RGB) × alpha. Opaque mid-grey: a=1, RGB
// 0.5 → luma 0.5.
static void test_matte_channel()
{
	const float a = 1.0f, r = 0.5f, g = 0.5f, b = 0.5f;
	const double luma = 0.2126 * 0.5 + 0.7152 * 0.5 + 0.0722 * 0.5;   // = 0.5
	double worst = 0.0;
	worst = std::fmax(worst, std::fabs((double)fs::MatteChannelValue(a,r,g,b, fs::FS_MATTE_CHAN_ALPHA_)     - a));
	worst = std::fmax(worst, std::fabs((double)fs::MatteChannelValue(a,r,g,b, fs::FS_MATTE_CHAN_ALPHA_INV_) - (1.0 - a)));
	worst = std::fmax(worst, std::fabs((double)fs::MatteChannelValue(a,r,g,b, fs::FS_MATTE_CHAN_LUMA_)      - luma * a));
	worst = std::fmax(worst, std::fabs((double)fs::MatteChannelValue(a,r,g,b, fs::FS_MATTE_CHAN_LUMA_INV_)  - (1.0 - luma) * a));
	check(worst <= 1e-6, "matte channel: alpha/alpha-inv/luma/luma-inv extracted correctly", worst);
}

// Property 5b: THE C4 "white square" case. A precomp with STRAIGHT color:
// white fills the whole frame (RGB=1), masks live in the ALPHA. In the
// transparent background RGB is still 1.0 but alpha is 0 — Luma MUST read 0
// there (luma × alpha), collapsing the white square to the mask shapes. Also
// checks a feather midpoint and Luma Inverted staying 0 where empty.
static void test_matte_luma_coverage_gate()
{
	// Transparent background of a straight-color precomp: white RGB, alpha 0.
	const float lu  = fs::MatteChannelValue(0.f, 1.f,1.f,1.f, fs::FS_MATTE_CHAN_LUMA_);
	const float lui = fs::MatteChannelValue(0.f, 1.f,1.f,1.f, fs::FS_MATTE_CHAN_LUMA_INV_);
	// Feather midpoint: white RGB, alpha 0.5 → luma 1.0 × 0.5 = 0.5; inv = 0.
	const float flu  = fs::MatteChannelValue(0.5f, 1.f,1.f,1.f, fs::FS_MATTE_CHAN_LUMA_);
	const float flui = fs::MatteChannelValue(0.5f, 1.f,1.f,1.f, fs::FS_MATTE_CHAN_LUMA_INV_);
	// Fully inside mask: white, alpha 1 → luma 1.
	const float ilu  = fs::MatteChannelValue(1.f, 1.f,1.f,1.f, fs::FS_MATTE_CHAN_LUMA_);
	const bool ok = (lu == 0.f) && (lui == 0.f) &&
					(std::fabs(flu - 0.5f) <= 1e-5f) && (std::fabs(flui - 0.f) <= 1e-5f) &&
					(std::fabs(ilu - 1.0f) <= 1e-5f);
	check(ok, "matte luma: straight-white background reads 0 (no square), feathers via alpha",
		  (double)lu);
}

// Property 6: no matte selected -> SampleMatte returns 1.0 (effect everywhere).
static void test_matte_inactive()
{
	fs::FS_MatteField m;   // active=false by construction
	const float v = fs::SampleMatte(m, 17, 42);
	check(std::fabs((double)v - 1.0) <= 1e-6, "matte inactive: SampleMatte == 1.0", std::fabs((double)v - 1.0));
}

// Property 7: Fit-to-Layer stretches a small matte across the SOURCE frame,
// entirely in layer space. A 2x2 alpha matte {0,1 / 0,1} whose matte-layer
// frame is 2x2, fit across a 100x100 source frame, sampled by a 100x100
// output world at layer origin (0,0): left edge ~0, right edge ~1. This is
// the geometry the render path uses (origins + ref sizes), not output dims.
static void test_matte_fit()
{
	// 2x2 premultiplied ARGB: alpha 0 on the left column, 1 on the right.
	float buf[2 * 2 * 4] = {
		/* (0,0) a=0 */ 0,0,0,0,   /* (1,0) a=1 */ 1,1,1,1,
		/* (0,1) a=0 */ 0,0,0,0,   /* (1,1) a=1 */ 1,1,1,1,
	};
	fs::FS_MatteField m;
	m.data = buf; m.W = 2; m.H = 2;
	m.mOriginX = 0; m.mOriginY = 0; m.mRefW = 2; m.mRefH = 2;      // matte frame
	m.outOriginX = 0; m.outOriginY = 0; m.srcRefW = 100; m.srcRefH = 100; // src frame
	m.channel = fs::FS_MATTE_CHAN_ALPHA_; m.fit = fs::FS_MATTE_FIT_FIT_; m.active = true;

	const float left  = fs::SampleMatte(m, 2,  50);   // near left edge
	const float right = fs::SampleMatte(m, 97, 50);   // near right edge
	check(left < 0.1f && right > 0.9f,
		  "matte fit: small matte stretched across source frame (left~0, right~1)",
		  (double)left + (double)(1.f - right));
}

// Property 8: the output-world origin offset works — a tiled render whose
// output buffer starts partway into the layer samples the matte at the right
// place. Two output buffers cover the SAME layer coordinate: one is the full
// frame (origin 0, pixel 60), the other a tile starting at layer x=50
// (origin 50, pixel 10). Both map to layer x≈60.5 and must return the same
// matte value — this is what stops tile seams and the res-dependent jump.
static void test_matte_origin_offset()
{
	// 4-wide alpha ramp so x=60/100 lands in a non-trivial spot.
	float buf[4 * 1 * 4] = {
		0,0,0,0,  0.33f,0.33f,0.33f,0.33f,  0.66f,0.66f,0.66f,0.66f,  1,1,1,1,
	};
	fs::FS_MatteField base;
	base.data = buf; base.W = 4; base.H = 1;
	base.mOriginX = 0; base.mOriginY = 0; base.mRefW = 4; base.mRefH = 1;
	base.srcRefW = 100; base.srcRefH = 1;
	base.channel = fs::FS_MATTE_CHAN_ALPHA_; base.fit = fs::FS_MATTE_FIT_FIT_;
	base.active = true;

	fs::FS_MatteField full = base;  full.outOriginX = 0;  full.outOriginY = 0;
	fs::FS_MatteField tile = base;  tile.outOriginX = 50; tile.outOriginY = 0;

	const float a = fs::SampleMatte(full, 60, 0);   // layer x = 60.5
	const float b = fs::SampleMatte(tile, 10, 0);   // layer x = 50 + 10.5 = 60.5
	check(std::fabs((double)a - (double)b) <= 1e-6,
		  "matte origin: full-frame and tiled buffers agree at same layer coord",
		  std::fabs((double)a - (double)b));
}

// Property 9: Fit is downsample-invariant (the F2 regression). A smooth alpha
// ramp matte fills a 90x1 source frame. At full res the matte buffer is 90px;
// at 1/3 res AE hands us a 30px matte buffer (same layer frame). Sampling the
// SAME layer point through each must agree. Before the fix, buffer coords were
// normalized by the full-res ref frame directly, so u only reached ~1/3 at 1/3
// res — the value at a given screen position drifted with resolution.
static float ramp_at(float u01) { return u01; }   // alpha == normalized x

static void test_matte_downsample_invariant()
{
	// Full-res matte buffer: 90 texels, alpha = x/90.
	static float full_buf[90 * 4];
	for (int x = 0; x < 90; ++x) {
		const float a = ramp_at(((float)x + 0.5f) / 90.f);
		full_buf[x*4+0] = a; full_buf[x*4+1] = a; full_buf[x*4+2] = a; full_buf[x*4+3] = a;
	}
	fs::FS_MatteField full;
	full.data = full_buf; full.W = 90; full.H = 1;
	full.mOriginX = 0; full.mOriginY = 0; full.mRefW = 90; full.mRefH = 1;
	full.outOriginX = 0; full.outOriginY = 0; full.srcRefW = 90; full.srcRefH = 1;
	full.dsx = 1.f; full.dsy = 1.f;
	full.channel = fs::FS_MATTE_CHAN_ALPHA_; full.fit = fs::FS_MATTE_FIT_FIT_;
	full.active = true;

	// 1/3-res matte buffer: 30 texels covering the SAME 90-wide layer frame.
	static float third_buf[30 * 4];
	for (int x = 0; x < 30; ++x) {
		const float a = ramp_at(((float)x + 0.5f) / 30.f);
		third_buf[x*4+0] = a; third_buf[x*4+1] = a; third_buf[x*4+2] = a; third_buf[x*4+3] = a;
	}
	fs::FS_MatteField third = full;
	third.data = third_buf; third.W = 30; third.mRefW = 90;   // frame unchanged
	third.dsx = 1.f / 3.f; third.dsy = 1.f / 3.f;

	// Layer x = 60.5. Full res: output pixel 60. 1/3 res: output pixel 20
	// (20.5 * 3 = 61.5, within a matte texel). Ramp value there ≈ 0.67.
	const float a = fs::SampleMatte(full,  60, 0);
	const float b = fs::SampleMatte(third, 20, 0);
	check(std::fabs((double)a - (double)b) <= 0.02,
		  "matte fit: downsample-invariant (full vs third res agree)",
		  std::fabs((double)a - (double)b));
}

// ==========================================================================
// Guided filter (Phase 4). Same shim-driven setup: these run the REAL
// src/guided.cpp through the serial ForEachRow above.
// ==========================================================================

// Helper: straight value of channel c at pixel i of a premult ARGB buffer.
static float straight_at(const float *buf, size_t i, int c)
{
	const float a = buf[i * 4];
	return (a > 1e-6f) ? buf[i * 4 + (size_t)c] / a : 0.f;
}

// Brute-force reference: the SAME alpha-weighted guided math as guided.cpp,
// computed naively per window in double, O(W*H*r^2). Mirrors guided.cpp's
// thresholds and zero extension exactly so any disagreement is a real bug in
// the O(1) box formulation, not a semantics difference.
static void guided_reference(const float *src, float *dst,
							 int W, int H, int rx, int ry, float eps)
{
	const int N = W * H;
	std::vector<double> A((size_t)N * 3), B((size_t)N * 3), wk((size_t)N);

	for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
		double sw = 0, sp[3] = {0,0,0}, sp2[3] = {0,0,0};
		for (int j = (y-ry < 0 ? 0 : y-ry); j <= (y+ry > H-1 ? H-1 : y+ry); ++j)
		for (int i = (x-rx < 0 ? 0 : x-rx); i <= (x+rx > W-1 ? W-1 : x+rx); ++i) {
			const float *s = &src[((size_t)j * W + i) * 4];
			const double a = s[0];
			sw += a;
			for (int c = 0; c < 3; ++c) {
				sp[c]  += s[c + 1];
				sp2[c] += (a > (double)fs::FS_GUIDED_AEPS)
					? (double)s[c + 1] * s[c + 1] / a : 0.0;
			}
		}
		const size_t k = (size_t)y * W + x;
		const double a_k = src[k * 4];
		if (sw > (double)fs::FS_GUIDED_WEPS) {
			wk[k] = a_k;
			for (int c = 0; c < 3; ++c) {
				const double mean = sp[c] / sw;
				double var = sp2[c] / sw - mean * mean;
				if (var < 0) var = 0;
				const double Ak = var / (var + (double)eps);
				A[k*3 + c] = Ak;
				B[k*3 + c] = mean * (1.0 - Ak);
			}
		} else {
			wk[k] = 0;   // guided.cpp zeroes the weight slot too
			for (int c = 0; c < 3; ++c) { A[k*3 + c] = 0; B[k*3 + c] = 0; }
		}
	}

	const double count = (double)(2*rx + 1) * (2*ry + 1);
	for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
		double sw = 0, sA[3] = {0,0,0}, sB[3] = {0,0,0};
		for (int j = (y-ry < 0 ? 0 : y-ry); j <= (y+ry > H-1 ? H-1 : y+ry); ++j)
		for (int i = (x-rx < 0 ? 0 : x-rx); i <= (x+rx > W-1 ? W-1 : x+rx); ++i) {
			const size_t k = (size_t)j * W + i;
			sw += wk[k];
			for (int c = 0; c < 3; ++c) {
				sA[c] += wk[k] * A[k*3 + c];
				sB[c] += wk[k] * B[k*3 + c];
			}
		}
		float *d = &dst[((size_t)y * W + x) * 4];
		if (sw > (double)fs::FS_GUIDED_WEPS) {
			const double abar = sw / count;
			const float  a_i  = src[((size_t)y * W + x) * 4];
			d[0] = (float)abar;
			for (int c = 0; c < 3; ++c) {
				const double p = (a_i > fs::FS_GUIDED_AEPS)
					? (double)src[((size_t)y * W + x) * 4 + c + 1] / a_i : 0.0;
				const double q = sA[c] / sw * p + sB[c] / sw;
				d[c + 1] = (float)(q * abar);
			}
		} else {
			d[0] = d[1] = d[2] = d[3] = 0.f;
		}
	}
}

// Property 10: the O(1) box formulation IS the guided filter — it matches the
// brute-force reference on a noisy image that includes an alpha ramp (so the
// weighted paths are exercised, not just the opaque fast path).
static void test_guided_matches_reference()
{
	const int W = 40, H = 28, r = 3;
	const float eps = 0.02f;
	std::vector<float> img((size_t)W*H*4), out((size_t)W*H*4), ref((size_t)W*H*4);

	unsigned seed = 12345u;
	auto frand = [&seed]() {
		seed = seed * 1664525u + 1013904223u;
		return (float)((seed >> 8) & 0xFFFFFFu) / 16777216.f;
	};
	for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
		const float a = (x < 12) ? (float)x / 12.f : 1.f;   // alpha ramp on the left
		float *p = &img[((size_t)y * W + x) * 4];
		p[0] = a; p[1] = a * frand(); p[2] = a * frand(); p[3] = a * frand();
	}

	fs::GuidedFilterARGB(nullptr, img.data(), out.data(), W, H, r, r, eps);
	guided_reference(img.data(), ref.data(), W, H, r, r, eps);

	double worst = 0.0;
	for (size_t i = 0; i < (size_t)W * H; ++i) {
		const float *o = &out[i * 4], *rf = &ref[i * 4];
		if (rf[0] > 1e-4f && o[0] > 1e-4f) {
			worst = std::fmax(worst, std::fabs((double)o[0] - (double)rf[0]));
			for (int c = 1; c < 4; ++c)
				worst = std::fmax(worst,
					std::fabs((double)straight_at(out.data(), i, c)
							- (double)straight_at(ref.data(), i, c)));
		} else {
			for (int c = 0; c < 4; ++c)
				worst = std::fmax(worst, std::fabs((double)o[c] - (double)rf[c]));
		}
	}
	check(worst <= 1e-3, "guided: O(1) box formulation matches brute-force reference", worst);
}

// Property 11: constant image is a fixed point for ANY eps (var == 0 → the
// filter returns the local mean == the constant). Guards the var
// cancellation / normalization plumbing.
static void test_guided_constant()
{
	const int W = 32, H = 16;
	std::vector<float> img((size_t)W*H*4), out((size_t)W*H*4);
	for (size_t i = 0; i < (size_t)W * H; ++i) {
		img[i*4] = 1.f; img[i*4+1] = img[i*4+2] = img[i*4+3] = 0.42f;
	}
	fs::GuidedFilterARGB(nullptr, img.data(), out.data(), W, H, 5, 5, 0.01f);
	double worst = 0.0;
	for (size_t i = 0; i < (size_t)W * H; ++i)
		for (int c = 1; c < 4; ++c)
			worst = std::fmax(worst,
				std::fabs((double)straight_at(out.data(), i, c) - 0.42));
	check(worst <= 1e-4, "guided: constant image is a fixed point (any eps)", worst);
}

// Property 12: THE reason Guided exists — a hard step edge with low eps comes
// through nearly untouched (no halo), while the variance-matched Gaussian
// smears the same edge badly. This is the jawline test in miniature.
static void test_guided_edge_preserving()
{
	const int W = 64, H = 1, r = 4, edge = 32;
	std::vector<float> img((size_t)W*4), gout((size_t)W*4), tmp((size_t)W*4), out((size_t)W*4);
	for (int x = 0; x < W; ++x) {
		const float p = (x < edge) ? 0.2f : 0.8f;
		img[(size_t)x*4] = 1.f;
		img[(size_t)x*4+1] = img[(size_t)x*4+2] = img[(size_t)x*4+3] = p;
	}

	fs::GuidedFilterARGB(nullptr, img.data(), out.data(), W, H, r, 0, 1e-4f);
	double worst = 0.0;
	for (int x = 0; x < W; ++x) {
		const float p = (x < edge) ? 0.2f : 0.8f;
		worst = std::fmax(worst,
			std::fabs((double)straight_at(out.data(), (size_t)x, 1) - (double)p));
	}
	check(worst <= 0.01, "guided low-eps: step edge preserved (no halo)", worst);

	// Control: the variance-matched Gaussian (sigma from inverting
	// fs::GuidedRadius: r(r+1) = 1.5*sigma^2) smears the edge — proving the
	// test discriminates and the two Methods really differ where it matters.
	const double sigma = std::sqrt((double)r * (r + 1) / 1.5);
	fs::GaussianBlurARGB(nullptr, img.data(), tmp.data(), gout.data(), W, H, sigma, 0.0);
	double gworst = 0.0;
	for (int x = 0; x < W; ++x) {
		const float p = (x < edge) ? 0.2f : 0.8f;
		gworst = std::fmax(gworst,
			std::fabs((double)straight_at(gout.data(), (size_t)x, 1) - (double)p));
	}
	check(gworst > 0.15, "control: matched Gaussian smears that same edge", gworst);
}

// Property 13: at the smoothing extreme (eps >> local variance) Guided
// approaches the variance-matched Gaussian — same smoothing SCALE, so
// toggling Method at equal slider values compares looks, not amounts.
// Kernel shapes differ (triangle vs Gaussian), hence the loose tolerance.
static void test_guided_smoothing_extreme()
{
	const int W = 64, H = 1, r = 4, edge = 32;
	std::vector<float> img((size_t)W*4), gout((size_t)W*4), tmp((size_t)W*4), out((size_t)W*4);
	for (int x = 0; x < W; ++x) {
		const float p = (x < edge) ? 0.2f : 0.8f;
		img[(size_t)x*4] = 1.f;
		img[(size_t)x*4+1] = img[(size_t)x*4+2] = img[(size_t)x*4+3] = p;
	}

	fs::GuidedFilterARGB(nullptr, img.data(), out.data(), W, H, r, 0, 10.f);
	const double sigma = std::sqrt((double)r * (r + 1) / 1.5);
	fs::GaussianBlurARGB(nullptr, img.data(), tmp.data(), gout.data(), W, H, sigma, 0.0);

	double diff = 0.0, smoothed = 0.0;
	for (int x = 0; x < W; ++x) {
		const float p = (x < edge) ? 0.2f : 0.8f;
		const double g = (double)straight_at(out.data(),  (size_t)x, 1);
		const double n = (double)straight_at(gout.data(), (size_t)x, 1);
		diff     = std::fmax(diff, std::fabs(g - n));
		smoothed = std::fmax(smoothed, std::fabs(g - (double)p));
	}
	check(diff <= 0.03, "guided high-eps ~ matched Gaussian on a step (same scale)", diff);
	check(smoothed > 0.15, "control: high-eps guided really is smoothing", smoothed);
}

// Property 14: alpha-weighted edge, guided edition — the exact invariant
// test_alpha_weighted_edge checks for the Gaussian path. Straight color at
// the last opaque pixel stays the opaque value (transparent neighbors carry
// no weight), while the reported coverage alpha genuinely drops there.
static void test_guided_alpha_edge()
{
	const int W = 64, H = 1, r = 4, edge = 32;
	const float COLOR = 0.6f;
	std::vector<float> img((size_t)W*4), out((size_t)W*4);
	for (int x = 0; x < W; ++x) {
		float *p = &img[(size_t)x*4];
		if (x < edge) { p[0] = 1.f; p[1] = p[2] = p[3] = COLOR; }
		else          { p[0] = 0.f; p[1] = p[2] = p[3] = 0.f;   }
	}
	fs::GuidedFilterARGB(nullptr, img.data(), out.data(), W, H, r, 0, 0.01f);

	const double err = std::fabs(
		(double)straight_at(out.data(), (size_t)(edge - 1), 1) - (double)COLOR);
	check(err <= 1e-3, "guided alpha edge: straight color ~= opaque value (no fringe)", err);

	const double abar = (double)out[(size_t)(edge - 1) * 4];
	check(abar < 0.95, "control: coverage alpha at the edge really dropped", abar);
}

// Property 15: radius/support plumbing. GuidedSupport is the 2r cascade
// PreRender must inflate by; GuidedRadius pins the variance-matched
// sigma → box radius mapping; r == 0 is an exact copy (identity chain).
static void test_guided_radius_mapping()
{
	bool ok = (fs::GuidedSupport(4) == 8) && (fs::GuidedSupport(0) == 0) &&
			  (fs::GuidedRadius(0.0) == 0) && (fs::GuidedRadius(0.3) == 0) &&
			  (fs::GuidedRadius(1.0) == 1) && (fs::GuidedRadius(8.0) == 9);
	check(ok, "guided: support = 2r, sigma->radius mapping pinned", ok ? 0.0 : 1.0);

	float img[8 * 4], out[8 * 4];
	for (int i = 0; i < 8 * 4; ++i) img[i] = 0.125f * (float)i;
	fs::GuidedFilterARGB(nullptr, img, out, 8, 1, 0, 0, 0.01f);
	double worst = 0.0;
	for (int i = 0; i < 8 * 4; ++i)
		worst = std::fmax(worst, std::fabs((double)out[i] - (double)img[i]));
	check(worst == 0.0, "guided r=0: exact copy (identity)", worst);
}

int main()
{
	std::printf("MM FreqSep Phase 2-4 core tests\n");
	test_identity();
	test_split_lossless();
	test_alpha_weighted_edge();
	test_high_view_gain();
	test_matte_channel();
	test_matte_luma_coverage_gate();
	test_matte_inactive();
	test_matte_fit();
	test_matte_origin_offset();
	test_matte_downsample_invariant();
	test_guided_matches_reference();
	test_guided_constant();
	test_guided_edge_preserving();
	test_guided_smoothing_extreme();
	test_guided_alpha_edge();
	test_guided_radius_mapping();
	std::printf(g_failures ? "\n%d FAILURE(S)\n" : "\nAll tests passed.\n", g_failures);
	return g_failures ? 1 : 0;
}
