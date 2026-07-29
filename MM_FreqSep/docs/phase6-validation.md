# MM FreqSep — Phase 6 Validation Protocol

Written 2026-07-18, at the point where the DX12 GPU path is code-complete,
deployed to AE 2026, and proven bit-exact against the CPU reference in the
AE-free harness (48/48, `tests/run_tests.ps1`). This doc scripts the half
that needs eyes and a live AE: everything below is a hands-on check.

**The one idea behind all of it:** the CPU path is the reference by
definition, and the AE-free tests already prove the GPU *math and tiling*
equal it exactly. So in AE you are only ever hunting **plumbing** faults —
wrong binding, wrong pitch, missed barrier, wrong band offset — and those
don't look like subtle drift; they look like garbage, shifts, or seams.

---

## 0. Setup / how to switch paths

- **GPU path:** File → Project Settings → Video Rendering and Effects →
  **Mercury GPU Acceleration (DirectX)**. This is the only renderer setting
  under which AE sends the plugin `PF_Cmd_SMART_RENDER_GPU`.
- **CPU path:** same menu → **Mercury Software Only**.
- Confirm the GPU is actually working: Task Manager → Performance → GPU
  while RAM-previewing; Compute usage should spike on the GPU path.
- The plugin also falls back to CPU **silently, by design** when: the
  renderer isn't DirectX; the device lacks shader doubles; or the frame's
  worst-case tile plan doesn't fit the VRAM budget (radius near 250 on 6K
  on a small card). If GPU-vs-CPU comparisons look *identical in timing*,
  check you're not in a fallback case before concluding anything.
- Work at **32 bpc, full resolution** unless a matrix row says otherwise.

## 1. Smoke (5 minutes)

1. Fresh comp, any 1080p footage, apply **MM → Skin Retouch**, defaults,
   DirectX renderer. Expect: a rendered frame (not black/garbage), no error
   dialog, no AE crash on apply/param-drag/scrub.
2. Cycle View: Result / Low Band / High Band / Matte. Low = blurred, High =
   grey detail field, Matte = white (none connected).
3. Toggle Method Gaussian ↔ Guided Filter. Edge Preservation greys
   in/out accordingly.
4. Radius 0 → Result == source (identity); High Band → flat grey.

## 2. GPU == CPU, per band (the Milestone-1 check)

For **Method = Gaussian, radius ~8** on a 1080p portrait still:

1. Render one frame with DirectX renderer, View = **Low Band** → save frame
   (32-bit EXR via Composition → Save Frame As, or a 32bpc PNG/TIFF).
2. Switch to Software Only, same frame, same View → save again.
3. Diff: stack both in a 32bpc comp, top layer blend = **Difference**, add
   an Exposure effect on top at **+12 stops**. Expect essentially black
   (≤1e-5 per channel shows as barely-visible speckle at +12; any real
   plumbing bug is unmissable at +0).
4. Repeat for View = **High Band**, then **Result**.
5. Repeat the whole set for **Method = Guided Filter** (Edge Preservation
   at default 35).
6. Repeat once with **Low Band Smooth = 50** (exercises the cascade).

A mismatch in Low/High but not Result (or vice versa) localizes the fault:
Low/High isolate blur/guided away from compose.

## 3. Matte driver on GPU

Driver comp: white solid with two soft masks, used as Matte Layer.

| Check | Expect |
|---|---|
| Matte Channel = Alpha, View = Matte, GPU vs CPU diff | black diff; shapes match masks |
| All 4 channels (Alpha/Alpha Inv/Luma/Luma Inv) | correct polarity; Luma gated by coverage (no "white square" over the layer's bounding box) |
| Matte layer smaller than source, Fit = Fit to Layer | stretched across frame, edge rows clamp (no transparent rim un-masking the border) |
| Fit = Center / Top-Left | matte placed 1:1; beyond it the effect is confined (reads transparent) |
| Half res ↔ Full res preview | matte lands identically (no drift/jump between res switches) |
| Matte layer with its own rotation/scale, precomped | works — AE bakes transforms into the checked-out layer |
| View = Result with matte connected, GPU vs CPU diff | black |

## 4. Tiling-specific hunts (the 6K cases)

These target the banded tiling on real hardware. On 6K footage:

1. **Radius 60–90, Gaussian, then Guided** — this forces multiple bands.
   View = High Band (it exaggerates any discontinuity), zoom 100%, pan
   across the full frame height hunting **horizontal seams** at band
   boundaries. Then GPU-vs-CPU diff the full frame (§2 method). The AE-free
   tests prove the seams can't come from the math; a seam here would mean a
   dispatch/binding fault — but look anyway, that's the point of eyes.
2. **Radius 250 (slider max)** — expect either a working render or an
   automatic CPU fallback (slower frame, identical output). No error, no
   garbage.
3. **ROI**: enable Region of Interest over a sub-area, render, disable —
   no offset shifts, no stale-edge artifacts (exercises srcOX/srcOY
   recovery at request-clipped edges).
4. **Layer smaller than comp** (e.g. scaled-down layer): edges must not
   fringe or brighten (alpha-weighted blur invariant) on GPU exactly as CPU.

## 5. Matrix (from the plan, now × GPU)

Run Result-view GPU-vs-CPU diffs across:

- **Depth:** comp at 8, 16, 32 bpc (AE renders the GPU path in float and
  converts; 8/16 diffs should be zero after the same conversion).
- **Res:** full / half / quarter preview (radius scaling must match CPU —
  a wrong downsample scale shows as different blur AMOUNT, not noise).
- **Matte:** none / connected / mismatched size / rotated / precomped.
- **Footage:** premultiplied with transparency; HDR source (EXR with
  superwhites, or precomp with +4 Exposure): High Band view must show
  unclamped detail identically on both paths (no GPU-side clamp).
- **Params:** Texture 0 / 100 / 200; Mix 0 / 50 / 100; Smooth 0 / 100.
  Texture=100, Smooth=0, Mix=100 → Result == source on BOTH paths
  (identity); Mix=0 → source on both.

## 6. MFR / stability

1. Preferences → Memory & Performance → Multi-Frame Rendering ON.
2. RAM-preview a 10s 4K clip with the effect + matte, radius 30: no crash,
   no frame-to-frame flicker (park playhead, step back/forth — identical
   frames must be identical).
3. Render queue export of the same range while scrubbing another comp —
   concurrency stress (per-render D3D objects mean no shared state; this
   verifies it on real hardware).
4. Apply the effect 3× on the same layer (stacked instances share one AE
   device + our per-device PSOs): renders correctly, no device faults.
5. Leave AE open ≥30 min with previews: watch for VRAM creep in Task
   Manager (scratch is freed per render; creep would indicate a leak in
   the free path).

## 7. Performance numbers (for the plan, after correctness)

Time a 100-frame RAM preview (or render-queue export) of 4K and 6K clips,
radius 30 / 60 / 90, Gaussian and Guided, GPU vs Software Only. Record in
the plan's Phase 5 section. This is where the "CPU slow at 4K/big radius"
pain either dies or tells us where the next optimization goes (candidates,
in order: fewer UAV barriers via per-resource barriers, box-filter guided
via prefix sums, IIR CPU fallback per the deferred plan item).

## 8. Original Phase 6 items (unchanged by GPU work, still due)

- **Reference test:** manual FS comp (duplicate layer + Gaussian Blur 32bpc
  + subtract + add-back at Working Curve = None) vs plugin — float-tolerance
  match. Do it once on the CPU path (the reference), then note the GPU path
  already equals CPU per §2.
- **Photoshop side-by-side** on a gamma-encoded still (the PS-accuracy
  product target).
- Transparency-edge matrix rows above (§4.4, §5) cover the rest.

---

## Sign-off criteria

Phase 6 closes when: §2 diffs are black for both methods and all bands; §3
matte grid passes; §4 shows no seams at multi-band radii; §5 matrix has no
row where GPU differs from CPU beyond the ≤1e-5 target; §6 completes with
zero crashes; §7 numbers are recorded. Then Phase 7 (UX polish) starts with
locked rendering behavior.
