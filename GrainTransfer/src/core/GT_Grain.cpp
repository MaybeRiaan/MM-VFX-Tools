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
    out = GrainSpectrum();
    if (W < n || H < n) return false;
    out.n = n;

    const size_t planeN = (size_t)W * H;
    const size_t tileN = (size_t)n * n;

    // Residuals for all three channels.
    std::vector<float> res[3];
    for (int c = 0; c < 3; ++c) {
        res[c].resize(planeN);
        HighPassResidual(rgb[c], res[c].data(), W, H, blurSigma);
    }

    // Patch grid (non-overlapping, fully inside).
    const int px = W / n, py = H / n;
    const int patchCount = px * py;

    // Combined residual sigma per patch — the selection statistic.
    std::vector<float> patchSigma((size_t)patchCount);
    for (int pj = 0; pj < py; ++pj) {
        for (int pi = 0; pi < px; ++pi) {
            double acc = 0.0;
            for (int c = 0; c < 3; ++c) {
                const float *r = res[c].data();
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

            for (int c = 0; c < 3; ++c) {
                // Copy patch, subtract mean, window.
                double mean = 0.0;
                for (int y = 0; y < n; ++y) {
                    const float *row = res[c].data() +
                        ((size_t)(pj * n + y) * W + (size_t)pi * n);
                    for (int x = 0; x < n; ++x) {
                        patch[(size_t)y * n + x] = row[x];
                        mean += row[x];
                    }
                }
                mean /= (double)tileN;

                double var = 0.0;
                for (size_t i = 0; i < tileN; ++i) {
                    const float v = (float)(patch[i] - mean);
                    var += (double)v * v;
                    patch[i] = v * win[i];
                }
                accVar[c] += var / (double)tileN;

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
    return true;
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
    std::vector<kiss_fft_cpx> spc(tileN);
    std::vector<float> mag(tileN);

    for (int t = 0; t < count; ++t) {
        for (int c = 0; c < 3; ++c) {
            // Magnitude from the measured average power.
            const float *pw = spec.power[c].data();
            for (size_t i = 0; i < tileN; ++i)
                mag[i] = std::sqrt(pw[i] < 0.f ? 0.f : pw[i]);

            // Fresh white-noise field → its spectrum donates the phase.
            // (Real field ⇒ Hermitian spectrum ⇒ real inverse. Gaussian via
            // Box–Muller for a well-conditioned, full-band phase source.)
            Rng rng(HashSeed(seed, (uint64_t)t * 3 + c, 0x67726169ull));
            for (size_t i = 0; i < tileN; i += 2) {
                float u1 = rng.nextFloat();
                if (u1 < 1e-12f) u1 = 1e-12f;
                const float u2 = rng.nextFloat();
                const float r = std::sqrt(-2.f * std::log(u1));
                const float a = 6.28318530717959f * u2;
                noise[i] = r * std::cos(a);
                if (i + 1 < tileN) noise[i + 1] = r * std::sin(a);
            }

            fft.forward(noise.data(), spc.data());
            for (size_t i = 0; i < tileN; ++i) {
                const float re = spc[i].r, im = spc[i].i;
                const float len = std::sqrt(re * re + im * im);
                if (len > 1e-20f) {
                    const float s = mag[i] / len;
                    spc[i].r = re * s;
                    spc[i].i = im * s;
                } else {
                    spc[i].r = mag[i];
                    spc[i].i = 0.f;
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
