/*  GT_Response.h — luminance→grain-amplitude response for Grain Transfer.

    Measures, from the source plate, how grain sigma varies with luminance
    (five bands across the plate's luminance range), per channel. At stamp
    time the target's local luminance looks up sigma through this curve.

    Applying in log-density space already gives the physically correct
    exposure dependence; this curve captures the source's remaining
    deviation from that baseline (stock-specific highlight/shadow
    behaviour). The Response parameter is a TRIM: 100% = full measured
    curve, 0% = flat (the source's overall sigma everywhere), lerped in
    between — resolving the double-counting question flagged in the brief
    by construction: the baseline lives in the encode, the curve only adds
    the measured deviation, and the trim can always flatten it.

    All luminances are APPLY-SPACE (GT_Colour.h Luma of encoded RGB), the
    same definition at measure and eval time. AE-free.
*/

#pragma once

namespace gt {

struct ResponseCurve {
    static const int kBands = 5;

    float lumaMid[kBands] = {};    // band centers, apply-space luma
    float sigma[3][kBands] = {};   // per-channel grain sigma at each band
    float flatSigma[3] = {};       // pixel-weighted overall sigma per channel
    bool  valid = false;
};

// Measure σ(L) from the source. rgb: apply-space planes (for luminance);
// residual: matching high-pass residual planes. Bands span the 1st–99th
// luminance percentile; per-band sigma is MAD-robust (×1.4826) so content
// edges don't inflate it; bands with too few pixels inherit their nearest
// measured neighbour.
void MeasureResponse(const float *const rgb[3], const float *const residual[3],
                     int W, int H, ResponseCurve &out);

// σ for `channel` at apply-space luminance L. Piecewise-linear between band
// centers, clamped at the ends, then lerped toward flatSigma by (1 − trim),
// trim in [0,1] (the Response param / 100).
float EvalSigma(const ResponseCurve &c, int channel, float L, float trim);

} // namespace gt
