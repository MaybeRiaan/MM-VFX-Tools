/*  blur.cpp — see blur.h. CPU reference implementation: direct separable
    convolution, rows threaded across AE's workers. Deliberately simple —
    this is the ground truth the Phase 5 GPU path and the IIR large-radius
    CPU path get validated against. No clamping anywhere (intermediates must
    stay unbounded; only the final 8/16bpc pack clamps).
*/

#define NOMINMAX
#include "blur.h"
#include "fs_common.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace fs {

// Sigmas below this blur less than a thousandth of a pixel — treat as no-op
// so radius 0 gives an exact copy (identity test depends on it).
static const double FS_SIGMA_EPS = 0.01;

A_long GaussianSupport(double sigma)
{
	if (sigma < FS_SIGMA_EPS) return 0;
	return (A_long)std::ceil(3.0 * sigma);
}

// Normalized sampled Gaussian, size 2R+1. R comes from GaussianSupport.
// Public (blur.h) since Phase 5: the GPU host uploads these exact weights.
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

// One threaded pass. horizontal=true convolves along x reading src rows;
// false convolves along y (each output row reads rows y-R..y+R of src).
// Out-of-range taps are skipped == transparent black (see blur.h). The
// kernel sums to 1 over full coverage; near buffer edges the effective sum
// drops, which is exactly the premultiplied-plane weight the caller's
// renormalization by blurred alpha expects — do NOT renormalize here.
struct PassRefcon {
	const float *src;
	float       *dst;
	A_long       W, H;
	const float *kern;   // 2R+1, centered
	A_long       R;
	bool         horizontal;
};

extern "C" {

static PF_Err
FS_BlurPassRow(void *refconPV, A_long /*thread_indexL*/, A_long y, A_long /*iterationsL*/)
{
	const PassRefcon *rc = static_cast<const PassRefcon*>(refconPV);
	const A_long W = rc->W, H = rc->H, R = rc->R;
	const float *kern = rc->kern + R;   // kern[i], i in [-R, R]
	float *dst = rc->dst + (size_t)y * W * 4;

	if (rc->horizontal) {
		const float *row = rc->src + (size_t)y * W * 4;
		for (A_long x = 0; x < W; ++x, dst += 4) {
			const A_long i0 = (x - R < 0)     ? -x         : -R;
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
					  A_long W, A_long H,
					  double sigmaX, double sigmaY)
{
	const A_long Rx = GaussianSupport(sigmaX);
	const A_long Ry = GaussianSupport(sigmaY);
	const size_t bytes = (size_t)W * H * 4 * sizeof(float);

	if (Rx == 0 && Ry == 0) {
		std::memcpy(dst, src, bytes);
		return;
	}

	// Kernels live on this stack frame only — nothing global (MFR).
	std::vector<float> kx, ky;

	if (Rx > 0 && Ry > 0) {
		// Two passes: src → tmp → dst. dst may alias src — the vertical pass
		// only reads tmp, so src is no longer needed once it runs.
		BuildGaussianKernel(sigmaX, Rx, kx);
		PassRefcon h{ src, tmp, W, H, kx.data(), Rx, true };
		fs::ForEachRow(in_data, H, &h, FS_BlurPassRow);

		BuildGaussianKernel(sigmaY, Ry, ky);
		PassRefcon v{ tmp, dst, W, H, ky.data(), Ry, false };
		fs::ForEachRow(in_data, H, &v, FS_BlurPassRow);
	} else {
		// Single pass: still route src → tmp, then copy tmp → dst, so that
		// dst == src stays safe (an in-place convolution would read
		// already-written neighbors).
		const bool horiz = (Rx > 0);
		BuildGaussianKernel(horiz ? sigmaX : sigmaY, horiz ? Rx : Ry, kx);
		PassRefcon rc{ src, tmp, W, H, kx.data(), horiz ? Rx : Ry, horiz };
		fs::ForEachRow(in_data, H, &rc, FS_BlurPassRow);
		std::memcpy(dst, tmp, bytes);
	}
}

} // namespace fs
