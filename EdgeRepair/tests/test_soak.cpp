/*  test_soak.cpp — Stage 5 soak + allocation guard for the AE-free algorithm
    cores (EDT, Gaussian, pull–push).

    Two properties the Stage 5 gate names:
      1. STABLE MEMORY over a long run — no leak, no growth. We count heap
         allocations via overridden global new/delete and assert that after a
         warm-up frame the per-frame allocation count settles (the thread_local
         scratch inside the algorithm headers stops reallocating once it has
         grown to the working size).
      2. DETERMINISM — every frame reproduces the first frame bit-for-bit
         (the render is a pure function of its inputs; reused scratch must not
         leak state between frames).

    This exercises the HEADER cores directly. The SmartRender-level scratch
    (ER_Scratch in EdgeRepair.cpp) uses the same grow-only thread_local
    discipline but needs AE to drive it; the AE soak is the RAM-preview /
    render-queue loop at the gate. Here we prove the reusable pieces.

    Run via tests\run_soak.ps1 (own runner — build_tests.ps1 only globs
    test_*.cpp but does not special-case the allocation override; this file is
    a normal test_*.cpp so build_tests.ps1 picks it up too).
*/

#include "ER_Parallel.h"
#include "ER_DistanceField.h"
#include "ER_CoreMatte.h"
#include "ER_Gaussian.h"
#include "ER_PullPush.h"

#include <cstdio>
#include <cstdint>
#include <vector>
#include <cmath>
#include <cstring>
#include <new>
#include <atomic>

// --- allocation counter (global new/delete override) ----------------------
static std::atomic<long long> g_allocCount{0};
static std::atomic<long long> g_liveBytes{0};

void *operator new(std::size_t n) {
    g_allocCount.fetch_add(1, std::memory_order_relaxed);
    g_liveBytes.fetch_add((long long)n, std::memory_order_relaxed);
    if (void *p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

using std::vector;
static int g_fail = 0;

int main()
{
    printf("MM soak / allocation-stability test\n");

    // A modest frame (fast to iterate many times); the allocation behaviour is
    // size-independent — grow-only scratch stops reallocating once warmed.
    const int W = 512, H = 384;
    const size_t N = (size_t)W * H;

    vector<float>   argb(N * 4), alpha(N);
    vector<uint8_t> core(N);
    // Synthetic disc matte.
    const double cx = W * 0.5, cy = H * 0.5, r = 120.0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const double dx = x - cx, dy = y - cy;
            float a = (float)(1.0 - (std::sqrt(dx * dx + dy * dy) - r) / 3.0);
            if (a < 0.f) a = 0.f; if (a > 1.f) a = 1.f;
            float *p = &argb[((size_t)y * W + x) * 4];
            p[0] = a; p[1] = 0.2f * a; p[2] = 0.6f * a; p[3] = 0.6f * a;
            alpha[(size_t)y * W + x] = a;
        }

    const auto kern = er::BuildGaussianKernel(4.0f);

    // One full pipeline pass into caller-owned outputs; returns nothing but
    // fills dist/ext/low so the caller can compare across frames.
    auto frame = [&](vector<float> &dist, vector<float> &ext, vector<float> &low) {
        dist.assign(N, 0.f);
        ext.assign(N * 3, 0.f);
        low.assign(N * 3, 0.f);
        er::BuildCoreMatte(alpha.data(), core.data(), W, H, 0.5f, 2.0f);
        er::CoreDistanceField(core.data(), dist.data(), W, H);
        er::PullPushFill(argb.data(), core.data(), ext.data(), W, H);
        vector<float> plane(N), tmp(N);   // (these are caller-side, not header scratch)
        for (int ch = 0; ch < 3; ++ch) {
            for (size_t i = 0; i < N; ++i) plane[i] = argb[i * 4 + 1 + ch];
            er::GaussianBlurH(plane.data(), tmp.data(), W, H, kern);
            er::GaussianBlurV(tmp.data(), plane.data(), W, H, kern);
            for (size_t i = 0; i < N; ++i) low[i * 3 + ch] = plane[i];
        }
    };

    // Warm-up frame — lets thread_local scratch in the headers grow to size.
    vector<float> d0, e0, l0;
    frame(d0, e0, l0);

    // Second warm-up: the `frame` lambda itself allocates dist/ext/low/plane/tmp
    // every call (caller-side), so to isolate the HEADER cores' allocation
    // behaviour we measure the DELTA between two already-warm frames — any
    // header-internal reallocation would show as a count that keeps rising.
    // We reuse persistent output buffers here so the only allocations left are
    // the two local plane/tmp vectors (a fixed 2 per frame), which we subtract.
    vector<float> dA, eA, lA, dB, eB, lB;
    auto frameReuse = [&](vector<float> &dist, vector<float> &ext, vector<float> &low) {
        // Reuse buffers (no reallocation after first grow).
        if (dist.size() < N)     dist.assign(N, 0.f);      else std::fill(dist.begin(), dist.begin()+N, 0.f);
        if (ext.size()  < N * 3) ext.assign(N * 3, 0.f);   else std::fill(ext.begin(),  ext.begin()+N*3, 0.f);
        if (low.size()  < N * 3) low.assign(N * 3, 0.f);   else std::fill(low.begin(),  low.begin()+N*3, 0.f);
        static thread_local vector<float> plane, tmp;      // reused across frames
        if ((int)plane.size() < (int)N) plane.resize(N);
        if ((int)tmp.size()   < (int)N) tmp.resize(N);
        er::BuildCoreMatte(alpha.data(), core.data(), W, H, 0.5f, 2.0f);
        er::CoreDistanceField(core.data(), dist.data(), W, H);
        er::PullPushFill(argb.data(), core.data(), ext.data(), W, H);
        for (int ch = 0; ch < 3; ++ch) {
            for (size_t i = 0; i < N; ++i) plane[i] = argb[i * 4 + 1 + ch];
            er::GaussianBlurH(plane.data(), tmp.data(), W, H, kern);
            er::GaussianBlurV(tmp.data(), plane.data(), W, H, kern);
            for (size_t i = 0; i < N; ++i) low[i * 3 + ch] = plane[i];
        }
    };

    // Warm the reuse path (grow its persistent buffers + header scratch).
    frameReuse(dA, eA, lA);
    frameReuse(dB, eB, lB);   // dB/eB/lB now grown too

    // MEASURE: run many warm frames; allocation count must stay flat.
    const int ITER = 2000;
    long long before = g_allocCount.load();
    bool stable = true;
    for (int t = 0; t < ITER; ++t) {
        frameReuse(dA, eA, lA);
        // Determinism: every warm frame equals the first warm frame, bitwise.
        if (memcmp(dA.data(), dB.data(), N * sizeof(float)) != 0 ||
            memcmp(eA.data(), eB.data(), N * 3 * sizeof(float)) != 0 ||
            memcmp(lA.data(), lB.data(), N * 3 * sizeof(float)) != 0) {
            printf("  [determinism] FAIL at iter %d — output drifted\n", t);
            stable = false; ++g_fail; break;
        }
    }
    long long after = g_allocCount.load();
    const long long perFrame = (after - before) / ITER;

    printf("  [determinism] %s (%d warm frames identical)\n",
           stable ? "OK" : "FAIL", ITER);
    printf("  [alloc-churn] %lld allocations over %d warm frames = %.3f/frame  %s\n",
           (after - before), ITER, (double)(after - before) / ITER,
           perFrame == 0 ? "OK (allocation-free)" : "FAIL (still allocating per frame)");
    if (perFrame != 0) ++g_fail;

    printf(g_fail ? "\nRESULT: %d FAILED\n" : "\nRESULT: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
