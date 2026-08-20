/*  GT_Response.cpp — see GT_Response.h.  */

#include "GT_Response.h"
#include "GT_Colour.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace gt {

void MeasureResponse(const float *const rgb[3], const float *const residual[3],
                     int W, int H, ResponseCurve &out)
{
    out = ResponseCurve();
    const size_t N = (size_t)W * H;
    if (!N) return;

    // Binning luminance comes from the BLURRED plate, not the raw one.
    //
    // This is the difference between measuring the source and measuring
    // ourselves. Binning by raw pixel luminance sorts pixels by a value that
    // CONTAINS the grain, then measures the grain inside each bin -- so on a
    // plate whose only luminance variation IS grain (a locked grain plate, the
    // canonical Grain Source), the top bin is by definition the pixels where
    // grain landed brightest. Measured on real stock that produced a U-shaped
    // curve, ends up to 3.2x the plate's own sigma and a collapsed middle,
    // which the stamp's end-clamping then applied to every target luminance
    // outside the plate's narrow range: 1.7-2.6x too much grain.
    //
    // blur(plate) = plate - residual, both already in hand, so removing the
    // grain from the binning variable costs nothing. It is also the more
    // correct question: the curve describes how grain scales with SCENE
    // luminance, not with the grain-perturbed pixel value.
    std::vector<float> luma(N);
    for (size_t i = 0; i < N; ++i)
        luma[i] = Luma(rgb[0][i] - residual[0][i],
                       rgb[1][i] - residual[1][i],
                       rgb[2][i] - residual[2][i]);

    const size_t step = N > 65536 ? N / 65536 : 1;
    std::vector<float> sample;
    sample.reserve(N / step + 1);
    for (size_t i = 0; i < N; i += step) sample.push_back(luma[i]);
    std::sort(sample.begin(), sample.end());
    const float lo = sample[(size_t)((double)(sample.size() - 1) * 0.01)];
    const float hi = sample[(size_t)((double)(sample.size() - 1) * 0.99)];
    const float span = hi - lo;

    // Identifiability. A curve can only be measured where the plate actually
    // HAS luminance range to measure it over. Judge that against the grain
    // itself: a span of a few residual sigmas carries no recoverable
    // luminance dependence, and fitting five bands across it produces noise
    // that the stamp's end-clamping then extrapolates far outside the
    // measured range. Collapsing to one honest number is strictly better
    // than inventing a curve. Real content clears this by orders of
    // magnitude (a graded plate measures hundreds of sigmas of span); flat
    // grain plates do not clear it at all, which is the correct verdict.
    // MAD-robust, exactly as the per-band sigma below: a plain RMS over the
    // whole plate measures CONTENT EDGES, not grain, and would judge a
    // perfectly good graded plate unidentifiable.
    float residSigma = 0.f;
    {
        std::vector<float> mag;
        mag.reserve(3 * (N / 7 + 1));
        for (int c = 0; c < 3; ++c)
            for (size_t i = 0; i < N; i += 7)
                mag.push_back(std::fabs(residual[c][i]));
        if (!mag.empty()) {
            std::nth_element(mag.begin(), mag.begin() + mag.size() / 2, mag.end());
            residSigma = mag[mag.size() / 2] * 1.4826f;
        }
    }
    const float kMinSpanSigmas = 10.0f;

    if (span <= 1e-6f || span < kMinSpanSigmas * residSigma) {
        // No usable luminance structure; the curve collapses to the overall
        // sigma, which is exactly right for a flat sample.
        double acc[3] = { 0, 0, 0 };
        for (int c = 0; c < 3; ++c) {
            for (size_t i = 0; i < N; ++i)
                acc[c] += (double)residual[c][i] * residual[c][i];
            out.flatSigma[c] = (float)std::sqrt(acc[c] / (double)N);
        }
        const float step5 = span > 1e-6f ? span / (float)ResponseCurve::kBands : 0.2f;
        for (int b = 0; b < ResponseCurve::kBands; ++b) {
            out.lumaMid[b] = lo + ((float)b + 0.5f) * step5;
            for (int c = 0; c < 3; ++c) out.sigma[c][b] = out.flatSigma[c];
        }
        out.valid = true;
        return;
    }

    const int K = ResponseCurve::kBands;
    const float bandW = span / (float)K;

    // Bucket pixel indices by luminance band (subsampled for the MAD sort's
    // sake on big plates — 6K needs no more than ~100k samples per band).
    std::vector<std::vector<float>> bandAbs[3];
    for (int c = 0; c < 3; ++c) bandAbs[c].assign((size_t)K, {});
    std::vector<size_t> bandCount((size_t)K, 0);

    const size_t pixStep = N > 4000000 ? N / 4000000 : 1;
    for (size_t i = 0; i < N; i += pixStep) {
        int b = (int)((luma[i] - lo) / bandW);
        if (b < 0) b = 0; else if (b >= K) b = K - 1;
        ++bandCount[(size_t)b];
        for (int c = 0; c < 3; ++c)
            bandAbs[c][(size_t)b].push_back(std::fabs(residual[c][i]));
    }

    // Per-band robust sigma: median(|residual|) × 1.4826 (MAD about an
    // assumed zero median — the residual is zero-mean by construction).
    bool measured[ResponseCurve::kBands] = {};
    const size_t minPix = 500;
    for (int b = 0; b < K; ++b) {
        out.lumaMid[b] = lo + ((float)b + 0.5f) * bandW;
        if (bandCount[(size_t)b] < minPix) continue;
        measured[b] = true;
        for (int c = 0; c < 3; ++c) {
            std::vector<float> &v = bandAbs[c][(size_t)b];
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            out.sigma[c][b] = v[v.size() / 2] * 1.4826f;
        }
    }

    // Unmeasured bands inherit the nearest measured neighbour.
    for (int b = 0; b < K; ++b) {
        if (measured[b]) continue;
        int best = -1, bestDist = K + 1;
        for (int m = 0; m < K; ++m) {
            if (!measured[m]) continue;
            const int d = std::abs(m - b);
            if (d < bestDist) { bestDist = d; best = m; }
        }
        if (best < 0) return;   // nothing measured at all → invalid
        for (int c = 0; c < 3; ++c) out.sigma[c][b] = out.sigma[c][best];
    }

    // Flat sigma: pixel-weighted average of band sigmas.
    size_t total = 0;
    for (int b = 0; b < K; ++b) total += bandCount[(size_t)b];
    for (int c = 0; c < 3; ++c) {
        double acc = 0.0;
        for (int b = 0; b < K; ++b)
            acc += (double)out.sigma[c][b] * (double)bandCount[(size_t)b];
        out.flatSigma[c] = total ? (float)(acc / (double)total) : 0.f;
    }

    out.valid = true;
}

GT_Resp CoreResp(const ResponseCurve &c)
{
    const int K = ResponseCurve::kBands;
    GT_Resp r;
    for (int b = 0; b < K; ++b) r.mid[b] = c.lumaMid[b];
    for (int ch = 0; ch < 3; ++ch) {
        for (int b = 0; b < K; ++b) r.sig[ch * K + b] = c.sigma[ch][b];
        r.flat[ch] = c.flatSigma[ch];
    }
    return r;
}

void PackResponse(const ResponseCurve &c, float *out23)
{
    const GT_Resp r = CoreResp(c);
    const int K = ResponseCurve::kBands;
    int i = 0;
    for (int b = 0; b < K; ++b)      out23[i++] = r.mid[b];
    for (int s = 0; s < 3 * K; ++s)  out23[i++] = r.sig[s];
    for (int ch = 0; ch < 3; ++ch)   out23[i++] = r.flat[ch];
}

// The interpolation itself lives in gt_core.h so the kernels compile the same
// text. This is the ResponseCurve-shaped door onto it.
float EvalSigma(const ResponseCurve &c, int channel, float L, float trim)
{
    return GT_EvalSigma(CoreResp(c), channel, L, trim);
}

} // namespace gt
