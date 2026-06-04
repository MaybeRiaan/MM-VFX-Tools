# Modern Edge Blur — Next Steps

> **STATUS 2026-06-04: clean-slate v1.2 rebuild.** All prior EdgeExtend/EdgeRepair/meb_common code removed. ModernEdgeBlur is a single self-contained plugin. The items below reflect the current working state and the road ahead.

## Priority 1 — Blend Radius: add true alpha erosion

Currently Blend Radius Gaussian-blurs the sharp alpha to create the blend weight, but does not erode the output alpha inward. Increasing Blend Radius softens the blend transition but the matte boundary stays at its original position.

**What's needed:** the output alpha should contract inward as Blend Radius increases, so the effect visually matches Red Giant Edge Blur's "Edge Width" behaviour — a widening band that both softens and erodes the matte edge.

Options:
- Run a morphological erosion on the alpha by Blend Radius before computing the weight (requires the circular morph kernel already stubbed in `ShiftEdgeAlpha`)
- Or simply use the Gaussian-blurred alpha as the output alpha directly (it already softens outward — may be sufficient)

## Priority 2 — Shift Edge

`ShiftEdgeAlpha` is implemented (circular min/max morphological filter) but stubbed — not called when shift_edge = 0. To wire it up properly:
- Set `preserve_rgb_of_zero_alpha = TRUE` on checkout so the morph sees the true alpha boundary
- After the morph, zero held RGB before passing to the blur (outside-matte pixels must be (0,0,0,0) for the Gaussian)
- Re-enable the `ShiftEdgeAlpha` call in SmartRender

## Priority 3 — Blend Radius threading

The alpha-only Gaussian blur (`GaussBlurAlphaOnly`) is serial. At large radii on 6K frames this will be the bottleneck. Candidate for `iterate_generic` threading — see the existing `GaussBlurWorld` path for the AE threading pattern.

## Priority 4 — EdgeRepair suite plugin

A second plugin (`EdgeRepair.aex`) was prototyped in an earlier session but removed in the clean-slate rebuild. It will be reintroduced as a separate `add_meb_plugin` entry in CMakeLists.txt once ModernEdgeBlur v1 is fully validated. The `add_meb_plugin` CMake function already supports multiple targets.

## Known working state (v1.2)

- Blur Amount: matches Red Giant Edge Blur A/B at equivalent values. Uses AE's internal convolve (threaded, native format). Fallback to hand-rolled separable Gaussian.
- Blend Radius: visibly softens edge, linear response, 0.01 precision. Alpha erosion not yet implemented (Priority 1).
- Shift Edge: slider present in UI, no-op at default (0). Implementation stubbed.
- No position shift, no dark halo, no premult artifacts.
- Passthrough at Blur=0 / Blend=0 preserves held RGB.
- 8/16/32 bpc clean.
- Loads and runs on 6K comps without crashing.
