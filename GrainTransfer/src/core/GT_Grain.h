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
    float crossCorr = 0.f;            // mean cross-channel grain correlation [0,1]:
                                      // 0 = independent (chroma-heavy/digital look),
                                      // 1 = monochromatic (luma grain, "unified")
    float crossPair[3] = { 0, 0, 0 }; // per-pair correlation [0]=R-G [1]=G-B [2]=R-B,
                                      // each SIGNED, [-1,1]. The pairs carry both
                                      // the asymmetry crossCorr averages away and
                                      // the sign it cannot represent at all:
                                      // debayered digital plates are commonly
                                      // ANTI-correlated (chroma reconstruction
                                      // drives G against R/B), which is real grain
                                      // structure, not noise.
    int patchesUsed = 0;              // grain patches that survived selection
    bool valid() const { return n > 0 && patchesUsed > 0; }
};

// Rank-1 common-factor decomposition of the measured cross-channel structure.
//
//     grain_c = a_c . L + N_c
//
// with L one shared unit-variance grain field common to all three channels and
// N_c independent per channel. Because the synthesized tiles are normalized to
// unit sigma per channel (absolute amplitude is reapplied at stamp time from
// the response curve), the quantity to reproduce is the CORRELATION matrix, not
// the covariance: corr(i,j) = a_i.a_j exactly, with no phase-blend nonlinearity
// to undershoot.
//
// The spectral split is p_c = a_c^2.p_L + p_Nc with p_Nc >= 0 required, which
// is what bounds the model: the shared field can only live at frequencies where
// ALL THREE channels have power. Putting p_L at the per-bin channel minimum
// makes that bound explicit and maximal -- `overlap` is the resulting spectral
// overlap Sum_bins min_c p_c, and `achieved` is the fraction of the measured
// correlation the spectra actually permit. A coarse blue channel against a fine
// red one caps `achieved` below 1 no matter how correlated the source measured,
// and no synthesis method can beat that: two fields with different power
// spectra cannot be perfectly correlated.
struct CrossChannelModel {
    bool  useCommonFactor = false;    // false -> caller uses the scalar path
    float a[3] = { 0, 0, 0 };         // shared-factor weight per channel
    float overlap = 0.f;              // spectral overlap M, [0,1]
    float achieved = 0.f;             // realized/target correlation ratio s, [0,1]
    std::vector<float> pShared;       // p_L,  n*n, sums to 1
    std::vector<float> pIndep[3];     // p_Nc, n*n, each sums to 1 - a_c^2
};

// Solve the common-factor model for a measured spectrum. Returns
// useCommonFactor = false when the measured correlation is too weak or too
// inconsistent to factor, leaving the caller on the legacy scalar phase-blend
// path. Pure function of `spec`: same spectrum, same model.
//
// `a_c` is SIGNED. corr(i,j) = a_i.a_j, so a single negative weight flips two
// of the three pairs -- which is exactly the (+,-,-) pattern a debayered plate
// measures. A rank-1 fit therefore exists iff every pair is non-negligible and
// their product is positive: r_RG.r_GB.r_RB = (a_R.a_G.a_B)^2 >= 0. One
// negative pair, or three, has no real solution and falls back.
CrossChannelModel SolveCrossChannel(const GrainSpectrum &spec);

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
// fresh white-noise field → FFT → keep phase, impose a measured magnitude →
// inverse FFT → normalize to zero mean, unit sigma. Deterministic in `seed`.
//
// Cross-channel colour is reproduced by the COMMON-FACTOR model above: one
// shared grain field L (spectrum p_L) and three independent fields N_c
// (spectra p_Nc) are summed as a_c·L + N_c in the frequency domain, where the
// sum stays Hermitian and the inverse stays exactly real. This reproduces the
// measured correlation matrix directly — including its per-pair asymmetry
// (R–G ≠ G–B ≠ R–B), which a single scalar cannot carry.
//
// The per-channel magnitude is therefore NOT re-imposed after the sum: doing so
// would restore the exact spectrum but destroy the correlation the sum just
// bought. Each COMPONENT carries an exactly imposed magnitude and the total is
// correct in expectation, fluctuating per bin by at most
// 2·a_c·√(p_L·p_Nc) — bounded, not Rayleigh, since only the phases are random.
// That is what a finite sample of a real stochastic grain process does anyway;
// the radial spectrum, which is what reads as grain character, is unaffected.
//
// If SolveCrossChannel declines to factor the measurement, synthesis falls back
// to the legacy scalar path: a per-tile shared PHASE field blended into each
// channel by spec.crossCorr, with the measured magnitude imposed exactly.
void SynthesizeTileBank(const GrainSpectrum &spec, int count, uint64_t seed,
                        TileBank &out);

} // namespace gt
