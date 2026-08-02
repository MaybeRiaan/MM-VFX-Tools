/*  ER_Composite.h — Edge Repair's Stage 4 per-pixel composite math, AE-free
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
        base = lerp(origStraight, ext, w)
        out  = base + w * detail * grain
    where
        ext    = pull–push extended trusted colour (straight)
        detail = Detail param / 100
        grain  = a ZERO-MEAN synthetic luma sample (ER_Grain.h) already scaled
                 to the plate's local grain amplitude (estimated by the caller
                 from the trusted core), added identically to R, G, B.
    Behaviour: at the band's outer edge (w -> 0) out -> origStraight exactly,
    for ANY detail — the grain term is weighted by w, so the band boundary is
    seamless. At the inner edge (w -> 1) the base is the flat extended colour
    and synthetic grain rides on top scaled by detail.

    ── WHY grain, not the plate's own high frequencies (changed 2026-07-22) ──
    The previous method was frequency separation:
        out = lerp(low, ext, w) + lerp(1, detail, w) * (origStraight - low),
    low = gaussianBlur(origStraight). It cannot distinguish a real luminance
    EDGE from texture — at the matte boundary origStraight steps from subject
    to background/contamination colour, so hi = orig - low is a SIGNED edge
    gradient, and re-adding it inside the band darkened the edge like a
    multiply (worse on premultiplied input). That is a fundamental limit of
    frequency separation, not a tuning issue. Synthetic grain is zero-mean by
    construction and never sourced from orig's edge, so it can only ADD
    texture, never darken — and it removes the Detail Gaussian pass entirely.
    (History kept here as the CPU/GPU-lockstep record of what NOT to do.)

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

namespace er {

// smoothstep(0,1) helper on a pre-normalized t.
inline float ER_Smooth01(float t)
{
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    return t * t * (3.f - 2.f * t);
}

// Distance-band weight with an INNER ease (2026-07-22):
//   outer = 1 - smoothstep(0, extend, d)      // roll-off to 0 at d = extend
//   innerW = smoothstep(0, inner, d)          // ease UP from 0 at the core
//   w = innerW * outer
// `inner` (px) is the inner-soft width — over the first `inner` px out from the
// core, w ramps 0 -> 1 instead of snapping to full replacement at d -> 0. That
// blends the recomposited band back into the REAL core colour rather than
// leaving a hard line where band meets core (visible on wide light wraps). It
// reshapes the WEIGHT only — the distance field and every sample point are
// unchanged. inner <= 0 recovers the original hard-inner behaviour (w -> 1 at
// the core). Both ends go to 0, so the band stays seamless top and bottom.
inline float BlendWeight(float d, float extend, float inner)
{
    if (extend <= 0.f || d >= extend) return 0.f;
    if (d <= 0.f) return 0.f;                 // ease starts AT the core now
    const float s = d / extend;
    const float outer = 1.f - s * s * (3.f - 2.f * s);
    if (inner <= 0.f) return outer;           // original hard-inner behaviour
    const float innerW = ER_Smooth01(d / inner);
    return innerW * outer;
}

// Back-compat 2-arg form (hard inner) — kept so existing tests/GPU stubs that
// don't pass an inner width still compile to the original curve.
inline float BlendWeight(float d, float extend)
{
    if (extend <= 0.f || d >= extend) return 0.f;
    if (d <= 0.f) return 1.f;
    const float s = d / extend;
    return 1.f - s * s * (3.f - 2.f * s);
}

// base = lerp(orig, ext, w); out = base + w*detail01*grain  (straight RGB).
//   orig    : the plate's straight RGB at this pixel (3 ch)
//   ext     : pull–push extended trusted colour (3 ch)
//   grain   : zero-mean luma sample ALREADY scaled to the plate's amplitude
//             (same value added to all three channels — monochrome grain)
// At w->0 out->orig (seamless band edge) for any detail; at w->1 the base is
// the flat extended colour plus detail-scaled grain.
inline void ComposeBandRGB(const float orig[3], const float ext[3],
                           float grain, float w, float detail01,
                           float out[3])
{
    const float g = w * detail01 * grain;
    for (int c = 0; c < 3; ++c)
        out[c] = orig[c] + (ext[c] - orig[c]) * w + g;
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

} // namespace er
