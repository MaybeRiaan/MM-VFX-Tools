/*  ER_Grain.h — deterministic synthetic grain for Edge Repair's Detail, AE-free
    and header-only (unit-testable headlessly).

    WHY grain instead of the plate's own high frequencies: the old Detail split
    (hi = orig - gaussianBlur(orig)) cannot tell a real luminance EDGE from
    texture — both are the same spatial frequency at the blur radius. At the
    matte boundary orig steps from subject colour to background/contamination
    colour, so `hi` there is a SIGNED edge gradient, and re-adding it inside the
    band paints that gradient back on → a directional DARKENING / multiply-like
    fringe (and, on premultiplied input, worse). Frequency separation cannot
    fix this; it is a fundamental limit (see EdgeRepair.md Stage-4 notes).

    Synthetic grain sidesteps it entirely: the noise is ZERO-MEAN by
    construction and never sourced from orig's edge, so it can only add texture,
    never darken. It also drops the Detail Gaussian pass completely.

    The noise is a hash of (x, y) — a pure function of pixel position, so it is
    deterministic (stable frame to frame), needs no state (MFR-safe), and is
    identical serial or threaded. Value is centred: mean ~0, range ~[-1, 1],
    roughly uniform (good enough for fine luma grain; a Gaussian-shaped variate
    would be marginally more film-like but uniform reads fine at ~1px).

    Monochrome/luma grain: callers add the SAME sample to R, G, B (chosen 2026-
    07-22 — neutral, no chroma speckle). Amplitude is set by the caller
    (estimated from the trusted core, see ER_CoreMatte/EdgeRepair.cpp), so this
    header only provides the zero-mean unit-scale field.

    No PF/SDK types — pure function, headlessly testable.
*/

#pragma once

#include <cstdint>

namespace er {

// Integer hash (Wang/xxhash-style avalanche) → uint32. Pure, fast, good bit
// mixing so adjacent pixels decorrelate (no visible lattice).
inline uint32_t GrainHash(uint32_t x, uint32_t y)
{
    uint32_t h = x * 0x9E3779B1u ^ y * 0x85EBCA77u;
    h ^= h >> 15; h *= 0xD168AAADu;
    h ^= h >> 13; h *= 0xAF723597u;
    h ^= h >> 16;
    return h;
}

// Zero-mean grain sample at integer pixel (x, y): a pure function of position,
// in roughly [-1, 1] with mean ~0. `seed` lets callers decorrelate multiple
// fields if ever needed (0 is fine for the single luma field).
inline float GrainAt(int x, int y, uint32_t seed = 0u)
{
    const uint32_t h = GrainHash((uint32_t)x + seed * 0x1000193u, (uint32_t)y);
    // Map to [0,1) then centre to [-1,1). 2^-24 keeps it in float's exact range.
    const float u = (float)(h >> 8) * (1.0f / 16777216.0f);   // [0,1)
    return u * 2.0f - 1.0f;                                    // [-1,1)
}

} // namespace er
