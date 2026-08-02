/*  perf_pipeline.cpp — headless timing harness for Edge Repair's hot passes.

    Not a pass/fail unit test — a benchmark. Builds a synthetic 4K keyed-matte
    scene and times the three O(n) hot passes (EDT, separable Gaussian x3
    channels, pull–push fill) that Stage 5 parallelizes, plus a full composite
    approximation, under two executors:
      - SERIAL   : er::ParallelFor default (threadIndex 0), the tests' path.
      - THREADED : a std::thread backend installed via SetParallelBackend,
                   simulating AE's iterate_generic fan-out so we can measure
                   the speedup without launching After Effects.

    Also verifies the two executors produce BYTE-IDENTICAL output — the Stage 5
    discipline that threading must not change a single result bit.

    Build/run via tests\build_tests.ps1 is skipped (that only globs test_*.cpp);
    run directly, e.g. from an x64 dev prompt in a space-free temp copy:
        cl /EHsc /O2 /std:c++17 perf_pipeline.cpp && perf_pipeline.exe
*/

#include "ER_Parallel.h"
#include "ER_DistanceField.h"
#include "ER_CoreMatte.h"
#include "ER_Gaussian.h"
#include "ER_PullPush.h"

#include <cstdio>
#include <cstdint>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <cmath>
#include <cstring>

using std::vector;
using Clock = std::chrono::high_resolution_clock;

static double ms_since(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// A std::thread backend that mimics iterate_generic: it slices [0,n) across
// hardware threads and passes each a stable threadIndex. Static work-stealing
// via an atomic counter keeps load balanced for the uneven pull–push levels.
static er::ParallelBackend MakeThreadBackend(int nThreads)
{
    return [nThreads](int n, const std::function<void(int, int)> &body) {
        if (n <= 0) return;
        int T = nThreads;
        if (T > n) T = n;
        if (T <= 1) { for (int i = 0; i < n; ++i) body(0, i); return; }

        std::atomic<int> next{0};
        auto worker = [&](int tid) {
            for (;;) {
                int i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= n) break;
                body(tid, i);
            }
        };
        vector<std::thread> pool;
        pool.reserve(T);
        for (int t = 1; t < T; ++t) pool.emplace_back(worker, t);
        worker(0);
        for (auto &th : pool) th.join();
    };
}

// Build a synthetic straight-alpha ARGB scene: a soft-edged disc matte with
// coloured interior + contaminated rim, on a coloured background.
static void BuildScene(vector<float> &argb, int W, int H)
{
    argb.assign((size_t)W * H * 4, 0.f);
    const double cx = W * 0.5, cy = H * 0.5;
    const double r  = std::min(W, H) * 0.35;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const double dx = x - cx, dy = y - cy;
            const double d  = std::sqrt(dx * dx + dy * dy);
            float a = (float)(1.0 - (d - r) / 3.0);   // 3px soft edge
            if (a < 0.f) a = 0.f; if (a > 1.f) a = 1.f;
            float *p = &argb[((size_t)y * W + x) * 4];
            // interior teal, rim tinted by leftover background (magenta)
            const float body[3] = { 0.10f, 0.55f, 0.55f };
            const float bg[3]   = { 0.60f, 0.10f, 0.45f };
            p[0] = a;
            for (int c = 0; c < 3; ++c) p[1 + c] = body[c] * a + bg[c] * (1.f - a);
        }
    }
}

int main()
{
    const int W = 3840, H = 2160;   // 4K
    const size_t N = (size_t)W * H;
    printf("Edge Repair hot-pass benchmark @ %dx%d (%.1f MP)\n",
           W, H, N / 1e6);
    const unsigned hw = std::thread::hardware_concurrency();
    printf("hardware_concurrency = %u\n\n", hw);

    vector<float> argb;
    BuildScene(argb, W, H);

    // Core matte + alpha plane (inputs to the hot passes).
    vector<float>   alpha(N);
    vector<uint8_t> core(N);
    for (size_t i = 0; i < N; ++i) alpha[i] = argb[i * 4 + 0];

    auto run = [&](const char *label, er::ParallelBackend backend,
                   vector<float> &distOut, vector<float> &lowOut,
                   vector<float> &extOut) {
        er::ScopedParallelBackend guard(backend);

        // BuildCoreMatte (EDT inside for erode) + CoreDistanceField (the driving EDT)
        auto t0 = Clock::now();
        er::BuildCoreMatte(alpha.data(), core.data(), W, H, 0.5f, 2.0f);
        const double tCore = ms_since(t0);

        distOut.assign(N, 0.f);
        t0 = Clock::now();
        er::CoreDistanceField(core.data(), distOut.data(), W, H);
        const double tEDT = ms_since(t0);

        // Pull–push fill
        extOut.assign(N * 3, 0.f);
        t0 = Clock::now();
        er::PullPushFill(argb.data(), core.data(), extOut.data(), W, H);
        const double tPP = ms_since(t0);

        // Detail low-pass: separable Gaussian on 3 channels, radius 4
        lowOut.assign(N * 3, 0.f);
        const auto kern = er::BuildGaussianKernel(4.0f);
        vector<float> plane(N), tmp(N);
        t0 = Clock::now();
        for (int ch = 0; ch < 3; ++ch) {
            for (size_t i = 0; i < N; ++i) plane[i] = argb[i * 4 + 1 + ch];
            er::GaussianBlurH(plane.data(), tmp.data(), W, H, kern);
            er::GaussianBlurV(tmp.data(), plane.data(), W, H, kern);
            for (size_t i = 0; i < N; ++i) lowOut[i * 3 + ch] = plane[i];
        }
        const double tG = ms_since(t0);

        const double tot = tCore + tEDT + tPP + tG;
        printf("  %-9s  core+erode %7.1f  EDT %7.1f  pull-push %7.1f  gauss3 %7.1f  | total %7.1f ms\n",
               label, tCore, tEDT, tPP, tG, tot);
        return tot;
    };

    vector<float> dS, lS, eS, dT, lT, eT;
    printf("Timings (lower is better):\n");
    const double totS = run("SERIAL", er::ParallelBackend{}, dS, lS, eS);
    const int T = hw ? (int)hw : 8;
    const double totT = run("THREADED", MakeThreadBackend(T), dT, lT, eT);
    printf("\n  speedup (total) = %.2fx on %d threads\n\n", totS / totT, T);

    // Byte-identical check between the two executors.
    auto bitcmp = [](const char *nm, const vector<float> &a, const vector<float> &b) {
        if (a.size() != b.size()) { printf("  [%s] SIZE MISMATCH\n", nm); return false; }
        size_t diffs = 0; double worst = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            if (memcmp(&a[i], &b[i], sizeof(float)) != 0) {
                ++diffs;
                double d = std::fabs((double)a[i] - (double)b[i]);
                if (d > worst) worst = d;
            }
        }
        printf("  [%s] %s  (%zu differing floats, worst |diff|=%.3g)\n",
               nm, diffs == 0 ? "BYTE-IDENTICAL" : "DIFFERS", diffs, worst);
        return diffs == 0;
    };
    printf("Serial vs threaded output:\n");
    bool ok = true;
    ok &= bitcmp("dist", dS, dT);
    ok &= bitcmp("low ", lS, lT);
    ok &= bitcmp("ext ", eS, eT);

    printf(ok ? "\nRESULT: threading is byte-identical\n"
              : "\nRESULT: THREADING CHANGED OUTPUT — BUG\n");
    return ok ? 0 : 1;
}
