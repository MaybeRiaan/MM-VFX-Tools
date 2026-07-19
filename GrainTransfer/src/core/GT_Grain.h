/*  GT_Grain.h — grain analysis (measure once) and RPN tile synthesis
    (precompute once) for Grain Transfer.

    Method: random-phase noise (RPN). Content position lives in the Fourier
    phase; grain character lives in the Fourier magnitude. We keep the real
    measured magnitude spectrum of the source's high-frequency residual and
    replace the phase with the phase of a fresh white-noise field. Because a
    real noise field's spectrum is Hermitian by construction, the phase-
    randomized spectrum stays Hermitian and the inverse FFT is exactly real —
    no explicit conjugate-symmetry bookkeeping needed. The result is a noise
    field with the exact grain character of the plate, all content structure
    destroyed (ghost-free by construction), and periodic (seam-free tiling by
    construction).

    All inputs are APPLY-SPACE planes (see GT_Colour.h) — analysis and
    application must share one space. AE-free: unit-testable headlessly.
*/

#pragma once

#include <cstdint>
#include <vector>

namespace gt {

// Measured grain character of a source plate.
struct GrainSpectrum {
    int n = 0;                        // FFT/tile size (power of two)
    std::vector<float> power[3];      // n*n average |F|² per RGB channel, DC zeroed
    float sigma[3] = { 0, 0, 0 };     // absolute residual sigma per channel (apply space)
    int patchesUsed = 0;              // grain patches that survived selection
    bool valid() const { return n > 0 && patchesUsed > 0; }
};

// Bank of seamless zero-mean unit-sigma grain tiles.
struct TileBank {
    int n = 0;                        // tile size
    int count = 0;                    // tiles in the bank
    std::vector<float> tiles;         // count × 3 × n × n, [tile][channel][y][x]

    const float *tile(int t, int c) const
    {
        return tiles.data() + ((size_t)t * 3 + c) * ((size_t)n * n);
    }
    bool valid() const { return n > 0 && count > 0; }
};

// Separable Gaussian blur of one plane, edge-clamped. Support = ceil(3σ).
void GaussianBlur(const float *src, float *dst, int W, int H, float sigma);

// residual = plane − blur(plane): the high-frequency band carrying grain
// plus content edges (the latter are suppressed by patch selection and
// phase randomization downstream).
void HighPassResidual(const float *plane, float *residual, int W, int H,
                      float blurSigma);

// Analyze source planes (apply-space, W×H, per channel) into an averaged
// patch power spectrum:
//   - high-pass residual per channel;
//   - non-overlapping n×n patches; a patch's combined residual sigma far
//     above the median marks content-edge contamination, far below marks
//     grainless clipped areas — both rejected;
//   - per kept patch and channel: subtract mean, 2D Hann window, FFT,
//     accumulate |F|², unbias by the window's power;
//   - DC forced to zero.
// Returns false (out.valid() == false) if the plate is smaller than n×n.
bool MeasureGrainSpectrum(const float *const rgb[3], int W, int H,
                          int n, float blurSigma, GrainSpectrum &out);

// Same, but from residuals the caller already computed (avoids re-blurring
// when the response measurement needs the same residual planes).
bool MeasureGrainSpectrumRes(const float *const res[3], int W, int H,
                             int n, GrainSpectrum &out);

// Bake `count` RPN tiles from the measured spectrum. Per tile and channel:
// fresh white-noise field → FFT → keep phase, impose measured magnitude →
// inverse FFT → normalize to zero mean, unit sigma. Channels use independent
// phases (film grain is decorrelated across emulsion layers; this is what
// the sampled per-channel spectra give for free). Deterministic in `seed`.
void SynthesizeTileBank(const GrainSpectrum &spec, int count, uint64_t seed,
                        TileBank &out);

} // namespace gt
