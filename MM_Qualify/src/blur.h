/*  blur.h — separable Gaussian blurs for MM Qualify.

    Two variants over contiguous float buffers, both threaded across AE's
    workers via mm::ForEachRow (serial fallback when in_data is null — which
    is how the AE-free tests drive them):

    GaussianBlurARGB — premultiplied ARGB (A,R,G,B per pixel). Out-of-range
    taps are SKIPPED (zero-extend); the reduced kernel sum at buffer edges IS
    the premultiplied coverage falloff the caller's renormalize-by-blurred-
    alpha step expects. Used for the Denoise pre-filter (the qualification
    input): straight color afterwards = rgb_blur / a_blur, i.e. the
    alpha-weighted average that keeps layer edges from fringing into the key.

    GaussianBlurF1 — single-float plane (the matte). Out-of-range taps are
    skipped AND the result is renormalized by the effective weight sum: a
    matte has no coverage semantics, so the blur must be edge-neutral (a
    solid-white matte stays solid white at the frame edge).

    Sigma semantics match MM FreqSep: the param IS sigma at full resolution;
    support (and PreRender inflation) is ceil(3*sigma) per pass. Sigmas below
    0.01 are treated as no-op (identity guarantees depend on it).

    dst may alias src in both variants (the pipeline goes src->tmp->dst; the
    second pass never re-reads src). tmp must be a distinct buffer.

    Weights are built on the CPU in double and uploaded verbatim to the GPU
    (gpu-kernel-rules §3.1) — BuildGaussianKernel is the one builder.
*/

#pragma once

#include "AE_Effect.h"
#include <vector>

namespace qk {

A_long GaussianSupport(double sigma);

// Normalized sampled Gaussian, size 2R+1, double-accumulated. R from
// GaussianSupport.
void BuildGaussianKernel(double sigma, A_long R, std::vector<float> &k);

void GaussianBlurARGB(PF_InData *in_data,
                      const float *src, float *tmp, float *dst,
                      A_long W, A_long H, double sigmaX, double sigmaY);

void GaussianBlurF1(PF_InData *in_data,
                    const float *src, float *tmp, float *dst,
                    A_long W, A_long H, double sigmaX, double sigmaY);

} // namespace qk
