/*  test_composite.cpp — headless unit test for MM_Composite.h.

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

#include "MM_Composite.h"

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
    printf("MM_Composite blend/choke math test\n");

    // 1. BlendWeight endpoints + midpoint + monotone + disable.
    {
        bool ok = true;
        ok &= mm::BlendWeight(0.f, 32.f) == 1.f;
        ok &= mm::BlendWeight(-1.f, 32.f) == 1.f;          // limit below band
        ok &= mm::BlendWeight(32.f, 32.f) == 0.f;
        ok &= mm::BlendWeight(100.f, 32.f) == 0.f;
        ok &= std::fabs(mm::BlendWeight(16.f, 32.f) - 0.5f) < 1e-6f;  // smoothstep midpoint
        ok &= mm::BlendWeight(5.f, 0.f) == 0.f;            // extend<=0 disables
        ok &= mm::BlendWeight(5.f, -3.f) == 0.f;
        float prev = 1.1f;
        for (int i = 0; i <= 64; ++i) {
            const float w = mm::BlendWeight((float)i, 64.f);
            if (w > prev + 1e-7f) ok = false;              // must not increase
            prev = w;
        }
        Check(ok, "blendweight", "");
    }

    // 2. ComposeBandRGB at w=0 returns low+hi for any detail — and when the
    //    caller sets hi = orig-low, that IS orig, bit-exact in each term.
    {
        const float low[3] = { 0.3f, 0.5f, 0.2f };
        const float ext[3] = { 0.9f, 0.1f, 0.4f };
        const float hi[3]  = { 0.05f, -0.03f, 0.11f };
        float o0[3], o1[3];
        mm::ComposeBandRGB(low, ext, hi, 0.f, 0.f, o0);
        mm::ComposeBandRGB(low, ext, hi, 0.f, 1.f, o1);
        bool ok = true;
        for (int c = 0; c < 3; ++c) {
            ok &= (o0[c] == o1[c]);                    // detail irrelevant at w=0
            ok &= (o0[c] == low[c] + hi[c]);           // == orig, exactly
        }
        Check(ok, "band-w0-exact-orig", "detail 0 vs 1 identical at w=0");
    }

    // 3. ComposeBandRGB at w=1: detail=1 → ext+hi; detail=0 → ext.
    {
        const float low[3] = { 0.3f, 0.5f, 0.2f };
        const float ext[3] = { 0.9f, 0.1f, 0.4f };
        const float hi[3]  = { 0.05f, -0.03f, 0.11f };
        float d1[3], d0[3];
        mm::ComposeBandRGB(low, ext, hi, 1.f, 1.f, d1);
        mm::ComposeBandRGB(low, ext, hi, 1.f, 0.f, d0);
        double worst = 0.0;
        for (int c = 0; c < 3; ++c) {
            worst = std::fmax(worst, std::fabs((double)d1[c] - (double)(ext[c] + hi[c])));
            worst = std::fmax(worst, std::fabs((double)d0[c] - (double)ext[c]));
        }
        char d[64]; snprintf(d, sizeof d, "worst|diff|=%.3g", worst);
        Check(worst < 1e-6, "band-w1-endpoints", d);
    }

    // 4. Continuity at the band's outer edge: as w -> 0, out -> orig for any
    //    detail (no seam at d = Extend at Detail 0).
    {
        const float low[3] = { 0.4f, 0.4f, 0.4f };
        const float ext[3] = { 0.8f, 0.2f, 0.6f };
        const float hi[3]  = { 0.1f, 0.1f, -0.1f };
        double worst = 0.0;
        for (float w : { 1e-3f, 1e-4f, 1e-5f }) {
            float o[3];
            mm::ComposeBandRGB(low, ext, hi, w, 0.f, o);   // harshest: detail 0
            for (int c = 0; c < 3; ++c)
                worst = std::fmax(worst,
                    std::fabs((double)o[c] - (double)(low[c] + hi[c])) / (double)w);
        }
        // Error must vanish LINEARLY with w (bounded slope), not stall.
        char d[64]; snprintf(d, sizeof d, "worst |err|/w=%.3g", worst);
        Check(worst < 2.0, "band-outer-continuity", d);
    }

    // 5. ChokeInAlpha: identity / ramp / zero, and profile preservation.
    {
        bool ok = true;
        ok &= mm::ChokeInAlpha(0.7f, 5.f, 3.f) == 0.7f;    // deeper than c+1: identity
        ok &= mm::ChokeInAlpha(0.7f, 4.f, 3.f) == 0.7f;    // exactly c+1: identity
        ok &= mm::ChokeInAlpha(0.7f, 3.f, 3.f) == 0.f;     // at c: gone
        ok &= mm::ChokeInAlpha(0.7f, 1.f, 3.f) == 0.f;     // inside c: gone
        const float mid = mm::ChokeInAlpha(0.6f, 3.5f, 3.f);
        ok &= std::fabs(mid - 0.3f) < 1e-6f;               // ramp scales a (0.6*0.5)
        // c = 0 must be identity for any support pixel (dOut >= 1) — the
        // caller skips entirely at 0, but the maths must agree.
        ok &= mm::ChokeInAlpha(0.42f, 1.f, 0.f) == 0.42f;
        Check(ok, "choke-in", "");
    }

    // 6. GrowAlpha: far identity, solid-interior identity (no hardening),
    //    full within grow, soft rim, max keeps existing softer alpha.
    {
        bool ok = true;
        ok &= mm::GrowAlpha(0.2f, 9.f, 5.f) == 0.2f;       // far: identity
        ok &= mm::GrowAlpha(0.7f, 0.f, 5.f) == 0.7f;       // solid interior: identity
        ok &= mm::GrowAlpha(0.0f, 3.f, 5.f) == 1.f;        // well within grow: full
        const float rim = mm::GrowAlpha(0.0f, 5.f, 5.f);
        ok &= std::fabs(rim - 0.5f) < 1e-6f;               // rim midpoint
        ok &= mm::GrowAlpha(0.9f, 5.f, 5.f) == 0.9f;       // max keeps higher a
        ok &= mm::GrowAlpha(0.3f, 6.f, 5.f) == 0.3f;       // beyond rim: identity
        Check(ok, "grow", "");
    }

    printf(g_fail ? "\nRESULT: %d FAILED\n" : "\nRESULT: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
