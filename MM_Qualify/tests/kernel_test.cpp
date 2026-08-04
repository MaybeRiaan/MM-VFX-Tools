/*  kernel_test.cpp — every GPU kernel BODY, run and diffed against the CPU
    reference. AE-free: no GPU, no CUDA toolkit, no DX12 device, no AE host.

    This does not transcribe the kernels — it COMPILES THEM. src/gpu/cuda/
    qk_kernel_args.h pulls the real .kernel files through qk_prologue_host.h,
    which is the same vocabulary qk_prologue_cuda.h defines with CUDA
    spellings, so `qkk_<entry>::Args{...}.run(x, y)` executes the shipped
    kernel source on the CPU. A transcription can rot silently; this cannot —
    if a kernel changes, this test runs the change.

    What each case actually proves, since both sides call the same qk_core.h
    for per-pixel math: the kernel's INDEXING, its buffer-slot order, its
    PARAM_BLOCK layout (including the 20-float gate block round-tripping
    through QK_PackGates), its BGRA<->ARGB swizzle and its pitch handling.
    Those are exactly the places a kernel diverges from the CPU it mirrors,
    and pipeline_test.cpp then proves the same bodies COMPOSE correctly under
    the real tile planner.

    Tolerance is EXACT (0.0). Same source, same arithmetic, same order — any
    difference at all is a real defect (AE_GPU_Effects.md §4.3). The ≤ 1e-5
    tolerance in gpu-kernel-rules §3.6 is for real silicon, where fma
    contraction legitimately differs; it has no business here.

    Exit 0 = all pass.
*/

#define NOMINMAX
#include "blur.h"
#include "garbage.h"
#include "qk_cpu_prologue.h"
#include "qk_core.h"
#include "gpu/qk_gpu_host.h"
#include "gpu/cuda/qk_kernel_args.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using std::vector;
using namespace qkgpu;

static int g_fail = 0, g_total = 0;

static float maxAbs(const vector<float> &a, const vector<float> &b)
{
	float m = 0.f;
	const size_t n = a.size() < b.size() ? a.size() : b.size();
	for (size_t i = 0; i < n; ++i) {
		const float d = std::fabs(a[i] - b[i]);
		if (d > m) m = d;
	}
	return m;
}

static void check(const char *name, float d)
{
	++g_total;
	const bool ok = (d == 0.f);
	std::printf("  %-46s max-abs = %.3e  %s\n", name, d, ok ? "OK" : "FAIL");
	if (!ok) ++g_fail;
}

// Deterministic fill. Alpha deliberately reaches exact 0 so the coverage
// epsilon branch in QK_Straight is exercised, and HDR values so nothing
// silently assumes [0,1].
static void fillARGB(vector<float> &buf, unsigned seed, bool hdr)
{
	unsigned s = seed * 2654435761u + 1u;
	auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (float)((s >> 8) & 0xFFFF) / 65535.f; };
	for (size_t i = 0; i < buf.size(); i += 4) {
		float a = rnd();
		if ((i / 4) % 11 == 0) a = 0.f;
		const float scale = hdr ? (0.5f + 3.f * rnd()) : 1.f;
		buf[i + 0] = a;
		buf[i + 1] = a * scale * rnd();   // premultiplied
		buf[i + 2] = a * scale * rnd();
		buf[i + 3] = a * scale * rnd();
	}
}

static void fillF1(vector<float> &buf, unsigned seed)
{
	unsigned s = seed * 2246822519u + 7u;
	for (float &v : buf) { s = s * 1664525u + 1013904223u; v = (float)((s >> 8) & 0xFFFF) / 65535.f; }
}

// A gate config that actually selects part of the image, including a hue arc
// that crosses the 0/360 seam.
static QK_Gates testGates()
{
	QK_Gates g = {};
	g.hueEn = 1.f; g.hueIn = 200.f; g.hueOut = 40.f; g.hueInSoft = 12.f; g.hueOutSoft = 25.f;
	g.satEn = 1.f; g.satIn = 0.1f;  g.satOut = 1.5f; g.satInSoft = 0.05f; g.satOutSoft = 0.2f;
	g.lumEn = 1.f; g.lumIn = 0.05f; g.lumOut = 4.f;  g.lumInSoft = 0.05f; g.lumOutSoft = 0.3f;
	g.lumR = 0.2126f; g.lumG = 0.7152f; g.lumB = 0.0722f;
	g.model = QK_MODEL_HSL; g.qin = QK_QIN_LINLOG;
	return g;
}

// ---------------------------------------------------------------------------
// qk_tile.kernel — load / store / fill. The world is BGRA with a row pitch
// deliberately WIDER than width*4, because deriving pitch from width is the
// classic GPU-world bug (AE_GPU_Effects.md §3).
// ---------------------------------------------------------------------------
static void testTile()
{
	const int W = 13, rows = 5, worldH = 9, pitch = W * 4 + 7, y0 = 2;

	vector<float> world((size_t)worldH * pitch, -1.f);
	{
		unsigned s = 12345u;
		for (float &v : world) { s = s * 1664525u + 1013904223u; v = (float)((s >> 8) & 0xFFFF) / 65535.f; }
	}

	// --- load: world (BGRA, pitched) -> tile (ARGB, packed) ----------------
	vector<float> tile((size_t)W * rows * 4, 0.f), want((size_t)W * rows * 4, 0.f);
	{
		ArgsLoad a{};
		a.gLdWorld = world.data(); a.gLdTile = tile.data();
		a.gLdW = W; a.gLdRows = rows; a.gLdWorldY0 = y0; a.gLdPitch = pitch;
		QK_RunGrid(a, W, rows);
	}
	for (int y = 0; y < rows; ++y)
		for (int x = 0; x < W; ++x)
			for (int c = 0; c < 4; ++c)
				want[((size_t)y * W + x) * 4 + c] =
					world[(size_t)(y0 + y) * pitch + (size_t)x * 4 + (3 - c)];
	check("qk_load_tile (BGRA->ARGB, pitched)", maxAbs(tile, want));

	// --- store: tile -> world rows at a DIFFERENT y0, untouched rows stay --
	const int sy0 = 3;
	vector<float> outWorld((size_t)worldH * pitch, -1.f), outWant = outWorld;
	{
		ArgsStore a{};
		a.gStTile = tile.data(); a.gStWorld = outWorld.data();
		a.gStW = W; a.gStRows = rows; a.gStWorldY0 = sy0; a.gStPitch = pitch;
		QK_RunGrid(a, W, rows);
	}
	for (int y = 0; y < rows; ++y)
		for (int x = 0; x < W; ++x)
			for (int c = 0; c < 4; ++c)
				outWant[(size_t)(sy0 + y) * pitch + (size_t)x * 4 + (3 - c)] =
					tile[((size_t)y * W + x) * 4 + c];
	check("qk_store_tile (ARGB->BGRA, pitched)", maxAbs(outWorld, outWant));

	// Round trip through both kernels must reproduce the source rows exactly.
	{
		vector<float> back((size_t)W * rows * 4, 0.f);
		ArgsLoad a{};
		a.gLdWorld = outWorld.data(); a.gLdTile = back.data();
		a.gLdW = W; a.gLdRows = rows; a.gLdWorldY0 = sy0; a.gLdPitch = pitch;
		QK_RunGrid(a, W, rows);
		check("qk_load_tile o qk_store_tile round trip", maxAbs(back, tile));
	}

	// --- fill_ones ---------------------------------------------------------
	{
		vector<float> plane((size_t)W * rows, 0.f), ones((size_t)W * rows, 1.f);
		ArgsFill a{};
		a.gFillDst = plane.data(); a.gFW = W; a.gFRows = rows;
		QK_RunGrid(a, W, rows);
		check("qk_fill_ones", maxAbs(plane, ones));
	}
}

// ---------------------------------------------------------------------------
// qk_gauss.kernel — both passes, against the REAL blur.cpp. Weights are the
// CPU-built kernels handed over verbatim (rules §3.1); a GPU-side rebuild is
// exactly what this would fail to catch if it built its own.
// ---------------------------------------------------------------------------
static void gaussRGBACase(const char *name, int W, int H, double sx, double sy)
{
	const int Rx = (int)qk::GaussianSupport(sx);
	const int Ry = (int)qk::GaussianSupport(sy);

	vector<float> src((size_t)W * H * 4);
	fillARGB(src, 3u, true);

	vector<float> refTmp(src.size()), ref(src.size());
	qk::GaussianBlurARGB(nullptr, src.data(), refTmp.data(), ref.data(), W, H, sx, sy);

	vector<float> kx, ky;
	if (Rx > 0) qk::BuildGaussianKernel(sx, Rx, kx);
	if (Ry > 0) qk::BuildGaussianKernel(sy, Ry, ky);

	// The host's schedule: X into tmp, then Y into dst (single axis = one pass).
	vector<float> tmp(src.size(), 0.f), got(src.size(), 0.f);
	auto pass = [&](const float *in, const float *kern, float *out, int R, int horiz) {
		ArgsGaussRGBA a{};
		a.gRgSrc = in; a.gRgKern = kern; a.gRgDst = out;
		a.gRgW = W; a.gRgRows = H; a.gRgR = R; a.gRgHoriz = horiz;
		QK_RunGrid(a, W, H);
	};
	if (Rx > 0 && Ry > 0) {
		pass(src.data(), kx.data(), tmp.data(), Rx, 1);
		pass(tmp.data(), ky.data(), got.data(), Ry, 0);
	} else if (Rx > 0) {
		pass(src.data(), kx.data(), got.data(), Rx, 1);
	} else {
		pass(src.data(), ky.data(), got.data(), Ry, 0);
	}
	check(name, maxAbs(got, ref));
}

static void gaussF1Case(const char *name, int W, int H, double sx, double sy)
{
	const int Rx = (int)qk::GaussianSupport(sx);
	const int Ry = (int)qk::GaussianSupport(sy);

	vector<float> src((size_t)W * H);
	fillF1(src, 5u);

	vector<float> refTmp(src.size()), ref(src.size());
	qk::GaussianBlurF1(nullptr, src.data(), refTmp.data(), ref.data(), W, H, sx, sy);

	vector<float> kx, ky;
	if (Rx > 0) qk::BuildGaussianKernel(sx, Rx, kx);
	if (Ry > 0) qk::BuildGaussianKernel(sy, Ry, ky);

	vector<float> tmp(src.size(), 0.f), got(src.size(), 0.f);
	auto pass = [&](const float *in, const float *kern, float *out, int R, int horiz) {
		ArgsGaussF1 a{};
		a.gF1Src = in; a.gF1Kern = kern; a.gF1Dst = out;
		a.gF1W = W; a.gF1Rows = H; a.gF1R = R; a.gF1Horiz = horiz;
		QK_RunGrid(a, W, H);
	};
	if (Rx > 0 && Ry > 0) {
		pass(src.data(), kx.data(), tmp.data(), Rx, 1);
		pass(tmp.data(), ky.data(), got.data(), Ry, 0);
	} else if (Rx > 0) {
		pass(src.data(), kx.data(), got.data(), Rx, 1);
	} else {
		pass(src.data(), ky.data(), got.data(), Ry, 0);
	}
	check(name, maxAbs(got, ref));
}

// ---------------------------------------------------------------------------
// qk_matte.kernel vs smart_render.cpp's QK_MatteRow. Both call QK_Straight /
// QK_RawMatte / QK_ClipMatte, so what is under test is the addressing and the
// gate block arriving intact through QK_PackGates -> PARAM_BLOCK ->
// QK_GATES_LOAD.
// ---------------------------------------------------------------------------
static void testMatte()
{
	const int W = 23, H = 17;
	const float clipB = 0.07f, clipW = 0.91f;
	const QK_Gates gates = testGates();

	vector<float> qual((size_t)W * H * 4), garb((size_t)W * H);
	fillARGB(qual, 9u, true);
	fillF1(garb, 13u);

	vector<float> ref((size_t)W * H, 0.f), got((size_t)W * H, 0.f);
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			const float *q = &qual[((size_t)y * W + x) * 4];
			float r, g, b;
			QK_Straight(q[0], q[1], q[2], q[3], r, g, b);
			float m = QK_RawMatte(r, g, b, gates) * garb[(size_t)y * W + x];
			ref[(size_t)y * W + x] = QK_ClipMatte(m, clipB, clipW);
		}

	float packed[QK_GATES_FLOATS];
	QK_PackGates(gates, packed);

	ArgsMatte a{};
	a.gQual = qual.data(); a.gGIn = garb.data(); a.gMOut = got.data();
	a.gW = W; a.gRows = H;
	QK_StoreGates(a, packed);
	a.gClipB = clipB; a.gClipW = clipW;
	QK_RunGrid(a, W, H);

	check("qk_matte (gates + garbage + clip)", maxAbs(got, ref));
}

// ---------------------------------------------------------------------------
// qk_garbage.kernel vs the REAL qk::SampleGarbage, over a non-trivial Fit
// mapping (different layer frames, non-zero origins, a downsample ratio) so
// the layer-space arithmetic is genuinely exercised and not identity.
// ---------------------------------------------------------------------------
static void testGarbage()
{
	const int gW = 19, gH = 15, gPitch = gW * 4 + 5;
	const int tileW = 21, tileRows = 11, srcY0 = 4, srcOX = 3, srcOY = 6;

	// Same pixel values in both layouts: BGRA pitched world for the kernel,
	// packed ARGB plane for the CPU field.
	vector<float> world((size_t)gH * gPitch, 0.f), plane((size_t)gW * gH * 4, 0.f);
	{
		unsigned s = 777u;
		auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (float)((s >> 8) & 0xFFFF) / 65535.f; };
		for (int y = 0; y < gH; ++y)
			for (int x = 0; x < gW; ++x) {
				const float a = rnd();
				const float r = a * rnd(), g = a * rnd(), b = a * rnd();
				float *p = &plane[((size_t)y * gW + x) * 4];
				p[0] = a; p[1] = r; p[2] = g; p[3] = b;
				float *w = &world[(size_t)y * gPitch + (size_t)x * 4];
				w[0] = b; w[1] = g; w[2] = r; w[3] = a;     // BGRA
			}
	}

	qk::QK_GarbageField f;
	f.data = plane.data(); f.W = gW; f.H = gH;
	f.mOriginX = -3; f.mOriginY = 5;
	f.mRefW = 44; f.mRefH = 36;
	f.outOriginX = 7; f.outOriginY = -2;
	f.srcRefW = 52; f.srcRefH = 40;
	f.dsx = 0.5f; f.dsy = 0.5f;
	f.active = true;

	for (int chan = QK_GCHAN_ALPHA; chan <= QK_GCHAN_LUMA_INV; ++chan) {
		f.channel = chan;

		vector<float> ref((size_t)tileW * tileRows, 0.f), got((size_t)tileW * tileRows, 0.f);
		for (int yr = 0; yr < tileRows; ++yr)
			for (int x = 0; x < tileW; ++x)
				ref[(size_t)yr * tileW + x] =
					qk::SampleGarbage(f, x - srcOX, (srcY0 + yr) - srcOY);

		ArgsGarbage a{};
		a.gGarbWorld = world.data(); a.gGPlane = got.data();
		a.gW = tileW; a.gRows = tileRows; a.gSrcY0 = srcY0;
		a.gSrcOX = srcOX; a.gSrcOY = srcOY;
		a.gMtW = gW; a.gMtH = gH; a.gMtPitch = gPitch;
		a.gMOriginX = f.mOriginX; a.gMOriginY = f.mOriginY;
		a.gMRefW = f.mRefW; a.gMRefH = f.mRefH;
		a.gOutOriginX = f.outOriginX; a.gOutOriginY = f.outOriginY;
		a.gSrcRefW = f.srcRefW; a.gSrcRefH = f.srcRefH;
		a.gDsx = f.dsx; a.gDsy = f.dsy; a.gChannel = chan;
		QK_RunGrid(a, tileW, tileRows);

		char name[80];
		std::snprintf(name, sizeof(name), "qk_garbage (Fit mapping, channel %d)", chan);
		check(name, maxAbs(got, ref));
	}
}

// ---------------------------------------------------------------------------
// qk_iir.kernel vs QK_IIRRow. The seed step (gFirst) must WRITE the state
// without depending on what was there — that is what makes the shared,
// grow-only CUDA scratch plane safe to reuse across renders.
// ---------------------------------------------------------------------------
static void testIIR()
{
	const int W = 29, H = 13, steps = 4;
	const float kA = QK_SmoothK(20.f), kR = QK_SmoothK(80.f);

	vector<vector<float>> mattes((size_t)steps, vector<float>((size_t)W * H));
	for (int i = 0; i < steps; ++i) fillF1(mattes[(size_t)i], 41u + (unsigned)i);

	vector<float> ref((size_t)W * H, 0.f);
	for (int i = 0; i < steps; ++i)
		for (size_t p = 0; p < ref.size(); ++p)
			ref[p] = (i == 0) ? mattes[0][p] : QK_IIRStep(ref[p], mattes[(size_t)i][p], kA, kR);

	// Start from garbage, exactly as an uninitialised scratch plane would.
	vector<float> state((size_t)W * H, -12345.f);
	for (int i = 0; i < steps; ++i) {
		ArgsIIR a{};
		a.gMIn = mattes[(size_t)i].data(); a.gState = state.data();
		a.gW = W; a.gRows = H; a.gFirst = (i == 0) ? 1 : 0;
		a.gKAttack = kA; a.gKRelease = kR;
		QK_RunGrid(a, W, H);
	}
	check("qk_iir (seed + 3 folds, from garbage state)", maxAbs(state, ref));
}

// ---------------------------------------------------------------------------
// qk_compose.kernel vs QK_ComposeRow, every output mode. Reads the tile
// planes through srcOX/srcOY and writes an output-sized plane, so the offset
// addressing is under test alongside the compose math.
// ---------------------------------------------------------------------------
static void testCompose()
{
	const int srcW = 31, srcRows = 19, outW = 24, outH = 11, srcOX = 4, srcOY = 5;
	const float ratio = 0.35f;
	const QK_Gates gates = testGates();

	vector<float> src((size_t)srcW * srcRows * 4), qual((size_t)srcW * srcRows * 4);
	vector<float> matte((size_t)srcW * srcRows), garb((size_t)srcW * srcRows);
	fillARGB(src, 17u, true);
	fillARGB(qual, 19u, true);
	fillF1(matte, 23u);
	fillF1(garb, 29u);

	float packed[QK_GATES_FLOATS];
	QK_PackGates(gates, packed);

	for (int mode = QK_OUT_OVERLAY; mode <= QK_OUT_RAW; ++mode) {
		vector<float> ref((size_t)outW * outH * 4, 0.f), got((size_t)outW * outH * 4, 0.f);

		for (int oy = 0; oy < outH; ++oy)
			for (int ox = 0; ox < outW; ++ox) {
				const size_t so1 = (size_t)(oy + srcOY) * srcW + (ox + srcOX);
				const float *s = &src[so1 * 4];
				const float m = QK_InOutRatio(matte[so1], ratio);
				float rawM = 0.f;
				if (mode == QK_OUT_RAW) {
					const float *q = &qual[so1 * 4];
					float r, g, b;
					QK_Straight(q[0], q[1], q[2], q[3], r, g, b);
					rawM = QK_RawMatte(r, g, b, gates) * garb[so1];
				}
				float oa, orr, og, ob;
				QK_Compose(mode, m, rawM, s[0], s[1], s[2], s[3], oa, orr, og, ob);
				float *o = &ref[((size_t)oy * outW + ox) * 4];
				o[0] = oa; o[1] = orr; o[2] = og; o[3] = ob;
			}

		ArgsCompose a{};
		a.gCSrc = src.data(); a.gCMatte = matte.data();
		a.gCQual = qual.data(); a.gCG = garb.data(); a.gCOut = got.data();
		a.gOutW = outW; a.gOutH = outH; a.gSrcW = srcW;
		a.gSrcOX = srcOX; a.gSrcOY = srcOY; a.gMode = mode; a.gRatio = ratio;
		QK_StoreGates(a, packed);
		QK_RunGrid(a, outW, outH);

		static const char *kModeName[] = { "overlay", "keyed", "matte", "matte inv", "raw" };
		char name[80];
		std::snprintf(name, sizeof(name), "qk_compose (mode %d: %s)", mode, kModeName[mode]);
		check(name, maxAbs(got, ref));
	}
}

int main()
{
	std::printf("=== MM Qualify GPU kernel bodies vs CPU reference (exact) ===\n");

	testTile();

	gaussRGBACase("qk_gauss_rgba (X+Y, sigma 2.3)",  37, 23, 2.3, 2.3);
	gaussRGBACase("qk_gauss_rgba (X only, sigma 1.4)", 37, 23, 1.4, 0.0);
	gaussRGBACase("qk_gauss_rgba (Y only, sigma 3.1)", 37, 23, 0.0, 3.1);
	// Support wider than the plane: every tap range is clipped, so the
	// skip-don't-clamp rule (§3.2) is the only thing holding parity.
	gaussRGBACase("qk_gauss_rgba (R > plane, sigma 9)", 11, 9, 9.0, 9.0);

	gaussF1Case("qk_gauss_f1 (X+Y, sigma 2.7)",    37, 23, 2.7, 2.7);
	gaussF1Case("qk_gauss_f1 (X only, sigma 1.9)", 37, 23, 1.9, 0.0);
	gaussF1Case("qk_gauss_f1 (Y only, sigma 2.2)", 37, 23, 0.0, 2.2);
	gaussF1Case("qk_gauss_f1 (R > plane, sigma 8)", 11, 9, 8.0, 8.0);

	testMatte();
	testGarbage();
	testIIR();
	testCompose();

	std::printf("--- %d/%d passed ---\n", g_total - g_fail, g_total);
	if (g_fail) { std::printf("FAILED (%d)\n", g_fail); return 1; }
	std::printf("ALL PASS\n");
	return 0;
}
