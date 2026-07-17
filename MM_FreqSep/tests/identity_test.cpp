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
         the REAL blur (src/blur.cpp) via a minimal mm::ForEachRow shim so the
         separable Gaussian under test is the shipped kernel.

    Build/run: tests/run_tests.ps1 (compiles this + blur.cpp with cl, runs it).
    Exit code 0 = all pass, 1 = a failure (printed).
*/

#include "../src/fs_core.h"
#include "../src/blur.h"
#include "../src/matte.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

// ---- Minimal AE shims so blur.cpp links without the SDK ------------------
// blur.cpp calls mm::ForEachRow and mm::GaussianSupport lives in blur.cpp
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
namespace mm {
void ForEachRow(PF_InData * /*in_data*/, A_long rows, void *refcon,
				PF_Err (*cb)(void *, A_long, A_long, A_long))
{
	for (A_long y = 0; y < rows; ++y) cb(refcon, 0, y, rows);
}
} // namespace mm

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

// Property 5: matte channel extraction (all four modes) from a straight RGBA.
static void test_matte_channel()
{
	// Inputs are PREMULTIPLIED. Opaque mid-grey: a=1, straight color 0.5 →
	// premult RGB 0.5. straightLuma(0.5) = 0.5.
	const float a = 1.0f, r = 0.5f, g = 0.5f, b = 0.5f;
	const double sLuma = 0.2126 * 0.5 + 0.7152 * 0.5 + 0.0722 * 0.5;   // = 0.5
	double worst = 0.0;
	worst = std::fmax(worst, std::fabs((double)fs::MatteChannelValue(a,r,g,b, fs::FS_MATTE_CHAN_ALPHA_)     - a));
	worst = std::fmax(worst, std::fabs((double)fs::MatteChannelValue(a,r,g,b, fs::FS_MATTE_CHAN_ALPHA_INV_) - (1.0 - a)));
	worst = std::fmax(worst, std::fabs((double)fs::MatteChannelValue(a,r,g,b, fs::FS_MATTE_CHAN_LUMA_)      - sLuma));              // ×a=1
	worst = std::fmax(worst, std::fabs((double)fs::MatteChannelValue(a,r,g,b, fs::FS_MATTE_CHAN_LUMA_INV_)  - (1.0 - sLuma)));      // ×a=1
	check(worst <= 1e-6, "matte channel: alpha/alpha-inv/luma/luma-inv extracted correctly", worst);
}

// Property 5b: Luma channels are gated by coverage — a TRANSPARENT sample
// (premult 0, a 0) contributes NO effect for both Luma and Luma Inverted.
// This is the C4 fix: without the gate, Luma Inverted would read 1 (full
// effect) everywhere the driver is empty.
static void test_matte_luma_coverage_gate()
{
	const float a = 0.f, r = 0.f, g = 0.f, b = 0.f;   // fully transparent tap
	const float lu  = fs::MatteChannelValue(a,r,g,b, fs::FS_MATTE_CHAN_LUMA_);
	const float lui = fs::MatteChannelValue(a,r,g,b, fs::FS_MATTE_CHAN_LUMA_INV_);
	// Half-covered bright pixel: a=0.5, straight white → premult 0.5. Luma
	// should be straightLuma(1.0)*0.5 = 0.5; Luma Inv = (1-1.0)*0.5 = 0.
	const float ha = 0.5f, hp = 0.5f;   // premult white at half coverage
	const float hlu  = fs::MatteChannelValue(ha,hp,hp,hp, fs::FS_MATTE_CHAN_LUMA_);
	const float hlui = fs::MatteChannelValue(ha,hp,hp,hp, fs::FS_MATTE_CHAN_LUMA_INV_);
	const bool ok = (lu == 0.f) && (lui == 0.f) &&
					(std::fabs(hlu - 0.5f) <= 1e-5f) && (std::fabs(hlui - 0.f) <= 1e-5f);
	check(ok, "matte luma: coverage-gated (transparent -> 0 for luma and luma-inv)",
		  (double)lui);
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

int main()
{
	std::printf("MM FreqSep Phase 2+3 core tests\n");
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
	std::printf(g_failures ? "\n%d FAILURE(S)\n" : "\nAll tests passed.\n", g_failures);
	return g_failures ? 1 : 0;
}
