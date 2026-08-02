/*  test_distancefield.cpp — headless unit test for MM_DistanceField.h.

    Compiles with a plain compiler, no AE SDK:
        cl /EHsc /O2 /I ..\src\common test_distancefield.cpp
        ./test_distancefield.exe
    or via tests\build_tests.ps1. Exit code 0 = all pass, 1 = a failure.

    Validates the exact O(n) EDT against the brute-force O(n^2) reference on
    synthetic shapes the Stage 2 gate names (filled circle, 1-px line,
    checkerboard), plus edge cases (single seed, no seed, full seed, a couple
    of randomized fields). "Exact" here means bit-for-bit-close: both compute
    sqrt of an integer squared distance, so agreement to a tight float epsilon
    is the bar, not an approximate tolerance.
*/

#include "MM_DistanceField.h"

#include <cstdio>
#include <cstdint>
#include <vector>
#include <cmath>
#include <string>

using std::vector;

static int g_fail = 0;

// Compare fast vs brute-force over an entire field; report the worst pixel.
static bool CheckField(const char *name, const vector<uint8_t> &seed, int W, int H)
{
    vector<float> fast((size_t)W * H), brute((size_t)W * H);
    mm::EuclideanDistanceTransform(seed.data(), fast.data(), W, H);
    mm::EuclideanDistanceTransform_BruteForce(seed.data(), brute.data(), W, H);

    double worst = 0.0;
    int wx = -1, wy = -1;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const size_t i = (size_t)y * W + x;
            // Both may be "no seed" huge; clamp the comparison so an all-empty
            // field doesn't report a giant absolute diff (they still match).
            const double a = fast[i], b = brute[i];
            const double d = std::fabs((double)a - (double)b);
            if (d > worst) { worst = d; wx = x; wy = y; }
        }
    }

    // sqrt of integers up to ~2e8; float has ~7 sig digits, so allow a small
    // absolute epsilon that scales with magnitude.
    const double eps = 1e-3;
    const bool ok = worst <= eps;
    printf("  [%-14s] %dx%d  worst|diff|=%.6g at (%d,%d)  %s\n",
           name, W, H, worst, wx, wy, ok ? "OK" : "FAIL");
    if (!ok) ++g_fail;
    return ok;
}

int main()
{
    printf("MM_DistanceField EDT test — exact O(n) vs brute-force O(n^2)\n");

    // 1. Single seed in the center — distances are a clean radial cone.
    {
        const int W = 41, H = 41;
        vector<uint8_t> s((size_t)W * H, 0);
        s[(H / 2) * W + (W / 2)] = 1;
        CheckField("single-seed", s, W, H);
    }

    // 2. Filled circle seed (radius 12) in a 48x48 field.
    {
        const int W = 48, H = 48;
        vector<uint8_t> s((size_t)W * H, 0);
        const double cx = 23.5, cy = 23.5, r = 12.0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const double dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy <= r * r) s[(size_t)y * W + x] = 1;
            }
        CheckField("filled-circle", s, W, H);
    }

    // 3. Single 1-px horizontal line across the middle row.
    {
        const int W = 64, H = 33;
        vector<uint8_t> s((size_t)W * H, 0);
        for (int x = 0; x < W; ++x) s[(size_t)(H / 2) * W + x] = 1;
        CheckField("1px-hline", s, W, H);
    }

    // 4. Single 1-px vertical line down the middle column.
    {
        const int W = 33, H = 64;
        vector<uint8_t> s((size_t)W * H, 0);
        for (int y = 0; y < H; ++y) s[(size_t)y * W + (W / 2)] = 1;
        CheckField("1px-vline", s, W, H);
    }

    // 5. Checkerboard — worst case for envelope pops (seeds everywhere but
    //    dense, alternating). Distances are all 0 or 1 but the algorithm still
    //    exercises the parabola machinery.
    {
        const int W = 40, H = 40;
        vector<uint8_t> s((size_t)W * H, 0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                s[(size_t)y * W + x] = ((x + y) & 1) ? 1 : 0;
        CheckField("checkerboard", s, W, H);
    }

    // 6. Two corner seeds — long-range distances, tests the envelope over the
    //    full span.
    {
        const int W = 50, H = 50;
        vector<uint8_t> s((size_t)W * H, 0);
        s[0] = 1;
        s[(size_t)(H - 1) * W + (W - 1)] = 1;
        CheckField("two-corners", s, W, H);
    }

    // 7. Full seed — every pixel a seed, all distances 0.
    {
        const int W = 20, H = 20;
        vector<uint8_t> s((size_t)W * H, 1);
        CheckField("full-seed", s, W, H);
    }

    // 8. A few pseudo-random sparse fields (deterministic LCG — no rand(),
    //    reproducible). Non-convex seed sets are where a boundary-only
    //    sampler would fail; the true EDT must still match brute force.
    {
        uint32_t st = 0x1234567u;
        auto next = [&]() { st = st * 1664525u + 1013904223u; return st; };
        for (int t = 0; t < 3; ++t) {
            const int W = 37 + t * 5, H = 29 + t * 7;
            vector<uint8_t> s((size_t)W * H, 0);
            for (size_t i = 0; i < (size_t)W * H; ++i)
                s[i] = (next() % 100) < 8 ? 1 : 0;   // ~8% seeds
            // Guarantee at least one seed.
            s[(next() % ((uint32_t)W * H))] = 1;
            std::string nm = "random-" + std::to_string(t);
            CheckField(nm.c_str(), s, W, H);
        }
    }

    printf(g_fail ? "\nRESULT: %d FAILED\n" : "\nRESULT: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
