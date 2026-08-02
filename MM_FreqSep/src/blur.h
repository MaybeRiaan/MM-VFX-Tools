/*  blur.h — separable Gaussian over interleaved ARGB float planes.

    The buffers are the mm::UnpackRow layout: W*H*4 floats, per pixel
    [A, R, G, B], with RGB PREMULTIPLIED by A (AE's native delivery). Blurring
    the premultiplied planes and later renormalizing by the blurred alpha
    (done by the caller in smart_render.cpp) IS the alpha-weighted blur the
    plan requires — the weight of each tap is kernel * alpha, so transparent
    pixels contribute no color and edges don't fringe.

    Out-of-buffer taps are treated as transparent black (skipped): beyond the
    checked-out region AE's layer is genuinely transparent, and inside a
    render tile PreRender's inflation guarantees every OUTPUT pixel has full
    kernel coverage of real data, so zero-extension is only ever "seen" by
    border pixels that the output never reads.

    Sigma semantics: Separation Radius IS sigma (px). Support radius per pass
    is ceil(3*sigma); PreRender must inflate by the SUM of the supports of
    every cascaded pass (separation + low-band smooth).
*/

#pragma once

#include "AE_Effect.h"

#include <vector>

namespace fs {

// Support radius ceil(3*sigma); 0 when sigma is too small to blur (< ~0.01).
A_long GaussianSupport(double sigma);

// Normalized sampled-Gaussian weights, size 2R+1, centered at index R, with
// the normalization sum accumulated in DOUBLE before the float cast. Exposed
// (rather than private to blur.cpp) because the GPU host must upload THESE
// exact weights — ../docs/gpu-kernel-rules.md §3.1: the GPU never rebuilds them in
// float, or the last-bit divergence turns the parity tolerance into noise.
void BuildGaussianKernel(double sigma, A_long R, std::vector<float> &k);

// Separable Gaussian of all 4 planes, sigmaX horizontal / sigmaY vertical
// (they differ under non-square downsample). src → dst, tmp is caller-owned
// scratch; all are W*H*4 floats. dst MAY alias src (cascaded blurs rely on
// it); tmp must be distinct from both. A no-op sigma pair degenerates to a
// copy. Row passes are threaded via mm::ForEachRow.
void GaussianBlurARGB(PF_InData *in_data,
					  const float *src, float *tmp, float *dst,
					  A_long W, A_long H,
					  double sigmaX, double sigmaY);

} // namespace fs
