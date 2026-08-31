/*  ER_Composite.h — Edge Repair's Stage 4 per-pixel composite math, AE-free
    and header-only (unit-testable headlessly — see tests/test_composite.cpp).

    These are THE canonical formulas: a future GPU path must implement them
    bit-for-bit-close or CPU/GPU parity dies. Every mapping is documented
    here precisely for that lockstep contract.

    ── Blend weight ──────────────────────────────────────────
        outer  = d <= gap ? 1 : 1 - smoothstep(0, extend, d - gap)
        innerW = smoothstep(0, inner, d)
        w      = innerW * outer
    `d` is the core distance field (0 inside the trusted core, growing
    outward), `gap` the erode gap between the core boundary and the ORIGINAL
    ALPHA EDGE. The gap carries full weight and Extend measures its roll-off
    from the ALPHA EDGE, so the outward reach past the matte is Erode-
    independent (fixed 2026-08-24). The inner ease is unchanged and anchored at
    the core: w ramps 0 -> 1 over the first `inner` px out from it. The core
    itself (d <= 0) is NOT part of the band — callers may passthrough there.

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

// Distance-band weight: a gap plateau, an Extend roll-off measured from the
// ALPHA EDGE, and the original inner ease at the core.
//
//   d      core distance field: 0 inside the trusted core, growing outward
//          through the erode gap, past the alpha edge, into the exterior.
//   gap    erode gap width (px) — the untrusted rim between the core boundary
//          (d = 0) and the ORIGINAL ALPHA EDGE (d = gap). Pass max(Erode, 0);
//          a grown core (Erode < 0) has no gap.
//   extend outward roll-off width (px), measured FROM THE ALPHA EDGE.
//   inner  inner-ease width (px), measured outward from the core.
//
// EXTEND ANCHORING (fixed 2026-08-24). The gap carries full weight and the
// roll-off starts at the alpha edge, so the reach past the matte is `extend` px
// whatever Erode is. It used to start at the CORE, spending its first `Erode`
// px just crossing the gap — Erode 10 + Extend 32 reached only 22 px past the
// alpha edge, and raising Erode silently shortened the extension.
//
// INNER EASE — deliberately UNCHANGED. Over the first `inner` px out from the
// core, w ramps 0 -> 1 instead of snapping to full replacement, blending the
// band back into the real core colour rather than leaving a hard line where
// band meets core (visible on wide light wraps). inner <= 0 recovers the hard
// inner. Two alternative geometries were built and rejected in AE on
// 2026-08-24 — a full-width feather running INWARD into the trusted core, and
// a band-bounded feather CENTRED on the core boundary. Both changed the look
// more than they helped and the inward one cost a second distance transform.
// Do not "fix" this again without a look in AE first; see docs/EdgeRepair.md.
inline float BlendWeight(float d, float gap, float extend, float inner)
{
    if (extend <= 0.f) return 0.f;            // Extend 0 disables the band
    if (d <= 0.f) return 0.f;                 // the core is never recomposited
    if (gap < 0.f) gap = 0.f;                 // grown core — no untrusted gap

    // Outward: flat across the gap, then roll off from the ALPHA EDGE.
    float outer = 1.f;
    if (d > gap) {
        const float s = (d - gap) / extend;
        if (s >= 1.f) return 0.f;
        outer = 1.f - s * s * (3.f - 2.f * s);
    }

    if (inner <= 0.f) return outer;           // hard inner
    return ER_Smooth01(d / inner) * outer;    // ease up from the core
}

// Back-compat 2-arg form — the ORIGINAL core-anchored hard-inner curve, with
// no gap plateau and no feather. Kept as the tests' reference shape and as the
// exact thing BlendWeight(d, 0, extend, 0) must reduce to for d > 0.
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
