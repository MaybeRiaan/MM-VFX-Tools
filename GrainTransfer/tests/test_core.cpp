/*  test_core.cpp — headless validation gates for Grain Transfer's DSP core.
    No AE. Exit 0 = all gates passed.

    Stage 0 gate: FFT round-trip reconstructs a test image within float
                  tolerance.
    Stage 1 gates (RPN, make-or-break):
      (a) synthesized tile matches the source grain's character — radial
          power spectrum and per-channel sigma within tolerance;
      (b) NO ghosting — peak normalized cross-correlation between the tile
          and the content structure is at noise level, while the naive
          residual patch (the method we rejected) correlates strongly;
      (c) NO seams — wrap-boundary gradient statistics match the interior;
      (+) Gaussianity report: skew/kurtosis of source grain vs tile.
    Stage 2 gates (response):
      recovered sigma(L) matches the ground-truth law; grain stamped onto a
      gradient sits IN it — measured output sigma follows the law; the log
      (scene-linear) path round-trips HDR and negatives NaN-free.
    Extras: stamp determinism per (seed, frame); frame-to-frame variation;
      6K single-thread stamp benchmark (reported, generous ceiling).

    Visual outputs land in %GT_TEST_OUT% (PNG, gain-boosted for grain) for
    human review — the quantitative gates do not depend on them.
*/

#define _CRT_SECURE_NO_WARNINGS
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "GT_Colour.h"
#include "GT_FFT.h"
#include "GT_Grain.h"
#include "GT_Random.h"
#include "GT_Response.h"
#include "GT_Stamp.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace gt;

static int g_failures = 0;

#define CHECK(cond, ...) do { \
    if (cond) { std::printf("  PASS: " __VA_ARGS__); std::printf("\n"); } \
    else { ++g_failures; std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); } \
} while (0)

static std::string OutDir()
{
    const char *d = std::getenv("GT_TEST_OUT");
    return d ? std::string(d) : std::string(".");
}

// Grey PNG of a plane, mapped v → 0.5 + v*gain (grain visualization).
static void DumpPlane(const char *name, const float *p, int W, int H, float gain)
{
    std::vector<unsigned char> img((size_t)W * H);
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        float v = 0.5f + p[i] * gain;
        if (v < 0.f) v = 0.f; else if (v > 1.f) v = 1.f;
        img[i] = (unsigned char)(v * 255.f + 0.5f);
    }
    const std::string path = OutDir() + "/" + name;
    stbi_write_png(path.c_str(), W, H, 1, img.data(), W);
}

// ---------------------------------------------------------------------------
// Synthetic ground truth. "Film-like" grain: white Gaussian noise convolved
// with a small Gaussian (correlated grain), per channel, known sigma.
// ---------------------------------------------------------------------------
static void MakeGrainField(int W, int H, float sigma, float corrSigma,
                           uint64_t seed, std::vector<float> &out)
{
    out.resize((size_t)W * H);
    Rng rng(seed);
    std::vector<float> white((size_t)W * H);
    for (size_t i = 0; i < white.size(); i += 2) {
        float u1 = rng.nextFloat(); if (u1 < 1e-12f) u1 = 1e-12f;
        const float u2 = rng.nextFloat();
        const float r = std::sqrt(-2.f * std::log(u1));
        const float a = 6.28318530717959f * u2;
        white[i] = r * std::cos(a);
        if (i + 1 < white.size()) white[i + 1] = r * std::sin(a);
    }
    GaussianBlur(white.data(), out.data(), W, H, corrSigma);
    // Renormalize to the requested sigma.
    double var = 0.0;
    for (float v : out) var += (double)v * v;
    var /= (double)out.size();
    const float s = sigma / (float)std::sqrt(var);
    for (float &v : out) v *= s;
}

// Content image: smooth gradient + hard-edged shapes (the ghost bait).
static void MakeContent(int W, int H, std::vector<float> &out)
{
    out.resize((size_t)W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float v = 0.15f + 0.7f * (float)x / (float)W;
            const float cx = (float)x - W * 0.35f, cy = (float)y - H * 0.4f;
            if (cx * cx + cy * cy < (W * 0.12f) * (W * 0.12f)) v = 0.85f;
            if (x > W * 0.6f && x < W * 0.78f && y > H * 0.55f && y < H * 0.85f)
                v = 0.08f;
            out[(size_t)y * W + x] = v;
        }
}

// Radially averaged power spectrum (n×n power grid → n/2 bins).
static void RadialAverage(const float *power, int n, std::vector<double> &bins)
{
    const int nb = n / 2;
    bins.assign((size_t)nb, 0.0);
    std::vector<double> cnt((size_t)nb, 0.0);
    for (int v = 0; v < n; ++v) {
        const int fv = v <= n / 2 ? v : v - n;
        for (int u = 0; u < n; ++u) {
            const int fu = u <= n / 2 ? u : u - n;
            const int r = (int)std::round(std::sqrt((double)fu * fu + (double)fv * fv));
            if (r < 1 || r >= nb) continue;   // skip DC, corner
            bins[(size_t)r] += power[(size_t)v * n + u];
            cnt[(size_t)r] += 1.0;
        }
    }
    for (int i = 1; i < nb; ++i)
        if (cnt[(size_t)i] > 0) bins[(size_t)i] /= cnt[(size_t)i];
}

// Peak |normalized cross-correlation| over all cyclic shifts, via FFT.
static float PeakNCC(const float *a, const float *b, int n)
{
    Fft2D fft(n);
    const size_t N = (size_t)n * n;
    std::vector<kiss_fft_cpx> fa(N), fb(N);
    fft.forward(a, fa.data());
    fft.forward(b, fb.data());
    std::vector<kiss_fft_cpx> prod(N);
    for (size_t i = 0; i < N; ++i) {
        // fa · conj(fb)
        prod[i].r = fa[i].r * fb[i].r + fa[i].i * fb[i].i;
        prod[i].i = fa[i].i * fb[i].r - fa[i].r * fb[i].i;
    }
    std::vector<float> cc(N);
    fft.inverse(prod.data(), cc.data());

    double sa = 0.0, sb = 0.0;
    for (size_t i = 0; i < N; ++i) { sa += (double)a[i] * a[i]; sb += (double)b[i] * b[i]; }
    const double norm = std::sqrt(sa * sb);
    if (norm < 1e-20) return 0.f;
    // inverse() includes the 1/N of the IDFT, which lands cc[] exactly on
    // the raw correlation sum Σ a[n]·b[n−s].
    float peak = 0.f;
    for (size_t i = 0; i < N; ++i) {
        const float v = std::fabs(cc[i] / (float)norm);
        if (v > peak) peak = v;
    }
    return peak;
}

static void MomentStats(const float *p, size_t N, double &skew, double &exKurt)
{
    double m = 0.0;
    for (size_t i = 0; i < N; ++i) m += p[i];
    m /= (double)N;
    double m2 = 0.0, m3 = 0.0, m4 = 0.0;
    for (size_t i = 0; i < N; ++i) {
        const double d = p[i] - m;
        m2 += d * d; m3 += d * d * d; m4 += d * d * d * d;
    }
    m2 /= (double)N; m3 /= (double)N; m4 /= (double)N;
    skew = m3 / std::pow(m2, 1.5);
    exKurt = m4 / (m2 * m2) - 3.0;
}

// Realized pairwise cross-channel correlation of a baked tile bank, pooled
// over every tile: [0]=R-G, [1]=G-B, [2]=R-B. This is the quantity the
// common-factor model exists to make equal the measured target.
static void BankCorrelation(const TileBank &bank, double outPair[3])
{
    const size_t N = (size_t)bank.n * bank.n;
    double sq[3] = { 0, 0, 0 }, cross[3] = { 0, 0, 0 };
    for (int t = 0; t < bank.count; ++t) {
        const float *ch[3] = { bank.tile(t, 0), bank.tile(t, 1), bank.tile(t, 2) };
        double mean[3] = { 0, 0, 0 };
        for (int c = 0; c < 3; ++c) {
            for (size_t i = 0; i < N; ++i) mean[c] += ch[c][i];
            mean[c] /= (double)N;
        }
        for (size_t i = 0; i < N; ++i) {
            const double r = ch[0][i] - mean[0];
            const double g = ch[1][i] - mean[1];
            const double b = ch[2][i] - mean[2];
            sq[0] += r * r; sq[1] += g * g; sq[2] += b * b;
            cross[0] += r * g; cross[1] += g * b; cross[2] += r * b;
        }
    }
    outPair[0] = cross[0] / (std::sqrt(sq[0] * sq[1]) + 1e-30);
    outPair[1] = cross[1] / (std::sqrt(sq[1] * sq[2]) + 1e-30);
    outPair[2] = cross[2] / (std::sqrt(sq[0] * sq[2]) + 1e-30);
}

// ---------------------------------------------------------------------------
int main()
{
    std::printf("=== Grain Transfer — headless core gates ===\n");

    // ------------------------------------------------------------------ S0
    std::printf("\n[Stage 0] FFT round-trip\n");
    {
        const int n = 256;
        const size_t N = (size_t)n * n;
        std::vector<float> src(N), back(N);
        Rng rng(42);
        for (auto &v : src) v = rng.nextFloat() * 2.f - 1.f;
        std::vector<kiss_fft_cpx> spec(N);
        Fft2D fft(n);
        fft.forward(src.data(), spec.data());
        fft.inverse(spec.data(), back.data());
        float maxErr = 0.f;
        for (size_t i = 0; i < N; ++i)
            maxErr = std::max(maxErr, std::fabs(src[i] - back[i]));
        CHECK(maxErr < 1e-4f, "round-trip max error %.2e < 1e-4", maxErr);
    }

    // ------------------------------------------------------------------ S1
    std::printf("\n[Stage 1] RPN — spectrum, ghosting, seams (make-or-break)\n");
    const int W = 1536, H = 1024, TILE = 256;
    const float blurSigma = 2.0f;
    const float trueSigma[3] = { 0.050f, 0.040f, 0.060f };
    const float corrSigma = 0.7f;

    std::vector<float> content;
    MakeContent(W, H, content);

    std::vector<float> grain[3], plate[3];
    for (int c = 0; c < 3; ++c) {
        MakeGrainField(W, H, trueSigma[c], corrSigma, 1000 + c, grain[c]);
        plate[c].resize((size_t)W * H);
        for (size_t i = 0; i < plate[c].size(); ++i)
            plate[c][i] = content[i] + grain[c][i];
    }

    GrainSpectrum spec;
    {
        const float *planes[3] = { plate[0].data(), plate[1].data(), plate[2].data() };
        const bool ok = MeasureGrainSpectrum(planes, W, H, TILE, blurSigma, spec);
        CHECK(ok && spec.valid(), "spectrum measured (%d patches used of %d)",
              spec.patchesUsed, (W / TILE) * (H / TILE));
    }

    TileBank bank;
    SynthesizeTileBank(spec, 8, /*seed*/ 7, bank);
    CHECK(bank.valid() && bank.count == 8, "tile bank baked (8 tiles, %dpx)", bank.n);

    // (a) character match — radial spectrum of tile vs ground-truth grain.
    // Ground truth is the HIGH-PASSED grain: frequency separation transfers
    // the residual band by definition — grain's lowest frequencies are
    // indistinguishable from content and are deliberately excluded (the
    // target keeps its own lows). Quantify that band limitation up front:
    {
        std::vector<float> grainHP((size_t)W * H);
        HighPassResidual(grain[1].data(), grainHP.data(), W, H, blurSigma);
        double rawE = 0.0, hpE = 0.0;
        for (size_t i = 0; i < grainHP.size(); ++i) {
            rawE += (double)grain[1][i] * grain[1][i];
            hpE += (double)grainHP[i] * grainHP[i];
        }
        std::printf("  info: residual band carries %.1f%% of raw grain energy "
                    "(inherent to frequency separation)\n", 100.0 * hpE / rawE);

        Fft2D fft(TILE);
        const size_t N = (size_t)TILE * TILE;
        std::vector<float> patch(N);
        std::vector<kiss_fft_cpx> fs(N);
        std::vector<float> truePow(N, 0.f), tilePow(N, 0.f);

        int patches = 0;
        for (int py = 0; py + TILE <= H; py += TILE)
            for (int px = 0; px + TILE <= W; px += TILE) {
                for (int y = 0; y < TILE; ++y)
                    std::memcpy(&patch[(size_t)y * TILE],
                                &grainHP[(size_t)(py + y) * W + px],
                                TILE * sizeof(float));
                fft.forward(patch.data(), fs.data());
                for (size_t i = 0; i < N; ++i)
                    truePow[i] += fs[i].r * fs[i].r + fs[i].i * fs[i].i;
                ++patches;
            }
        for (auto &v : truePow) v /= (float)patches;

        // Tile spectrum (average over the bank, channel G) — scale-free
        // comparison via normalized radial curves (tiles are unit sigma).
        for (int t = 0; t < bank.count; ++t) {
            fft.forward(bank.tile(t, 1), fs.data());
            for (size_t i = 0; i < N; ++i)
                tilePow[i] += fs[i].r * fs[i].r + fs[i].i * fs[i].i;
        }
        for (auto &v : tilePow) v /= (float)bank.count;

        std::vector<double> rt, rb;
        RadialAverage(truePow.data(), TILE, rt);
        RadialAverage(tilePow.data(), TILE, rb);
        double st = 0.0, sb = 0.0;
        for (size_t i = 1; i < rt.size(); ++i) { st += rt[i]; sb += rb[i]; }
        double err = 0.0, wsum = 0.0;
        for (size_t i = 1; i < rt.size(); ++i) {
            const double a = rt[i] / st, b = rb[i] / sb;
            err += std::fabs(a - b) * rt[i];   // energy-weighted
            wsum += (a > b ? a : b) * rt[i];
        }
        const double relErr = err / wsum;
        CHECK(relErr < 0.15, "radial spectrum match, energy-weighted rel err %.3f < 0.15", relErr);

        // Per-channel sigma recovery (MAD-free, spectrum-side).
        for (int c = 0; c < 3; ++c) {
            const double rel = std::fabs(spec.sigma[c] - trueSigma[c]) / trueSigma[c];
            CHECK(rel < 0.15, "channel %d sigma %.4f vs true %.4f (rel %.3f < 0.15)",
                  c, spec.sigma[c], trueSigma[c], rel);
        }
    }

    // (b) ghosting — tile must NOT correlate with content structure.
    {
        // Content-only residual (the ghost source), one strong patch: cover
        // the circle edge region.
        std::vector<float> contentRes((size_t)W * H);
        HighPassResidual(content.data(), contentRes.data(), W, H, blurSigma);

        const size_t N = (size_t)TILE * TILE;
        std::vector<float> ghostBait(N);
        // Patch centered on the dark rectangle's corner — two hard edges
        // cross it, the strongest content structure in the frame.
        const int gx = (int)(W * 0.6f) - TILE / 2, gy = (int)(H * 0.55f) - TILE / 2;
        for (int y = 0; y < TILE; ++y)
            std::memcpy(&ghostBait[(size_t)y * TILE],
                        &contentRes[(size_t)(gy + y) * W + gx], TILE * sizeof(float));

        // Our tile vs content structure — must be noise-level.
        const float nccTile = PeakNCC(bank.tile(0, 1), ghostBait.data(), TILE);

        // Naive approach (re-tiling the raw residual — what we rejected):
        // plate residual patch at the same location vs content structure.
        std::vector<float> plateRes((size_t)W * H), naive(N);
        HighPassResidual(plate[1].data(), plateRes.data(), W, H, blurSigma);
        for (int y = 0; y < TILE; ++y)
            std::memcpy(&naive[(size_t)y * TILE],
                        &plateRes[(size_t)(gy + y) * W + gx], TILE * sizeof(float));
        const float nccNaive = PeakNCC(naive.data(), ghostBait.data(), TILE);

        CHECK(nccTile < 0.08f, "tile vs content NCC %.4f < 0.08 (no ghost)", nccTile);
        CHECK(nccNaive > 0.3f, "naive residual patch NCC %.3f > 0.3 (control: ghost is real)", nccNaive);
    }

    // (c) seams — wrap-boundary gradients match interior gradients.
    {
        const float *t = bank.tile(0, 1);
        double seam = 0.0, interior = 0.0;
        size_t sc = 0, ic = 0;
        for (int y = 0; y < TILE; ++y) {
            seam += std::fabs(t[(size_t)y * TILE + TILE - 1] - t[(size_t)y * TILE]);
            ++sc;
            for (int x = 0; x < TILE - 1; ++x) {
                interior += std::fabs(t[(size_t)y * TILE + x] - t[(size_t)y * TILE + x + 1]);
                ++ic;
            }
        }
        for (int x = 0; x < TILE; ++x) {
            seam += std::fabs(t[(size_t)(TILE - 1) * TILE + x] - t[(size_t)x]);
            ++sc;
        }
        for (int x = 0; x < TILE; ++x)
            for (int y = 0; y < TILE - 1; ++y) {
                interior += std::fabs(t[(size_t)y * TILE + x] - t[(size_t)(y + 1) * TILE + x]);
                ++ic;
            }
        const double ratio = (seam / (double)sc) / (interior / (double)ic);
        CHECK(ratio > 0.8 && ratio < 1.25, "wrap/interior gradient ratio %.3f in [0.8,1.25]", ratio);
    }

    // Gaussianity report (informational — RPN is Gaussian by construction).
    {
        double sk1, ku1, sk2, ku2;
        MomentStats(grain[1].data(), grain[1].size(), sk1, ku1);
        MomentStats(bank.tile(0, 1), (size_t)TILE * TILE, sk2, ku2);
        std::printf("  info: source grain skew %+.3f exkurt %+.3f | tile skew %+.3f exkurt %+.3f\n",
                    sk1, ku1, sk2, ku2);
    }

    // Visual dumps.
    DumpPlane("s1_tile.png", bank.tile(0, 1), TILE, TILE, 4.f);
    DumpPlane("s1_true_grain_patch.png", grain[1].data(), W, H, 4.f);
    {
        // 1024² canvas tiled from the bank over 4 frames — flip/offset/tile
        // variation, seams would be obvious.
        const int C = 1024;
        std::vector<float> canvas((size_t)C * C * 4);
        ResponseCurve flat;
        flat.valid = true;
        for (int b = 0; b < ResponseCurve::kBands; ++b) {
            flat.lumaMid[b] = (b + 0.5f) / 5.f;
            for (int c = 0; c < 3; ++c) flat.sigma[c][b] = 0.05f;
        }
        for (int c = 0; c < 3; ++c) flat.flatSigma[c] = 0.05f;

        for (int f = 0; f < 4; ++f) {
            for (size_t i = 0; i < (size_t)C * C; ++i) {
                canvas[i * 4 + 0] = 1.f;
                canvas[i * 4 + 1] = canvas[i * 4 + 2] = canvas[i * 4 + 3] = 0.5f;
            }
            StampParams sp = MakeFrameStamp(0, f, bank.count, bank.n);
            sp.amount = 1.f; sp.trim = 1.f; sp.encode = Encode::Display;
            StampRows(canvas.data(), C, C, 0, C, bank, flat, sp);
            std::vector<float> lum((size_t)C * C);
            for (size_t i = 0; i < lum.size(); ++i)
                lum[i] = canvas[i * 4 + 2] - 0.5f;
            char nm[64];
            std::snprintf(nm, sizeof nm, "s1_canvas_frame%d.png", f);
            DumpPlane(nm, lum.data(), C, C, 4.f);
        }
    }

    // ----------------------------------------------------------------- S1c
    // Cross-channel colour fidelity. A plate whose grain is built by the same
    // common-factor construction the synthesis inverts, with DELIBERATELY
    // ASYMMETRIC pairs (a = .9/.8/.6 -> R-G .72, G-B .48, R-B .54) so a single
    // scalar cannot fit all three. All component fields share one correlation
    // length, so the channel spectra match and the overlap ceiling is ~1:
    // anything the model fails to reproduce here is the model's own fault.
    std::printf("\n[Stage 1c] Cross-channel colour fidelity\n");
    {
        const float trueA[3] = { 0.90f, 0.80f, 0.60f };
        const double wantPair[3] = {                       // a_i.a_j
            (double)trueA[0] * trueA[1],
            (double)trueA[1] * trueA[2],
            (double)trueA[0] * trueA[2],
        };

        std::vector<float> shared, indep[3], cplate[3];
        MakeGrainField(W, H, 1.f, corrSigma, 4242, shared);
        for (int c = 0; c < 3; ++c)
            MakeGrainField(W, H, 1.f, corrSigma, 5000 + c, indep[c]);

        for (int c = 0; c < 3; ++c) {
            const float aI = std::sqrt(1.f - trueA[c] * trueA[c]);
            cplate[c].resize((size_t)W * H);
            for (size_t i = 0; i < cplate[c].size(); ++i)
                cplate[c][i] = content[i] +
                    trueSigma[c] * (trueA[c] * shared[i] + aI * indep[c][i]);
        }

        GrainSpectrum cspec;
        {
            const float *planes[3] = { cplate[0].data(), cplate[1].data(),
                                       cplate[2].data() };
            const bool ok = MeasureGrainSpectrum(planes, W, H, TILE, blurSigma, cspec);
            CHECK(ok && cspec.valid(), "correlated plate measured");
        }

        std::printf("  info: target pairs   R-G %.3f  G-B %.3f  R-B %.3f\n",
                    wantPair[0], wantPair[1], wantPair[2]);
        std::printf("  info: measured pairs R-G %.3f  G-B %.3f  R-B %.3f (mean %.3f)\n",
                    cspec.crossPair[0], cspec.crossPair[1], cspec.crossPair[2],
                    cspec.crossCorr);

        // The measurement must see the asymmetry at all -- a mean-only
        // measurement reports one number and loses R-G > R-B > G-B.
        for (int i = 0; i < 3; ++i)
            CHECK(std::fabs(cspec.crossPair[i] - wantPair[i]) < 0.10,
                  "measured pair %d = %.3f vs true %.3f (|d| < 0.10)",
                  i, cspec.crossPair[i], wantPair[i]);
        CHECK(cspec.crossPair[0] > cspec.crossPair[2] &&
              cspec.crossPair[2] > cspec.crossPair[1],
              "pair ordering R-G > R-B > G-B preserved (asymmetry survives)");

        // The solve should factor cleanly and hit the ceiling, since every
        // component field was built at one correlation length.
        const CrossChannelModel cm = SolveCrossChannel(cspec);
        CHECK(cm.useCommonFactor, "common-factor model accepted the measurement");
        std::printf("  info: solved a = %.3f/%.3f/%.3f (true %.2f/%.2f/%.2f), "
                    "overlap %.3f, achieved %.3f\n",
                    cm.a[0], cm.a[1], cm.a[2], trueA[0], trueA[1], trueA[2],
                    cm.overlap, cm.achieved);
        CHECK(cm.achieved > 0.95f,
              "matched spectra leave the correlation unclipped (achieved %.3f > 0.95)",
              cm.achieved);
        for (int c = 0; c < 3; ++c)
            CHECK(std::fabs(cm.a[c] - trueA[c]) < 0.10f,
                  "recovered a[%d] = %.3f vs true %.2f (|d| < 0.10)",
                  c, cm.a[c], trueA[c]);

        // p_Nc >= 0 everywhere is the model's feasibility contract.
        {
            size_t neg = 0;
            for (int c = 0; c < 3; ++c)
                for (float v : cm.pIndep[c]) if (v < 0.f) ++neg;
            CHECK(neg == 0, "independent spectra non-negative in all %zu bins",
                  (size_t)3 * cm.pShared.size());
        }

        // The gate: realized correlation of the baked bank vs the target it
        // was asked to reproduce.
        TileBank cbank;
        SynthesizeTileBank(cspec, 8, /*seed*/ 11, cbank);
        CHECK(cbank.valid(), "correlated tile bank baked");
        double got[3];
        BankCorrelation(cbank, got);

        // A/B against the legacy scalar path. Zeroing the pairs is exactly the
        // condition that makes SolveCrossChannel decline, so this re-bakes the
        // SAME spectrum through the old phase-blend using the same mean rho.
        GrainSpectrum lspec = cspec;
        lspec.crossPair[0] = lspec.crossPair[1] = lspec.crossPair[2] = 0.f;
        TileBank lbank;
        SynthesizeTileBank(lspec, 8, /*seed*/ 11, lbank);
        double leg[3];
        BankCorrelation(lbank, leg);

        std::printf("  info: realized  common-factor %.3f/%.3f/%.3f | "
                    "legacy scalar %.3f/%.3f/%.3f\n",
                    got[0], got[1], got[2], leg[0], leg[1], leg[2]);

        double sumNew = 0.0, sumOld = 0.0;
        for (int i = 0; i < 3; ++i) {
            const double tgt = cspec.crossPair[i];
            CHECK(std::fabs(got[i] - tgt) < 0.05,
                  "realized pair %d = %.3f vs measured target %.3f (|d| < 0.05)",
                  i, got[i], tgt);
            sumNew += std::fabs(got[i] - tgt);
            sumOld += std::fabs(leg[i] - tgt);
        }
        std::printf("  info: total correlation error  common-factor %.3f  "
                    "legacy %.3f  (%.1fx better)\n",
                    sumNew, sumOld, sumOld / (sumNew + 1e-9));
        CHECK(sumNew < sumOld,
              "common-factor beats the scalar path (err %.3f < %.3f)",
              sumNew, sumOld);

        // Character must survive the change: the tiles are still unit sigma,
        // because only the cross-channel split moved.
        for (int c = 0; c < 3; ++c) {
            double var = 0.0;
            const float *tl = cbank.tile(0, c);
            for (size_t i = 0; i < (size_t)cbank.n * cbank.n; ++i)
                var += (double)tl[i] * tl[i];
            var /= (double)cbank.n * cbank.n;
            CHECK(std::fabs(std::sqrt(var) - 1.0) < 0.02,
                  "channel %d tile sigma %.4f == 1 (normalization intact)",
                  c, std::sqrt(var));
        }
    }

    // ---------------------------------------------------------------- S1c2
    // The same model with an OPPOSING channel: a = .9/.8/-.6, so R-G is
    // positive while G-B and R-B are negative. This is the sign pattern a
    // debayered digital plate actually measures (chroma reconstruction drives
    // green against red and blue), and it is representable by rank-1 only
    // because a_c is signed. Clamping correlation to [0,1] would destroy it.
    std::printf("\n[Stage 1c2] Cross-channel colour fidelity, anti-correlated\n");
    {
        const float trueA[3] = { 0.90f, 0.80f, -0.60f };
        const double wantPair[3] = {
            (double)trueA[0] * trueA[1],
            (double)trueA[1] * trueA[2],
            (double)trueA[0] * trueA[2],
        };

        std::vector<float> shared, indep[3], cplate[3];
        MakeGrainField(W, H, 1.f, corrSigma, 909, shared);
        for (int c = 0; c < 3; ++c)
            MakeGrainField(W, H, 1.f, corrSigma, 6000 + c, indep[c]);

        for (int c = 0; c < 3; ++c) {
            const float aI = std::sqrt(1.f - trueA[c] * trueA[c]);
            cplate[c].resize((size_t)W * H);
            for (size_t k = 0; k < cplate[c].size(); ++k)
                cplate[c][k] = content[k] +
                    trueSigma[c] * (trueA[c] * shared[k] + aI * indep[c][k]);
        }

        GrainSpectrum cspec;
        const float *planes[3] = { cplate[0].data(), cplate[1].data(),
                                   cplate[2].data() };
        CHECK(MeasureGrainSpectrum(planes, W, H, TILE, blurSigma, cspec),
              "anti-correlated plate measured");

        std::printf("  info: target pairs   R-G %+.3f  G-B %+.3f  R-B %+.3f\n",
                    wantPair[0], wantPair[1], wantPair[2]);
        std::printf("  info: measured pairs R-G %+.3f  G-B %+.3f  R-B %+.3f\n",
                    cspec.crossPair[0], cspec.crossPair[1], cspec.crossPair[2]);

        // The sign must survive the measurement at all.
        CHECK(cspec.crossPair[0] > 0.f && cspec.crossPair[1] < 0.f &&
              cspec.crossPair[2] < 0.f,
              "sign pattern (+,-,-) preserved through measurement");
        for (int k = 0; k < 3; ++k)
            CHECK(std::fabs(cspec.crossPair[k] - wantPair[k]) < 0.10,
                  "measured pair %d = %+.3f vs true %+.3f (|d| < 0.10)",
                  k, cspec.crossPair[k], wantPair[k]);

        const CrossChannelModel cm = SolveCrossChannel(cspec);
        CHECK(cm.useCommonFactor,
              "two-negative sign pattern accepted (rank-1 solution exists)");
        std::printf("  info: solved a = %+.3f/%+.3f/%+.3f (true %+.2f/%+.2f/%+.2f), "
                    "achieved %.3f\n",
                    cm.a[0], cm.a[1], cm.a[2], trueA[0], trueA[1], trueA[2],
                    cm.achieved);
        for (int c = 0; c < 3; ++c)
            CHECK(std::fabs(cm.a[c] - trueA[c]) < 0.12f,
                  "recovered a[%d] = %+.3f vs true %+.2f (|d| < 0.12)",
                  c, cm.a[c], trueA[c]);

        TileBank cbank;
        SynthesizeTileBank(cspec, 8, /*seed*/ 13, cbank);
        CHECK(cbank.valid(), "anti-correlated tile bank baked");
        double got[3];
        BankCorrelation(cbank, got);
        std::printf("  info: realized %+.3f/%+.3f/%+.3f\n", got[0], got[1], got[2]);
        for (int k = 0; k < 3; ++k)
            CHECK(std::fabs(got[k] - cspec.crossPair[k]) < 0.05,
                  "realized pair %d = %+.3f vs measured target %+.3f (|d| < 0.05)",
                  k, got[k], cspec.crossPair[k]);

        // An impossible sign pattern (all three negative) has no real rank-1
        // solution and must fall back rather than produce nonsense.
        {
            GrainSpectrum bad = cspec;
            bad.crossPair[0] = -0.40f; bad.crossPair[1] = -0.35f;
            bad.crossPair[2] = -0.30f;
            CHECK(!SolveCrossChannel(bad).useCommonFactor,
                  "all-negative pattern declined (no real rank-1 solution)");
        }
    }

    // ------------------------------------------------------------------ S2
    std::printf("\n[Stage 2] Luminance response\n");
    {
        // Plate: smooth vertical gradient, grain sigma follows a known law.
        auto lawSigma = [](float L) { return 0.02f + 0.06f * L; };
        const int W2 = 1536, H2 = 1024;
        std::vector<float> lum((size_t)W2 * H2);
        for (int y = 0; y < H2; ++y)
            for (int x = 0; x < W2; ++x)
                lum[(size_t)y * W2 + x] = 0.05f + 0.9f * (float)x / (float)W2;

        std::vector<float> g0[3], plate2[3];
        const float *planes2[3];
        for (int c = 0; c < 3; ++c) {
            MakeGrainField(W2, H2, 1.f, corrSigma, 2000 + c, g0[c]);
            plate2[c].resize((size_t)W2 * H2);
            for (size_t i = 0; i < plate2[c].size(); ++i)
                plate2[c][i] = lum[i] + g0[c][i] * lawSigma(lum[i]);
            planes2[c] = plate2[c].data();
        }

        std::vector<float> res2[3];
        const float *resP[3];
        for (int c = 0; c < 3; ++c) {
            res2[c].resize((size_t)W2 * H2);
            HighPassResidual(plate2[c].data(), res2[c].data(), W2, H2, blurSigma);
            resP[c] = res2[c].data();
        }

        ResponseCurve rc;
        MeasureResponse(planes2, resP, W2, H2, rc);
        CHECK(rc.valid, "response curve measured");

        // Exact ground truth per band: the high-passed PURE grain's sigma
        // over that band's pixels (the extractable band is what the method
        // measures and reproduces — see the Stage 1 note).
        float bandTruth[ResponseCurve::kBands];
        {
            std::vector<float> gimg((size_t)W2 * H2), gres((size_t)W2 * H2);
            for (size_t i = 0; i < gimg.size(); ++i)
                gimg[i] = g0[1][i] * lawSigma(lum[i]);
            HighPassResidual(gimg.data(), gres.data(), W2, H2, blurSigma);
            double acc[ResponseCurve::kBands] = {};
            size_t cnt[ResponseCurve::kBands] = {};
            for (size_t i = 0; i < gres.size(); ++i) {
                int b = (int)((lum[i] - 0.05f) / 0.9f * 5.f);
                if (b < 0) b = 0; else if (b > 4) b = 4;
                acc[b] += (double)gres[i] * gres[i];
                ++cnt[b];
            }
            for (int b = 0; b < ResponseCurve::kBands; ++b)
                bandTruth[b] = (float)std::sqrt(acc[b] / (double)cnt[b]);
        }

        for (int b = 0; b < ResponseCurve::kBands; ++b) {
            const float want = bandTruth[b];
            const float got = rc.sigma[1][b];
            const float rel = std::fabs(got - want) / want;
            CHECK(rel < 0.10f, "band %d (L=%.2f): sigma %.4f vs truth %.4f (rel %.3f < 0.10)",
                  b, rc.lumaMid[b], got, want, rel);
        }

        // Grain sits IN a gradient: stamp onto a clean gradient, then check
        // the output's local sigma follows the same law.
        {
            GrainSpectrum spec2;
            MeasureGrainSpectrum(planes2, W2, H2, TILE, blurSigma, spec2);
            TileBank bank2;
            SynthesizeTileBank(spec2, 8, 7, bank2);

            std::vector<float> img((size_t)W2 * H2 * 4);
            for (size_t i = 0; i < (size_t)W2 * H2; ++i) {
                img[i * 4 + 0] = 1.f;
                img[i * 4 + 1] = img[i * 4 + 2] = img[i * 4 + 3] = lum[i];
            }
            StampParams sp = MakeFrameStamp(0, 0, bank2.count, bank2.n);
            sp.amount = 1.f; sp.trim = 1.f; sp.encode = Encode::Display;
            StampRows(img.data(), W2, H2, 0, H2, bank2, rc, sp);

            // Measure added-grain sigma in 5 vertical strips (columns bands).
            for (int b = 0; b < 5; ++b) {
                const int x0 = W2 * b / 5 + 40, x1 = W2 * (b + 1) / 5 - 40;
                double acc = 0.0; size_t cnt = 0;
                for (int y = 0; y < H2; ++y)
                    for (int x = x0; x < x1; ++x) {
                        const size_t i = (size_t)y * W2 + x;
                        const double d = img[i * 4 + 2] - lum[i];
                        acc += d * d; ++cnt;
                    }
                const float got = (float)std::sqrt(acc / (double)cnt);
                const float want = bandTruth[b];
                const float rel = std::fabs(got - want) / want;
                CHECK(rel < 0.15f, "stamped strip %d: sigma %.4f vs source residual %.4f (rel %.3f < 0.15)",
                      b, got, want, rel);
            }

            std::vector<float> diff((size_t)W2 * H2);
            for (size_t i = 0; i < diff.size(); ++i)
                diff[i] = img[i * 4 + 2] - lum[i];
            DumpPlane("s2_stamped_grain.png", diff.data(), W2, H2, 4.f);
        }

        // Log path: HDR gradient round-trips NaN-free and grain lands with
        // the curve's sigma in LOG space.
        {
            const int W3 = 1024, H3 = 256;
            std::vector<float> img((size_t)W3 * H3 * 4);
            for (int y = 0; y < H3; ++y)
                for (int x = 0; x < W3; ++x) {
                    const size_t i = (size_t)y * W3 + x;
                    // linear values spanning −0.01 … 10 (negatives + HDR)
                    const float lin = -0.01f + 10.01f * (float)x / (float)(W3 - 1);
                    img[i * 4 + 0] = 1.f;
                    img[i * 4 + 1] = img[i * 4 + 2] = img[i * 4 + 3] = lin;
                }
            std::vector<float> ref(img);

            ResponseCurve flat;
            flat.valid = true;
            for (int b = 0; b < ResponseCurve::kBands; ++b) {
                flat.lumaMid[b] = 0.2f + 0.15f * b;
                for (int c = 0; c < 3; ++c) flat.sigma[c][b] = 0.03f;
            }
            for (int c = 0; c < 3; ++c) flat.flatSigma[c] = 0.03f;

            StampParams sp = MakeFrameStamp(3, 0, bank.count, bank.n);
            sp.amount = 1.f; sp.trim = 0.f; sp.encode = Encode::LogFromLinear;
            StampRows(img.data(), W3, H3, 0, H3, bank, flat, sp);

            int nan = 0;
            double logAcc = 0.0; size_t cnt = 0;
            for (size_t i = 0; i < (size_t)W3 * H3; ++i) {
                for (int c = 1; c < 4; ++c)
                    if (!std::isfinite(img[i * 4 + c])) ++nan;
                const float before = ref[i * 4 + 2], after = img[i * 4 + 2];
                const double dLog = (double)LinToLog(after) - (double)LinToLog(before);
                logAcc += dLog * dLog; ++cnt;
            }
            const float logSigma = (float)std::sqrt(logAcc / (double)cnt);
            CHECK(nan == 0, "log path NaN/Inf count %d == 0 (incl. negatives, HDR)", nan);
            CHECK(std::fabs(logSigma - 0.03f) / 0.03f < 0.10f,
                  "log-space grain sigma %.4f vs 0.0300 (rel %.3f < 0.10)",
                  logSigma, std::fabs(logSigma - 0.03f) / 0.03f);
        }
    }

    // ----------------------------------------------------------------- S2b
    // The FLAT-PLATE case: a locked grain plate, which is what the Grain
    // Source is actually for. Such a plate has no content, so ALL of its
    // luminance variation is its own grain. Binning by raw pixel luminance
    // therefore sorts pixels by the very quantity being measured and
    // fabricates a U-shaped curve -- measured on real stock at up to 3.2x the
    // plate's sigma at the ends and ~0.6x in the middle, which the stamp's
    // end-clamping then applied to every target luminance outside the plate's
    // narrow range. The curve must instead be recognised as unidentifiable
    // and collapse to one honest number.
    std::printf("\n[Stage 2b] Flat grain plate -- response must not invent a curve\n");
    {
        const int WF = 1024, HF = 1024;
        const float base = 0.18f, gs = 0.02f;

        std::vector<float> gf[3], plate[3];
        for (int c = 0; c < 3; ++c) {
            MakeGrainField(WF, HF, gs, 0.8f, 7700 + c, gf[c]);
            plate[c].resize((size_t)WF * HF);
            for (size_t k = 0; k < plate[c].size(); ++k)
                plate[c][k] = base + gf[c][k];
        }

        const float bsF = 2.0f;
        std::vector<float> resF[3];
        const float *planesF[3], *resPF[3];
        for (int c = 0; c < 3; ++c) {
            resF[c].resize((size_t)WF * HF);
            HighPassResidual(plate[c].data(), resF[c].data(), WF, HF, bsF);
            planesF[c] = plate[c].data();
            resPF[c] = resF[c].data();
        }

        ResponseCurve rcF;
        MeasureResponse(planesF, resPF, WF, HF, rcF);
        CHECK(rcF.valid, "flat-plate response measured");

        // Every band must carry the same sigma -- no fabricated curve.
        float lo = rcF.sigma[1][0], hi = rcF.sigma[1][0];
        for (int b = 1; b < ResponseCurve::kBands; ++b) {
            lo = std::min(lo, rcF.sigma[1][b]);
            hi = std::max(hi, rcF.sigma[1][b]);
        }
        std::printf("  info: band sigmas span %.6f..%.6f, flat %.6f\n",
                    lo, hi, rcF.flatSigma[1]);
        CHECK(hi - lo < 1e-6f, "curve is flat across all bands (span %.2e)", hi - lo);

        // And it must sit at the plate's real grain level at EVERY target
        // luminance -- including far outside the plate's own narrow range,
        // which is where the old end-clamping did its damage.
        double rv = 0.0;
        for (float v : resF[1]) rv += (double)v * v;
        const float trueResid = (float)std::sqrt(rv / (double)resF[1].size());
        const float probe[5] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
        for (int k = 0; k < 5; ++k) {
            const float got = EvalSigma(rcF, 1, probe[k], 1.0f);
            const float rel = std::fabs(got - trueResid) / trueResid;
            CHECK(rel < 0.10f,
                  "L=%.2f -> sigma %.6f vs plate residual %.6f (rel %.3f < 0.10)",
                  probe[k], got, trueResid, rel);
        }
    }

    // ------------------------------------------------------------- extras
    std::printf("\n[Extras] determinism, variation, benchmark\n");
    {
        const StampParams a = MakeFrameStamp(123, 10, 8, 256);
        const StampParams b = MakeFrameStamp(123, 10, 8, 256);
        const StampParams c = MakeFrameStamp(123, 11, 8, 256);
        CHECK(a.tileIndex == b.tileIndex && a.offX == b.offX && a.offY == b.offY &&
              a.flipX == b.flipX && a.flipY == b.flipY,
              "same (seed,frame) → identical stamp");
        CHECK(a.offX != c.offX || a.offY != c.offY || a.tileIndex != c.tileIndex ||
              a.flipX != c.flipX || a.flipY != c.flipY,
              "next frame → different stamp");
    }
    {
        const int BW = 6144, BH = 3456;   // 6K, the real-world frame size
        std::vector<float> img((size_t)BW * BH * 4, 0.5f);
        for (size_t i = 0; i < (size_t)BW * BH; ++i) img[i * 4 + 0] = 1.f;

        ResponseCurve flat;
        flat.valid = true;
        for (int b = 0; b < ResponseCurve::kBands; ++b) {
            flat.lumaMid[b] = (b + 0.5f) / 5.f;
            for (int c2 = 0; c2 < 3; ++c2) flat.sigma[c2][b] = 0.04f;
        }
        for (int c2 = 0; c2 < 3; ++c2) flat.flatSigma[c2] = 0.04f;

        StampParams sp = MakeFrameStamp(0, 0, bank.count, bank.n);
        sp.encode = Encode::Display;

        const auto t0 = std::chrono::steady_clock::now();
        StampRows(img.data(), BW, BH, 0, BH, bank, flat, sp);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::printf("  info: 6K (%dx%d) single-thread stamp: %.1f ms\n", BW, BH, ms);
        CHECK(ms < 2000.0, "6K single-thread stamp %.1f ms < 2000 ms ceiling", ms);

        // Size ≠ 100%% exercises the bilinear-wrap path.
        sp.invScale = 100.f / 173.f;
        const auto t2 = std::chrono::steady_clock::now();
        StampRows(img.data(), BW, BH, 0, BH, bank, flat, sp);
        const auto t3 = std::chrono::steady_clock::now();
        const double ms2 = std::chrono::duration<double, std::milli>(t3 - t2).count();
        std::printf("  info: 6K bilinear (Size 173%%) stamp: %.1f ms\n", ms2);
        CHECK(ms2 < 3000.0, "6K bilinear stamp %.1f ms < 3000 ms ceiling", ms2);
    }

    std::printf("\n=== %s (%d failure%s) ===\n",
                g_failures ? "FAILED" : "ALL GATES PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
