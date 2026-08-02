/*  test_pullpush.cpp — headless unit test for MM_PullPush.h.

    Compiles with plain cl.exe, no AE SDK (via tests\build_tests.ps1).
    Exit code 0 = all pass, 1 = a failure.

    Validates the Stage 3 gate properties analytically:
      - constant trusted colour fills to EXACTLY that colour everywhere
        (averaging and bilinear interpolation preserve constants), including
        with varying alpha and odd (non-power-of-2) dimensions;
      - zero-alpha / invalid garbage pixels contribute NOTHING — bit-identical
        output whether invalid pixels hold garbage or zeros (the coverage-
        weighting property the gate names);
      - outputs stay inside the convex hull of the trusted colours (no over/
        undershoot), and filled pixels resemble the NEARBY trusted colour;
      - semi-transparent trusted pixels unpremultiply back to their straight
        colour;
      - degenerate cases: no valid pixel (returns false), single valid pixel,
        1-wide strips, full validity (input passes through exactly).
*/

#include "MM_PullPush.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

using std::vector;

static int g_fail = 0;

static void Check(bool ok, const char *name, const char *detail)
{
    printf("  [%-24s] %s%s%s\n", name, ok ? "OK" : "FAIL", detail[0] ? "  " : "", detail);
    if (!ok) ++g_fail;
}

// Build an ARGB buffer (STRAIGHT RGB + alpha — AE's effect-world convention,
// matching PullPushFill's input contract) from colour + alpha lambdas.
template <typename FA, typename FC>
static vector<float> MakeARGB(int W, int H, FA alphaAt, FC colourAt)
{
    vector<float> argb((size_t)W * H * 4, 0.f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = ((size_t)y * W + x) * 4;
            const float a = alphaAt(x, y);
            float rgb[3];
            colourAt(x, y, rgb);
            argb[i + 0] = a;
            argb[i + 1] = rgb[0];
            argb[i + 2] = rgb[1];
            argb[i + 3] = rgb[2];
        }
    return argb;
}

int main()
{
    printf("MM_PullPush fill test\n");

    // 1. No valid pixel — must return false.
    {
        const int W = 16, H = 16;
        vector<float>   argb((size_t)W * H * 4, 0.5f);
        vector<uint8_t> valid((size_t)W * H, 0);
        vector<float>   out((size_t)W * H * 3, -1.f);
        const bool r = mm::PullPushFill(argb.data(), valid.data(), out.data(), W, H);
        Check(!r && out[0] == -1.f, "no-valid", "returns false, output untouched");
    }

    // 2. Constant colour, circle of trusted pixels, VARYING alpha, odd dims —
    //    fill must be exactly the constant everywhere (within float eps).
    {
        const int W = 37, H = 29;
        const float C[3] = { 0.25f, 0.5f, 0.75f };
        auto argb = MakeARGB(W, H,
            [](int x, int y) { return 0.6f + 0.4f * (float)((x * 7 + y * 13) % 10) / 10.f; },
            [&](int, int, float *rgb) { rgb[0] = C[0]; rgb[1] = C[1]; rgb[2] = C[2]; });
        vector<uint8_t> valid((size_t)W * H, 0);
        const double cx = 18.0, cy = 14.0, r = 6.0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r)
                    valid[(size_t)y * W + x] = 1;

        vector<float> out((size_t)W * H * 3);
        const bool ok = mm::PullPushFill(argb.data(), valid.data(), out.data(), W, H);
        double worst = 0.0;
        for (size_t i = 0; i < (size_t)W * H * 3; ++i)
            worst = std::fmax(worst, std::fabs((double)out[i] - (double)C[i % 3]));
        char d[64]; snprintf(d, sizeof d, "worst|diff|=%.3g", worst);
        Check(ok && worst < 1e-4, "constant-odd-dims", d);
    }

    // 3. Garbage immunity: same trusted set, but invalid pixels stuffed with
    //    garbage RGBA vs zeroed — outputs must be BIT-IDENTICAL.
    {
        const int W = 40, H = 24;
        vector<uint8_t> valid((size_t)W * H, 0);
        for (int y = 8; y < 16; ++y)
            for (int x = 6; x < 30; ++x)
                valid[(size_t)y * W + x] = 1;

        auto colour = [](int x, int y, float *rgb) {
            rgb[0] = 0.2f + 0.01f * (x % 7);
            rgb[1] = 0.7f - 0.01f * (y % 5);
            rgb[2] = 0.4f;
        };
        auto argbClean = MakeARGB(W, H, [](int, int) { return 0.9f; }, colour);
        auto argbDirty = argbClean;

        // Zero the invalid pixels in "clean"; stuff garbage into "dirty".
        uint32_t st = 0xBEEF1234u;
        auto next = [&]() { st = st * 1664525u + 1013904223u; return (float)(st >> 8) / 16777216.f; };
        for (size_t i = 0; i < (size_t)W * H; ++i) {
            if (valid[i]) continue;
            for (int ch = 0; ch < 4; ++ch) {
                argbClean[i * 4 + ch] = 0.f;
                argbDirty[i * 4 + ch] = ch == 0 ? 0.f : next() * 10.f;  // zero alpha, garbage RGB
            }
        }

        vector<float> outClean((size_t)W * H * 3), outDirty((size_t)W * H * 3);
        mm::PullPushFill(argbClean.data(), valid.data(), outClean.data(), W, H);
        mm::PullPushFill(argbDirty.data(), valid.data(), outDirty.data(), W, H);
        const bool same = std::memcmp(outClean.data(), outDirty.data(),
                                      outClean.size() * sizeof(float)) == 0;
        Check(same, "garbage-immunity", "invalid-pixel garbage changes nothing");
    }

    // 4. Two colour blocks (red left, blue right), gap between. Outputs must
    //    stay in the convex hull (g==0, r+b==1, channels in [0,1]) and filled
    //    pixels must lean toward the NEARBY block.
    {
        const int W = 64, H = 33;
        auto argb = MakeARGB(W, H, [](int, int) { return 1.f; },
            [&](int x, int, float *rgb) {
                rgb[0] = x < 32 ? 1.f : 0.f;   // red left half
                rgb[1] = 0.f;
                rgb[2] = x < 32 ? 0.f : 1.f;   // blue right half
            });
        vector<uint8_t> valid((size_t)W * H, 0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (x < 8 || x >= 56) valid[(size_t)y * W + x] = 1;

        vector<float> out((size_t)W * H * 3);
        mm::PullPushFill(argb.data(), valid.data(), out.data(), W, H);

        double worstHull = 0.0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = ((size_t)y * W + x) * 3;
                const double r = out[i], g = out[i + 1], b = out[i + 2];
                worstHull = std::fmax(worstHull, std::fabs(g));            // g must stay 0
                worstHull = std::fmax(worstHull, std::fabs(r + b - 1.0));  // convex combo
                worstHull = std::fmax(worstHull, std::fmax(-r, r - 1.0));  // r in [0,1]
                worstHull = std::fmax(worstHull, std::fmax(-b, b - 1.0));
            }
        char d[64]; snprintf(d, sizeof d, "worst hull violation=%.3g", worstHull);
        Check(worstHull < 1e-4, "convex-hull", d);

        // Proximity: just outside each block, the fill should still be
        // dominated by that block's colour.
        const int yMid = H / 2;
        const size_t iL = ((size_t)yMid * W + 10) * 3;   // near red
        const size_t iR = ((size_t)yMid * W + 53) * 3;   // near blue
        const bool prox = out[iL] > out[iL + 2] && out[iR + 2] > out[iR];
        char d2[96]; snprintf(d2, sizeof d2, "near-red r=%.2f b=%.2f; near-blue r=%.2f b=%.2f",
                              out[iL], out[iL + 2], out[iR], out[iR + 2]);
        Check(prox, "proximity", d2);
    }

    // 5. Semi-transparent trusted pixels (alpha 0.5) — unpremultiply must
    //    recover the straight colour exactly.
    {
        const int W = 20, H = 20;
        const float C[3] = { 0.8f, 0.3f, 0.1f };
        auto argb = MakeARGB(W, H, [](int, int) { return 0.5f; },
            [&](int, int, float *rgb) { rgb[0] = C[0]; rgb[1] = C[1]; rgb[2] = C[2]; });
        vector<uint8_t> valid((size_t)W * H, 0);
        for (int y = 6; y < 14; ++y)
            for (int x = 6; x < 14; ++x) valid[(size_t)y * W + x] = 1;

        vector<float> out((size_t)W * H * 3);
        mm::PullPushFill(argb.data(), valid.data(), out.data(), W, H);
        double worst = 0.0;
        for (size_t i = 0; i < (size_t)W * H * 3; ++i)
            worst = std::fmax(worst, std::fabs((double)out[i] - (double)C[i % 3]));
        char d[64]; snprintf(d, sizeof d, "worst|diff|=%.3g", worst);
        Check(worst < 1e-4, "semi-transparent", d);
    }

    // 6. Full validity — output is the input's straight colour, exactly.
    {
        const int W = 31, H = 17;
        auto argb = MakeARGB(W, H,
            [](int x, int y) { return 0.5f + 0.5f * (float)((x + y) % 2); },
            [](int x, int y, float *rgb) {
                rgb[0] = (float)x / 31.f; rgb[1] = (float)y / 17.f; rgb[2] = 0.5f;
            });
        vector<uint8_t> valid((size_t)W * H, 1);
        vector<float> out((size_t)W * H * 3);
        mm::PullPushFill(argb.data(), valid.data(), out.data(), W, H);

        // Straight in → straight out: every valid pixel's own colour comes
        // back through the internal premult/unpremult round trip.
        double worst = 0.0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = (size_t)y * W + x;
                for (int ch = 0; ch < 3; ++ch)
                    worst = std::fmax(worst, std::fabs(
                        (double)out[i * 3 + ch] - (double)argb[i * 4 + 1 + ch]));
            }
        char d[64]; snprintf(d, sizeof d, "worst|diff|=%.3g", worst);
        Check(worst < 1e-5, "full-valid-passthrough", d);
    }

    // 7. Single valid pixel — its colour must fill the whole frame; also
    //    exercise 1-wide strips (degenerate mip chains).
    {
        const float C[3] = { 0.1f, 0.9f, 0.4f };
        struct { int W, H; } dims[] = { { 33, 21 }, { 1, 64 }, { 64, 1 }, { 1, 1 } };
        bool allOK = true;
        double worstAll = 0.0;
        for (const auto &dm : dims) {
            const int W = dm.W, H = dm.H;
            auto argb = MakeARGB(W, H, [](int, int) { return 1.f; },
                [&](int, int, float *rgb) { rgb[0] = C[0]; rgb[1] = C[1]; rgb[2] = C[2]; });
            vector<uint8_t> valid((size_t)W * H, 0);
            valid[(size_t)(H / 2) * W + (W / 3)] = 1;
            vector<float> out((size_t)W * H * 3);
            if (!mm::PullPushFill(argb.data(), valid.data(), out.data(), W, H)) {
                allOK = false;
                continue;
            }
            for (size_t i = 0; i < (size_t)W * H * 3; ++i)
                worstAll = std::fmax(worstAll, std::fabs((double)out[i] - (double)C[i % 3]));
        }
        char d[64]; snprintf(d, sizeof d, "worst|diff|=%.3g across 4 dims", worstAll);
        Check(allOK && worstAll < 1e-4, "single-seed-strips", d);
    }

    // 8. A "valid" pixel with alpha == 0 must be ignored (no colour info) —
    //    equivalent to marking it invalid.
    {
        const int W = 24, H = 24;
        const float C[3] = { 0.3f, 0.6f, 0.9f };
        auto argb = MakeARGB(W, H, [](int, int) { return 1.f; },
            [&](int, int, float *rgb) { rgb[0] = C[0]; rgb[1] = C[1]; rgb[2] = C[2]; });
        vector<uint8_t> valid((size_t)W * H, 0);
        for (int y = 10; y < 14; ++y)
            for (int x = 10; x < 14; ++x) valid[(size_t)y * W + x] = 1;

        // Poison one extra "valid" pixel: alpha 0, garbage premult RGB.
        const size_t poison = (size_t)2 * W + 2;
        valid[poison] = 1;
        argb[poison * 4 + 0] = 0.f;
        argb[poison * 4 + 1] = 999.f;
        argb[poison * 4 + 2] = -999.f;
        argb[poison * 4 + 3] = 999.f;

        vector<float> out((size_t)W * H * 3);
        mm::PullPushFill(argb.data(), valid.data(), out.data(), W, H);
        double worst = 0.0;
        for (size_t i = 0; i < (size_t)W * H * 3; ++i)
            worst = std::fmax(worst, std::fabs((double)out[i] - (double)C[i % 3]));
        char d[64]; snprintf(d, sizeof d, "worst|diff|=%.3g", worst);
        Check(worst < 1e-4, "zero-alpha-valid-guard", d);
    }

    printf(g_fail ? "\nRESULT: %d FAILED\n" : "\nRESULT: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
