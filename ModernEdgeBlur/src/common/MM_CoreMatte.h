/*  MM_CoreMatte.h — core-matte construction and the driving distance field,
    AE-free and header-only (unit-testable headlessly).

    Two steps, both built on the exact EDT in MM_DistanceField.h:

    1. BuildCoreMatte: threshold the layer alpha into a binary interior, then
       apply the SIGNED Erode. Erode > 0 shrinks the interior inward by that
       many pixels; Erode < 0 grows it outward. Rather than iterative 3x3
       erosion (banned — accumulates error and is O(radius) passes), we move
       the boundary with a distance field:
         - shrink by E: a pixel stays core iff its distance to the NEAREST
           NON-interior pixel is > E (it's deeper than E inside).
         - grow by |E|: a non-interior pixel becomes core iff its distance to
           the nearest interior pixel is <= |E|.
       Sub-pixel Erode is honoured because the distance is continuous.

    2. CoreDistanceField: the single per-frame field d(x,y) = Euclidean
       distance from each pixel to the nearest CORE pixel (0 inside the core).
       This one field drives everything downstream — extension falloff band,
       blend weight, and alpha choke all read it. There is exactly one such
       computation per frame; the erosion transform above is a separate,
       earlier step that only exists to build the core the field measures from.
*/

#pragma once

#include "MM_DistanceField.h"

#include <vector>
#include <cstdint>

namespace mm {

// Threshold + signed erode → binary core matte (1 = core, 0 = not).
//   alpha : W*H straight-alpha plane in [0,1].
//   core  : W*H, receives 0/1.
//   thresh: alpha above this is "interior" before eroding (0.5 default).
//   erode : signed px. >0 shrinks, <0 grows, ~0 leaves the threshold as-is.
inline void BuildCoreMatte(const float *alpha, uint8_t *core, int W, int H,
                           float thresh, float erode)
{
    const size_t N = (size_t)W * H;
    // thread_local grow-only scratch — reused across frames on the render
    // worker thread with no reallocation (Stage 5), per-thread so MFR-safe.
    // All fully written before read below.
    static thread_local std::vector<uint8_t> interior;
    if (interior.size() < N) interior.resize(N);
    for (size_t i = 0; i < N; ++i) interior[i] = alpha[i] > thresh ? 1 : 0;

    // No erode → the thresholded interior is the core.
    if (erode > -0.01f && erode < 0.01f) {
        for (size_t i = 0; i < N; ++i) core[i] = interior[i];
        return;
    }

    static thread_local std::vector<float> dist;
    if (dist.size() < N) dist.resize(N);

    if (erode > 0.f) {
        // Shrink: seeds are NON-interior pixels; keep only interior pixels
        // deeper than `erode` from the nearest edge.
        static thread_local std::vector<uint8_t> nonint;
        if (nonint.size() < N) nonint.resize(N);
        for (size_t i = 0; i < N; ++i) nonint[i] = interior[i] ? 0 : 1;

        // Degenerate: no non-interior pixel anywhere (fully opaque frame) —
        // nothing to erode toward, the whole frame stays core.
        bool anySeed = false;
        for (size_t i = 0; i < N; ++i) if (nonint[i]) { anySeed = true; break; }
        if (!anySeed) { for (size_t i = 0; i < N; ++i) core[i] = interior[i]; return; }

        EuclideanDistanceTransform(nonint.data(), dist.data(), W, H);
        for (size_t i = 0; i < N; ++i)
            core[i] = (interior[i] && dist[i] > erode) ? 1 : 0;
    } else {
        // Grow by |erode|: seeds are interior pixels; a pixel is core if it's
        // within |erode| of the interior (includes the interior itself, dist 0).
        const float g = -erode;

        bool anySeed = false;
        for (size_t i = 0; i < N; ++i) if (interior[i]) { anySeed = true; break; }
        if (!anySeed) { for (size_t i = 0; i < N; ++i) core[i] = 0; return; }

        EuclideanDistanceTransform(interior.data(), dist.data(), W, H);
        for (size_t i = 0; i < N; ++i)
            core[i] = (dist[i] <= g) ? 1 : 0;
    }
}

// The single driving field: d(x,y) = distance to the nearest core pixel.
//   core : W*H 0/1 from BuildCoreMatte.
//   dist : W*H, receives Euclidean distance (0 inside the core).
// Returns false if the core is empty (no seeds) — caller must treat the
// field as "infinitely far" everywhere and typically fall back to passthrough.
inline bool CoreDistanceField(const uint8_t *core, float *dist, int W, int H)
{
    const size_t N = (size_t)W * H;
    bool any = false;
    for (size_t i = 0; i < N; ++i) if (core[i]) { any = true; break; }
    if (!any) return false;

    EuclideanDistanceTransform(core, dist, W, H);
    return true;
}

} // namespace mm
