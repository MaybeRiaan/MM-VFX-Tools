/*  core_test.cpp — AE-free unit tests for MM Qualify's shared per-pixel math
    (src/qk_core.h). No AE SDK, no GPU, no host — just the pure functions the
    render path and the GPU kernels BOTH compile.

    Focus areas (the plan's stated risks, §6):
      - Hue wraparound across the 0/360 seam, including In > Out ranges.
      - Gate softness (trapezoid falloff) and the AND (product) composition.
      - Qualify In transforms: monotonic, no NaN on negatives/HDR.
      - Matte cleanup identity + endpoints; In/Out Ratio identity + shrink/grow.
      - Temporal IIR: asymmetric attack/release, convergence, warm start.
      - Output-mode compose: keyed = premult*matte, matte views, highlight.

    Exit 0 = all pass.
*/

#include "qk_cpu_prologue.h"
#include "qk_core.h"

#include <cstdio>
#include <cmath>
#include <cstdlib>

static int g_fail = 0;
static int g_total = 0;

static void check(bool ok, const char *name)
{
	++g_total;
	if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", name); }
}

static bool near(float a, float b, float tol = 1e-4f)
{
	return std::fabs(a - b) <= tol;
}

static QK_Gates gates_off()
{
	QK_Gates g = {};
	g.lumR = 0.2126f; g.lumG = 0.7152f; g.lumB = 0.0722f;
	g.model = QK_MODEL_HSV; g.qin = QK_QIN_ASIS;
	return g;
}

// ---------------------------------------------------------------------------
static void test_wrap()
{
	std::printf("wrap360\n");
	check(near(QK_Wrap360(370.f), 10.f), "370->10");
	check(near(QK_Wrap360(-10.f), 350.f), "-10->350");
	check(near(QK_Wrap360(0.f), 0.f), "0->0");
	check(near(QK_Wrap360(720.f), 0.f), "720->0");
	check(near(QK_Wrap360(359.9f), 359.9f), "359.9");
}

// ---------------------------------------------------------------------------
static void test_hue_gate()
{
	std::printf("hue gate (seam-free)\n");

	// Normal range 100..140, no softness: inside pass, outside reject.
	check(QK_HueGate(120.f, 100.f, 140.f, 0.f, 0.f) == 1.f, "inside 120 in [100,140]");
	check(QK_HueGate(90.f,  100.f, 140.f, 0.f, 0.f) == 0.f, "outside 90");
	check(QK_HueGate(160.f, 100.f, 140.f, 0.f, 0.f) == 0.f, "outside 160");

	// THE SEAM: In=350, Out=20 -> arc crosses 0. Reds behave naturally.
	check(QK_HueGate(0.f,   350.f, 20.f, 0.f, 0.f) == 1.f, "seam: 0 in [350,20]");
	check(QK_HueGate(355.f, 350.f, 20.f, 0.f, 0.f) == 1.f, "seam: 355 in");
	check(QK_HueGate(10.f,  350.f, 20.f, 0.f, 0.f) == 1.f, "seam: 10 in");
	check(QK_HueGate(180.f, 350.f, 20.f, 0.f, 0.f) == 0.f, "seam: 180 out");
	check(QK_HueGate(349.f, 350.f, 20.f, 0.f, 0.f) == 0.f, "seam: 349 just-out");
	check(QK_HueGate(21.f,  350.f, 20.f, 0.f, 0.f) == 0.f, "seam: 21 just-out");

	// Full circle: In==Out (width wraps to 0) -> everything passes.
	check(QK_HueGate(123.f, 0.f, 360.f, 0.f, 0.f) == 1.f, "full circle 0..360");
	check(QK_HueGate(200.f, 45.f, 45.f, 0.f, 0.f) == 1.f, "full circle 45..45");

	// Softness trapezoid: 10 deg out-soft past Out=140 -> half at 145.
	check(near(QK_HueGate(145.f, 100.f, 140.f, 0.f, 10.f), 0.5f), "outsoft half at +5/10");
	check(near(QK_HueGate(150.f, 100.f, 140.f, 0.f, 10.f), 0.0f), "outsoft zero at +10");
	// In-soft 10 deg before In=100 -> half at 95.
	check(near(QK_HueGate(95.f, 100.f, 140.f, 10.f, 0.f), 0.5f), "insoft half at -5/10");

	// Softness across the seam: In=350 in-soft 10 -> half at 345.
	check(near(QK_HueGate(345.f, 350.f, 20.f, 10.f, 0.f), 0.5f), "seam insoft half 345");
}

// ---------------------------------------------------------------------------
static void test_lin_gate()
{
	std::printf("linear gate\n");
	check(QK_LinGate(0.5f, 0.2f, 0.8f, 0.f, 0.f) == 1.f, "inside");
	check(QK_LinGate(0.1f, 0.2f, 0.8f, 0.f, 0.f) == 0.f, "below hard");
	check(QK_LinGate(0.9f, 0.2f, 0.8f, 0.f, 0.f) == 0.f, "above hard");
	check(near(QK_LinGate(0.15f, 0.2f, 0.8f, 0.1f, 0.f), 0.5f), "insoft half");
	check(near(QK_LinGate(0.85f, 0.2f, 0.8f, 0.f, 0.1f), 0.5f), "outsoft half");
	// reversed bounds get swapped
	check(QK_LinGate(0.5f, 0.8f, 0.2f, 0.f, 0.f) == 1.f, "reversed bounds inside");
	// HDR: bound above 1
	check(QK_LinGate(3.5f, 1.0f, 4.0f, 0.f, 0.f) == 1.f, "hdr inside");
}

// ---------------------------------------------------------------------------
static void test_rgb_to_hs()
{
	std::printf("rgb->hs\n");
	float h, s;
	QK_RGBToHS(1.f, 0.f, 0.f, QK_MODEL_HSV, h, s);
	check(near(h, 0.f) && near(s, 1.f), "red -> h0 s1");
	QK_RGBToHS(0.f, 1.f, 0.f, QK_MODEL_HSV, h, s);
	check(near(h, 120.f) && near(s, 1.f), "green -> h120");
	QK_RGBToHS(0.f, 0.f, 1.f, QK_MODEL_HSV, h, s);
	check(near(h, 240.f), "blue -> h240");
	QK_RGBToHS(0.5f, 0.5f, 0.5f, QK_MODEL_HSV, h, s);
	check(near(s, 0.f), "grey -> s0");
	// HSL saturation of pure red is 1; of mid-grey is 0.
	QK_RGBToHS(1.f, 0.f, 0.f, QK_MODEL_HSL, h, s);
	check(near(s, 1.f), "hsl red s1");
	// negative channel clamped, no NaN
	QK_RGBToHS(-0.2f, 0.3f, 0.1f, QK_MODEL_HSV, h, s);
	check(std::isfinite(h) && std::isfinite(s), "negative -> finite");
}

// ---------------------------------------------------------------------------
static void test_qualify_in()
{
	std::printf("qualify-in transforms\n");
	// Display (As-Is) is identity.
	check(near(QK_QualifyIn1(0.42f, QK_QIN_ASIS), 0.42f), "display identity");
	// Both modes monotonic increasing on a sweep, finite on HDR + negatives.
	const int modes[2] = { QK_QIN_ASIS, QK_QIN_LINLOG };
	for (int mi = 0; mi < 2; ++mi) {
		int mode = modes[mi];
		float prev = QK_QualifyIn1(-0.5f, mode);
		bool mono = std::isfinite(prev), fin = std::isfinite(prev);
		for (float x = -0.5f; x <= 8.f; x += 0.05f) {
			float y = QK_QualifyIn1(x, mode);
			if (!std::isfinite(y)) fin = false;
			if (y < prev - 1e-4f) mono = false;
			prev = y;
		}
		char nm[64];
		std::snprintf(nm, sizeof(nm), "mode %d monotonic", mode); check(mono, nm);
		std::snprintf(nm, sizeof(nm), "mode %d finite", mode); check(fin, nm);
	}
	// Linear->Log (ACEScct) midpoint sanity: 0.18 linear -> ~0.41 (Resolve's
	// log midtone).
	check(near(QK_QualifyIn1(0.18f, QK_QIN_LINLOG), 0.4135f, 2e-3f), "linlog 0.18");
}

// ---------------------------------------------------------------------------
static void test_raw_matte()
{
	std::printf("raw matte (ANDed gates)\n");
	// No gates enabled -> everything is 1.
	QK_Gates g = gates_off();
	check(QK_RawMatte(0.3f, 0.6f, 0.1f, g) == 1.f, "no gates -> 1");

	// Hue gate only: pick out greens (h ~120). Green passes, red rejected.
	g.hueEn = 1.f; g.hueIn = 90.f; g.hueOut = 150.f;
	check(near(QK_RawMatte(0.f, 1.f, 0.f, g), 1.f), "green in hue gate");
	check(near(QK_RawMatte(1.f, 0.f, 0.f, g), 0.f), "red out of hue gate");

	// AND with saturation: enable sat gate 0.5..1. A desaturated green now fails.
	g.satEn = 1.f; g.satIn = 0.5f; g.satOut = 1.f;
	check(near(QK_RawMatte(0.f, 1.f, 0.f, g), 1.f), "saturated green passes both");
	check(near(QK_RawMatte(0.4f, 0.5f, 0.4f, g), 0.f), "desat green fails sat");
}

// ---------------------------------------------------------------------------
static void test_clean_clip()
{
	std::printf("clean/clip\n");
	// Identity at 0,0,0,1.
	for (float m = 0.f; m <= 1.f; m += 0.1f)
		check(near(QK_CleanClip(m, 0.f, 0.f, 0.f, 1.f), m), "cleanclip identity");
	// Clean black pushes small values to 0; 1 stays 1.
	check(QK_CleanClip(0.05f, 0.2f, 0.f, 0.f, 1.f) < 0.05f, "clean black lowers small");
	check(near(QK_CleanClip(1.f, 0.2f, 0.f, 0.f, 1.f), 1.f), "clean black keeps 1");
	// Clean white pushes near-1 up to 1; 0 stays 0.
	check(QK_CleanClip(0.95f, 0.f, 0.2f, 0.f, 1.f) > 0.95f, "clean white raises near-1");
	check(near(QK_CleanClip(0.f, 0.f, 0.2f, 0.f, 1.f), 0.f), "clean white keeps 0");
	// Clip levels: black clip 0.25 maps 0.25 -> 0; white clip 0.75 maps 0.75 -> 1.
	check(near(QK_CleanClip(0.25f, 0.f, 0.f, 0.25f, 0.75f), 0.f), "black clip");
	check(near(QK_CleanClip(0.75f, 0.f, 0.f, 0.25f, 0.75f), 1.f), "white clip");
	check(near(QK_CleanClip(0.5f, 0.f, 0.f, 0.25f, 0.75f), 0.5f), "clip midpoint");
	// Output always in [0,1].
	check(QK_CleanClip(-1.f, 0.f, 0.f, 0.f, 1.f) == 0.f, "clamped low");
	check(QK_CleanClip(2.f, 0.f, 0.f, 0.f, 1.f) == 1.f, "clamped high");
}

// ---------------------------------------------------------------------------
static void test_ratio()
{
	std::printf("in/out ratio\n");
	for (float m = 0.f; m <= 1.f; m += 0.1f)
		check(near(QK_InOutRatio(m, 0.f), m), "ratio identity");
	// Grow (positive): a mid value increases, endpoints pinned.
	check(QK_InOutRatio(0.5f, 0.5f) > 0.5f, "grow raises mid");
	check(near(QK_InOutRatio(0.f, 0.5f), 0.f), "grow pins 0");
	// Shrink (negative): a mid value decreases.
	check(QK_InOutRatio(0.5f, -0.5f) < 0.5f, "shrink lowers mid");
	check(near(QK_InOutRatio(1.f, -0.5f), 1.f), "shrink pins 1");
}

// ---------------------------------------------------------------------------
static void test_iir()
{
	std::printf("temporal IIR\n");
	// k=1 (0% smoothing) follows instantly.
	check(near(QK_IIRStep(0.f, 1.f, 1.f, 1.f), 1.f), "k1 instant up");
	check(near(QK_IIRStep(1.f, 0.f, 1.f, 1.f), 0.f), "k1 instant down");
	// Asymmetric: attack 1.0 (instant), release 0.15 (slow).
	float kA = QK_SmoothK(0.f);      // 1.0
	float kR = QK_SmoothK(100.f);    // 0.15
	check(near(kA, 1.f), "smoothk 0 ->1");
	check(near(kR, 0.15f), "smoothk 100 ->0.15");
	// Growing selection: matte jumps to 1 instantly (attack instant).
	check(near(QK_IIRStep(0.f, 1.f, kA, kR), 1.f), "attack instant grow");
	// Shrinking: from 1 toward 0, one step of release only moves 0.15.
	check(near(QK_IIRStep(1.f, 0.f, kA, kR), 0.85f), "release one step");
	// Convergence: repeated release steps approach the target.
	float s = 1.f;
	for (int i = 0; i < 200; ++i) s = QK_IIRStep(s, 0.f, kA, kR);
	check(s < 0.01f, "release converges");
}

// ---------------------------------------------------------------------------
static void test_compose()
{
	std::printf("compose (output modes)\n");
	float oa, orr, og, ob;
	// Keyed: out = premult * matte. Full matte over an opaque red = red.
	QK_Compose(QK_OUT_KEYED, 1.f, 0.f, 1.f, 1.f, 0.f, 0.f, oa, orr, og, ob);
	check(near(oa,1.f)&&near(orr,1.f)&&near(og,0.f)&&near(ob,0.f), "keyed full red");
	// Keyed matte 0 -> fully transparent.
	QK_Compose(QK_OUT_KEYED, 0.f, 0.f, 1.f, 1.f, 0.f, 0.f, oa, orr, og, ob);
	check(near(oa,0.f)&&near(orr,0.f), "keyed zero matte -> clear");
	// Matte view: opaque greyscale of m.
	QK_Compose(QK_OUT_MATTE, 0.7f, 0.f, 1.f, 0.5f, 0.5f, 0.5f, oa, orr, og, ob);
	check(near(oa,1.f)&&near(orr,0.7f)&&near(og,0.7f), "matte view grey");
	// Matte inverted.
	QK_Compose(QK_OUT_MATTE_INV, 0.7f, 0.f, 1.f, 0.5f, 0.5f, 0.5f, oa, orr, og, ob);
	check(near(orr,0.3f), "matte inv");
	// Raw view uses rawM.
	QK_Compose(QK_OUT_RAW, 0.7f, 0.42f, 1.f, 0.5f, 0.5f, 0.5f, oa, orr, og, ob);
	check(near(orr,0.42f), "raw view");
	// Highlight: matte 0 -> flat grey; matte 1 -> image.
	QK_Compose(QK_OUT_HILITE, 0.f, 0.f, 1.f, 0.8f, 0.2f, 0.2f, oa, orr, og, ob);
	check(near(orr, QK_HILITE_GREY), "hilite unselected grey");
	QK_Compose(QK_OUT_HILITE, 1.f, 0.f, 1.f, 0.8f, 0.2f, 0.2f, oa, orr, og, ob);
	check(near(orr, 0.8f), "hilite selected shows image");
}

// ---------------------------------------------------------------------------
// §2.0 regression: with all gates passing (matte == 1), the KEYED output must
// equal the premultiplied source EXACTLY — including at antialiased edge
// pixels (0 < a < 1) and fully transparent pixels (a == 0). If this holds,
// the plugin cannot itself introduce an alpha fringe at default settings;
// any edge artifact seen in AE is the footage's own matte or host compositing,
// not the effect adding coverage.
// Build the DEFAULT gate state as QK_BuildGates would from factory params:
// all three gates ENABLED, hue full circle, sat/luma bounds OPEN (the
// extreme-handle sentinels). This is what the plugin actually runs at reset,
// so the passthrough test below exercises the real default, not gates_off().
static QK_Gates gates_default()
{
	QK_Gates g = {};
	g.hueEn = 1.f; g.hueIn = -180.f; g.hueOut = 180.f;   // width 360 -> full circle
	g.satEn = 1.f; g.satIn = -1e9f;  g.satOut = 1e9f;    // open bounds
	g.lumEn = 1.f; g.lumIn = -1e9f;  g.lumOut = 1e9f;
	g.lumR = 0.2126f; g.lumG = 0.7152f; g.lumB = 0.0722f;
	g.model = QK_MODEL_HSL; g.qin = QK_QIN_ASIS;
	return g;
}

static void test_keyed_passthrough()
{
	std::printf("keyed passthrough (default gates -> exact premult source)\n");
	QK_Gates g = gates_default();      // real reset state: enabled, open bounds

	// Sweep representative premultiplied pixels, incl. edge/superwhite/zero.
	struct { float a, pr, pg, pb; } px[] = {
		{1.00f, 0.80f, 0.20f, 0.20f},  // opaque
		{0.50f, 0.40f, 0.10f, 0.10f},  // half-covered (premult = straight*a)
		{0.01f, 0.008f,0.002f,0.002f}, // sliver edge — the fringe suspect
		{0.00f, 0.00f, 0.00f, 0.00f},  // fully transparent
		{0.75f, 2.10f, 0.30f, 0.00f},  // superwhite premult (HDR)
	};
	bool ok = true;
	for (auto &p : px) {
		float m = QK_RawMatte(p.pr / (p.a > 1e-8f ? p.a : 1.f),
		                      p.pg / (p.a > 1e-8f ? p.a : 1.f),
		                      p.pb / (p.a > 1e-8f ? p.a : 1.f), g);
		m = QK_CleanClip(m, 0.f, 0.f, 0.f, 1.f);   // default finesse = identity
		float oa, orr, og, ob;
		QK_Compose(QK_OUT_KEYED, QK_InOutRatio(m, 0.f), 0.f,
		           p.a, p.pr, p.pg, p.pb, oa, orr, og, ob);
		if (!(near(oa,p.a) && near(orr,p.pr) && near(og,p.pg) && near(ob,p.pb)))
			ok = false;
	}
	check(ok, "keyed default == premult source (all alphas)");
}

// ---------------------------------------------------------------------------
// The dark-footage bug: near-black chromatic pixels (dark leather, hair) have
// CORRECT HSL saturation >= 1.0 (they sit on the pure-hue-to-black line). A
// default gate whose upper edge is a hard 1.0 rejects them -> the matte drops
// on dark opaque pixels -> alpha holes. With the open-bound default (In 0 /
// Out at slider max mapped to +/-BIG), every such pixel must pass (matte 1).
static void test_dark_chromatic()
{
	std::printf("dark chromatic pixels pass default gates\n");
	QK_Gates g = gates_default();
	struct { float r, g, b; } px[] = {
		{0.001f, 0.000f, 0.000f},   // near-black pure red — HSL sat == 1.0
		{0.003f, 0.001f, 0.000f},   // near-black warm
		{0.008f, 0.004f, 0.002f},   // dark leather-ish
		{0.020f, 0.000f, 0.010f},   // dark magenta
		{0.900f, 0.010f, 0.010f},   // bright saturated red (sanity: also passes)
	};
	bool ok = true;
	for (auto &q : px) {
		float m = QK_RawMatte(q.r, q.g, q.b, g);
		if (m < 0.999f) { ok = false;
			std::printf("    rgb(%.3f,%.3f,%.3f) -> matte %.4f (expected 1)\n", q.r, q.g, q.b, m); }
	}
	check(ok, "near-black chromatic -> matte 1 at defaults");
}

// ---------------------------------------------------------------------------
int main()
{
	std::printf("=== MM Qualify core math tests ===\n");
	test_wrap();
	test_hue_gate();
	test_lin_gate();
	test_rgb_to_hs();
	test_qualify_in();
	test_raw_matte();
	test_clean_clip();
	test_ratio();
	test_iir();
	test_compose();
	test_keyed_passthrough();
	test_dark_chromatic();

	std::printf("--- %d/%d passed ---\n", g_total - g_fail, g_total);
	if (g_fail) { std::printf("FAILED (%d)\n", g_fail); return 1; }
	std::printf("ALL PASS\n");
	return 0;
}
