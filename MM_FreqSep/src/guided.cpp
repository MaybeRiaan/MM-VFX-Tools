/*  guided.cpp — see guided.h. CPU reference implementation of the O(1)
    box-sum guided filter: two separable unnormalized box passes around each
    per-pixel stage, rows/columns fanned out across AE's workers. This is the
    ground truth the Phase 5 GPU kernels get validated against. No clamping
    anywhere (intermediates stay unbounded; only the final 8/16bpc pack
    clamps).
*/

#define NOMINMAX
#include "guided.h"
#include "common/MM_Common.h"

#include <cstring>
#include <new>
#include <vector>

namespace fs {

A_long GuidedSupport(A_long r)
{
	return (r > 0) ? 2 * r : 0;
}

// ---------------------------------------------------------------------------
// Unnormalized box sums with zero extension, separable. dst may alias src
// (the vertical pass reads only tmp). Sums are UNNORMALIZED on purpose: the
// guided stages divide by the alpha-weight sum (box(a)) themselves, which is
// what makes every mean alpha-weighted and edge shrinkage self-normalizing.
// ---------------------------------------------------------------------------
struct BoxRefcon {
	const float *src;
	float       *dst;
	A_long       W, H, R;
	bool         horizontal;
};

extern "C" {

// One box pass line. horizontal=true slides along x of row `idx`;
// false slides along y of COLUMN `idx` (ForEachRow is a generic index
// fanout — writes stay disjoint per index either way).
static PF_Err
FS_BoxSumPass(void *refconPV, A_long /*thread_indexL*/, A_long idx, A_long /*iterationsL*/)
{
	const BoxRefcon *rc = static_cast<const BoxRefcon*>(refconPV);
	const A_long W = rc->W, H = rc->H, R = rc->R;

	// Sliding sums in double: float accumulators drift over long scanlines
	// (each add/subtract rounds at the magnitude of the running sum); double
	// keeps the stored float sums exact to ~1 ulp at any realistic size.
	double acc[4] = { 0, 0, 0, 0 };

	if (rc->horizontal) {
		const float *row = rc->src + (size_t)idx * W * 4;
		float *dst = rc->dst + (size_t)idx * W * 4;
		const A_long lead = (R < W - 1) ? R : (W - 1);
		for (A_long x = 0; x <= lead; ++x)
			for (int c = 0; c < 4; ++c) acc[c] += row[(size_t)x * 4 + c];
		for (A_long x = 0; x < W; ++x, dst += 4) {
			for (int c = 0; c < 4; ++c) dst[c] = (float)acc[c];
			const A_long add = x + R + 1, sub = x - R;
			if (add < W)
				for (int c = 0; c < 4; ++c) acc[c] += row[(size_t)add * 4 + c];
			if (sub >= 0)
				for (int c = 0; c < 4; ++c) acc[c] -= row[(size_t)sub * 4 + c];
		}
	} else {
		const size_t stride = (size_t)W * 4;
		const float *col = rc->src + (size_t)idx * 4;
		float *dst = rc->dst + (size_t)idx * 4;
		const A_long lead = (R < H - 1) ? R : (H - 1);
		for (A_long y = 0; y <= lead; ++y)
			for (int c = 0; c < 4; ++c) acc[c] += col[(size_t)y * stride + c];
		for (A_long y = 0; y < H; ++y, dst += stride) {
			for (int c = 0; c < 4; ++c) dst[c] = (float)acc[c];
			const A_long add = y + R + 1, sub = y - R;
			if (add < H)
				for (int c = 0; c < 4; ++c) acc[c] += col[(size_t)add * stride + c];
			if (sub >= 0)
				for (int c = 0; c < 4; ++c) acc[c] -= col[(size_t)sub * stride + c];
		}
	}
	return PF_Err_NONE;
}

} // extern "C"

static void
BoxSumARGB(PF_InData *in_data, const float *src, float *tmp, float *dst,
		   A_long W, A_long H, A_long rx, A_long ry)
{
	BoxRefcon h{ src, tmp, W, H, rx, true };
	mm::ForEachRow(in_data, H, &h, FS_BoxSumPass);
	BoxRefcon v{ tmp, dst, W, H, ry, false };
	mm::ForEachRow(in_data, W, &v, FS_BoxSumPass);
}

// ---------------------------------------------------------------------------
// Per-pixel stages. One refcon serves all three (they use different subsets).
// Plane roles over the pipeline:
//   s1 = box(src)             = [box(a), box(a*p)]           (stats, 1st box)
//   s2 = box(squares)         = [box(a), box(a*p^2)]
//   sa = [a, a*A]  → boxed →  [box(a), box(a*A)]             (aggregation)
//   sb = [a, a*B]  → boxed →  [box(a), box(a*B)]
// ---------------------------------------------------------------------------
struct GuidedRefcon {
	const float *src;      // pristine premultiplied ARGB
	float       *s1, *s2, *sa, *sb;
	float       *dst;
	A_long       W;
	float        eps;
	float        invCount; // 1 / ((2rx+1)*(2ry+1)) — full-window coverage norm
};

extern "C" {

// src → s2 (pre-box): [a, a*p^2] per pixel. a*p^2 == premult^2 / a.
static PF_Err
FS_GuidedSquaresRow(void *refconPV, A_long /*thread_indexL*/, A_long y, A_long /*iterationsL*/)
{
	const GuidedRefcon *rc = static_cast<const GuidedRefcon*>(refconPV);
	const size_t off = (size_t)y * rc->W * 4;
	const float *s = rc->src + off;
	float *sq = rc->s2 + off;
	for (A_long x = 0; x < rc->W; ++x, s += 4, sq += 4) {
		const float a = s[0];
		sq[0] = a;
		if (a > FS_GUIDED_AEPS) {
			const float inva = 1.f / a;
			sq[1] = s[1] * s[1] * inva;
			sq[2] = s[2] * s[2] * inva;
			sq[3] = s[3] * s[3] * inva;
		} else {
			sq[1] = sq[2] = sq[3] = 0.f;
		}
	}
	return PF_Err_NONE;
}

// (src, s1, s2) → sa/sb (pre-box): per-window linear coefficients A, B,
// carried as [a, a*A] / [a, a*B] so the next box pass aggregates them
// alpha-weighted (transparent centers don't vote).
static PF_Err
FS_GuidedABRow(void *refconPV, A_long /*thread_indexL*/, A_long y, A_long /*iterationsL*/)
{
	const GuidedRefcon *rc = static_cast<const GuidedRefcon*>(refconPV);
	const size_t off = (size_t)y * rc->W * 4;
	const float *s = rc->src + off, *m1 = rc->s1 + off, *m2 = rc->s2 + off;
	float *sa = rc->sa + off, *sb = rc->sb + off;
	const float eps = rc->eps;
	for (A_long x = 0; x < rc->W; ++x, s += 4, m1 += 4, m2 += 4, sa += 4, sb += 4) {
		const float a = s[0], N = m1[0];
		if (N > FS_GUIDED_WEPS) {
			const float invN = 1.f / N;
			sa[0] = a; sb[0] = a;
			for (int c = 1; c < 4; ++c) {
				const float mean = m1[c] * invN;
				float var = m2[c] * invN - mean * mean;
				if (var < 0.f) var = 0.f;              // float cancellation guard
				const float A = var / (var + eps);
				sa[c] = a * A;
				sb[c] = a * (mean * (1.f - A));
			}
		} else {
			// No coverage in the stats window (own alpha is ≤ WEPS too) —
			// contribute nothing to the aggregation, weight included.
			sa[0] = sa[1] = sa[2] = sa[3] = 0.f;
			sb[0] = sb[1] = sb[2] = sb[3] = 0.f;
		}
	}
	return PF_Err_NONE;
}

// (src, boxed sa/sb) → dst: q = meanA * p + meanB in straight color, then
// re-premultiplied by the windowed mean coverage (Gaussian-path semantics:
// output alpha == "blurred alpha", the caller renormalizes by it).
static PF_Err
FS_GuidedFinalRow(void *refconPV, A_long /*thread_indexL*/, A_long y, A_long /*iterationsL*/)
{
	const GuidedRefcon *rc = static_cast<const GuidedRefcon*>(refconPV);
	const size_t off = (size_t)y * rc->W * 4;
	const float *s = rc->src + off, *sa = rc->sa + off, *sb = rc->sb + off;
	float *dst = rc->dst + off;
	for (A_long x = 0; x < rc->W; ++x, s += 4, sa += 4, sb += 4, dst += 4) {
		const float N2 = sa[0];                        // box(a), aggregation pass
		if (N2 > FS_GUIDED_WEPS) {
			const float invN2 = 1.f / N2;
			const float abar  = N2 * rc->invCount;
			const float a     = s[0];
			const float inva  = (a > FS_GUIDED_AEPS) ? 1.f / a : 0.f;
			dst[0] = abar;
			for (int c = 1; c < 4; ++c) {
				const float p = s[c] * inva;           // straight (0 if a ~ 0)
				const float q = sa[c] * invN2 * p + sb[c] * invN2;
				dst[c] = q * abar;
			}
		} else {
			dst[0] = dst[1] = dst[2] = dst[3] = 0.f;   // caller falls back to src
		}
	}
	return PF_Err_NONE;
}

} // extern "C"

PF_Err GuidedFilterARGB(PF_InData *in_data,
						const float *src, float *dst,
						A_long W, A_long H,
						A_long rx, A_long ry, float eps)
{
	const size_t N4 = (size_t)W * H * 4;

	if (rx < 0) rx = 0;
	if (ry < 0) ry = 0;
	if (rx == 0 && ry == 0) {
		// Degenerate window (1x1): var == 0 everywhere → exact identity.
		if (dst != src) std::memcpy(dst, src, N4 * sizeof(float));
		return PF_Err_NONE;
	}
	if (eps < FS_GUIDED_EPS_MIN) eps = FS_GUIDED_EPS_MIN;

	// Scratch planes live on this call's heap only — nothing global (MFR).
	std::vector<float> s1, s2, sa, sb, tmp;
	try {
		s1.resize(N4); s2.resize(N4); sa.resize(N4); sb.resize(N4); tmp.resize(N4);
	} catch (const std::bad_alloc &) {
		return PF_Err_OUT_OF_MEMORY;
	}

	GuidedRefcon rc;
	rc.src = src;
	rc.s1 = s1.data(); rc.s2 = s2.data();
	rc.sa = sa.data(); rc.sb = sb.data();
	rc.dst = dst;
	rc.W = W;
	rc.eps = eps;
	rc.invCount = 1.f / (float)((2 * rx + 1) * (2 * ry + 1));

	BoxSumARGB(in_data, src, tmp.data(), rc.s1, W, H, rx, ry);   // box(a), box(a*p)
	mm::ForEachRow(in_data, H, &rc, FS_GuidedSquaresRow);        // s2 = [a, a*p^2]
	BoxSumARGB(in_data, rc.s2, tmp.data(), rc.s2, W, H, rx, ry); // box(a*p^2)
	mm::ForEachRow(in_data, H, &rc, FS_GuidedABRow);             // sa/sb = [a, a*A/B]
	BoxSumARGB(in_data, rc.sa, tmp.data(), rc.sa, W, H, rx, ry); // aggregate A
	BoxSumARGB(in_data, rc.sb, tmp.data(), rc.sb, W, H, rx, ry); // aggregate B
	mm::ForEachRow(in_data, H, &rc, FS_GuidedFinalRow);          // q → dst

	return PF_Err_NONE;
}

} // namespace fs
