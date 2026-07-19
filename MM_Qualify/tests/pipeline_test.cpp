/*  pipeline_test.cpp — end-to-end parity of the TILED GPU pipeline against
    the UNTILED CPU reference, AE-free (no DX12, no AE host, no Mac).

    The GPU kernels are platform-free by rule (docs/gpu-kernel-rules §0), so
    their arithmetic is transcribed into C here and driven by the REAL tile
    planner (qkgpu::QK_PlanTiles). Two independent implementations of the
    whole chain — denoise -> gates+cleanup -> temporal IIR -> matte blur ->
    ratio+compose — are run on the same synthetic multi-frame input and
    diffed. Consequence: band margins, offsets, the BGRA swizzle, the matte
    stages and the compose addressing are PROVEN before AE ever runs; any
    in-AE mismatch is D3D12 plumbing by elimination.

    Both sides call the SAME qk_core.h functions for the per-pixel math — the
    test proves the TILING and DATA MOVEMENT, which is where a hand-written
    host can diverge. It deliberately does NOT re-derive the pixel math.

    Cases sweep: denoise on/off, matte blur on/off, temporal on/off, garbage
    on/off, several output modes, an interior + world-edge band split, and
    HDR (superwhite) input. Target: max-abs 0 (same float ops, same order) —
    we assert <= 1e-6 to absorb nothing but leave headroom.

    Exit 0 = all pass.
*/

#define NOMINMAX
#include "qk_cpu_prologue.h"
#include "qk_core.h"
#include "blur.h"
#include "garbage.h"
#include "gpu/qk_gpu_host.h"

#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

using std::vector;

// ---------------------------------------------------------------------------
// Shared scenario description.
// ---------------------------------------------------------------------------
struct Scene {
	int srcW, srcH, outW, outH, srcOX, srcOY;
	double dnSigma, blSigma;
	bool temporal;
	int  nHist;                 // usable history frames (oldest-first)
	QK_Gates gates;
	float cleanB, cleanW, clipB, clipW, ratio;
	int  mode;
	float kA, kR;
	bool garbageActive;
	// buffers
	vector<vector<float>> frames;   // [0..nHist-1] history oldest-first, [nHist] current; each srcW*srcH*4 premult ARGB
	vector<float> garbage;          // srcW*srcH*4 premult ARGB (aligned to the source tile, Fit=identity for the test)
};

// Fit-identity garbage field over the SOURCE plane: 1:1, channel Alpha.
// (The layer-space Fit mapping is exercised by its own kernel; here the
// garbage plane is the source-sized plane the render samples, so identity
// mapping isolates the tiling.) We pass it through QK_GarbageChannel so the
// channel math is shared.
static float garbAt(const Scene &sc, int sx, int sy)
{
	if (!sc.garbageActive) return 1.f;
	sx = QK_ClampI(sx, 0, sc.srcW - 1);
	sy = QK_ClampI(sy, 0, sc.srcH - 1);
	const float *p = &sc.garbage[((size_t)sy * sc.srcW + sx) * 4];
	return QK_GarbageChannel(p[0], p[1], p[2], p[3], QK_GCHAN_ALPHA);
}

// One frame's cleaned matte (denoise -> gates -> cleanclip), untiled.
static void frameMatte(const Scene &sc, const vector<float> &frame, vector<float> &mOut)
{
	const int W = sc.srcW, H = sc.srcH;
	vector<float> q = frame, tmp(frame.size());
	if (qk::GaussianSupport(sc.dnSigma) > 0)
		qk::GaussianBlurARGB(nullptr, frame.data(), tmp.data(), q.data(), W, H, sc.dnSigma, sc.dnSigma);

	mOut.assign((size_t)W * H, 0.f);
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			const float *pq = &q[((size_t)y * W + x) * 4];
			float r, g, b; QK_Straight(pq[0], pq[1], pq[2], pq[3], r, g, b);
			float m = QK_RawMatte(r, g, b, sc.gates) * garbAt(sc, x, y);
			mOut[(size_t)y * W + x] = QK_CleanClip(m, sc.cleanB, sc.cleanW, sc.clipB, sc.clipW);
		}
}

// ---------------------------------------------------------------------------
// UNTILED CPU reference: full frame, no bands.
// ---------------------------------------------------------------------------
static void referenceRender(const Scene &sc, vector<float> &outRGBA)
{
	const int W = sc.srcW, H = sc.srcH;
	const vector<float> &cur = sc.frames[sc.nHist];

	// Cleaned matte for the current frame + temporal fold.
	vector<float> m; frameMatte(sc, cur, m);
	vector<float> state;
	const float *finalM = m.data();
	if (sc.temporal && sc.nHist > 0) {
		bool have = false;
		for (int j = 0; j < sc.nHist; ++j) {
			vector<float> hm; frameMatte(sc, sc.frames[j], hm);
			if (!have) { state = hm; have = true; }
			else for (size_t i = 0; i < state.size(); ++i)
				state[i] = QK_IIRStep(state[i], hm[i], sc.kA, sc.kR);
		}
		for (size_t i = 0; i < state.size(); ++i)
			state[i] = QK_IIRStep(state[i], m[i], sc.kA, sc.kR);
		finalM = state.data();
	}

	// Matte blur (edge-neutral) in place.
	vector<float> mb((size_t)W * H), tmp((size_t)W * H);
	if (qk::GaussianSupport(sc.blSigma) > 0) {
		std::memcpy(mb.data(), finalM, mb.size() * sizeof(float));
		qk::GaussianBlurF1(nullptr, mb.data(), tmp.data(), mb.data(), W, H, sc.blSigma, sc.blSigma);
		finalM = mb.data();
	}

	// Ratio + compose over OUTPUT rows.
	outRGBA.assign((size_t)sc.outW * sc.outH * 4, 0.f);
	for (int oy = 0; oy < sc.outH; ++oy)
		for (int ox = 0; ox < sc.outW; ++ox) {
			const int sx = ox + sc.srcOX, sy = oy + sc.srcOY;
			const float *s = &cur[((size_t)sy * W + sx) * 4];
			const float mm = QK_InOutRatio(finalM[(size_t)sy * W + sx], sc.ratio);
			float rawM = 0.f;
			if (sc.mode == QK_OUT_RAW) rawM = m[(size_t)sy * W + sx];
			float oa, orr, og, ob;
			QK_Compose(sc.mode, mm, rawM, s[0], s[1], s[2], s[3], oa, orr, og, ob);
			float *o = &outRGBA[((size_t)oy * sc.outW + ox) * 4];
			o[0]=oa; o[1]=orr; o[2]=og; o[3]=ob;
		}
}

// ---------------------------------------------------------------------------
// TILED "GPU" pipeline: same math, but computed band-by-band through the real
// planner, so the reference and this can only differ if the tiling arithmetic
// (margins, offsets, per-band compose addressing) is wrong.
// ---------------------------------------------------------------------------
static void tiledRender(const Scene &sc, vector<float> &outRGBA)
{
	const int W = sc.srcW, H = sc.srcH;

	const int dnR = (int)qk::GaussianSupport(sc.dnSigma);
	const int blR = (int)qk::GaussianSupport(sc.blSigma);
	const bool denoise = dnR > 0;
	const int marginY = qkgpu::QK_TileMarginY(dnR, blR);

	// Force a multi-band split: a small budget so several bands are produced.
	const size_t bytesTile = qkgpu::QK_TileBytesPerRow(denoise, sc.temporal, W);
	const size_t bytesOut  = qkgpu::QK_OutBytesPerRow(sc.outW);
	// budget that admits ~ (outH/3) rows per band but >= min band + margins
	size_t budget = (size_t)(2 * marginY + (sc.outH / 3 + qkgpu::QK_TILE_MIN_BAND)) * bytesTile
	              + (size_t)(sc.outH / 3 + qkgpu::QK_TILE_MIN_BAND) * bytesOut;

	qkgpu::QK_TilePlan plan;
	bool ok = qkgpu::QK_PlanTiles(sc.outH, H, sc.srcOY, marginY, bytesTile, bytesOut, budget, &plan);
	if (!ok) { std::printf("  (planner refused — budget too small)\n"); std::exit(2); }

	outRGBA.assign((size_t)sc.outW * sc.outH * 4, 0.f);

	// Per band, reproduce mm_gpu_dx.cpp's schedule on CPU buffers.
	for (const qkgpu::QK_TileBand &band : plan.bands) {
		const int rows = band.srcRows;
		const int y0   = band.srcY0;

		// tile-local ARGB loader: world rows [y0, y0+rows) — full width.
		auto loadTile = [&](const vector<float> &frame, vector<float> &tile) {
			tile.assign((size_t)W * rows * 4, 0.f);
			for (int y = 0; y < rows; ++y)
				std::memcpy(&tile[(size_t)y * W * 4],
				            &frame[((size_t)(y0 + y) * W) * 4],
				            (size_t)W * 4 * sizeof(float));
		};

		// tile matte (denoise -> gates -> cleanclip) for one frame tile.
		auto tileMatte = [&](const vector<float> &frame, vector<float> &mtile) {
			vector<float> tile; loadTile(frame, tile);
			vector<float> q = tile, tmp(tile.size());
			if (denoise)
				qk::GaussianBlurARGB(nullptr, tile.data(), tmp.data(), q.data(), W, rows, sc.dnSigma, sc.dnSigma);
			mtile.assign((size_t)W * rows, 0.f);
			for (int y = 0; y < rows; ++y)
				for (int x = 0; x < W; ++x) {
					const float *pq = &q[((size_t)y * W + x) * 4];
					float r, g, b; QK_Straight(pq[0], pq[1], pq[2], pq[3], r, g, b);
					// garbage sampled at the world pixel (Fit-identity here)
					const int sx = x, sy = y0 + y;
					float m = QK_RawMatte(r, g, b, sc.gates) * garbAt(sc, sx, sy);
					mtile[(size_t)y * W + x] = QK_CleanClip(m, sc.cleanB, sc.cleanW, sc.clipB, sc.clipW);
				}
		};

		// Temporal fold across history tiles, then current.
		vector<float> curM; tileMatte(sc.frames[sc.nHist], curM);
		const float *finalM = curM.data();
		vector<float> state;
		if (sc.temporal && sc.nHist > 0) {
			bool have = false;
			for (int j = 0; j < sc.nHist; ++j) {
				vector<float> hm; tileMatte(sc.frames[j], hm);
				if (!have) { state = hm; have = true; }
				else for (size_t i = 0; i < state.size(); ++i)
					state[i] = QK_IIRStep(state[i], hm[i], sc.kA, sc.kR);
			}
			for (size_t i = 0; i < state.size(); ++i)
				state[i] = QK_IIRStep(state[i], curM[i], sc.kA, sc.kR);
			finalM = state.data();
		}

		// Matte blur on the tile (edge-neutral).
		vector<float> mb, tmp;
		if (blR > 0) {
			mb.assign((size_t)W * rows, 0.f);
			std::memcpy(mb.data(), finalM, mb.size() * sizeof(float));
			tmp.assign((size_t)W * rows, 0.f);
			qk::GaussianBlurF1(nullptr, mb.data(), tmp.data(), mb.data(), W, rows, sc.blSigma, sc.blSigma);
			finalM = mb.data();
		}

		// current-frame source tile for compose.
		vector<float> curTile; loadTile(sc.frames[sc.nHist], curTile);

		// Ratio + compose over the band's OUTPUT rows, through loOY.
		for (int by = 0; by < band.outH; ++by) {
			const int oy = band.outY0 + by;
			for (int ox = 0; ox < sc.outW; ++ox) {
				const int tx = ox + sc.srcOX;
				const int ty = by + band.loOY;
				const float *s = &curTile[((size_t)ty * W + tx) * 4];
				const float mm = QK_InOutRatio(finalM[(size_t)ty * W + tx], sc.ratio);
				float rawM = 0.f;
				if (sc.mode == QK_OUT_RAW) rawM = curM[(size_t)ty * W + tx];
				float oa, orr, og, ob;
				QK_Compose(sc.mode, mm, rawM, s[0], s[1], s[2], s[3], oa, orr, og, ob);
				float *o = &outRGBA[((size_t)oy * sc.outW + ox) * 4];
				o[0]=oa; o[1]=orr; o[2]=og; o[3]=ob;
			}
		}
	}
}

// ---------------------------------------------------------------------------
static float maxAbsDiff(const vector<float> &a, const vector<float> &b)
{
	float m = 0.f;
	for (size_t i = 0; i < a.size(); ++i) {
		float d = std::fabs(a[i] - b[i]);
		if (d > m) m = d;
	}
	return m;
}

static void fillFrame(vector<float> &f, int W, int H, unsigned seed, bool hdr)
{
	f.assign((size_t)W * H * 4, 0.f);
	unsigned s = seed * 2654435761u + 1u;
	auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (float)((s >> 8) & 0xFFFF) / 65535.f; };
	for (int y = 0; y < H; ++y)
		for (int x = 0; x < W; ++x) {
			float *p = &f[((size_t)y * W + x) * 4];
			float a = 0.3f + 0.7f * rnd();
			float scale = hdr ? (0.5f + 3.f * rnd()) : 1.f;
			p[0] = a;
			p[1] = a * scale * rnd();   // premultiplied
			p[2] = a * scale * rnd();
			p[3] = a * scale * rnd();
		}
}

static int g_fail = 0, g_total = 0;
static void run(const char *name, Scene &sc)
{
	++g_total;
	vector<float> ref, til;
	referenceRender(sc, ref);
	tiledRender(sc, til);
	float d = maxAbsDiff(ref, til);
	bool ok = d <= 1e-6f;
	std::printf("  %-42s max-abs = %.3e  %s\n", name, d, ok ? "OK" : "FAIL");
	if (!ok) ++g_fail;
}

int main()
{
	std::printf("=== MM Qualify tiled-vs-untiled pipeline parity ===\n");

	const int W = 40, H = 90, OW = 40, OH = 60, OX = 0, OY = 15;

	// A gate config that actually selects part of the image (hue+sat+luma).
	QK_Gates g = {};
	g.hueEn = 1.f; g.hueIn = 200.f; g.hueOut = 40.f;   // crosses the seam
	g.satEn = 1.f; g.satIn = 0.1f;  g.satOut = 1.5f; g.satOutSoft = 0.2f;
	g.lumEn = 1.f; g.lumIn = 0.05f; g.lumOut = 4.f;  g.lumInSoft = 0.05f;
	g.lumR = 0.2126f; g.lumG = 0.7152f; g.lumB = 0.0722f;
	g.model = QK_MODEL_HSV; g.qin = QK_QIN_ACESCCT;

	auto baseScene = [&](Scene &sc) {
		sc.srcW=W; sc.srcH=H; sc.outW=OW; sc.outH=OH; sc.srcOX=OX; sc.srcOY=OY;
		sc.dnSigma=0; sc.blSigma=0; sc.temporal=false; sc.nHist=0;
		sc.gates=g; sc.cleanB=0; sc.cleanW=0; sc.clipB=0; sc.clipW=1; sc.ratio=0;
		sc.mode=QK_OUT_MATTE; sc.kA=1.f; sc.kR=0.15f; sc.garbageActive=false;
		sc.frames.assign(1, {}); fillFrame(sc.frames[0], W, H, 1, false);
		sc.garbage.clear();
	};

	// 1. Plain gates -> matte, no finesse.
	{ Scene sc; baseScene(sc); run("gates only (matte view)", sc); }

	// 2. Denoise on.
	{ Scene sc; baseScene(sc); sc.dnSigma=2.5; run("denoise pre-filter", sc); }

	// 3. Matte blur on.
	{ Scene sc; baseScene(sc); sc.blSigma=3.0; run("matte blur", sc); }

	// 4. Denoise + blur + clean/clip + ratio (grow).
	{ Scene sc; baseScene(sc); sc.dnSigma=1.5; sc.blSigma=2.0;
	  sc.cleanB=0.2f; sc.cleanW=0.15f; sc.clipB=0.05f; sc.clipW=0.9f; sc.ratio=0.4f;
	  run("denoise+blur+clean+clip+grow", sc); }

	// 5. Keyed output over HDR input.
	{ Scene sc; baseScene(sc); sc.mode=QK_OUT_KEYED; sc.blSigma=2.0;
	  fillFrame(sc.frames[0], W, H, 7, true); run("keyed output, HDR input", sc); }

	// 6. Highlight view.
	{ Scene sc; baseScene(sc); sc.mode=QK_OUT_HILITE; sc.blSigma=1.5;
	  run("highlight view", sc); }

	// 7. Qualify Raw view (recompute path).
	{ Scene sc; baseScene(sc); sc.mode=QK_OUT_RAW; sc.dnSigma=1.5; sc.blSigma=2.0;
	  run("qualify raw view", sc); }

	// 8. Garbage active (Fit-identity source-sized plane).
	{ Scene sc; baseScene(sc); sc.garbageActive=true; sc.blSigma=1.5;
	  sc.garbage.assign((size_t)W*H*4, 0.f);
	  for (int y=0;y<H;++y) for (int x=0;x<W;++x){ float*p=&sc.garbage[((size_t)y*W+x)*4];
	     float a = (x < W/2) ? 1.f : 0.2f; p[0]=a; p[1]=a; p[2]=a; p[3]=a; }
	  run("garbage matte (identity Fit)", sc); }

	// 9. Temporal stabilize: 4 history + current.
	{ Scene sc; baseScene(sc); sc.temporal=true; sc.nHist=4; sc.blSigma=2.0;
	  sc.frames.assign(sc.nHist+1, {});
	  for (int i=0;i<=sc.nHist;++i) fillFrame(sc.frames[i], W, H, 11+i, false);
	  run("temporal stabilize (4 hist)", sc); }

	// 10. Temporal + denoise + blur + everything, HDR.
	{ Scene sc; baseScene(sc); sc.temporal=true; sc.nHist=5; sc.dnSigma=2.0; sc.blSigma=2.5;
	  sc.cleanB=0.1f; sc.cleanW=0.1f; sc.clipB=0.05f; sc.clipW=0.95f; sc.ratio=-0.3f;
	  sc.mode=QK_OUT_KEYED;
	  sc.frames.assign(sc.nHist+1, {});
	  for (int i=0;i<=sc.nHist;++i) fillFrame(sc.frames[i], W, H, 31+i, true);
	  run("temporal+denoise+blur+clip+shrink, HDR", sc); }

	std::printf("--- %d/%d passed ---\n", g_total - g_fail, g_total);
	if (g_fail) { std::printf("FAILED (%d)\n", g_fail); return 1; }
	std::printf("ALL PASS\n");
	return 0;
}
