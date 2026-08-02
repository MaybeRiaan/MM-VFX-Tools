/*  test_composite.cpp — headless unit test for ER_Composite.h.

    Compiles with plain cl.exe, no AE SDK (via tests\build_tests.ps1).
    Exit code 0 = all pass, 1 = a failure.

    Validates the Stage 4 gate properties analytically:
      - BlendWeight endpoints, midpoint, monotonicity, extend<=0 disable;
      - ComposeBandRGB: w=0 returns orig EXACTLY for any detail (the
        "Detail 0 vs 100 differs only inside the blend band" property),
        w=1 endpoints, continuity approaching the band's outer edge;
      - ChokeInAlpha: identity beyond choke+1, zero within choke, ramp
        between, profile preservation (scales a, doesn't replace it);
      - GrowAlpha: identity far away AND inside the solid (a semi-opaque
        solid must not harden), full rim within grow, soft rim at grow.
*/

#include "ER_Composite.h"

#include <cstdio>
#include <cmath>
#include <initializer_list>

static int g_fail = 0;

static void Check(bool ok, const char *name, const char *detail)
{
    printf("  [%-26s] %s%s%s\n", name, ok ? "OK" : "FAIL", detail[0] ? "  " : "", detail);
    if (!ok) ++g_fail;
}

int main()
{
    printf("ER_Composite blend/choke math test\n");

    // 1. BlendWeight endpoints + midpoint + monotone + disable.
    {
        bool ok = true;
        ok &= er::BlendWeight(0.f, 32.f) == 1.f;
        ok &= er::BlendWeight(-1.f, 32.f) == 1.f;          // limit below band
        ok &= er::BlendWeight(32.f, 32.f) == 0.f;
        ok &= er::BlendWeight(100.f, 32.f) == 0.f;
        ok &= std::fabs(er::BlendWeight(16.f, 32.f) - 0.5f) < 1e-6f;  // smoothstep midpoint
        ok &= er::BlendWeight(5.f, 0.f) == 0.f;            // extend<=0 disables
        ok &= er::BlendWeight(5.f, -3.f) == 0.f;
        float prev = 1.1f;
        for (int i = 0; i <= 64; ++i) {
            const float w = er::BlendWeight((float)i, 64.f);
            if (w > prev + 1e-7f) ok = false;              // must not increase
            prev = w;
        }
        Check(ok, "blendweight", "");
    }

    // 2. ComposeBandRGB (grain method) at w=0 returns orig EXACTLY for any
    //    detail/grain — grain is weighted by w, so the band's outer edge is
    //    seamless ("Detail 0 vs 100 differs only inside the band").
    {
        const float orig[3] = { 0.3f, 0.5f, 0.2f };
        const float ext[3]  = { 0.9f, 0.1f, 0.4f };
        float o0[3], o1[3];
        er::ComposeBandRGB(orig, ext, 0.7f, 0.f, 0.f, o0);   // grain, detail 0
        er::ComposeBandRGB(orig, ext, 0.7f, 0.f, 1.f, o1);   // grain, detail 1
        bool ok = true;
        for (int c = 0; c < 3; ++c) {
            ok &= (o0[c] == o1[c]);        // detail/grain irrelevant at w=0
            ok &= (o0[c] == orig[c]);      // == orig, exactly (no seam)
        }
        Check(ok, "band-w0-exact-orig", "detail 0 vs 1 identical at w=0");
    }

    // 3. ComposeBandRGB at w=1: base is ext, plus detail*grain added to all ch.
    {
        const float orig[3] = { 0.3f, 0.5f, 0.2f };
        const float ext[3]  = { 0.9f, 0.1f, 0.4f };
        const float grain   = 0.08f;
        float d1[3], d0[3];
        er::ComposeBandRGB(orig, ext, grain, 1.f, 1.f, d1);  // detail 1
        er::ComposeBandRGB(orig, ext, grain, 1.f, 0.f, d0);  // detail 0
        double worst = 0.0;
        for (int c = 0; c < 3; ++c) {
            worst = std::fmax(worst, std::fabs((double)d1[c] - (double)(ext[c] + grain)));
            worst = std::fmax(worst, std::fabs((double)d0[c] - (double)ext[c]));
        }
        char d[64]; snprintf(d, sizeof d, "worst|diff|=%.3g", worst);
        Check(worst < 1e-6, "band-w1-endpoints", d);
    }

    // 4. Grain NEVER darkens on average: for zero-mean grain, the composite's
    //    expected value equals lerp(orig, ext, w) — no directional bias. Check
    //    the mean of ComposeBandRGB over a symmetric grain sweep is unbiased.
    {
        const float orig[3] = { 0.4f, 0.4f, 0.4f };
        const float ext[3]  = { 0.8f, 0.2f, 0.6f };
        double sum[3] = {0,0,0}; int n = 0;
        for (float g = -1.f; g <= 1.0001f; g += 0.05f) {  // symmetric zero-mean
            float o[3];
            er::ComposeBandRGB(orig, ext, g, 0.6f, 1.f, o);
            for (int c = 0; c < 3; ++c) sum[c] += o[c];
            ++n;
        }
        double worst = 0.0;
        for (int c = 0; c < 3; ++c) {
            const double mean = sum[c] / n;
            const double base = orig[c] + (ext[c] - orig[c]) * 0.6;  // lerp
            worst = std::fmax(worst, std::fabs(mean - base));
        }
        char d[64]; snprintf(d, sizeof d, "grain mean bias=%.3g (no darkening)", worst);
        Check(worst < 1e-6, "grain-unbiased", d);
    }

    // 4b. Inner-ease BlendWeight: w -> 0 at the core (d->0) AND at d>=extend,
    //     rises in between, and peaks inside the band. This is what softens the
    //     band/core handoff (the wide-light-wrap line). The 2-arg form must be
    //     UNCHANGED (hard inner, w=1 at core).
    {
        bool ok = true;
        const float ext = 32.f, inner = 0.15f * ext;  // ~4.8px
        ok &= er::BlendWeight(0.f, ext, inner) == 0.f;      // core: no replace now
        ok &= er::BlendWeight(0.001f, ext, inner) < 0.05f;  // just outside: still low
        ok &= er::BlendWeight(ext, ext, inner) == 0.f;      // outer edge: 0
        // Rises then falls: sample a few points, peak strictly inside.
        const float a = er::BlendWeight(inner, ext, inner);     // end of inner ease
        const float b = er::BlendWeight(1.f, ext, inner);       // very inner
        ok &= (a > b);                                          // eases up outward
        ok &= (a > 0.5f);                                       // reaches strong replace
        // 2-arg form unchanged (hard inner). Inside the inner-ease region the
        // 3-arg weight is strictly lower (it's ramping up); at/after `inner`
        // they coincide (innerW == 1), so compare at a point INSIDE the ease.
        ok &= er::BlendWeight(0.f, ext) == 1.f;
        ok &= er::BlendWeight(inner * 0.5f, ext) > er::BlendWeight(inner * 0.5f, ext, inner);
        // inner<=0 recovers the hard curve.
        ok &= er::BlendWeight(5.f, ext, 0.f) == er::BlendWeight(5.f, ext);
        // Softness param contract (2026-07-22, disk ID 106): the Composited
        // band feeds `inner = Softness px` (a FIXED PIXEL width) straight to
        // BlendWeight. Softness 0 == hard inner; a px width behaves as the
        // inner arg; a width >= Extend is still well-defined (very soft, w>0
        // stays small but positive across the band, no divide issue).
        ok &= er::BlendWeight(3.f, ext, 0.f) == er::BlendWeight(3.f, ext);    // 0 px = hard
        ok &= er::BlendWeight(3.f, ext, 5.f) < er::BlendWeight(3.f, ext);     // 5 px eases the inner
        ok &= er::BlendWeight(ext*0.5f, ext, 250.f) > 0.f;                    // width > extend: still valid
        Check(ok, "blendweight-inner", "eases 0 at core, peaks in band");
    }

    // 5. ChokeInAlpha: identity / ramp / zero, and profile preservation.
    {
        bool ok = true;
        ok &= er::ChokeInAlpha(0.7f, 5.f, 3.f) == 0.7f;    // deeper than c+1: identity
        ok &= er::ChokeInAlpha(0.7f, 4.f, 3.f) == 0.7f;    // exactly c+1: identity
        ok &= er::ChokeInAlpha(0.7f, 3.f, 3.f) == 0.f;     // at c: gone
        ok &= er::ChokeInAlpha(0.7f, 1.f, 3.f) == 0.f;     // inside c: gone
        const float mid = er::ChokeInAlpha(0.6f, 3.5f, 3.f);
        ok &= std::fabs(mid - 0.3f) < 1e-6f;               // ramp scales a (0.6*0.5)
        // c = 0 must be identity for any support pixel (dOut >= 1) — the
        // caller skips entirely at 0, but the maths must agree.
        ok &= er::ChokeInAlpha(0.42f, 1.f, 0.f) == 0.42f;
        Check(ok, "choke-in", "");
    }

    // 6. GrowAlpha: far identity, solid-interior identity (no hardening),
    //    full within grow, soft rim, max keeps existing softer alpha.
    {
        bool ok = true;
        ok &= er::GrowAlpha(0.2f, 9.f, 5.f) == 0.2f;       // far: identity
        ok &= er::GrowAlpha(0.7f, 0.f, 5.f) == 0.7f;       // solid interior: identity
        ok &= er::GrowAlpha(0.0f, 3.f, 5.f) == 1.f;        // well within grow: full
        const float rim = er::GrowAlpha(0.0f, 5.f, 5.f);
        ok &= std::fabs(rim - 0.5f) < 1e-6f;               // rim midpoint
        ok &= er::GrowAlpha(0.9f, 5.f, 5.f) == 0.9f;       // max keeps higher a
        ok &= er::GrowAlpha(0.3f, 6.f, 5.f) == 0.3f;       // beyond rim: identity
        Check(ok, "grow", "");
    }

    printf(g_fail ? "\nRESULT: %d FAILED\n" : "\nRESULT: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
