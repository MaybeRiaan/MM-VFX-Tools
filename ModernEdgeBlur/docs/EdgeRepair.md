# Edge Repair — build brief and status

`EdgeRepair.aex` (match name `MM Edge Repair`, category `MM`) is a NEW After
Effects SmartFX plugin, built from scratch alongside ModernEdgeBlur in this
repo. **ModernEdgeBlur's plugin code must never be modified** for Edge
Repair's sake — it is reference only (CMake structure, PiPL sync discipline,
`UnpackRow`/`PackRow` pack/unpack, MFR-safe patterns). Any prior
EdgeExtend/EdgeRepair prototype code that existed before this rebuild is dead
— do not salvage, reference, or adapt it (see `Architecture.md`'s note on the
2026-06-04 clean-slate rebuild, which predates this effort).

This doc is the durable source of truth for Edge Repair's spec and staged
plan — everything here originates from the build brief the user supplied,
plus one amendment (both dated 2026-07-06). Keep this file up to date as
stages land; it is what lets a fresh session resume without re-deriving the
spec from conversation history.

## Product philosophy

A **paring knife, not a multitool**. Nuke's EdgeExtend node is the model: a
pure stream operator — RGBA in, RGBA out, RGB modified, alpha untouched
except an explicit opt-in choke. How the result composites downstream is the
user's business, not this plugin's.

Keyed/rotoscoped footage has contaminated semi-transparent edge pixels
(motion blur rims, halos from bright backgrounds). Edge Repair:

1. Builds a **core matte**: threshold the layer's alpha, then erode/dilate by
   a signed `Erode` so only trusted interior colour remains (or, if negative,
   grows the trusted region).
2. Computes a **Euclidean distance field** `d(x,y)` = distance from each pixel
   to the nearest core pixel. Exactly one distance computation per frame
   drives erosion, extension falloff, blend weight, and alpha choke — the
   field never performs colour fill itself.
3. **Extends** trusted colour outward via **pull–push** fill (mip-chain pull
   of premultiplied RGBA + validity, push filling only invalid pixels,
   unpremultiply at the end) — the ONLY fill engine. Extension runs
   effectively unlimited; `Extend` clamps how far the result is *used*, not
   how far the fill runs.
4. **Blends** within the band `0 < d <= Extend`: replaces the original RGB's
   low frequencies with the extended colour, weighted by distance. Original
   alpha passes through unchanged (unless choked).
5. **Detail** (frequency separation): `out = extendedLow + (origRGB -
   gaussianBlur(origRGB, r)) * detail`, so plate grain/texture/motion-blur
   micro-detail survives on top of the decontaminated colour base within the
   blend band. This is the differentiator.
6. **Alpha Choke** (default 0 = pure RGB operator): remaps output alpha along
   the same distance field. Positive chokes the matte inward with smooth
   Euclidean falloff; negative grows it. No morphological min/max filters —
   reuse the distance field.

## Parameters — LOCKED, exactly five (amended 2026-07-06)

An earlier version of this brief specified seven params including Softness
and a Premultiplied checkbox. **That version is superseded.** The
authoritative list:

| # | Param | Type | Range | Default | Disk ID |
|---|-------|------|-------|---------|---------|
| 1 | Erode | float slider (px), **signed** | −50 to +50 | 2.0 | 101 |
| 2 | Extend | float slider (px) | 0–500 | 32.0 | 102 |
| 3 | Detail | float slider (%) | 0–100 | 100 | 103 |
| 4 | Alpha Choke | float slider (px) | −50 to +50 | 0 | 104 |
| 5 | View Mode | popup: Result / Core Matte / Distance Field / Extended Colour | — | Result | 105 |

Disk IDs are forever — never reorder, never reuse. Future additions append
with new disk IDs only.

**Erode is a single signed control**: positive erodes the core inward,
negative dilates it outward (not one-directional).

**Removed — must NOT appear anywhere (code, PiPL, or UI), and must not be
reintroduced without an explicit new ask:**
- **Softness / falloff / edge-blur** — a separate suite plugin
  (ModernEdgeBlur) owns edge shaping. Not this tool's job.
- **Premultiplied as a user parameter** — internal only. AE hands the effect
  premultiplied pixels and expects them back premultiplied:
  unpremultiply on input (guard alpha=0), do the extend/Detail math in
  unpremult space, re-premultiply on output. No knob, no override of AE's
  footage interpretation.
- External matte/layer parameters, "recompose with original," channel
  selectors, invert toggles, grain/noise synthesis (Detail supersedes it), GPU
  code before Stage 6.

If an eighth parameter or any of the above ever seems needed: stop and ask,
don't add it unilaterally.

## Critical AE SDK requirements

1. **Input rect inflation — the #1 divergence from ModernEdgeBlur**, which
   deliberately does none. Edge Repair MUST inflate: in
   `PF_Cmd_SMART_PRE_RENDER`, expand the checkout request rect by
   `ceil(max(Erode,0) + max(Extend,0) + |AlphaChoke| + textureBlurRadius)` on
   all sides before checking out the input (AE clamps to layer bounds).
   Output result rect never grows past the original request. Getting this
   wrong produces seams that differ between RAM preview and final render —
   the single most important correctness property in the project (see Stage
   1 gate below).
2. SmartFX only (`PF_Cmd_SMART_PRE_RENDER` / `PF_Cmd_SMART_RENDER`). No legacy
   render path.
3. Flags: `PF_OutFlag_DEEP_COLOR_AWARE`, `PF_OutFlag2_FLOAT_COLOR_AWARE`,
   `PF_OutFlag2_SUPPORTS_SMART_RENDER`, `PF_OutFlag2_SUPPORTS_THREADED_RENDERING`
   (plus `PF_OutFlag_PIX_INDEPENDENT` / `PF_OutFlag_USE_OUTPUT_EXTENT`,
   following ModernEdgeBlur's precedent). PiPL flags must match `GlobalSetup`
   exactly or AE rejects the plugin.
4. 8/16/32bpc supported; all internal math in float32 via pack/unpack.
5. MFR-safe: zero mutable global/static state. Every render reentrant.
6. Premult handling is internal-only (see Parameters above) — no user-facing
   footage-interpretation override.

## Algorithms — required implementations

- **Distance field (CPU):** exact Euclidean distance transform,
  Meijster/Felzenszwalb–Huttenlocher two-pass (rows then columns), O(n), each
  pass directly parallelizable. No iterative 3x3 erosion.
- **Fill:** pull–push over a mip chain of (premult R, G, B, coverage). Pull:
  average valid texels per 2x2. Push: fill only invalid pixels from the
  coarser level (bilinear), leave valid pixels untouched. Unpremultiply by
  coverage at the end. ~2*log2(max dimension) passes. This is the ONLY fill
  engine — the distance field never fills colour itself.
- **Blend weight:** `w = smoothstep(edge0, edge1, d)` mapped through the
  distance band; document the exact mapping in code so CPU and a future GPU
  path stay in lockstep.
- **Texture blur (for Detail):** small separable Gaussian, fixed radius
  (~4–8px is fine for v1, currently `ER_TEXTURE_BLUR_RADIUS = 8.0f` in
  `EdgeRepair.cpp`), exposed nowhere as a parameter.

## Staged plan with validation gates

Work stage by stage; stop at each gate, report, and wait for confirmation
before continuing. Keep every stage compiling and loadable in AE.

- **Stage 0 — Skeleton + shared foundation.** DONE, commit `053d9b5`. New
  plugin target, pure passthrough SmartFX, shared helpers
  (`src/common/MM_Common.h/.cpp`: pack/unpack, `ForEachRow` over
  `iterate_generic`). *Gate (PASSED, user-confirmed):* loads in AE,
  byte-identical passthrough at 8/16/32bpc.
- **Stage 1 — Params + inflated plumbing.** DONE, commits `37482c2`,
  `cf6e593` (sampler fix), version-sync fix `2076adf`. All five params
  registered with the locked disk IDs. Inflated checkout implemented
  (`ER_InflateRadius`, `PreRender`). Tiling debug mode (`ER_TILING_DEBUG`
  switch in `EdgeRepair.cpp`, default OFF now that the gate passed) renders a
  true circular-disc dilation of input alpha by the inflation radius — a
  seam at any tile boundary would show as a straight-edged break in an
  otherwise-round halo. *Gate (PASSED, user-confirmed on a 5K comp):*
  concentric circle grows with no facets/corners, RAM preview == render
  queue output, stable across zoom levels. (An earlier 4-tap "diamond"
  sampler produced false-positive facets that looked like seams but weren't
  — see `cf6e593` for the fix and why it mattered.)
- **Stage 2 — Core matte + distance field (CPU).** DONE, commit `5ceb927`.
  `src/common/MM_DistanceField.h` (AE-free EDT + brute-force reference),
  `src/common/MM_CoreMatte.h` (threshold + signed erode via distance-to-
  boundary, `CoreDistanceField` = the single driving field). View Mode wired
  for Core Matte and Distance Field (Result/Extended Colour still
  passthrough). *Gate:* headless unit test
  (`tests/test_distancefield.cpp` + `tests/build_tests.ps1`, no AE needed)
  — PASSED, worst |diff| = 0 across filled circle, 1px line, checkerboard,
  single/no/full-seed, two-corners, non-convex random fields, vs a
  brute-force O(n^2) reference. AE visual check: **PASSED, user-confirmed
  2026-07-08** — Core Matte erode/grow uniform with no faceting; Distance
  Field radially even. (User flagged a "bunching" crease converging into a
  sharp matte corner — that is the field's medial axis, where distance
  wavefronts from the corner's two edges collide along the angle bisector.
  Expected/correct for an exact EDT on a sharp corner; the failure signature
  would have been faceting on the smooth regions instead. Re-check blend
  continuity across that ridge at the Stage 4 gate.)
- **Stage 3 — Pull–push fill (CPU, float32 only).** BUILT, commit `06bf37e`
  (v1.0.0.6). `src/common/MM_PullPush.h` (AE-free, Gortler hole-filling
  form): mip pull of (premult RGB, coverage=alpha) over CORE pixels only,
  bilinear push fills only coverage-0 texels from the already-fully-covered
  coarser level, unpremultiply at the end; total fill guaranteed when any
  core pixel exists. View Mode = Extended Colour shows the unlimited
  (pre-`Extend`-clamp) straight-RGB fill, opaque; empty core renders black.
  Headless test `tests/test_pullpush.cpp` PASSED: constant-colour exactness
  (odd dims, varying alpha), bit-identical output under invalid-pixel
  garbage, convex-hull no-overshoot, proximity to nearest trusted block,
  semi-transparent unpremult recovery, full-valid passthrough, single-seed +
  1-wide strips, zero-alpha "valid" guard. *Gate (PASSED, user-confirmed
  2026-07-09 on real keyed plates):* full-frame fill, no blockiness, no
  zero-alpha bleed, Erode changes the colour source, corners fan cleanly.
  Two expected-behaviour observations recorded: (1) a near-edge "glow" —
  the outermost trusted core pixels still carry background contamination at
  small Erode; confirmed it RECEDES as Erode increases (the knob working as
  designed — deeper core = cleaner source). (2) far from the core the fill
  converges to the coverage-weighted global average colour (the 1x1 mip) —
  inherent to pull–push, harmless because Stage 4 only uses the band
  d <= Extend; visible only in the deliberately-unclamped Extended Colour
  view.
- **Stage 4 — Composite + Detail + Alpha Choke.** BUILT (v1.0.0.7). Full
  Result mode. Per-pixel formulas live in `src/common/MM_Composite.h` — the
  documented CPU/GPU lockstep contract:
  - Blend: `w(d) = 1 - smoothstep(0, Extend, d)`; in the band,
    `out = lerp(low, ext, w) + lerp(1, detail, w) * hi` in straight space,
    where `low` = alpha-weighted Gaussian split (`src/common/MM_Gaussian.h`,
    blur(premult)/blur(alpha), radius `ER_TEXTURE_BLUR_RADIUS`) and
    `hi = origStraight - low` (forced 0 at alpha~0). Detail attenuation is
    itself weighted by `w`, so the band's outer edge is seamless at any
    Detail — that's what makes the "differs only inside the band" gate hold.
    Core (d = 0) and beyond-band pixels take a bitwise-verbatim copy path.
  - Alpha Choke (per 2026-07-09 decision): anchored at the ALPHA'S OWN edge,
    not the core field — Erode-independent, profile-preserving. Choke-in:
    `newA = a * clamp01(dOut - c)`, dOut = EDT to nearest zero-alpha pixel
    (shaves the outermost c px, keeps the rest of the profile). Grow:
    dilates the solid (a>0.5) matte with a 1px soft rim, solid interior
    gated to identity (semi-opaque solids must not harden; rim fills toward
    1.0 — documented v1 simplification). The extra EDT runs ONLY when
    Choke != 0; Choke == 0 skips the path entirely → bit-identical alpha by
    construction. Grown zero-alpha pixels take the extension as their colour.
  - Early-out: Result view with Extend <= 0 and Choke == 0 is a full bitwise
    passthrough with no field computation.
  - 8/16/32bpc all flow through the same float pipeline via pack/unpack (no
    separate dispatch needed).
  Headless tests `tests/test_gaussian.cpp` + `tests/test_composite.cpp`
  PASSED (kernel/constant/impulse/linearity/weighted-divide; blend-weight
  endpoints+monotonicity, w=0 exact-orig for any Detail, outer-edge
  continuity, choke identity/ramp/zero, grow no-harden). *Gate (PENDING
  user visual check):* motion-blurred bright-background plate over dark
  shows no fringe and no dead-smooth synthetic zone; Alpha Choke=0 produces
  bit-identical alpha to input; Detail 0 vs 100 differs only inside the
  blend band. NOTE for Stage 5: the Gaussian/EDT/fill run single-threaded
  and allocate per frame — Result at 4K float will be slow (seconds/frame);
  that is Stage 5's whole job, judge Stage 4 on correctness only.
- **Stage 5 — CPU performance.** NOT STARTED. Parallelize EDT rows/columns
  and pull–push levels, reuse scratch buffers, early-out passthrough when
  Extend=0 and Alpha Choke=0. *Gate:* <~40ms/frame at 4K float on a modern
  desktop CPU, zero allocation churn per frame after warm-up, clean
  10,000-frame soak with stable memory.
- **Stage 6 — GPU path.** NOT STARTED, only after 0–5 signed off. DX12/HLSL
  kernels mirroring the CPU math exactly (threshold, Jump Flood distance,
  pull–push mip chain, composite). `PF_OutFlag2_SUPPORTS_GPU_RENDER_F32` +
  GPU device suites. *Gate:* CPU/GPU parity within tolerance, soak test
  repeated on GPU path.

## Build mechanics specific to Edge Repair

- Build with **`build_edgerepair.ps1`**, never `build.ps1` (that bumps
  ModernEdgeBlur's version — the user does not want EdgeRepair's work
  incrementing ModernEdgeBlur's build number). `build_edgerepair.ps1` bumps
  ONLY `src/EdgeRepair.h`'s `BUILD_VERSION` and, critically, mirrors the same
  bump into `resources/EdgeRepairPiPL.r` in the same call (see
  `Update-BuildVersion` in that script) — AE compares the packed
  `AE_Effect_Version` from both the compiled code and the PiPL, and any
  drift between them produces a misleading "version mismatch. Code version
  is 1.0 and PiPL version is 1.0" error (it prints only MAJOR.MINOR but
  compares the full packed integer). This bit us once already (fixed in
  `2076adf`) — if this error recurs, check both files' `BUILD_VERSION` match
  before assuming it's a stale AE cache.
- `build_edgerepair.ps1` does incremental configure (skips CMake
  reconfigure unless the generator changed) and does NOT auto-commit,
  unlike `build.ps1`.
- After building, copy `build/EdgeRepair.aex` to `dist/EdgeRepair.aex` and
  commit it — the `dist/` override in `.gitignore` allows this
  (`!dist/*.aex`).
- Headless algorithm tests live in `tests/` and run via
  `tests/build_tests.ps1` (plain `cl.exe`, no AE SDK). Add a new
  `tests/test_*.cpp` for each new AE-free algorithm header
  (`MM_DistanceField.h`, `MM_CoreMatte.h`, and future pull–push /
  blend-weight modules) — the brief requires the pure algorithms be
  unit-tested headlessly.
