/*  pipeline_test.cpp — end-to-end parity of the TILED GPU pipeline against
    the UNTILED CPU reference, AE-free (no GPU, no DX12, no CUDA toolkit).

    The tiled side is not an emulation. It RUNS THE SHIPPED KERNELS: the real
    .kernel bodies, compiled as ordinary C++ through qk_prologue_host.h (see
    src/gpu/cuda/qk_kernel_args.h), driven by the REAL tile planner
    (qkgpu::QK_PlanTiles), in the exact pass sequence and plane schedule that
    mm_gpu_dx.cpp and qk_gpu_cuda.cpp enqueue — including the BGRA world
    load/store with a pitch wider than the width, the denoise A->T->Q roles,
    the temporal IIR streaming history frames oldest-first through one state
    plane, and the matte-blur ping-pong that keeps in != out on every pass.

    The reference side is the real CPU render path: qk::GaussianBlurARGB,
    qk::GaussianBlurF1, qk::SampleGarbage and qk_core.h, untiled, whole-frame.

    Together with kernel_test.cpp (which proves each body against the CPU in
    isolation) this closes the loop AE_GPU_Effects.md §4.3 asks for: per-pixel
    formulas AND their composition. A mismatch in AE after both are green is
    backend plumbing — module load, bindings, launch config — by elimination.

    Cases sweep: denoise on/off, matte blur on/off (both axes and single
    axis), temporal on/off, garbage on/off under a non-identity Fit mapping,
    every output mode, an interior + world-edge band split, and HDR
    (superwhite) input. Target: max-abs 0 — same source, same arithmetic, same
    order (rules §5.1). We assert <= 1e-6 to leave headroom, not to absorb.

    Exit 0 = all pass.
*/

#define NOMINMAX
#include "qk_cpu_prologue.h"
#include "qk_core.h"
#include "blur.h"
#include "garbage.h"
#include "gpu/qk_gpu_host.h"
#include "gpu/cuda/qk_kernel_args.h"

#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

using std::vector;
using namespace qkgpu;

// ---------------------------------------------------------------------------
// Shared scenario description. Frames live as AE GPU worlds — BGRA float with
// a row pitch deliberately wider than width*4 — because that is what both
// hosts are handed, and deriving pitch from width is the classic bug.
// ---------------------------------------------------------------------------
struct Scene {
	int srcW, srcH, outW, outH, srcOX, srcOY;
	int srcPitch, outPitch;              // in FLOATS (rowbytes / 4)
	double dnSigma, blSigmaX, blSigmaY;
	bool temporal;
	int  nHist;                          // usable history frames (oldest-first)
	QK_Gates gates;
	float clipB, clipW, ratio;
	int  mode;
	float kA, kR;

	// [0..nHist-1] history oldest-first, [nHist] current. BGRA, pitched.
	vector<vector<float>> worlds;

	// Garbage layer, in both layouts: the BGRA pitched world the kernel reads
	// and the packed ARGB plane qk::SampleGarbage reads. Same values.
	bool  garbActive = false;
	int   gW = 0, gH = 0, gPitch = 0;
	vector<float> garbWorld, garbPlane;
	qk::QK_GarbageField garb;            // geometry for the CPU side
};

// World (BGRA, pitched) -> packed ARGB plane, the whole frame. The reference
// side works in ARGB exactly as the CPU render does after qk::UnpackRow.
static void unswizzle(const float *world, int pitch, int W, int H, vector<float> &argb)
{
	argb.assign((size_t)W * H * 4, 0.f);
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x)
			for (int c = 0; c < 4; ++c)
				argb[((size_t)y * W + x) * 4 + c] = world[(size_t)y * pitch + (size_t)x * 4 + (3 - c)];
}

static void gaussKernels(const Scene &sc, vector<float> &kDnX, vector<float> &kDnY,
                         vector<float> &kBlX, vector<float> &kBlY,
                         int &dnRx, int &dnRy, int &blRx, int &blRy)
{
	dnRx = (int)qk::GaussianSupport(sc.dnSigma);
	dnRy = (int)qk::GaussianSupport(sc.dnSigma);
	blRx = (int)qk::GaussianSupport(sc.blSigmaX);
	blRy = (int)qk::GaussianSupport(sc.blSigmaY);
	if (dnRx > 0) qk::BuildGaussianKernel(sc.dnSigma,  dnRx, kDnX);
	if (dnRy > 0) qk::BuildGaussianKernel(sc.dnSigma,  dnRy, kDnY);
	if (blRx > 0) qk::BuildGaussianKernel(sc.blSigmaX, blRx, kBlX);
	if (blRy > 0) qk::BuildGaussianKernel(sc.blSigmaY, blRy, kBlY);
}

// ---------------------------------------------------------------------------
// UNTILED CPU reference: the real render path, full frame, no bands.
// ---------------------------------------------------------------------------

// One frame's cleaned matte (denoise -> gates -> clip), untiled.
static void frameMatte(const Scene &sc, const vector<float> &frameARGB,
                       const vector<float> &gPlane, vector<float> &mOut)
{
	const int W = sc.srcW, H = sc.srcH;
	vector<float> q = frameARGB, tmp(frameARGB.size());
	if (qk::GaussianSupport(sc.dnSigma) > 0)
		qk::GaussianBlurARGB(nullptr, frameARGB.data(), tmp.data(), q.data(),
		                     W, H, sc.dnSigma, sc.dnSigma);

	mOut.assign((size_t)W * H, 0.f);
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			const float *pq = &q[((size_t)y * W + x) * 4];
			float r, g, b; QK_Straight(pq[0], pq[1], pq[2], pq[3], r, g, b);
			const float m = QK_RawMatte(r, g, b, sc.gates) * gPlane[(size_t)y * W + x];
			mOut[(size_t)y * W + x] = QK_ClipMatte(m, sc.clipB, sc.clipW);
		}
}

static void referenceRender(const Scene &sc, vector<float> &outARGB)
{
	const int W = sc.srcW, H = sc.srcH;

	// Garbage weights over the whole source plane — the real qk::SampleGarbage,
	// addressed exactly as smart_render.cpp's QK_GarbageRow addresses it.
	vector<float> gPlane((size_t)W * H, 1.f);
	if (sc.garbActive)
		for (int y = 0; y < H; ++y)
			for (int x = 0; x < W; ++x)
				gPlane[(size_t)y * W + x] =
					qk::SampleGarbage(sc.garb, x - sc.srcOX, y - sc.srcOY);

	vector<float> cur;
	unswizzle(sc.worlds[(size_t)sc.nHist].data(), sc.srcPitch, W, H, cur);

	// Qualification copy of the current frame (needed by the Raw view).
	vector<float> qual = cur, qtmp(cur.size());
	if (qk::GaussianSupport(sc.dnSigma) > 0)
		qk::GaussianBlurARGB(nullptr, cur.data(), qtmp.data(), qual.data(),
		                     W, H, sc.dnSigma, sc.dnSigma);

	vector<float> m; frameMatte(sc, cur, gPlane, m);

	// Temporal fold: warm-started IIR, oldest -> current.
	vector<float> state;
	const float *finalM = m.data();
	if (sc.temporal && sc.nHist > 0) {
		bool have = false;
		for (int j = 0; j < sc.nHist; ++j) {
			vector<float> hARGB, hm;
			unswizzle(sc.worlds[(size_t)j].data(), sc.srcPitch, W, H, hARGB);
			frameMatte(sc, hARGB, gPlane, hm);
			if (!have) { state = hm; have = true; }
			else for (size_t i = 0; i < state.size(); ++i)
				state[i] = QK_IIRStep(state[i], hm[i], sc.kA, sc.kR);
		}
		for (size_t i = 0; i < state.size(); ++i)
			state[i] = QK_IIRStep(state[i], m[i], sc.kA, sc.kR);
		finalM = state.data();
	}

	// Matte blur (edge-neutral) in place on the final matte plane.
	vector<float> mb((size_t)W * H), tmp((size_t)W * H);
	if (qk::GaussianSupport(sc.blSigmaX) > 0 || qk::GaussianSupport(sc.blSigmaY) > 0) {
		std::memcpy(mb.data(), finalM, mb.size() * sizeof(float));
		qk::GaussianBlurF1(nullptr, mb.data(), tmp.data(), mb.data(),
		                   W, H, sc.blSigmaX, sc.blSigmaY);
		finalM = mb.data();
	}

	// Ratio + compose over OUTPUT rows.
	outARGB.assign((size_t)sc.outW * sc.outH * 4, 0.f);
	for (int oy = 0; oy < sc.outH; ++oy)
		for (int ox = 0; ox < sc.outW; ++ox) {
			const size_t so1 = (size_t)(oy + sc.srcOY) * W + (ox + sc.srcOX);
			const float *s = &cur[so1 * 4];
			const float mm = QK_InOutRatio(finalM[so1], sc.ratio);
			float rawM = 0.f;
			if (sc.mode == QK_OUT_RAW) {
				const float *q = &qual[so1 * 4];
				float r, g, b; QK_Straight(q[0], q[1], q[2], q[3], r, g, b);
				rawM = QK_RawMatte(r, g, b, sc.gates) * gPlane[so1];
			}
			float oa, orr, og, ob;
			QK_Compose(sc.mode, mm, rawM, s[0], s[1], s[2], s[3], oa, orr, og, ob);
			float *o = &outARGB[((size_t)oy * sc.outW + ox) * 4];
			o[0]=oa; o[1]=orr; o[2]=og; o[3]=ob;
		}
}

// ---------------------------------------------------------------------------
// TILED pipeline: the REAL kernels, the REAL planner, the hosts' own pass
// sequence and plane schedule. Read this beside QK_DXRender / QK_CudaRender —
// the three are meant to stay identical, and a divergence is a bug in one of
// them, not a backend difference.
// ---------------------------------------------------------------------------
static void tiledRender(const Scene &sc, vector<float> &outARGB)
{
	const int W = sc.srcW, H = sc.srcH;

	vector<float> kDnX, kDnY, kBlX, kBlY;
	int dnRx, dnRy, blRx, blRy;
	gaussKernels(sc, kDnX, kDnY, kBlX, kBlY, dnRx, dnRy, blRx, blRy);

	const bool denoise = (dnRx > 0 || dnRy > 0);
	const int marginY = qkgpu::QK_TileMarginY(dnRy, blRy);

	// Force a multi-band split: a budget that admits roughly outH/3 rows.
	const size_t bytesTile = qkgpu::QK_TileBytesPerRow(denoise, sc.temporal, W);
	const size_t bytesOut  = qkgpu::QK_OutBytesPerRow(sc.outW);
	const size_t budget = (size_t)(2 * marginY + (sc.outH / 3 + qkgpu::QK_TILE_MIN_BAND)) * bytesTile
	                    + (size_t)(sc.outH / 3 + qkgpu::QK_TILE_MIN_BAND) * bytesOut;

	qkgpu::QK_TilePlan plan;
	if (!qkgpu::QK_PlanTiles(sc.outH, H, sc.srcOY, marginY, bytesTile, bytesOut, budget, &plan)) {
		std::printf("  (planner refused - budget too small)\n"); std::exit(2);
	}

	float gatePacked[QK_GATES_FLOATS];
	qkgpu::QK_PackGates(sc.gates, gatePacked);

	// Scratch planes, sized for the worst band exactly as the hosts size them.
	const int rowsMax = plan.tileRowsMax;
	vector<float> pA((size_t)rowsMax * W * 4, 0.f);
	vector<float> pT((size_t)rowsMax * W * 4, 0.f);
	vector<float> pQstore((size_t)rowsMax * W * 4, 0.f);
	vector<float> pM((size_t)rowsMax * W, 0.f);
	vector<float> pB1((size_t)rowsMax * W, 0.f);
	vector<float> pG((size_t)rowsMax * W, 0.f);
	// Deliberately garbage: the IIR seed step must not depend on prior content,
	// which is what makes the CUDA host's cached, shared scratch plane safe.
	vector<float> pS((size_t)rowsMax * W, -12345.f);
	vector<float> pOut((size_t)plan.bandHMax * sc.outW * 4, 0.f);

	// Output world: BGRA, pitched, pre-poisoned so an unwritten row is loud.
	vector<float> outWorld((size_t)sc.outH * sc.outPitch, -7777.f);

	float *const Q = denoise ? pQstore.data() : pA.data();   // Q aliases A when off

	auto gaussRGBA = [&](const float *in, float *tmp, float *out, int rows) {
		const float *cur = in;
		if (dnRx > 0) {
			float *dst = (dnRy > 0) ? tmp : out;
			ArgsGaussRGBA a{};
			a.gRgSrc = cur; a.gRgKern = kDnX.data(); a.gRgDst = dst;
			a.gRgW = W; a.gRgRows = rows; a.gRgR = dnRx; a.gRgHoriz = 1;
			QK_RunGrid(a, W, rows);
			cur = dst;
		}
		if (dnRy > 0) {
			ArgsGaussRGBA a{};
			a.gRgSrc = cur; a.gRgKern = kDnY.data(); a.gRgDst = out;
			a.gRgW = W; a.gRgRows = rows; a.gRgR = dnRy; a.gRgHoriz = 0;
			QK_RunGrid(a, W, rows);
		}
	};

	auto loadTile = [&](const vector<float> &world, int y0, int rows, float *tile) {
		ArgsLoad a{};
		a.gLdWorld = world.data(); a.gLdTile = tile;
		a.gLdW = W; a.gLdRows = rows; a.gLdWorldY0 = y0; a.gLdPitch = sc.srcPitch;
		QK_RunGrid(a, W, rows);
	};

	auto matteKernel = [&](const float *qual, int rows) {
		ArgsMatte a{};
		a.gQual = qual; a.gGIn = pG.data(); a.gMOut = pM.data();
		a.gW = W; a.gRows = rows;
		QK_StoreGates(a, gatePacked);
		a.gClipB = sc.clipB; a.gClipW = sc.clipW;
		QK_RunGrid(a, W, rows);
	};

	// ---------------- Per band ----------------
	for (const qkgpu::QK_TileBand &band : plan.bands) {
		const int rows = band.srcRows;

		// Garbage weights for the tile, or fill 1.0 when inactive.
		if (sc.garbActive) {
			ArgsGarbage a{};
			a.gGarbWorld = sc.garbWorld.data(); a.gGPlane = pG.data();
			a.gW = W; a.gRows = rows; a.gSrcY0 = band.srcY0;
			a.gSrcOX = sc.srcOX; a.gSrcOY = sc.srcOY;
			a.gMtW = sc.gW; a.gMtH = sc.gH; a.gMtPitch = sc.gPitch;
			a.gMOriginX = sc.garb.mOriginX; a.gMOriginY = sc.garb.mOriginY;
			a.gMRefW = sc.garb.mRefW; a.gMRefH = sc.garb.mRefH;
			a.gOutOriginX = sc.garb.outOriginX; a.gOutOriginY = sc.garb.outOriginY;
			a.gSrcRefW = sc.garb.srcRefW; a.gSrcRefH = sc.garb.srcRefH;
			a.gDsx = sc.garb.dsx; a.gDsy = sc.garb.dsy; a.gChannel = sc.garb.channel;
			QK_RunGrid(a, W, rows);
		} else {
			ArgsFill a{};
			a.gFillDst = pG.data(); a.gFW = W; a.gFRows = rows;
			QK_RunGrid(a, W, rows);
		}

		// TEMPORAL: stream history frames oldest-first through the IIR.
		bool haveState = false;
		if (sc.temporal) {
			for (int hi = 0; hi < sc.nHist; ++hi) {
				loadTile(sc.worlds[(size_t)hi], band.srcY0, rows, pA.data());
				if (denoise) gaussRGBA(pA.data(), pT.data(), Q, rows);
				matteKernel(Q, rows);
				ArgsIIR a{};
				a.gMIn = pM.data(); a.gState = pS.data();
				a.gW = W; a.gRows = rows; a.gFirst = haveState ? 0 : 1;
				a.gKAttack = sc.kA; a.gKRelease = sc.kR;
				QK_RunGrid(a, W, rows);
				haveState = true;
			}
		}

		// Current frame -> A (loaded LAST so compose reads it).
		loadTile(sc.worlds[(size_t)sc.nHist], band.srcY0, rows, pA.data());
		if (denoise) gaussRGBA(pA.data(), pT.data(), Q, rows);
		matteKernel(Q, rows);

		// Final IIR fold: state <- current matte; the matte then IS the state.
		float *matte = pM.data();
		if (sc.temporal && haveState) {
			ArgsIIR a{};
			a.gMIn = pM.data(); a.gState = pS.data();
			a.gW = W; a.gRows = rows; a.gFirst = 0;
			a.gKAttack = sc.kA; a.gKRelease = sc.kR;
			QK_RunGrid(a, W, rows);
			matte = pS.data();
		}

		// Matte blur ping-pong — in != out on every pass, no third plane.
		float *finalMatte = matte;
		if (blRx > 0 || blRy > 0) {
			float *cur = matte;
			if (blRx > 0) {
				ArgsGaussF1 a{};
				a.gF1Src = cur; a.gF1Kern = kBlX.data(); a.gF1Dst = pB1.data();
				a.gF1W = W; a.gF1Rows = rows; a.gF1R = blRx; a.gF1Horiz = 1;
				QK_RunGrid(a, W, rows);
				cur = pB1.data();
			}
			if (blRy > 0) {
				float *dst = (cur == matte) ? pB1.data() : matte;
				ArgsGaussF1 a{};
				a.gF1Src = cur; a.gF1Kern = kBlY.data(); a.gF1Dst = dst;
				a.gF1W = W; a.gF1Rows = rows; a.gF1R = blRy; a.gF1Horiz = 0;
				QK_RunGrid(a, W, rows);
				cur = dst;
			}
			finalMatte = cur;
		}

		// Ratio + compose -> output tile, through the band's loOY.
		{
			ArgsCompose a{};
			a.gCSrc = pA.data(); a.gCMatte = finalMatte; a.gCQual = Q; a.gCG = pG.data();
			a.gCOut = pOut.data();
			a.gOutW = sc.outW; a.gOutH = band.outH; a.gSrcW = W;
			a.gSrcOX = sc.srcOX; a.gSrcOY = band.loOY;
			a.gMode = sc.mode; a.gRatio = sc.ratio;
			QK_StoreGates(a, gatePacked);
			QK_RunGrid(a, sc.outW, band.outH);
		}

		// Store the band into the output world.
		{
			ArgsStore a{};
			a.gStTile = pOut.data(); a.gStWorld = outWorld.data();
			a.gStW = sc.outW; a.gStRows = band.outH; a.gStWorldY0 = band.outY0;
			a.gStPitch = sc.outPitch;
			QK_RunGrid(a, sc.outW, band.outH);
		}
	}

	unswizzle(outWorld.data(), sc.outPitch, sc.outW, sc.outH, outARGB);
}

// ---------------------------------------------------------------------------
static float maxAbsDiff(const vector<float> &a, const vector<float> &b)
{
	float m = 0.f;
	for (size_t i = 0; i < a.size(); ++i) {
		const float d = std::fabs(a[i] - b[i]);
		if (d > m) m = d;
	}
	return m;
}

// A BGRA world with a pitch wider than the row, poisoned in the padding so a
// pitch mistake reads as an enormous error rather than a plausible one.
static void fillWorld(vector<float> &w, int W, int H, int pitch, unsigned seed, bool hdr)
{
	w.assign((size_t)H * pitch, -9999.f);
	unsigned s = seed * 2654435761u + 1u;
	auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (float)((s >> 8) & 0xFFFF) / 65535.f; };
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			float *p = &w[(size_t)y * pitch + (size_t)x * 4];
			const float a = 0.3f + 0.7f * rnd();
			const float scale = hdr ? (0.5f + 3.f * rnd()) : 1.f;
			const float r = a * scale * rnd(), g = a * scale * rnd(), b = a * scale * rnd();
			p[0] = b; p[1] = g; p[2] = r; p[3] = a;      // BGRA
		}
}

static int g_fail = 0, g_total = 0;
static void run(const char *name, Scene &sc)
{
	++g_total;
	vector<float> ref, til;
	referenceRender(sc, ref);
	tiledRender(sc, til);
	const float d = maxAbsDiff(ref, til);
	const bool ok = d <= 1e-6f;
	std::printf("  %-42s max-abs = %.3e  %s\n", name, d, ok ? "OK" : "FAIL");
	if (!ok) ++g_fail;
}

int main()
{
	std::printf("=== MM Qualify tiled GPU kernels vs untiled CPU reference ===\n");

	const int W = 40, H = 90, OW = 40, OH = 60, OX = 0, OY = 15;
	const int SPITCH = W * 4 + 9, OPITCH = OW * 4 + 5;

	// A gate config that actually selects part of the image (hue+sat+luma).
	QK_Gates g = {};
	g.hueEn = 1.f; g.hueIn = 200.f; g.hueOut = 40.f;   // crosses the seam
	g.satEn = 1.f; g.satIn = 0.1f;  g.satOut = 1.5f; g.satOutSoft = 0.2f;
	g.lumEn = 1.f; g.lumIn = 0.05f; g.lumOut = 4.f;  g.lumInSoft = 0.05f;
	g.lumR = 0.2126f; g.lumG = 0.7152f; g.lumB = 0.0722f;
	g.model = QK_MODEL_HSL; g.qin = QK_QIN_LINLOG;

	auto baseScene = [&](Scene &sc) {
		sc.srcW=W; sc.srcH=H; sc.outW=OW; sc.outH=OH; sc.srcOX=OX; sc.srcOY=OY;
		sc.srcPitch=SPITCH; sc.outPitch=OPITCH;
		sc.dnSigma=0; sc.blSigmaX=0; sc.blSigmaY=0; sc.temporal=false; sc.nHist=0;
		sc.gates=g; sc.clipB=0; sc.clipW=1; sc.ratio=0;
		sc.mode=QK_OUT_MATTE; sc.kA=1.f; sc.kR=0.15f;
		sc.worlds.assign(1, {}); fillWorld(sc.worlds[0], W, H, SPITCH, 1, false);
		sc.garbActive=false; sc.garbWorld.clear(); sc.garbPlane.clear();
		sc.garb = qk::QK_GarbageField();
	};

	// A garbage layer under a NON-identity Fit mapping (different layer frame,
	// non-zero origins, half-res buffers) — the tiled and untiled sides must
	// agree on layer-space arithmetic, not just on a 1:1 copy.
	auto addGarbage = [&](Scene &sc) {
		sc.gW = 26; sc.gH = 34; sc.gPitch = sc.gW * 4 + 3;
		sc.garbWorld.assign((size_t)sc.gH * sc.gPitch, -9999.f);
		sc.garbPlane.assign((size_t)sc.gW * sc.gH * 4, 0.f);
		unsigned s = 4242u;
		auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (float)((s >> 8) & 0xFFFF) / 65535.f; };
		for (int y = 0; y < sc.gH; ++y)
			for (int x = 0; x < sc.gW; ++x) {
				const float a = (x < sc.gW / 2) ? 1.f : 0.15f + 0.5f * rnd();
				const float r = a * rnd(), gg = a * rnd(), b = a * rnd();
				float *pp = &sc.garbPlane[((size_t)y * sc.gW + x) * 4];
				pp[0]=a; pp[1]=r; pp[2]=gg; pp[3]=b;
				float *pw = &sc.garbWorld[(size_t)y * sc.gPitch + (size_t)x * 4];
				pw[0]=b; pw[1]=gg; pw[2]=r; pw[3]=a;
			}
		sc.garb.data = sc.garbPlane.data();
		sc.garb.W = sc.gW; sc.garb.H = sc.gH;
		sc.garb.mOriginX = -2; sc.garb.mOriginY = 4;
		sc.garb.mRefW = 52; sc.garb.mRefH = 68;
		sc.garb.outOriginX = 6; sc.garb.outOriginY = -3;
		sc.garb.srcRefW = 80; sc.garb.srcRefH = 120;
		sc.garb.dsx = 0.5f; sc.garb.dsy = 0.5f;
		sc.garb.channel = QK_GCHAN_LUMA;
		sc.garb.active = true;
		sc.garbActive = true;
	};

	// 1. Plain gates -> matte, no finesse.
	{ Scene sc; baseScene(sc); run("gates only (matte view)", sc); }

	// 2. Denoise on.
	{ Scene sc; baseScene(sc); sc.dnSigma=2.5; run("denoise pre-filter", sc); }

	// 3. Matte blur on, both axes.
	{ Scene sc; baseScene(sc); sc.blSigmaX=3.0; sc.blSigmaY=3.0; run("matte blur (X+Y)", sc); }

	// 3b/3c. Single-axis blur — the ping-pong picks a different destination
	// plane in each case, which is where an aliasing bug would hide.
	{ Scene sc; baseScene(sc); sc.blSigmaX=2.5; run("matte blur (X only)", sc); }
	{ Scene sc; baseScene(sc); sc.blSigmaY=2.5; run("matte blur (Y only)", sc); }

	// 4. Denoise + blur + clip + ratio (grow).
	{ Scene sc; baseScene(sc); sc.dnSigma=1.5; sc.blSigmaX=2.0; sc.blSigmaY=2.0;
	  sc.clipB=0.05f; sc.clipW=0.9f; sc.ratio=0.4f;
	  run("denoise+blur+clip+grow", sc); }

	// 5. Keyed output over HDR input.
	{ Scene sc; baseScene(sc); sc.mode=QK_OUT_KEYED; sc.blSigmaX=2.0; sc.blSigmaY=2.0;
	  fillWorld(sc.worlds[0], W, H, SPITCH, 7, true); run("keyed output, HDR input", sc); }

	// 6. Selection overlay view.
	{ Scene sc; baseScene(sc); sc.mode=QK_OUT_OVERLAY; sc.blSigmaX=1.5; sc.blSigmaY=1.5;
	  run("selection overlay view", sc); }

	// 7. Qualify Raw view (the compose-side recompute path).
	{ Scene sc; baseScene(sc); sc.mode=QK_OUT_RAW; sc.dnSigma=1.5;
	  sc.blSigmaX=2.0; sc.blSigmaY=2.0; run("qualify raw view", sc); }

	// 8. Garbage matte under a non-identity Fit mapping.
	{ Scene sc; baseScene(sc); addGarbage(sc); sc.blSigmaX=1.5; sc.blSigmaY=1.5;
	  run("garbage matte (Fit mapping)", sc); }

	// 9. Temporal stabilize: 4 history + current.
	{ Scene sc; baseScene(sc); sc.temporal=true; sc.nHist=4;
	  sc.blSigmaX=2.0; sc.blSigmaY=2.0;
	  sc.worlds.assign((size_t)sc.nHist+1, {});
	  for (int i=0;i<=sc.nHist;++i) fillWorld(sc.worlds[(size_t)i], W, H, SPITCH, 11+(unsigned)i, false);
	  run("temporal stabilize (4 hist)", sc); }

	// 10. Everything at once, HDR: temporal + denoise + blur + clip + shrink
	//     + garbage, keyed out.
	{ Scene sc; baseScene(sc); addGarbage(sc);
	  sc.temporal=true; sc.nHist=5; sc.dnSigma=2.0; sc.blSigmaX=2.5; sc.blSigmaY=2.5;
	  sc.clipB=0.05f; sc.clipW=0.95f; sc.ratio=-0.3f;
	  sc.mode=QK_OUT_KEYED;
	  sc.worlds.assign((size_t)sc.nHist+1, {});
	  for (int i=0;i<=sc.nHist;++i) fillWorld(sc.worlds[(size_t)i], W, H, SPITCH, 31+(unsigned)i, true);
	  run("temporal+denoise+blur+clip+shrink+garbage, HDR", sc); }

	std::printf("--- %d/%d passed ---\n", g_total - g_fail, g_total);
	if (g_fail) { std::printf("FAILED (%d)\n", g_fail); return 1; }
	std::printf("ALL PASS\n");
	return 0;
}
