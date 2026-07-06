/*  MM_DistanceField.h — exact Euclidean distance transform, AE-free.

    Header-only, no PF/SDK types, so it compiles and unit-tests headlessly
    (see tests/test_distancefield.cpp). This is the single distance primitive
    the whole Edge Repair pipeline is built on: erode/dilate of the core
    matte, extension falloff band, blend weight, and alpha choke all read the
    same field — there is exactly ONE distance computation per frame.

    Algorithm: Felzenszwalb & Huttenlocher, "Distance Transforms of Sampled
    Functions" (2012). Exact Euclidean, O(n) per row/column, two separable
    passes (columns then rows) over SQUARED distance. Each 1-D pass computes
    the lower envelope of a set of parabolas; rows are independent of each
    other and columns are independent of each other, so both passes are
    directly parallelizable (the AE build fans them out per line — the pure
    core here stays serial and dependency-free for testability).

    Convention: the transform measures, for every pixel, the Euclidean
    distance to the nearest SEED pixel (a pixel where seed[i] != 0). Non-seed
    pixels get their true distance; seed pixels get 0. Callers choose what a
    seed means — for the core matte, seeds are the trusted-interior pixels, so
    d(x,y) is "distance from here to the nearest core pixel."
*/

#pragma once

#include <vector>
#include <cmath>
#include <cstdint>
#include <limits>
#include <algorithm>

namespace mm {

// Sentinel for "no seed in this line yet" — squared-distance space, so it
// must be large enough that (INF + n^2) never overflows for realistic n.
// 1e20 leaves >18 orders of headroom below double's max; distances are at
// most ~(width+height)^2 ~ 1e8 for 10k frames.
inline double DT_INF() { return 1e20; }

// One 1-D pass of the F&H lower-envelope transform, in place on `f` (squared
// distances). f[i] is the current best squared distance at position i BEFORE
// this pass considers the perpendicular direction; on return f[i] is updated
// to min over q of (f[q] + (i-q)^2). n is the line length. v[] and z[] are
// caller-provided scratch of length n and n+1 — passing them in lets the AE
// build reuse per-thread scratch instead of allocating per line.
inline void DT_1D(double *f, int n, int *v, double *z)
{
    int k = 0;
    v[0] = 0;
    z[0] = -DT_INF();
    z[1] =  DT_INF();
    for (int q = 1; q < n; ++q) {
        // Intersection of the parabola from q with the one currently on top.
        double s;
        for (;;) {
            const int    p  = v[k];
            s = ((f[q] + (double)q * q) - (f[p] + (double)p * p))
                / (2.0 * (double)(q - p));
            if (s <= z[k]) {
                // q's parabola dominates back past z[k]; pop and retry.
                --k;
                if (k < 0) { k = 0; break; }
            } else {
                break;
            }
        }
        ++k;
        v[k]   = q;
        z[k]   = s;
        z[k+1] = DT_INF();
    }

    // Walk the envelope left→right, reading off the min at each i.
    // Use a temp copy of f's parabola heights so we don't clobber values we
    // still need (f[v[k]] is read while writing f[i]).
    static thread_local std::vector<double> fq;
    if ((int)fq.size() < n) fq.resize(n);
    for (int i = 0; i < n; ++i) fq[i] = f[i];

    k = 0;
    for (int i = 0; i < n; ++i) {
        while (z[k + 1] < (double)i) ++k;
        const int    p  = v[k];
        const double di = (double)i - (double)p;
        f[i] = di * di + fq[p];
    }
}

// Exact Euclidean distance transform.
//   seed : W*H, row-major. seed[y*W+x] != 0 marks a seed pixel.
//   out  : W*H, receives the Euclidean (not squared) distance to the nearest
//          seed. Seed pixels get 0. If there are NO seeds anywhere, every
//          output is +inf-ish (sqrt(DT_INF)); callers should guard that case.
// The two passes run over squared distance; the final sqrt makes it Euclidean.
template <typename SeedT>
inline void EuclideanDistanceTransform(const SeedT *seed, float *out, int W, int H)
{
    std::vector<double> f((size_t)W * H);

    // Seed → initial squared distance: 0 at seeds, +INF elsewhere.
    for (size_t i = 0, n = (size_t)W * H; i < n; ++i)
        f[i] = seed[i] ? 0.0 : DT_INF();

    const int maxdim = W > H ? W : H;
    std::vector<int>    v(maxdim);
    std::vector<double> z(maxdim + 1);
    std::vector<double> col(H);

    // Pass 1 — columns (vertical).
    for (int x = 0; x < W; ++x) {
        for (int y = 0; y < H; ++y) col[y] = f[(size_t)y * W + x];
        DT_1D(col.data(), H, v.data(), z.data());
        for (int y = 0; y < H; ++y) f[(size_t)y * W + x] = col[y];
    }

    // Pass 2 — rows (horizontal), in place on each row.
    for (int y = 0; y < H; ++y) {
        DT_1D(f.data() + (size_t)y * W, W, v.data(), z.data());
    }

    for (size_t i = 0, n = (size_t)W * H; i < n; ++i)
        out[i] = (float)std::sqrt(f[i]);
}

// Brute-force O(n^2) reference EDT — for tests only, never ship in a render
// path. Same contract as EuclideanDistanceTransform.
template <typename SeedT>
inline void EuclideanDistanceTransform_BruteForce(const SeedT *seed, float *out, int W, int H)
{
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            double best = DT_INF();
            for (int sy = 0; sy < H; ++sy) {
                for (int sx = 0; sx < W; ++sx) {
                    if (!seed[(size_t)sy * W + sx]) continue;
                    const double dx = x - sx, dy = y - sy;
                    const double d2 = dx * dx + dy * dy;
                    if (d2 < best) best = d2;
                }
            }
            out[(size_t)y * W + x] = (float)std::sqrt(best);
        }
    }
}

} // namespace mm
