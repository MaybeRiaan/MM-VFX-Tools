# MM FreqSep — Testing (Phase 1–4)

Two layers of verification: an **automated AE-free unit test** for the core
math and rect reasoning, and a **manual AE checklist** for the parts that only
the host can exercise (tiling, downsample, real deep-color footage).

---

## 1. Automated core tests

`tests/identity_test.cpp` compiles against the vendored SDK headers (for the
`A_long`/`PF_*` types only) plus the **real** `src/blur.cpp`, and drives the
**same** `src/fs_core.h` recombine function the render path calls — the math is
shared, not re-implemented, so a green test means the shipped compose loop has
these properties.

Run:

```powershell
cd MM_FreqSep\tests
.\run_tests.ps1
```

Exit code 0 = all pass. Current output (21 tests):

```
  PASS  identity: gain=1,m=1 -> out==src (per channel <=1e-6)
  PASS  split: (src-lo)+lo == src (<=1e-6)
  PASS  alpha-weighted edge: straight color ~= opaque value (no fringe)
  PASS  control: un-renormalized value IS a fringe (darker)
  ...
  PASS  guided: O(1) box formulation matches brute-force reference
  PASS  guided: constant image is a fixed point (any eps)
  PASS  guided low-eps: step edge preserved (no halo)
  PASS  control: matched Gaussian smears that same edge
  PASS  guided high-eps ~ matched Gaussian on a step (same scale)
  PASS  control: high-eps guided really is smoothing
  PASS  guided alpha edge: straight color ~= opaque value (no fringe)
  PASS  control: coverage alpha at the edge really dropped
  PASS  guided: support = 2r, sigma->radius mapping pinned
  PASS  guided r=0: exact copy (identity)
```

What each asserts:

| Test | Property | Why it matters |
|------|----------|----------------|
| identity | `RecombineChannel(src, lo, gain=1, m=1) == src` within 1e-6 | The plan's required identity: Texture 100 / Smooth 0 / Mix 100 / no matte must be a perfect passthrough. Written as `src + (proc-src)*m` so it collapses to `src` in exact float, independent of `lo`. |
| split lossless | `(src-lo)+lo == src` within 1e-6 | Frequency split loses nothing in float — the whole premise of doing FS in 32f with no +0.5 offset hack. |
| alpha-weighted edge | Straight color at a hard opaque/transparent boundary stays the opaque value after blur+renorm | The #1 "accuracy" bug: a naive (non-alpha-weighted) blur darkens edges. Drives the real separable Gaussian. |
| control | The un-renormalized value at that same edge IS darker | Proves the edge test discriminates — it would fail if the pipeline forgot to renormalize. |
| high view gain | `HighBandDisplay` scales with texGain (0 → flat 0.5, 2 → doubled) | High Band view previews the band as Result uses it. |
| matte channel | alpha / alpha-inv / luma (Rec.709) / luma-inv extracted correctly | Phase 3 channel selection. |
| matte inactive | `SampleMatte` returns 1.0 with no layer | Matte Layer = None must apply the effect everywhere. |
| matte fit | a 2×2 matte fit across a 100×100 output reads ~0 left / ~1 right | Fit-to-Layer stretch + edge-clamp sampling. |
| guided vs reference | O(1) box guided filter matches a brute-force double-precision reference on a noisy image with an alpha ramp (≤1e-3) | Proves the summed-area formulation IS the guided filter, exercising the alpha-weighted paths, not just the opaque fast path. |
| guided constant | a constant image is a fixed point for any ε | Guards the variance-cancellation and normalization plumbing. |
| guided edge-preserving | a hard step edge survives low-ε Guided (≤0.01) while the variance-matched Gaussian smears it (>0.15) | The reason Guided exists — the jawline halo in miniature; the control proves the two methods genuinely differ. |
| guided smoothing extreme | high-ε Guided ≈ the variance-matched Gaussian on a step (≤0.03) | Confirms Separation Radius means the same smoothing *scale* on either method, so toggling Method compares looks not amounts. |
| guided alpha edge | straight color at an opaque/transparent boundary stays the opaque value (no fringe); coverage alpha there really drops | The Gaussian path's anti-fringe invariant carried over to Guided. |
| guided radius mapping | `GuidedSupport = 2r`, σ→radius mapping pinned, r=0 is an exact copy | Locks the PreRender inflation (2r cascade) and the identity chain. |

### How rect handling was verified

The identity test is a **math** guarantee; correct rect handling is a
**structural** guarantee established by construction and reviewed against the
EdgeRepair reference (the sibling MM plugin that gates specifically on
inflation). The reasoning, all commented at the decision sites in
`src/smart_render.cpp`:

- **PreRender inflation** (`FS_PreRender`): the checkout request is expanded per
  axis by `GaussianSupport(sigma_sep) + GaussianSupport(sigma_smooth)` — the
  cascaded blurs' supports **add** because the smooth pass reads low-band pixels
  that themselves read source pixels. `GaussianSupport = ceil(3*sigma)`.
- **result / max_result rects**: `UnionLRect` of what checkout can produce
  (standard SmartFX contract), then the result rect is **intersected back** to
  the original request so the inflated read never grows our advertised output
  extent.
- **Origin offset recovery** (`FS_SmartRender`): the output→source offset is
  recomputed from the returned world's actual size (`srcW-outW`, `srcH-outH`),
  capped at the requested inflate and floored at 0 — so when AE clamps the
  inflated request at a layer edge, addressing stays correct instead of reading
  past the buffer.
- **Blur boundary** = zero-extension of the premultiplied planes. Inside a tile,
  the inflation guarantees every OUTPUT pixel has full kernel coverage of real
  data, so the zero-extension is only ever "seen" by border pixels the output
  never reads. At a true layer edge, zero-extension IS the truth (transparent
  beyond the layer), and the alpha-renormalization makes it fringe-free.

The manual AE checklist below is what actually exercises tiling and downsample
end-to-end — those depend on AE's ROI/tile scheduler and can't be reproduced in
the standalone harness.

---

## 2. Manual AE test checklist

Load After Effects 2026. The build auto-deploys `MM_FreqSep.aex` to the
plugins folder; if AE was open, restart it.

### A. Load & params (Phase 1 milestone)
- [ ] Effect appears under **Effect ▸ MM ▸ Skin Retouch**.
- [ ] Apply to any layer — all 11 params visible in this order: Matte Layer,
      Matte Channel, Matte Fit, Separation Radius, Separation Method, **Edge
      Preservation**, Low Band Smooth, High Band Texture, Mix, View, Working
      Curve. (Edge Preservation sits directly under Method. There is no "Low
      Band: Color Only" checkbox — it was cut and moved to a standalone effect.)
- [ ] Separation Method popup has exactly two items: **Gaussian | Guided Filter**
      (Bilateral removed).
- [ ] Defaults: Radius 8, Method Gaussian, Edge Preservation 35 (greyed),
      Smooth 0, Texture 100, Mix 100, View Result, Curve None, Matte Layer None.
- [ ] Default state renders a **passthrough** (output looks identical to input).

### B. Identity (Phase 2, 32bpc)
- [ ] Set the project to **32 bpc** (File ▸ Project Settings ▸ Color ▸ Depth).
- [ ] Apply to 32bpc footage. Texture 100 / Smooth 0 / Mix 100, no matte.
- [ ] Output is visually identical to source. (Use Difference blend against a
      duplicate un-effected layer if you want pixel confirmation — should be
      black.)

### C. View popup — Low / High bands
- [ ] Radius ~15. **View ▸ Low Band**: a blurred version of the layer (tone
      only, texture gone).
- [ ] **View ▸ High Band**: mid-grey image with only fine detail/texture (this
      is `texGain·hi + 0.5`; flat areas sit at 0.5 grey, edges show as
      light/dark). **High Band Texture affects this view**: at Texture 0 it
      flattens to solid grey, at Texture 200% the detail contrast doubles —
      consistent with Low Band Smooth previewing in the Low Band view.
- [ ] **View ▸ Matte**: solid white (Phase 3 stub — matte = 1 everywhere).
- [ ] Sanity: at Texture 200, texture/pores visibly exaggerated; at Texture 0,
      skin goes plastic-smooth (all detail dropped). Mix scales the effect.

### D. Downsample scaling (the previews-vs-render trap)
- [ ] At **Full** resolution, note the Low Band blur amount at Radius 20.
- [ ] Switch the comp viewer to **Half**, then **Quarter** resolution.
- [ ] The Low Band blur should look the **same relative strength** at every
      resolution (not sharper at half, not softer at quarter). If it changes,
      the downsample scaling in `FS_EffectiveSigmas` is wrong.
- [ ] Confirm a RAM preview at Half matches the look of a Full-res still.

### E. Edge fringing (alpha-weighted blur)
- [ ] Make a layer with a **feathered mask** (soft-edged), or footage with a
      real alpha channel / transparent regions.
- [ ] Radius ~20, View Result, Texture 0 (max smoothing to stress the blur).
- [ ] Inspect the feathered/transparent boundary against a contrasting comp
      background: there should be **no dark or bright halo** ringing the edge.
      A fringe here means the alpha-weighting regressed.

### F. Deep color & tiling
- [ ] Repeat B–C at **8 bpc** and **16 bpc** — bands look identical (internal
      math is always 32f; only the final write is quantized).
- [ ] Scrub/zoom/pan a large (4K+) comp at Radius 40+ so AE tiles the render:
      **no seams** should appear at tile boundaries in any View mode. (Seams =
      PreRender under-inflation.)
- [ ] HDR: footage with values >1.0 (e.g. a bright highlight) — the High Band
      view must not clip the highlight detail (intermediates are never clamped).

### G. Matte driver (Phase 3)

**Luma matte + alpha (the C4 fix):**
- [ ] Driver precomp with straight color (e.g. white solid + feathered masks,
      background transparent), Channel = **Luma**: the result must match the
      **Alpha** channel's shapes — NOT a full white square. Luma is
      `luma(RGB) × alpha`, so transparent regions read 0 and the feather comes
      from the alpha.
- [ ] Channel = **Luma Inverted** on the same driver: transparent regions still
      contribute **no effect** (not full effect) — inversion applies to the
      covered brightness only.

**Resolution invariance (the F2 fix):**
- [ ] With any matte set, switch the comp viewer **Full → Half → Third →
      Quarter**. The affected region must stay **locked to the same place** —
      no horizontal/vertical shift of a pixel or two between resolutions.

- [ ] **No matte**: Matte Layer = None → effect applies everywhere (same as
      before Phase 3). **View ▸ Matte** = solid white.
- [ ] **White solid with masks** (the headline deliverable): add a white solid
      to the comp, draw one or more masks on it, set it as **Matte Layer**.
      Set a strong look (Texture 0, Radius 20) so the effect is obvious. The
      effect must be **confined to the mask shapes**; outside the masks the
      output equals the untouched source. **View ▸ Matte** shows the mask
      shapes in white on black.
- [ ] **Matte Channel**: with an alpha matte, switch Alpha ↔ Alpha Inverted —
      the affected region inverts. With an RGB gradient driver, Luma /
      Luma Inverted select by brightness.
- [ ] **Fit-to-Layer**: a matte layer at a *different resolution* than the comp
      still stretches to cover the whole frame (no black border, no effect
      fading out at the frame edge). Verify with a matte layer both smaller and
      larger than the comp.
- [ ] **Soft matte**: a feathered mask on the driver gives a **gradual**
      transition in/out of the effect (matte acts as a continuous 0..1 weight,
      not a hard cut).
- [ ] **Matte + Mix**: effective strength = matte × Mix. At Mix 50% inside a
      full-white matte region, the effect is half strength.
- [ ] **Animated / transformed matte**: keyframe the driver layer's position or
      a mask — the affected region tracks it frame-accurately (no one-frame lag).

### H. Separation Method — Guided Filter & Edge Preservation (Phase 4)

**Grey-out (the UI contract):**
- [ ] With Method = **Gaussian**, **Edge Preservation is greyed out** (visible
      but disabled — can't be dragged).
- [ ] Switch Method to **Guided Filter** → Edge Preservation becomes **active**
      immediately (no need to click away / re-open the effect).
- [ ] Switch back to Gaussian → it greys out again.
- [ ] Save the comp, reopen it with Method = Guided → Edge Preservation loads
      **already enabled** (UPDATE_PARAMS_UI path, not just the interactive edit).
- [ ] Undo across a Method change → the grey state follows the undone value.

**Guided vs Gaussian (the halo test — the headline deliverable):**
- [ ] Portrait or footage with a **contrasty jawline / hairline** against a
      darker background. Radius ~15, View Result, Texture 0 (max smoothing to
      make the halo obvious).
- [ ] Method = **Gaussian**: a **halo** (light/dark ring) bleeds across the
      hard edge — tone from one side leaks onto the other. This is expected
      and is the Gaussian "look".
- [ ] Method = **Guided Filter**: at low Edge Preservation the **halo
      disappears** — the low band respects the edge, no cross-contamination —
      while flat skin is still smoothed. This is the whole point of Guided.
- [ ] **Same slider = same scale**: toggling Method at a fixed Radius changes
      the *character* (halo vs no-halo), not the overall amount of smoothing.

**Edge Preservation sweep:**
- [ ] View ▸ Low Band, Method Guided, Radius ~15. Drag Edge Preservation
      **0 → 100**: at 0 hard edges stay crisp in the low band (strongest
      preservation); climbing toward 100 the low band progressively resembles
      the plain Gaussian blur (edges soften). The change should feel roughly
      **even across the slider** (exponential ε map), not all bunched at one end.
- [ ] Downsample invariance: at a fixed Radius + Edge value, the Guided look
      holds its relative strength Full → Half → Quarter (radius is variance-
      matched and scaled by downsample, same as Gaussian).
- [ ] Edge fringing (§E) with Method = Guided: feathered/transparent boundaries
      still show **no halo** (Guided is alpha-weighted too).

### I. Low Band Smooth visibility (the Texture-100 trap)

> ⚠️ **You cannot see a Low Band Smooth change in the Result view at Texture
> 100 / Mix 100.** The recombine is `out = lo + gain·hi` with `hi = src − lo`;
> at gain 1 the low band cancels exactly (`out = src` — the identity property).
> Low Band Smooth only alters `lo`, so it's invisible until you break that
> cancellation: **set View ▸ Low Band**, or **lower High Band Texture**
> (Texture 0 → Result equals the low band). This bit the first manual tester.

- [ ] **View ▸ Low Band**, Radius ~15, drag Low Band Smooth 0 → 100: the tone
      band progressively softens. At Texture 0 in Result view the same softening
      shows in the output; at Texture 100 it does not (identity — expected).

> **Note:** "Low Band: Color Only" was removed this session. As a low-band-only
> operation it only reached low-frequency color variation (blotchy skin is
> mid/high frequency), and it doesn't use the frequency split at all — it's a
> whole-image luma/chroma smooth. It's being spun out as a standalone
> **MM Skin Color** effect (edge-aware chroma evening via the shared guided
> filter). See `docs/mm-freqsep-plan.md` §6.

### Known limits this session (by design)
- **Working Curve** is still **present but inert** (later phase).
- Low Band Smooth runs as a second Gaussian on the tone band regardless of
  separation method (edge-aware smooth variant not planned for v1).
- Guided Filter is **per-channel self-guided** — on edges where the channels
  disagree in contrast it can shift color slightly; the 3×3-covariance color
  guide is the documented upgrade path if that shows up on real footage.
- Matte Fit ships **Fit-to-Layer** as the primary path; Center / Top-Left are
  implemented (1:1 placement, out-of-bounds = no effect) but Fit is the v1
  default and the most-tested mode.
