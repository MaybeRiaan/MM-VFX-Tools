/*  MM_Composite.h — Edge Repair's Stage 4 per-pixel composite math, AE-free
    and header-only (unit-testable headlessly — see tests/test_composite.cpp).

    These are THE canonical formulas: a future GPU path must implement them
    bit-for-bit-close or CPU/GPU parity dies. Every mapping is documented
    here precisely for that lockstep contract.

    ── Blend weight ─────────────────────────────────────────────────────────
        w(d) = 1 - smoothstep(0, extend, d)
    d is the core distance field (0 inside the trusted core, growing
    outward). w = 1 just outside the core (full low-frequency replacement,
    where contamination lives), easing smoothly to 0 at d >= extend so the
    band's outer boundary is invisible. The core itself (d <= 0) is NOT part
    of the band — callers must passthrough there; BlendWeight returns 1 for
    d <= 0 only as the mathematical limit.

    ── In-band RGB composite (straight/unpremultiplied space) ───────────────
        out = lerp(low, ext, w) + lerp(1, detail, w) * hi
    where
        low    = alpha-weighted Gaussian low-pass of the plate's straight RGB
        ext    = pull–push extended trusted colour (straight)
        hi     = origStraight - low  (the plate's high-frequency texture;
                 callers must force hi = 0 where alpha ~ 0 — no plate
                 detail exists there)
        detail = Detail param / 100
    Behaviour: at the band's outer edge (w -> 0) out -> low + hi = orig
    exactly, for ANY detail value — the Detail attenuation is itself
    weighted by w, which is what makes "Detail 0 vs 100 differs only inside
    the blend band" hold with no seam. At the inner edge (w -> 1) the low
    band is fully replaced by extended colour and the plate texture rides on
    top scaled by detail.

    ── Alpha Choke (anchored at the ALPHA'S OWN EDGE, per 2026-07-09
       decision — NOT the core field, so the knob is independent of Erode
       and preserves the matte's softness profile) ──────────────────────────
    Choke IN (choke > 0), field dOut = Euclidean distance to the nearest
    zero-alpha (a <= eps) pixel:
        newA = a * clamp01(dOut - choke)
    Pixels deeper than choke+1 px from the outer edge are untouched
    (identity); the outermost `choke` px are removed with a 1 px soft ramp.
    Profile-preserving: motion-blur gradients survive except where shaved.

    Grow (choke < 0, g = -choke), field dSolid = Euclidean distance to the
    nearest SOLID (a > 0.5) pixel:
        newA = (dSolid <= 0) ? a : max(a, clamp01(g + 0.5 - dSolid))
    The solid matte dilates by g px with a 1 px soft rim; existing softer
    alpha survives via the max; solid-interior pixels are identity (the
    dSolid <= 0 gate — a semi-transparent solid region must not harden).
    v1 simplification: the grown rim fills toward full opacity even beside
    a semi-opaque solid (propagating the edge's own alpha needs a feature
    transform — revisit if semi-transparent solids matter).

    choke == 0 must be handled by the CALLER as a hard skip (no field, no
    remap) — that is what guarantees bit-identical alpha, not these ramps.
*/

#pragma once

namespace mm {

// w(d) = 1 - smoothstep(0, extend, d). See header comment — the canonical
// distance-band weight. extend <= 0 disables the band entirely.
inline float BlendWeight(float d, float extend)
{
    if (extend <= 0.f || d >= extend) return 0.f;
    if (d <= 0.f) return 1.f;
    const float s = d / extend;
    return 1.f - s * s * (3.f - 2.f * s);
}

// out = lerp(low, ext, w) + lerp(1, detail01, w) * hi  (straight RGB).
// hi must already be 0 where the plate has no colour info (alpha ~ 0).
inline void ComposeBandRGB(const float low[3], const float ext[3],
                           const float hi[3], float w, float detail01,
                           float out[3])
{
    const float dw = 1.f + (detail01 - 1.f) * w;
    for (int c = 0; c < 3; ++c)
        out[c] = low[c] + (ext[c] - low[c]) * w + dw * hi[c];
}

// newA = a * clamp01(dOut - choke). dOut = distance to nearest zero-alpha
// pixel. Identity for dOut >= choke + 1; zero for dOut <= choke.
inline float ChokeInAlpha(float a, float dOut, float choke)
{
    float t = dOut - choke;
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return a;
    return a * t;
}

// newA = (dSolid <= 0) ? a : max(a, clamp01(grow + 0.5 - dSolid)).
// dSolid = distance to nearest solid (a > 0.5) pixel; grow = |choke|.
inline float GrowAlpha(float a, float dSolid, float grow)
{
    if (dSolid <= 0.f) return a;      // inside the solid — never harden
    float t = grow + 0.5f - dSolid;
    if (t <= 0.f) return a;
    if (t > 1.f)  t = 1.f;
    return t > a ? t : a;
}

} // namespace mm
