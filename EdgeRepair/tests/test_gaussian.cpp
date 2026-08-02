/*  test_gaussian.cpp — headless unit test for ER_Gaussian.h.

    Compiles with plain cl.exe, no AE SDK (via tests\build_tests.ps1).
    Exit code 0 = all pass, 1 = a failure.

    Validates: kernel normalization, constant preservation (both passes),
    impulse symmetry, interior linearity preservation (a Gaussian of a
    linear ramp equals the ramp away from borders), and the alpha-weighted
    divide recovering a constant straight colour exactly under wildly
    varying alpha — the property Edge Repair's Detail low-pass depends on
    to keep zero-alpha garbage out of the low band.
*/

#include "ER_Gaussian.h"

#include <cstdio>
#include <cmath>
#include <vector>

using std::vector;

static int g_fail = 0;

static void Check(bool ok, const char *name, const char *detail)
{
    printf("  [%-24s] %s%s%s\n", name, ok ? "OK" : "FAIL", detail[0] ? "  " : "", detail);
    if (!ok) ++g_fail;
}

int main()
{
    printf("ER_Gaussian separable blur test\n");

    // 1. Kernel normalization across radii.
    {
        double worst = 0.0;
        for (float r : { 1.f, 2.f, 4.f, 8.f, 16.f }) {
            auto k = er::BuildGaussianKernel(r);
            double s = 0.0;
            for (float v : k) s += v;
            worst = std::fmax(worst, std::fabs(s - 1.0));
        }
        char d[64]; snprintf(d, sizeof d, "worst|sum-1|=%.3g", worst);
        Check(worst < 1e-6, "kernel-normalized", d);
    }

    // 2. Constant plane preserved exactly through H then V.
    {
        const int W = 41, H = 27;
        const float C = 0.371f;
        vector<float> src((size_t)W * H, C), tmp((size_t)W * H), dst((size_t)W * H);
        auto k = er::BuildGaussianKernel(8.f);
        er::GaussianBlurH(src.data(), tmp.data(), W, H, k);
        er::GaussianBlurV(tmp.data(), dst.data(), W, H, k);
        double worst = 0.0;
        for (float v : dst) worst = std::fmax(worst, std::fabs((double)v - (double)C));
        char d[64]; snprintf(d, sizeof d, "worst|diff|=%.3g", worst);
        Check(worst < 1e-5, "constant-preserved", d);
    }

    // 3. Impulse response is symmetric with the peak at the impulse.
    {
        const int W = 33, H = 33, cx = 16, cy = 16;
        vector<float> src((size_t)W * H, 0.f), tmp((size_t)W * H), dst((size_t)W * H);
        src[(size_t)cy * W + cx] = 1.f;
        auto k = er::BuildGaussianKernel(6.f);
        er::GaussianBlurH(src.data(), tmp.data(), W, H, k);
        er::GaussianBlurV(tmp.data(), dst.data(), W, H, k);

        bool peakOK = true;
        double worstAsym = 0.0;
        const float peak = dst[(size_t)cy * W + cx];
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float v = dst[(size_t)y * W + x];
                if (v > peak + 1e-9f) peakOK = false;
                const int mx = 2 * cx - x, my = 2 * cy - y;
                if (mx >= 0 && mx < W && my >= 0 && my < H)
                    worstAsym = std::fmax(worstAsym,
                        std::fabs((double)v - (double)dst[(size_t)my * W + mx]));
            }
        char d[64]; snprintf(d, sizeof d, "worst asym=%.3g", worstAsym);
        Check(peakOK && worstAsym < 1e-6, "impulse-symmetry", d);
    }

    // 4. Linear ramp preserved in the interior (Gaussian is symmetric, so a
    //    linear function is its own blur away from clamped borders).
    {
        const int W = 64, H = 48;
        vector<float> src((size_t)W * H), tmp((size_t)W * H), dst((size_t)W * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                src[(size_t)y * W + x] = 0.01f * x + 0.02f * y;
        auto k = er::BuildGaussianKernel(8.f);
        const int half = (int)k.size() / 2;
        er::GaussianBlurH(src.data(), tmp.data(), W, H, k);
        er::GaussianBlurV(tmp.data(), dst.data(), W, H, k);
        double worst = 0.0;
        for (int y = half; y < H - half; ++y)
            for (int x = half; x < W - half; ++x)
                worst = std::fmax(worst, std::fabs(
                    (double)dst[(size_t)y * W + x] - (double)src[(size_t)y * W + x]));
        char d[64]; snprintf(d, sizeof d, "worst interior|diff|=%.3g", worst);
        Check(worst < 1e-5, "linear-interior", d);
    }

    // 5. Alpha-weighted low-pass: constant straight colour C with wildly
    //    varying alpha (including zero-alpha pixels holding garbage premult
    //    values that must NOT leak). blur(a*C')/blur(a) == C wherever
    //    blur(a) > 0, where C' = C at a>0 pixels and garbage at a==0 pixels
    //    (premult garbage at a==0 is zero by definition of premult, so the
    //    test stuffs zeros there — the property under test is the weighting
    //    of the a>0 pixels).
    {
        const int W = 40, H = 40;
        const float C = 0.63f;
        vector<float> pa((size_t)W * H), pc((size_t)W * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = (size_t)y * W + x;
                const float a = ((x * 5 + y * 3) % 7 == 0) ? 0.f
                              : 0.1f + 0.9f * (float)((x + 2 * y) % 11) / 11.f;
                pa[i] = a;
                pc[i] = a * C;          // premultiplied
            }
        auto k = er::BuildGaussianKernel(8.f);
        vector<float> tmp((size_t)W * H), ba((size_t)W * H), bc((size_t)W * H);
        er::GaussianBlurH(pa.data(), tmp.data(), W, H, k);
        er::GaussianBlurV(tmp.data(), ba.data(), W, H, k);
        er::GaussianBlurH(pc.data(), tmp.data(), W, H, k);
        er::GaussianBlurV(tmp.data(), bc.data(), W, H, k);

        double worst = 0.0;
        for (size_t i = 0; i < (size_t)W * H; ++i)
            if (ba[i] > 1e-6f)
                worst = std::fmax(worst, std::fabs((double)(bc[i] / ba[i]) - (double)C));
        char d[64]; snprintf(d, sizeof d, "worst|diff|=%.3g", worst);
        Check(worst < 1e-4, "alpha-weighted-divide", d);
    }

    printf(g_fail ? "\nRESULT: %d FAILED\n" : "\nRESULT: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
