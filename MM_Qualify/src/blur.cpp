/*  blur.cpp — see blur.h. CPU reference implementation: direct separable
    convolution, rows threaded across AE's workers. Deliberately simple —
    this is the ground truth the GPU path is validated against. No clamping
    anywhere (intermediates must stay unbounded).
*/

#define NOMINMAX
#include "blur.h"
#include "common/MM_Common.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace qk {

// Sigmas below this blur less than a thousandth of a pixel — treat as no-op
// so radius 0 gives an exact copy (identity behavior depends on it).
static const double QK_SIGMA_EPS = 0.01;

A_long GaussianSupport(double sigma)
{
	if (sigma < QK_SIGMA_EPS) return 0;
	return (A_long)std::ceil(3.0 * sigma);
}

void BuildGaussianKernel(double sigma, A_long R, std::vector<float> &k)
{
	k.resize((size_t)(2 * R + 1));
	double sum = 0.0;
	const double inv2s2 = 1.0 / (2.0 * sigma * sigma);
	for (A_long i = -R; i <= R; ++i) {
		const double w = std::exp(-(double)(i * i) * inv2s2);
		k[(size_t)(i + R)] = (float)w;
		sum += w;
	}
	const float inv = (float)(1.0 / sum);
	for (float &w : k) w *= inv;
}

// ---------------------------------------------------------------------------
// ARGB pass (premultiplied; taps skipped, NO renormalization — see blur.h).
// ---------------------------------------------------------------------------
struct PassRefconARGB {
	const float *src;
	float       *dst;
	A_long       W, H;
	const float *kern;   // 2R+1, centered
	A_long       R;
	bool         horizontal;
};

extern "C" {

static PF_Err
QK_BlurPassRowARGB(void *refconPV, A_long /*thread*/, A_long y, A_long /*n*/)
{
	const PassRefconARGB *rc = static_cast<const PassRefconARGB*>(refconPV);
	const A_long W = rc->W, H = rc->H, R = rc->R;
	const float *kern = rc->kern + R;
	float *dst = rc->dst + (size_t)y * W * 4;

	if (rc->horizontal) {
		const float *row = rc->src + (size_t)y * W * 4;
		for (A_long x = 0; x < W; ++x, dst += 4) {
			const A_long i0 = (x - R < 0)     ? -x          : -R;
			const A_long i1 = (x + R > W - 1) ? (W - 1 - x) :  R;
			float a = 0.f, r = 0.f, g = 0.f, b = 0.f;
			const float *p = row + (size_t)(x + i0) * 4;
			for (A_long i = i0; i <= i1; ++i, p += 4) {
				const float w = kern[i];
				a += w * p[0]; r += w * p[1]; g += w * p[2]; b += w * p[3];
			}
			dst[0] = a; dst[1] = r; dst[2] = g; dst[3] = b;
		}
	} else {
		const A_long i0 = (y - R < 0)     ? -y          : -R;
		const A_long i1 = (y + R > H - 1) ? (H - 1 - y) :  R;
		for (A_long x = 0; x < W; ++x, dst += 4) {
			float a = 0.f, r = 0.f, g = 0.f, b = 0.f;
			const float *p = rc->src + ((size_t)(y + i0) * W + x) * 4;
			for (A_long i = i0; i <= i1; ++i, p += (size_t)W * 4) {
				const float w = kern[i];
				a += w * p[0]; r += w * p[1]; g += w * p[2]; b += w * p[3];
			}
			dst[0] = a; dst[1] = r; dst[2] = g; dst[3] = b;
		}
	}
	return PF_Err_NONE;
}

} // extern "C"

void GaussianBlurARGB(PF_InData *in_data,
					  const float *src, float *tmp, float *dst,
					  A_long W, A_long H, double sigmaX, double sigmaY)
{
	const A_long Rx = GaussianSupport(sigmaX);
	const A_long Ry = GaussianSupport(sigmaY);
	const size_t bytes = (size_t)W * H * 4 * sizeof(float);

	if (Rx == 0 && Ry == 0) {
		if (dst != src) std::memcpy(dst, src, bytes);
		return;
	}

	std::vector<float> kx, ky;   // stack-scoped only (MFR)

	if (Rx > 0 && Ry > 0) {
		BuildGaussianKernel(sigmaX, Rx, kx);
		PassRefconARGB h{ src, tmp, W, H, kx.data(), Rx, true };
		mm::ForEachRow(in_data, H, &h, QK_BlurPassRowARGB);

		BuildGaussianKernel(sigmaY, Ry, ky);
		PassRefconARGB v{ tmp, dst, W, H, ky.data(), Ry, false };
		mm::ForEachRow(in_data, H, &v, QK_BlurPassRowARGB);
	} else {
		// Single pass via tmp so dst == src stays safe.
		const bool horiz = (Rx > 0);
		BuildGaussianKernel(horiz ? sigmaX : sigmaY, horiz ? Rx : Ry, kx);
		PassRefconARGB rc{ src, tmp, W, H, kx.data(), horiz ? Rx : Ry, horiz };
		mm::ForEachRow(in_data, H, &rc, QK_BlurPassRowARGB);
		std::memcpy(dst, tmp, bytes);
	}
}

// ---------------------------------------------------------------------------
// Single-float pass (matte; taps skipped AND renormalized — edge-neutral).
// ---------------------------------------------------------------------------
struct PassRefconF1 {
	const float *src;
	float       *dst;
	A_long       W, H;
	const float *kern;
	A_long       R;
	bool         horizontal;
};

extern "C" {

static PF_Err
QK_BlurPassRowF1(void *refconPV, A_long /*thread*/, A_long y, A_long /*n*/)
{
	const PassRefconF1 *rc = static_cast<const PassRefconF1*>(refconPV);
	const A_long W = rc->W, H = rc->H, R = rc->R;
	const float *kern = rc->kern + R;
	float *dst = rc->dst + (size_t)y * W;

	if (rc->horizontal) {
		const float *row = rc->src + (size_t)y * W;
		for (A_long x = 0; x < W; ++x) {
			const A_long i0 = (x - R < 0)     ? -x          : -R;
			const A_long i1 = (x + R > W - 1) ? (W - 1 - x) :  R;
			float s = 0.f, ws = 0.f;
			const float *p = row + (size_t)(x + i0);
			for (A_long i = i0; i <= i1; ++i, ++p) {
				const float w = kern[i];
				s += w * (*p); ws += w;
			}
			dst[x] = (ws > 0.f) ? s / ws : 0.f;
		}
	} else {
		const A_long i0 = (y - R < 0)     ? -y          : -R;
		const A_long i1 = (y + R > H - 1) ? (H - 1 - y) :  R;
		for (A_long x = 0; x < W; ++x) {
			float s = 0.f, ws = 0.f;
			const float *p = rc->src + (size_t)(y + i0) * W + x;
			for (A_long i = i0; i <= i1; ++i, p += (size_t)W) {
				const float w = kern[i];
				s += w * (*p); ws += w;
			}
			dst[x] = (ws > 0.f) ? s / ws : 0.f;
		}
	}
	return PF_Err_NONE;
}

} // extern "C"

void GaussianBlurF1(PF_InData *in_data,
					const float *src, float *tmp, float *dst,
					A_long W, A_long H, double sigmaX, double sigmaY)
{
	const A_long Rx = GaussianSupport(sigmaX);
	const A_long Ry = GaussianSupport(sigmaY);
	const size_t bytes = (size_t)W * H * sizeof(float);

	if (Rx == 0 && Ry == 0) {
		if (dst != src) std::memcpy(dst, src, bytes);
		return;
	}

	std::vector<float> kx, ky;

	if (Rx > 0 && Ry > 0) {
		BuildGaussianKernel(sigmaX, Rx, kx);
		PassRefconF1 h{ src, tmp, W, H, kx.data(), Rx, true };
		mm::ForEachRow(in_data, H, &h, QK_BlurPassRowF1);

		BuildGaussianKernel(sigmaY, Ry, ky);
		PassRefconF1 v{ tmp, dst, W, H, ky.data(), Ry, false };
		mm::ForEachRow(in_data, H, &v, QK_BlurPassRowF1);
	} else {
		const bool horiz = (Rx > 0);
		BuildGaussianKernel(horiz ? sigmaX : sigmaY, horiz ? Rx : Ry, kx);
		PassRefconF1 rc{ src, tmp, W, H, kx.data(), horiz ? Rx : Ry, horiz };
		mm::ForEachRow(in_data, H, &rc, QK_BlurPassRowF1);
		std::memcpy(dst, tmp, bytes);
	}
}

} // namespace qk
