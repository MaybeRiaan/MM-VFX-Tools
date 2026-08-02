/*  guided.h — self-guided (guide == input) edge-aware filter over interleaved
    ARGB float planes, O(1) per pixel via box sums (He et al. guided filter).

    Same buffer contract as blur.h: fs::UnpackRow layout, W*H*4 floats, per
    pixel [A, R, G, B] with RGB PREMULTIPLIED by A. Every box mean inside the
    filter is weighted by alpha (mean = box(a*p) / box(a)), which is the same
    anti-fringing invariant the Gaussian path gets from blurring premultiplied
    planes: transparent neighbors contribute no color. Output is delivered the
    same way GaussianBlurARGB delivers it — alpha = windowed mean coverage,
    RGB = straight result re-premultiplied by that alpha — so the caller's
    renormalize-by-blurred-alpha compose consumes either method unchanged.

    Per-channel math, straight color p, window radius r per axis:
        mean = box(a*p)/box(a)          var  = box(a*p^2)/box(a) - mean^2
        A    = var / (var + eps)        B    = mean * (1 - A)
        q    = boxw(A) * p + boxw(B)    (boxw = alpha-weighted box mean again)
    Flat regions (var << eps) go to the local mean — smoothing; strong edges
    (var >> eps) keep A ~= 1 — preserved, no halo. eps is in SQUARED straight
    color units of the working range: edges whose local variance exceeds eps
    survive, texture below it is smoothed. Guide is the channel itself
    (per-channel self-guided) — simple, GPU-friendly, and the standard choice
    for retouch; a full 3x3-covariance color guide is a possible later upgrade.

    Out-of-buffer taps contribute 0 to every box sum (zero extension), same
    rationale as blur.h: beyond the checked-out region the layer is genuinely
    transparent, and PreRender inflation keeps border effects out of the
    output. Since the sums are alpha-normalized, a shrunken window changes
    only the reported coverage alpha, never the straight color.

    No clamping — intermediates stay unbounded (HDR-safe); q is a convex-ish
    combination of local values plus an identity term, never a hard clamp.

    Keep AE usage to what blur.h uses: PF types + fs::ForEachRow only, so
    tests/identity_test.cpp drives this exact code through the same shim.
*/

#pragma once

#include "AE_Effect.h"

#include <cmath>

namespace fs {

// Alpha-weight sums at or below this count as "no coverage in the window".
constexpr float FS_GUIDED_WEPS = 1e-6f;
// Per-pixel alpha at or below this treats the straight color as 0 (matches
// the compose pass's EPS renormalization guard).
constexpr float FS_GUIDED_AEPS = 1e-8f;
// eps floor: keeps var/(var+eps) defined when a flat window meets eps == 0.
constexpr float FS_GUIDED_EPS_MIN = 1e-6f;

// Read reach per axis: q aggregates A/B coefficients over a box of radius r,
// and each coefficient came from stats over another box of radius r — the
// supports CASCADE, so PreRender must inflate by 2r (cf. GaussianSupport).
A_long GuidedSupport(A_long r);

// Separation Radius (Gaussian sigma, px) → box window radius, matched by
// kernel variance so the same slider value gives the same smoothing scale on
// either Method. The smoothing extreme of the guided filter is a double box
// (triangle kernel) of variance 2*r*(r+1)/3; solving against sigma^2 gives
// r = (sqrt(1 + 6*sigma^2) - 1) / 2, rounded. 0 = identity (sub-pixel sigma
// quantizes to no-op — the box radius is integral).
inline A_long GuidedRadius(double sigma)
{
	if (sigma <= 0.0) return 0;
	return (A_long)(0.5 * (std::sqrt(1.0 + 6.0 * sigma * sigma) - 1.0) + 0.5);
}

// Guided-filter all color channels, window radius rx/ry per axis (they differ
// under non-square downsample), regularizer eps (floored at
// FS_GUIDED_EPS_MIN). src → dst, both W*H*4 floats; dst MAY alias src.
// Scratch planes are allocated internally (CPU reference tradeoff — five
// W*H*4 planes; the Phase 5 GPU path restructures this).
// Returns PF_Err_OUT_OF_MEMORY if scratch allocation fails.
PF_Err GuidedFilterARGB(PF_InData *in_data,
						const float *src, float *dst,
						A_long W, A_long H,
						A_long rx, A_long ry, float eps);

} // namespace fs
