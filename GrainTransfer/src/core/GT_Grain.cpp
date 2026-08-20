/*  GT_Grain.cpp — see GT_Grain.h.  */

#include "GT_Grain.h"
#include "GT_FFT.h"
#include "GT_Random.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gt {

// ---------------------------------------------------------------------------
// Gaussian blur — separable, edge-clamped.
// ---------------------------------------------------------------------------
static void MakeKernel(float sigma, std::vector<float> &k, int &half)
{
    half = (int)std::ceil(3.f * sigma);
    if (half < 1) half = 1;
    k.resize((size_t)half * 2 + 1);
    const float inv2s2 = 1.f / (2.f * sigma * sigma);
    float sum = 0.f;
    for (int i = -half; i <= half; ++i) {
        const float v = std::exp(-(float)(i * i) * inv2s2);
        k[(size_t)(i + half)] = v;
        sum += v;
    }
    for (float &v : k) v /= sum;
}

void GaussianBlur(const float *src, float *dst, int W, int H, float sigma)
{
    std::vector<float> k;
    int half = 0;
    MakeKernel(sigma, k, half);

    std::vector<float> tmp((size_t)W * H);

    // Horizontal pass.
    for (int y = 0; y < H; ++y) {
        const float *row = src + (size_t)y * W;
        float *out = tmp.data() + (size_t)y * W;
        for (int x = 0; x < W; ++x) {
            float acc = 0.f;
            for (int i = -half; i <= half; ++i) {
                int xx = x + i;
                if (xx < 0) xx = 0; else if (xx >= W) xx = W - 1;
                acc += row[xx] * k[(size_t)(i + half)];
            }
            out[x] = acc;
        }
    }

    // Vertical pass.
    for (int y = 0; y < H; ++y) {
        float *out = dst + (size_t)y * W;
        for (int x = 0; x < W; ++x) {
            float acc = 0.f;
            for (int i = -half; i <= half; ++i) {
                int yy = y + i;
                if (yy < 0) yy = 0; else if (yy >= H) yy = H - 1;
                acc += tmp[(size_t)yy * W + x] * k[(size_t)(i + half)];
            }
            out[x] = acc;
        }
    }
}

void HighPassResidual(const float *plane, float *residual, int W, int H,
                      float blurSigma)
{
    std::vector<float> low((size_t)W * H);
    GaussianBlur(plane, low.data(), W, H, blurSigma);
    const size_t N = (size_t)W * H;
    for (size_t i = 0; i < N; ++i)
        residual[i] = plane[i] - low[i];
}

// ---------------------------------------------------------------------------
// Spectrum measurement.
// ---------------------------------------------------------------------------
bool MeasureGrainSpectrum(const float *const rgb[3], int W, int H,
                          int n, float blurSigma, GrainSpectrum &out)
{
    std::vector<float> res[3];
    const float *resP[3];
    for (int c = 0; c < 3; ++c) {
        res[c].resize((size_t)W * H);
        HighPassResidual(rgb[c], res[c].data(), W, H, blurSigma);
        resP[c] = res[c].data();
    }
    return MeasureGrainSpectrumRes(resP, W, H, n, out);
}

// ---------------------------------------------------------------------------
// Robust cross-channel correlation.
// ---------------------------------------------------------------------------
// Measured on its OWN grid of small blocks rather than on the FFT patches,
// because the two measurements want opposite things. The spectrum wants large
// patches (frequency resolution) and tolerates a little content, which phase
// randomization discards downstream anyway. Correlation wants the opposite:
// content is the DOMINANT error term here and has to be excluded, and a small
// block is both likelier to be content-free and cheap enough to have hundreds
// of them instead of the six a 6K plate yields at n=256.
//
// Why content is the enemy specifically: the residual is plane - blur(plane),
// so content edges survive in it, and they bias this measurement in a
// direction set by the content's own colour. A grey edge appears in all three
// channels and drives every pair positive; a coloured edge drives pairs in
// opposite directions. Neither has anything to do with grain. Three defences:
//
//   1. Small blocks, so any one edge contaminates few of them.
//   2. Flattest-first selection: blocks are ranked by combined residual sigma
//      and only a low percentile window survives -- an edge inflates sigma,
//      and the bottom of the range is clipped or grainless.
//   3. Winsorized correlation inside each block: clamping at kClip sigma
//      strips the leverage of the few high-amplitude edge pixels that get
//      through 1-2, without discarding the grain pixels around them.
//
// The per-block correlations are then combined by MEDIAN, never by pooling.
// Pooling weights each block by its variance, which hands the answer to
// exactly the contaminated blocks the first three defences just excluded.
static void MeasureCrossPairs(const float *const res[3], int W, int H,
                              double out[3])
{
    out[0] = out[1] = out[2] = 0.0;

    constexpr int   kBlock = 32;       // correlation block edge, px
    constexpr float kClip  = 2.5f;     // winsorization point, in block sigmas
    constexpr int   kMinForSelect = 16;// below this, keep every block

    const int bx = W / kBlock, by = H / kBlock;
    const int nBlocks = bx * by;
    if (nBlocks < 1) return;

    constexpr size_t kN = (size_t)kBlock * kBlock;

    // Pass 1: combined residual sigma per block -- the flatness statistic.
    std::vector<float> sig((size_t)nBlocks);
    for (int j = 0; j < by; ++j) {
        for (int i = 0; i < bx; ++i) {
            double acc = 0.0;
            for (int c = 0; c < 3; ++c) {
                for (int y = 0; y < kBlock; ++y) {
                    const float *row = res[c] +
                        ((size_t)(j * kBlock + y) * W + (size_t)i * kBlock);
                    for (int x = 0; x < kBlock; ++x) acc += (double)row[x] * row[x];
                }
            }
            sig[(size_t)(j * bx + i)] = (float)std::sqrt(acc / ((double)kN * 3.0));
        }
    }

    // Keep the flattest band: above the 10th percentile (clipped/grainless
    // blocks carry no usable correlation) and below the 60th (edge-bearing).
    float loCut = -1.f, hiCut = 3.0e38f;
    if (nBlocks >= kMinForSelect) {
        std::vector<float> srt(sig);
        std::sort(srt.begin(), srt.end());
        loCut = srt[(size_t)nBlocks * 10 / 100];
        hiCut = srt[(size_t)nBlocks * 60 / 100];
    }

    // Pass 2: winsorized correlation per surviving block.
    std::vector<float> corr[3];
    for (int k = 0; k < 3; ++k) corr[k].reserve((size_t)nBlocks);

    std::vector<float> v[3] = { std::vector<float>(kN), std::vector<float>(kN),
                                std::vector<float>(kN) };

    for (int j = 0; j < by; ++j) {
        for (int i = 0; i < bx; ++i) {
            const float bs = sig[(size_t)(j * bx + i)];
            if (bs < loCut || bs > hiCut || bs <= 0.f) continue;

            bool degenerate = false;
            for (int c = 0; c < 3 && !degenerate; ++c) {
                float *d = v[c].data();
                double mean = 0.0;
                for (int y = 0; y < kBlock; ++y) {
                    const float *row = res[c] +
                        ((size_t)(j * kBlock + y) * W + (size_t)i * kBlock);
                    for (int x = 0; x < kBlock; ++x) {
                        const float t = row[x];
                        d[(size_t)y * kBlock + x] = t;
                        mean += t;
                    }
                }
                mean /= (double)kN;

                double var = 0.0;
                for (size_t q = 0; q < kN; ++q) {
                    const float t = (float)(d[q] - mean);
                    d[q] = t;
                    var += (double)t * t;
                }
                var /= (double)kN;
                if (!(var > 0.0)) { degenerate = true; break; }

                // Winsorize, then re-centre: clamping is slightly asymmetric on
                // a finite sample and would otherwise leave a residual mean.
                const float lim = kClip * (float)std::sqrt(var);
                double m2 = 0.0;
                for (size_t q = 0; q < kN; ++q) {
                    if (d[q] >  lim) d[q] =  lim;
                    else if (d[q] < -lim) d[q] = -lim;
                    m2 += d[q];
                }
                m2 /= (double)kN;
                for (size_t q = 0; q < kN; ++q) d[q] = (float)(d[q] - m2);
            }
            if (degenerate) continue;

            double sq[3] = { 0, 0, 0 }, cr[3] = { 0, 0, 0 };
            for (size_t q = 0; q < kN; ++q) {
                const double r = v[0][q], g = v[1][q], b = v[2][q];
                sq[0] += r * r; sq[1] += g * g; sq[2] += b * b;
                cr[0] += r * g; cr[1] += g * b; cr[2] += r * b;
            }
            if (!(sq[0] > 0.0 && sq[1] > 0.0 && sq[2] > 0.0)) continue;

            corr[0].push_back((float)(cr[0] / std::sqrt(sq[0] * sq[1])));
            corr[1].push_back((float)(cr[1] / std::sqrt(sq[1] * sq[2])));
            corr[2].push_back((float)(cr[2] / std::sqrt(sq[0] * sq[2])));
        }
    }

    if (corr[0].empty()) return;
    for (int k = 0; k < 3; ++k) {
        std::sort(corr[k].begin(), corr[k].end());
        const size_t m = corr[k].size();
        out[k] = (m & 1) ? corr[k][m / 2]
                         : 0.5 * ((double)corr[k][m / 2 - 1] + corr[k][m / 2]);
    }
}

bool MeasureGrainSpectrumRes(const float *const res[3], int W, int H,
                             int n, GrainSpectrum &out)
{
    out = GrainSpectrum();
    if (W < n || H < n) return false;
    out.n = n;

    const size_t tileN = (size_t)n * n;

    // Patch grid (non-overlapping, fully inside).
    const int px = W / n, py = H / n;
    const int patchCount = px * py;

    // Combined residual sigma per patch — the selection statistic.
    std::vector<float> patchSigma((size_t)patchCount);
    for (int pj = 0; pj < py; ++pj) {
        for (int pi = 0; pi < px; ++pi) {
            double acc = 0.0;
            for (int c = 0; c < 3; ++c) {
                const float *r = res[c];
                for (int y = 0; y < n; ++y) {
                    const float *row = r + ((size_t)(pj * n + y) * W + (size_t)pi * n);
                    for (int x = 0; x < n; ++x) acc += (double)row[x] * row[x];
                }
            }
            patchSigma[(size_t)(pj * px + pi)] =
                (float)std::sqrt(acc / ((double)tileN * 3.0));
        }
    }

    std::vector<float> sorted(patchSigma);
    std::sort(sorted.begin(), sorted.end());
    const float median = sorted[sorted.size() / 2];
    // Reject content-edge-heavy patches (≫ median) and grainless clipped
    // patches (≪ median). With one patch, median == the patch — it survives.
    const float hi = median * 1.5f;
    const float lo = median * 0.3f;

    // Hann window and its power (for unbiasing the accumulated spectra).
    std::vector<float> win(tileN);
    double winPower = 0.0;
    for (int y = 0; y < n; ++y) {
        const float wy = 0.5f - 0.5f * std::cos(2.f * 3.14159265358979f * y / n);
        for (int x = 0; x < n; ++x) {
            const float wx = 0.5f - 0.5f * std::cos(2.f * 3.14159265358979f * x / n);
            const float w = wx * wy;
            win[(size_t)y * n + x] = w;
            winPower += (double)w * w;
        }
    }
    // Windowing scales a periodogram by the window's mean-square Σw²/N;
    // multiplying by N/Σw² restores the unwindowed energy scale. (The tile
    // is re-normalized to unit sigma later, so this only keeps reported
    // spectra in honest units — the shape is what matters.)
    const float winNorm = (float)((double)tileN / winPower);

    Fft2D fft(n);
    std::vector<float> ms[3] = { std::vector<float>(tileN),
                                 std::vector<float>(tileN),
                                 std::vector<float>(tileN) };
    std::vector<float> patch(tileN);
    std::vector<kiss_fft_cpx> spec(tileN);
    std::vector<double> accPower[3];
    double accVar[3] = { 0, 0, 0 };
    for (int c = 0; c < 3; ++c) accPower[c].assign(tileN, 0.0);

    int used = 0;
    for (int pj = 0; pj < py; ++pj) {
        for (int pi = 0; pi < px; ++pi) {
            const float s = patchSigma[(size_t)(pj * px + pi)];
            if (s > hi || s < lo) continue;
            ++used;

            // Mean-subtract each channel's patch (the window and FFT below
            // both assume a zero-mean patch).
            for (int c = 0; c < 3; ++c) {
                double mean = 0.0;
                for (int y = 0; y < n; ++y) {
                    const float *row = res[c] +
                        ((size_t)(pj * n + y) * W + (size_t)pi * n);
                    for (int x = 0; x < n; ++x) {
                        const float v = row[x];
                        ms[c][(size_t)y * n + x] = v;
                        mean += v;
                    }
                }
                mean /= (double)tileN;
                double var = 0.0;
                for (size_t i = 0; i < tileN; ++i) {
                    const float v = (float)(ms[c][i] - mean);
                    ms[c][i] = v;
                    var += (double)v * v;
                }
                accVar[c] += var / (double)tileN;
            }

            // Window + FFT + accumulate power per channel.
            for (int c = 0; c < 3; ++c) {
                for (size_t i = 0; i < tileN; ++i) patch[i] = ms[c][i] * win[i];
                fft.forward(patch.data(), spec.data());
                double *acc = accPower[c].data();
                for (size_t i = 0; i < tileN; ++i)
                    acc[i] += (double)spec[i].r * spec[i].r +
                              (double)spec[i].i * spec[i].i;
            }
        }
    }

    if (!used) return false;
    out.patchesUsed = used;

    for (int c = 0; c < 3; ++c) {
        out.power[c].resize(tileN);
        const double scale = (double)winNorm / (double)used;
        for (size_t i = 0; i < tileN; ++i)
            out.power[c][i] = (float)(accPower[c][i] * scale);
        out.power[c][0] = 0.f;   // DC — grain is zero-mean by definition
        out.sigma[c] = (float)std::sqrt(accVar[c] / (double)used);
    }

    // Pairwise correlations, kept SEPARATELY and SIGNED. The three carry both
    // the asymmetry a mean averages away and the sign a mean cannot represent:
    // clamping a real -0.7 G-B anti-correlation to zero throws away structure
    // the common-factor model can reproduce exactly (with a negative weight).
    //
    // crossCorr remains the mean of the POSITIVE parts, because it feeds the
    // legacy scalar phase-blend, whose rho is a [0,1] quantity that cannot
    // express anti-correlation. It is a fallback summary, not the measurement.
    //
    // Measured on its own block grid, NOT on the patches above: see
    // MeasureCrossPairs for why the two measurements cannot share a grid.
    double pair[3];
    MeasureCrossPairs(res, W, H, pair);
    double sum = 0.0;
    for (int i = 0; i < 3; ++i) {
        double r = pair[i];
        if (r < -1.0) r = -1.0; else if (r > 1.0) r = 1.0;
        out.crossPair[i] = (float)r;
        if (r > 0.0) sum += r;
    }
    out.crossCorr = (float)(sum / 3.0);

    return true;
}

// ---------------------------------------------------------------------------
// Cross-channel common-factor solve.
// ---------------------------------------------------------------------------
CrossChannelModel SolveCrossChannel(const GrainSpectrum &spec)
{
    CrossChannelModel m;
    if (!spec.valid()) return m;

    const size_t tileN = (size_t)spec.n * spec.n;

    // A pair at ~zero means those two channels measured independent, and the
    // rank-1 solve divides by it either way -- the shared factor cannot be
    // identified, so hand back to the scalar path.
    const float kMinPair = 1e-3f;
    for (int i = 0; i < 3; ++i)
        if (std::fabs(spec.crossPair[i]) < kMinPair) return m;

    // A real rank-1 fit exists iff r_RG.r_GB.r_RB = (a_R.a_G.a_B)^2 >= 0, i.e.
    // all three pairs positive, or exactly two negative (one channel opposing
    // the other two -- the debayer signature). One negative pair, or three, has
    // no real solution: no assignment of signs to a_c reproduces it.
    const double prod = (double)spec.crossPair[0] * spec.crossPair[1] *
                        (double)spec.crossPair[2];
    if (prod <= 0.0) return m;

    // Per-bin spectra normalized to unit total. Absolute scale is irrelevant:
    // tiles are normalized to unit sigma per channel after synthesis, so what
    // survives is shape, and what we reproduce is correlation, not covariance.
    std::vector<float> p[3];
    for (int c = 0; c < 3; ++c) {
        p[c].assign(tileN, 0.f);
        double tot = 0.0;
        for (size_t i = 0; i < tileN; ++i) {
            const float v = spec.power[c][i];
            if (v > 0.f) tot += (double)v;
        }
        if (tot <= 0.0) return m;                    // a channel with no grain
        const float inv = (float)(1.0 / tot);
        for (size_t i = 0; i < tileN; ++i) {
            const float v = spec.power[c][i];
            p[c][i] = v > 0.f ? v * inv : 0.f;
        }
    }

    // Rank-1 factor of the correlation matrix: corr(i,j) = a_i.a_j.
    //   crossPair = [0]=R-G  [1]=G-B  [2]=R-B
    //   a_R^2 = (R_RG.R_RB)/R_GB,  a_G^2 = (R_RG.R_GB)/R_RB,
    //   a_B^2 = (R_GB.R_RB)/R_RG.
    // The guard above makes all three ratios positive. A measurement that is
    // not consistently rank-1 can still push a_c^2 past 1; clamping there
    // degrades gracefully (that channel becomes pure shared field).
    const float rRG = spec.crossPair[0], rGB = spec.crossPair[1],
                rRB = spec.crossPair[2];
    float aSq[3] = { rRG * rRB / rGB, rRG * rGB / rRB, rGB * rRB / rRG };
    for (int c = 0; c < 3; ++c) {
        if (!(aSq[c] > 0.f)) return m;               // NaN / non-finite guard
        if (aSq[c] > 1.f) aSq[c] = 1.f;
    }

    // Signs. a_R > 0 without loss of generality (negating all three leaves
    // every product unchanged), then a_G and a_B follow from the pairs that
    // involve R. The guard above guarantees these are consistent with r_GB.
    float sgn[3] = { 1.f, rRG < 0.f ? -1.f : 1.f, rRB < 0.f ? -1.f : 1.f };

    // The shared field can only occupy frequencies where ALL THREE channels
    // have power, so put p_L at the per-bin channel minimum. That choice is
    // both feasible and maximal for this family, and its total M is exactly
    // the spectral overlap that bounds how correlated the channels can be.
    m.pShared.assign(tileN, 0.f);
    double M = 0.0;
    for (size_t i = 0; i < tileN; ++i) {
        float lo = p[0][i];
        if (p[1][i] < lo) lo = p[1][i];
        if (p[2][i] < lo) lo = p[2][i];
        m.pShared[i] = lo;
        M += (double)lo;
    }
    if (M <= 1e-9) return m;                         // disjoint spectra
    {
        const float inv = (float)(1.0 / M);
        for (size_t i = 0; i < tileN; ++i) m.pShared[i] *= inv;
    }

    // Feasibility: p_Nc = p_c - a_c^2.p_L must stay non-negative everywhere.
    // With p_L at the per-bin minimum that reduces to a_c^2 <= M, so one
    // scalar s scales every a_c^2 into range -- and because corr = a_i.a_j,
    // the realized correlation is exactly s times the measured target. s is
    // reported rather than hidden: it is the honest ceiling.
    float aMax = aSq[0];
    if (aSq[1] > aMax) aMax = aSq[1];
    if (aSq[2] > aMax) aMax = aSq[2];
    float s = 1.f;
    if (aMax > 0.f && (float)M < aMax) s = (float)M / aMax;

    for (int c = 0; c < 3; ++c) {
        const float sc = aSq[c] * s;
        m.a[c] = sgn[c] * std::sqrt(sc);
        m.pIndep[c].assign(tileN, 0.f);
        for (size_t i = 0; i < tileN; ++i) {
            float v = p[c][i] - sc * m.pShared[i];
            m.pIndep[c][i] = v > 0.f ? v : 0.f;      // exact bar float rounding
        }
    }

    m.overlap = (float)M;
    m.achieved = s;
    m.useCommonFactor = true;
    return m;
}

// ---------------------------------------------------------------------------
// RPN tile synthesis.
// ---------------------------------------------------------------------------
void SynthesizeTileBank(const GrainSpectrum &spec, int count, uint64_t seed,
                        TileBank &out)
{
    out = TileBank();
    if (!spec.valid() || count < 1) return;

    const int n = spec.n;
    const size_t tileN = (size_t)n * n;
    out.n = n;
    out.count = count;
    out.tiles.resize((size_t)count * 3 * tileN);

    Fft2D fft(n);
    std::vector<float> noise(tileN);
    std::vector<float> noiseShared(tileN);
    std::vector<kiss_fft_cpx> spc(tileN);
    std::vector<kiss_fft_cpx> specShared(tileN);
    std::vector<float> mag(tileN);

    // Cross-channel structure. Preferred: the common-factor model, which sums
    // a shared FIELD into each channel and so reproduces the measured
    // correlation matrix directly. Fallback (unfactorable measurement): the
    // legacy scalar path, which blends a shared PHASE field by rho -- cheaper
    // to reason about but nonlinear in rho, so it undershoots.
    //
    // Both donors are FFTs of real fields scaled per bin by real, EVEN factors
    // (measured power spectra are even), so every spectrum below stays
    // Hermitian and each inverse FFT is exactly real -- no conjugate-symmetry
    // bookkeeping, exactly as with plain RPN.
    const CrossChannelModel model = SolveCrossChannel(spec);

    float rho = spec.crossCorr;
    if (rho < 0.f) rho = 0.f; else if (rho > 1.f) rho = 1.f;
    const float sIndep = std::sqrt(1.f - rho * rho);

    // Impose magnitude `mg` on the phase of donor bin `d`.
    auto imposeMag = [](kiss_fft_cpx &d, float mg) {
        const float len = std::sqrt(d.r * d.r + d.i * d.i);
        if (len > 1e-20f) {
            const float sc = mg / len;
            d.r *= sc;
            d.i *= sc;
        } else {
            d.r = mg;
            d.i = 0.f;
        }
    };

    // Box–Muller Gaussian white field into `buf`, deterministic in `key`.
    auto fillNoise = [&](std::vector<float> &buf, uint64_t key) {
        Rng rng(key);
        for (size_t i = 0; i < tileN; i += 2) {
            float u1 = rng.nextFloat();
            if (u1 < 1e-12f) u1 = 1e-12f;
            const float u2 = rng.nextFloat();
            const float r = std::sqrt(-2.f * std::log(u1));
            const float a = 6.28318530717959f * u2;
            buf[i] = r * std::cos(a);
            if (i + 1 < tileN) buf[i + 1] = r * std::sin(a);
        }
    };

    for (int t = 0; t < count; ++t) {
        // Shared phase donor for this tile (identical across all 3 channels).
        fillNoise(noiseShared, HashSeed(seed, (uint64_t)t, 0x73686172ull));
        fft.forward(noiseShared.data(), specShared.data());

        // Common-factor mode: turn the shared donor into L itself, once per
        // tile, by imposing the shared spectrum p_L on its phase. All three
        // channels then add the SAME field, which is what carries correlation.
        if (model.useCommonFactor) {
            const float *pL = model.pShared.data();
            for (size_t i = 0; i < tileN; ++i)
                imposeMag(specShared[i], std::sqrt(pL[i]));
        }

        for (int c = 0; c < 3; ++c) {
            // Channel-independent phase donor.
            fillNoise(noise, HashSeed(seed, (uint64_t)t * 3 + c, 0x67726169ull));
            fft.forward(noise.data(), spc.data());

            if (model.useCommonFactor) {
                // N_c at its own spectrum, then grain_c = a_c·L + N_c. The sum
                // is NOT re-normalized to the measured magnitude: that would
                // undo the correlation this addition just bought (see header).
                const float *pN = model.pIndep[c].data();
                const float a = model.a[c];
                for (size_t i = 0; i < tileN; ++i) {
                    imposeMag(spc[i], std::sqrt(pN[i]));
                    spc[i].r += a * specShared[i].r;
                    spc[i].i += a * specShared[i].i;
                }
            } else {
                // Legacy scalar path: blend shared + independent PHASE by rho,
                // then impose the measured per-channel magnitude exactly.
                const float *pw = spec.power[c].data();
                for (size_t i = 0; i < tileN; ++i)
                    mag[i] = std::sqrt(pw[i] < 0.f ? 0.f : pw[i]);

                for (size_t i = 0; i < tileN; ++i) {
                    const float re = rho * specShared[i].r + sIndep * spc[i].r;
                    const float im = rho * specShared[i].i + sIndep * spc[i].i;
                    spc[i].r = re;
                    spc[i].i = im;
                    imposeMag(spc[i], mag[i]);
                }
            }
            spc[0].r = 0.f;   // DC stays zero
            spc[0].i = 0.f;

            float *dst = out.tiles.data() + ((size_t)t * 3 + c) * tileN;
            fft.inverse(spc.data(), dst);

            // Normalize to zero mean, unit sigma — absolute amplitude is
            // reapplied at stamp time from the response curve.
            double mean = 0.0;
            for (size_t i = 0; i < tileN; ++i) mean += dst[i];
            mean /= (double)tileN;
            double var = 0.0;
            for (size_t i = 0; i < tileN; ++i) {
                const float v = (float)(dst[i] - mean);
                dst[i] = v;
                var += (double)v * v;
            }
            var /= (double)tileN;
            const float inv = var > 1e-30 ? (float)(1.0 / std::sqrt(var)) : 0.f;
            for (size_t i = 0; i < tileN; ++i) dst[i] *= inv;
        }
    }
}

} // namespace gt
