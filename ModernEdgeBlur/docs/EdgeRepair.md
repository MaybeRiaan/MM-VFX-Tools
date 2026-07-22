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
spec from conversation history. (There is no separate handoff doc — this file
IS the handoff; it absorbed `docs/HANDOFF_VIEWMODES.md` on 2026-07-22.)

## Current status & resuming (2026-07-22)

- **Tip:** `origin/main` @ `c85d268`, **EdgeRepair v1.0.0.23**. Stages 0–5 are
  DONE + AE-gated; the view modes are now real deliverables (see that section).
  **Next real stage is Stage 6 — GPU (NOT STARTED)** — only after the user
  signs off that 0–5 + the view-mode pass are final.
- **This project is built on TWO PCs.** ALWAYS `git fetch origin` and reconcile
  BEFORE touching anything, and `git log --oneline -6` to confirm you're on top
  of the tip above. If you're behind, STOP and reconcile — in-flight edits on
  the other machine touch the same `src/EdgeRepair.*` files.
- **Do not modify ModernEdgeBlur's plugin code** (reference only).

## Product philosophy

A **paring knife, not a multitool**. Nuke's EdgeExtend node is the model: a
pure stream operator — RGBA in, RGBA out, RGB modified, alpha untouched
except an explicit opt-in choke. How the result composites downstream is the
user's business, not this plugin's.

Keyed/rotoscoped footage has contaminated semi-transparent edge pixels
(motion blur rims, halos from bright backgrounds). Edge Repair:

1. Builds a **core matte**: threshold the layer's alpha, then erode inward by
   `Erode` (px) so only trusted interior colour remains. (The engine's grow /
   dilate branch still exists but Erode is clamped >= 0 — see the param note.)
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
5. **Detail** (SYNTHETIC GRAIN — changed 2026-07-22, replacing frequency
   separation): `out = lerp(origRGB, ext, w) + w * detail * grain`, where
   `grain` is zero-mean luma noise scaled to the plate's own local grain
   amplitude (auto-estimated from the trusted core). So the repaired flat
   colour gets believable grain matched to the footage, instead of the plate's
   real high frequencies. This is the differentiator. (The old frequency-
   separation split `out = extendedLow + (origRGB - gaussianBlur(origRGB,r)) *
   detail` DARKENED the edge like a multiply — a real luminance edge and
   texture are the same frequency, so `hi = orig - low` re-added a signed edge
   step. Grain is zero-mean by construction and never sourced from orig's edge,
   so it cannot darken; it also fixed the premultiplied-input rim and dropped
   the Gaussian pass. See MM_Composite.h / MM_Grain.h.)
6. **Alpha Choke** (default 0 = pure RGB operator): remaps output alpha along
   the same distance field. Positive chokes the matte inward with smooth
   Euclidean falloff; negative grows it. No morphological min/max filters —
   reuse the distance field.

## Parameters — six (originally five, amended 2026-07-06; Softness appended 2026-07-22)

An earlier version of this brief specified seven params including a Softness
and a Premultiplied checkbox. **That version is superseded** — but note the
Softness added 2026-07-22 (row 6) is NOT that old edge-blur softness (which
ModernEdgeBlur owns and stays removed); it is a BAND inner-ease control (see
its row + the View modes section). The authoritative list:

| # | Param | Type | Range | Default | Disk ID |
|---|-------|------|-------|---------|---------|
| 1 | Erode | float slider (px) | 0 to 50 | 2.0 | 101 |
| 2 | Extend | float slider (px) | 0–500 | 32.0 | 102 |
| 3 | Detail | float slider (%) | 0–100 | 50 | 103 |
| 4 | Alpha Choke | float slider (px) | −50 to +50 | 0 | 104 |
| 5 | View Mode | popup: Composited / Core Matte / Distance Field / Extended Colour | — | Composited | 105 |
| 6 | Softness | float slider (px) | 0–250 | 5 | 106 |

Disk IDs are forever — never reorder, never reuse. Future additions append
with new disk IDs only. (Softness, disk ID 106, is the first append past the
original five — added on explicit user sign-off 2026-07-22.)

**Softness** is a fixed-**PIXEL** softness/ease width — one unified slider
shaping two things (see the View modes section for each mechanism):
- In the **Composited** repair: the inner-ease width = **Softness px**, fed
  straight to `BlendWeight`. Over the first Softness px out from the core the
  blend weight `w` ramps 0→1 instead of snapping to full replacement — easing
  the recomposited band back into the real core colour (kills the hard
  band/core line on wide light wraps).
- In the **Distance Field** matte: the ramp width = **Softness px** (clamped to
  the band) — how much of the band is solid white vs. a white→black gradient.

Range **0–250 px**, default **5 px** (≈ the old 15%-of-Extend-32 inner ease,
~4.8px, so the Composited look barely shifts — no longer byte-identical, but
visually indistinguishable). **0** = hard (Composited: snaps at core; Distance
Field: solid band with a hard cut). Switched from a % to fixed px on user
request 2026-07-22 — px is predictable, it doesn't rescale when Extend/Erode
change. This is NOT edge-blur/feather softness (that is ModernEdgeBlur's job
and remains a removed concept, below) — it reshapes distance-band WEIGHTS only;
sample points and the distance field are unchanged.

**Erode shrinks the trusted core inward** by that many px (0–50). It was a
signed −50..50 control through v1.0.0.22; **negative (dilate) was removed
2026-07-22** — it has no legitimate use for the decontamination job (growing
into the transparent exterior adds no colour; growing into the semi-transparent
rim re-trusts the contamination Erode exists to exclude). The engine's grow
branch (`BuildCoreMatte`, erode<0) remains for the headless tests and possible
future use, but is unreachable from the UI. Alpha Choke keeps its own signed
−50..50 (grow is meaningful there — matte dilation).

**Removed — must NOT appear anywhere (code, PiPL, or UI), and must not be
reintroduced without an explicit new ask:**
- **Edge-blur / feather / falloff softness** — a separate suite plugin
  (ModernEdgeBlur) owns edge shaping. Not this tool's job. (NOT to be confused
  with the Softness param added 2026-07-22, which is the distance-BAND inner
  ease — a weight-reshape, not an edge blur. See the param table.)
- **Premultiplied as a user parameter** — internal only. No knob, no
  override of AE's footage interpretation. **[CORRECTED 2026-07-09 — the
  original brief text here claimed "AE hands the effect premultiplied
  pixels"; that is WRONG and caused the Stage 4 round-1 "dark edge along
  the alpha boundary" bug.]** AE effect worlds are STRAIGHT (unmatted)
  alpha, in AND out — the same convention ModernEdgeBlur's black-halo bug
  established (premult→blur→unpremult). Wherever the maths must be
  coverage-weighted (pull–push level-0 init, the Detail low-pass Gaussian)
  the premultiply happens INTERNALLY and is divided back out; output RGB is
  never scaled by alpha.
- External matte/layer parameters, "recompose with original," channel
  selectors, invert toggles, grain/noise synthesis (Detail supersedes it), GPU
  code before Stage 6.

If a seventh parameter or any of the above ever seems needed: stop and ask,
don't add it unilaterally. (The current count is six as of 2026-07-22 —
Softness, disk ID 106, added on explicit sign-off.)

## View modes — deliverable semantics (2026-07-22, current as of v1.0.0.23)

The View Mode popup order and disk IDs are LOCKED (see the param table). This
pass promoted the diagnostic entries into real, deliverable-quality outputs and
renamed the default for clarity. **Label + render-quality only — no new params,
no reorder/renumber.** Per-view render code is the `ER_*ViewRow` / `ER_ResultRow`
functions in `EdgeRepair.cpp`.

- **Composited** (was "Result"; `ER_VIEW_RESULT` = 1, disk ID 105 — code
  identifier/value/ID all unchanged so saved `.aep`s still resolve here). The
  full repair composite (extend + Detail grain + Alpha Choke). Label-only
  change — the render output is **byte-identical** to what it produced before
  the rename; alpha is the original layer alpha (passthrough except Alpha
  Choke). This is the actual RGBA deliverable.

- **Extended Colour** — first-class **clean-plate** deliverable. Fully **opaque
  (α = 1) by design**: edge-extend has no transparency; it is the filled colour
  source an artist mattes themselves downstream. Renders the clean
  unpremultiplied straight-RGB pull–push fill (`ER_ExtViewRow`, `dst[0]=1.f`);
  `PullPushFill` premultiplies internally by coverage and divides it back out,
  so `ext` is straight RGB, colour-correct at 8/16/32bpc (all bit depths flow
  through the same float pack/unpack). Deliberately UNLIMITED (ignores the
  Extend clamp) so raw fill quality is inspectable everywhere. **Empty-core
  fallback = black** (opaque): no trusted pixels ⇒ no colour to pull ⇒ whole
  frame renders black (user-confirmed 2026-07-22; matches the Distance Field
  empty-core guard).

- **Distance Field** — first-class **luma MATTE** deliverable, fully **opaque
  (α = 1) across the whole frame**, usable directly as a luma/track matte for
  downstream edge work (light wraps, etc.). It is an **INWARD light-wrap matte
  of the erode gap**, WHITE-anchored at the ORIGINAL ALPHA EDGE, fading to BLACK
  toward the eroded core over an `Erode`-wide band. **Geometry settled
  2026-07-22:** the first cut rendered the composite band `w` (measured OUTWARD
  from the eroded core) — wrong side of the edge, radiating into the transparent
  exterior (caught at the gate from AE screenshots). Round 2 flipped it to the
  inward erode-gap band below. (A negative-Erode outward variant was briefly
  added and then removed with negative Erode itself — see the Erode note.)
  Render code: `ER_DistViewRow` / the `ER_VIEW_DISTANCE_FIELD` branch in
  `EdgeRepair.cpp`.
  - **Region:** pixels inside the alpha (`alpha > 0.5`, the pre-Erode
    silhouette) AND within the erode gap (`edgeDist <= Erode`, where `edgeDist`
    = Euclidean distance INWARD from the alpha edge). Black in the transparent
    exterior; black in the trusted core interior. `Erode == 0` → no gap → all
    black.
  - **Polarity:** **WHITE (1.0) at the original alpha edge**, gradienting
    INWARD to **BLACK (0.0)** at the eroded core boundary — the light-wrap
    convention (wrap strongest at the very edge, falling off into the subject).
    Measured inward from the alpha edge, NOT outward-from-core like the repair
    band.
  - **Reach = `Erode` px.** The band width tracks the Erode slider; Erode-DRIVEN
    and anchored at the alpha edge.
  - **Softness (fixed px)** shapes the ramp: `plateau = gap − Softness px`
    stays solid white, then smoothstep white→black over the remaining `Softness`
    px to the core (clamped to the gap). At Softness 0 the whole gap is solid
    white with a hard cut at the core; higher Softness lengthens the inward
    fade. Exact Euclidean distance, identical across 8/16/32bpc.
  - **NOTE — decoupled from the repair band.** Because it measures inward from
    the alpha edge (Erode-driven) rather than outward from the core, this matte
    is a STANDALONE light-wrap tool, NOT a bit-for-bit preview of the
    Composited repair band. (The round-1 design tried to make it a literal
    preview; the user's screenshots showed that's the wrong tool — they want a
    usable inward edge matte.)

- **Core Matte** — left AS-IS, diagnostic only (the layer's own alpha is the
  real matte). Not deleted (disk IDs are permanent); not invested in.

**Softness slider — ADDED 2026-07-22 (param #6, disk ID 106), on the user's
explicit sign-off during this pass.** They decided the Distance Field is "much
more useful" with a real softness control. It's one unified fixed-**px** slider
shaping softness in BOTH places (different mechanisms — the Distance Field is an
Erode-driven edge matte, not the repair band):
- **Composited band:** `inner = Softness px` feeds `BlendWeight`'s inner ease
  directly. **0 px** = hard inner (snaps at the core).
- **Distance Field matte:** `Softness px` shapes the white→black ramp across the
  `|Erode|`-wide band. 0 px = solid band with a hard cut; higher = longer fade.
Range **0–250 px**, default **5 px**. It was first shipped as a 0–50 % slider
(v1.0.0.19–.21) then switched to fixed px on user request — px is predictable
(doesn't rescale with Extend/Erode). Default 5px ≈ the old 15%-of-Extend-32
(~4.8px), so the Composited look barely shifts (no longer byte-identical, but
visually indistinguishable). See the Softness param row above and `ER_SOFT*` in
`EdgeRepair.h`. This replaces the fixed `ER_INNER_SOFT_FRAC` constant that
shaped the composite band through v1.0.0.18.

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
  continuity, choke identity/ramp/zero, grow no-harden).

  **Gate round 1 (2026-07-09, v1.0.0.7): FAILED — two real bugs, both fixed
  in v1.0.0.8:**
  1. *Dark edge along the alpha boundary* — the composite premultiplied its
     output (and unpremultiplied its input), but AE effect worlds are
     STRAIGHT alpha (see the corrected Parameters note above). Every
     semi-transparent pixel came out darkened by exactly its alpha. Fix:
     straight-in/straight-out everywhere; pull–push and the Detail low-pass
     premultiply internally only (`MM_PullPush.h` contract updated + its
     test; `EdgeRepair.cpp` ResultRow no longer scales RGB by alpha).
  2. *Layer shifts on effect toggle* — SmartRender inferred the
     output→source offset from buffer-size differences, which assumed AE
     clamps the inflated checkout symmetrically; one-sided clamping (layer
     larger than comp on one side, tiles) shifted the image by up to the
     inflation radius. Fix: PreRender stashes `in_result.result_rect` and
     the clamped output rect in pre_render_data; SmartRender derives the
     offset from the rect origins.
  Also recorded from round 1 (not bugs, by-design v1 semantics to revisit):
  choke-in truncates the soft skirt with a 1px ramp (Simple Choker-like —
  user provisionally OK with it); grow produces a full-opacity collar that
  steps against the surviving soft skirt (documented simplification in
  `MM_Composite.h`).

  **Gate round 2 (2026-07-09, v1.0.0.8): both round-1 fixes verified by the
  user** — (a) constant-colour solid + feathered mask: perfectly smooth
  gradient, zero colour introduced; (b) shift test: passes. **One further
  real bug found on a real-world plate and fixed in v1.0.0.9:** a light
  blue-grey rim at defaults — the Detail low-pass was ALPHA-WEIGHTED, which
  skews `low` toward the opaque side at edge pixels, so `hi = orig - low`
  captured the contamination difference instead of texture and Detail=100
  (the default) re-added the keyer fringe on top of the decontaminated
  base. Fix: `low` is now the brief's literal plain `gaussianBlur(origRGB)`
  (well-posed in straight-alpha worlds). Related observation from the same
  round: a fat dark repaint band at high Erode (8.2) — partly consequence
  of fighting the rim bug with Erode, partly inherent: eroded-away OPAQUE
  pixels are by definition untrusted and get their shading replaced by flat
  extension colour. Expect far less Erode to be needed post-fix.

  **Gate round 3 (2026-07-09, v1.0.0.9): user clarified the primary use
  case is LIGHT WRAP decontamination** (not defocus softness — that's edge
  extend + soft matting). Findings, with the deciding algebra:
  - At Detail 0 the composite reduces EXACTLY to `lerp(orig, ext, w)` (the
    low-pass cancels) — and the user confirmed Erode 2 / Detail 0 fully
    removes the wrap rim → the core pipeline (core → fill → weighted
    replacement) is verified correct.
  - At Detail 100 the composite is `orig + w*(ext - low)` — a purely
    LOW-frequency correction. A 2-4px wrap rim is high-frequency at texture
    radius 8, so Detail 100 mathematically cannot remove it; it rides on
    the darker repaired base and reads worse than source (jagged key
    structure). Fix in v1.0.0.10: `ER_TEXTURE_BLUR_RADIUS` 8 → 4 (in-spec
    bottom of the brief's ~4-8 range) — the radius IS the cutoff between
    "texture Detail keeps" and "contamination the repair removes". If rims
    still persist at Detail 100, going below 4 is a spec conversation
    (frequency alone cannot separate wrap rims from same-scale motion-blur
    micro-detail).
  - Erode > 2 producing a dark edge on the test pole plate is NOT a bug:
    the pole has genuine rim lighting; eroding past the wrap declares those
    lit pixels untrusted and the extension repaints them with deeper body
    colour. Erode should be set just past the wrap (2 on this plate).

  **Gate (PASSED 2026-07-22 on v1.0.0.11):** rim gone at Detail 0; Alpha
  Choke=0 bit-identical alpha; Detail 0 vs 100 differs only inside the band.
  At Detail 100 a same-scale contamination ring still rides back on the
  extended base — confirmed CORRECT per the composite algebra (`out = orig +
  w*(ext - low)` is a purely low-frequency correction; frequency separation
  cannot distinguish a same-scale contamination ring from legitimate edge
  texture — a fundamental limit, not a bug). Deciding this is expectation,
  not defect, the user chose to soften the out-of-box result rather than add
  a param: **Detail default lowered 100 -> 50** (`ER_DETAIL_DFLT` in
  `EdgeRepair.h`; range unchanged 0..100; spec table above updated).

  **DETAIL METHOD REPLACED + inner softness added (2026-07-22, v1.0.0.16,
  user-confirmed in AE after Stage 5):** revisiting Detail on live footage, the
  frequency-separation split was still DARKENING the edge like a multiply/
  overlay (not just re-adding contamination) — because a real luminance edge
  and texture are the same frequency, so `hi = orig - gaussianBlur(orig)` at
  the matte boundary is a SIGNED edge step, and re-adding it inside the band
  paints that step back on. This is a fundamental limit of frequency
  separation for this job, not tuning. **Detail is now SYNTHETIC GRAIN**
  (`src/common/MM_Grain.h`): deterministic zero-mean per-pixel luma noise,
  amplitude auto-estimated from the trusted core (mean |luma neighbour diff|
  over core pixels * 1.53), composited as `out = lerp(orig,ext,w) +
  w*detail*grain` (`ComposeBandRGB`). Zero-mean by construction and never
  sourced from orig's edge → CANNOT darken (headless-proven: mean composite
  bias ~1e-4). Bonus: fixes the premultiplied-input rim (never touches orig's
  edge RGB) and DROPS the Detail Gaussian pass entirely. User AE-confirmed: no
  darkening at any Detail 0..100, grain plausible + self-scales with footage
  grain, on both live keyed and premultiplied footage. The Gaussian header/
  test remain (general utility) but nothing in the plugin calls them now.

  Same round, the OPEN DESIGN NOTE was resolved: a hard `w=1` replacement at
  the core boundary left a visible line where band meets core on WIDE LIGHT
  WRAPS (band side = flat `ext`, core side = real `orig`). Fixed by reshaping
  the blend weight with an INNER EASE — `w = smoothstep(0,inner,d) *
  (1 - smoothstep(0,extend,d))`, `inner = ER_INNER_SOFT_FRAC(0.15) * Extend` —
  so w ramps 0->1 up from the core instead of snapping, blending the band back
  into the real core colour. Reshapes the WEIGHT only; sample points/distance
  field unchanged. No new param (tied to Extend, resolution-relative). User
  confirmed 15% "feels good". `BlendWeight` gained a 3-arg (extend, inner)
  form; the 2-arg form is unchanged for back-compat. Tests: `test_grain.cpp`
  (grain zero-mean/decorrelated/seamless/no-darkening), `test_composite.cpp`
  `blendweight-inner` (eases 0 at core, peaks in band).

  NOTE for Stage 5: the Gaussian/EDT/fill run single-threaded and allocate
  per frame — Result at 4K float will be slow (seconds/frame); that is Stage
  5's whole job, judge Stage 4 on correctness only.
- **Stage 5 — CPU performance.** DONE (v1.0.0.14), gate re-defined and PASSED
  2026-07-22. All work is BYTE-IDENTICAL to Stage 4 output — proven, not
  asserted (see tests below). What landed:
  - **Injected parallel-for** (`src/common/MM_Parallel.h`): `mm::ParallelFor`
    with a pluggable backend — serial (deterministic) in the headless tests,
    `iterate_generic` inside AE (`mm::MakeIterateGenericBackend` in
    `MM_Common.cpp`; installed for the render scope via `ScopedParallelBackend`
    in `SmartRender`). EDT columns+rows, separable Gaussian H/V, and pull–push
    (per mip level) all fan out through it. Every parallel work item writes
    DISJOINT output, so the result is independent of thread count — that is why
    threading changes no bits.
  - **Zero per-frame allocation** after warm-up: all scratch is grow-only
    `thread_local` — both the header internals (EDT `f`, core-matte temps,
    pull–push mip chain) and SmartRender's working set (`ER_Scratch`).
    `thread_local` = per AE worker = MFR-safe, no mutable global/static state.
  - **Band-box-limited Detail Gaussian:** `low` is only computed inside the
    bounding box of the blend band `{0<d<=Extend}` + kernel margin (the rest of
    the frame is never read by the composite). Uses `GaussianBlurH/V_Rect`,
    which clamp source reads against the FULL image, so every pixel the
    composite reads is bit-for-bit identical to the old full-frame blur. Win is
    subject-dependent (~34% of a frame-filling subject's edge, ~3% for a small
    one; see `stage5_perf_edgerepair` memory).
  - **Byte-identical interior/border split** in the Gaussian, and the
    early-out passthrough (Extend<=0 && Choke==0) allocates nothing after
    warm-up.
  - Gaussian box-blur APPROXIMATION was built and REJECTED — measured 2.3x
    SLOWER (the blur is memory-bandwidth-bound at 4K, not compute-bound), so
    the exact Gaussian stays.

  *Original gate was <~40ms/frame at 4K.* Re-defined 2026-07-22: 40ms at 4K is
  not reachable on an 8-thread CPU (test machine = i7-9700K, 8c/8t, no HT) with
  correctness-preserving CPU work alone; true 4K-realtime is the GPU path's job
  (Stage 6). **Re-defined gate (PASSED):** large multiplicative speedup
  (~980ms→~250ms headless hot-passes, ~4x on 8 threads; ~1150ms→~300ms in AE),
  ZERO allocation churn per frame after warm-up, clean multi-thousand-frame
  soak with stable memory, and BYTE-IDENTICAL output vs Stage 4. AE-confirmed
  2026-07-22 via an A/B (`EDGEREPAIR_FULLBLUR` env toggle, since removed):
  band-box vs full-frame blur gave identical results on real footage.

  Tests (all headless, `tests/build_tests.ps1`, no AE): `test_soak.cpp`
  (2000-frame determinism + 0 allocations/frame after warm-up), plus
  distancefield/gaussian/pullpush/composite/grain all green. Bench harness:
  `tests/run_perf.ps1` (serial-vs-threaded bit-compare + timings). AE perf
  instrumentation: `ER_PERF_LOG` in `EdgeRepair.cpp` (default 0), read with
  `tests/read_ae_perf.ps1`.

  SUPERSEDED NOTE: Stage 5's band-box-limited Gaussian (blur only the Detail
  blend band's bbox) and its `test_bandblur.cpp`/`test_bandcomposite.cpp`
  guardrails were REMOVED on 2026-07-22 when Detail switched from frequency
  separation to synthetic grain (see the Stage 4 entry) — grain needs no
  Gaussian at all, so the band-box optimisation became moot. The
  `GaussianBlurH/V_Rect` region-limited passes were removed with it. The rest
  of Stage 5 (threading, zero-alloc, early-out) stands unchanged.

  RESOLVED (was a known separate issue): premultiplied-alpha input re-adding a
  light edge rim at high Detail — the synthetic-grain Detail fixed it (grain
  never touches orig's edge RGB), confirmed in AE on premultiplied footage
  2026-07-22.
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
