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

#include "../gt_cpu_prologue.h"
#include "../gt_core.h"

namespace gt {

struct ResponseCurve {
    static const int kBands = GT_RESP_BANDS;

    float lumaMid[kBands] = {};    // band centers, apply-space luma
    float sigma[3][kBands] = {};   // per-channel grain sigma at each band
    float flatSigma[3] = {};       // pixel-weighted overall sigma per channel
    bool  valid = false;
};

// Measure σ(L) from the source. rgb: apply-space planes; residual: matching
// high-pass residual planes. Bands span the 1st–99th percentile of the
// BLURRED luminance (rgb − residual, so the binning variable carries no
// grain — binning by raw luminance sorts pixels by the very quantity being
// measured, which on a flat grain plate fabricates a U-shaped curve); per-band
// sigma is MAD-robust (×1.4826) so content edges don't inflate it; bands with
// too few pixels inherit their nearest measured neighbour. If the plate's
// luminance span is under ~10 residual sigmas the curve is not identifiable
// at all and collapses to flatSigma rather than extrapolating noise.
void MeasureResponse(const float *const rgb[3], const float *const residual[3],
                     int W, int H, ResponseCurve &out);

// σ for `channel` at apply-space luminance L. Piecewise-linear between band
// centers, clamped at the ends, then lerped toward flatSigma by (1 − trim),
// trim in [0,1] (the Response param / 100). Forwards to gt_core.h's
// GT_EvalSigma — the body the GPU kernels compile.
float EvalSigma(const ResponseCurve &c, int channel, float L, float trim);

// The curve as gt_core.h's flat struct: the form the shared math takes.
GT_Resp CoreResp(const ResponseCurve &c);

// Flatten into the 23-float block a kernel's GT_RESP_DECL declares — 5 band
// mids, then 15 sigmas in CHANNEL-MAJOR order (c*5 + b), then 3 flat sigmas.
// This ORDER is the binding contract with GT_RESP_LOAD; both derive from
// gt_core.h and gpu/cuda/gt_kernel_args.h asserts they stay contiguous.
void PackResponse(const ResponseCurve &c, float *out23);

} // namespace gt
