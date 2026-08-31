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

> **GPU work: read [`AE_GPU_Effects.md`](../../docs/AE_GPU_Effects.md) first.** How AE
> 2026 actually GPU-accelerates effects (MediaCore GPUVideoFilter /
> `xGPUFilterEntry` — NOT the documented AE-SDK `SUPPORTS_GPU_RENDER_F32`
> path), the five-point checklist, the silent-failure traps, and how to
> verify it is really running. That doc is **suite-wide and applies to all
> five plugins**; the Stage 6 section below is only Edge Repair's own status
> and raw experiment log.

## Current status & resuming (2026-08-02)

- **EdgeRepair v1.0.0.36. ALL SIX STAGES ARE DONE AND AE-GATED.** The CPU path
  remains the reference/fallback (correct, threaded, ~300 ms/frame at 4K on 8
  threads); CUDA carries the full pipeline when AE offers it.
- **Stage 6 (GPU): GATE PASSED 2026-08-02, user-confirmed.** CPU/GPU parity
  holds on real footage across the checklist (difference render at Detail 0 and
  50, all four View Modes, Alpha Choke identity and ±, 8/16/32 bpc, RAM preview
  + Render Queue, no launch failures in the log). Nothing outstanding on
  correctness.
- **Parked, by the user's call (2026-08-02):** some look/parameter values want
  fine-tuning at some point — deferred behind higher priorities, and NOT a
  defect list. Which values was left unspecified; ask before changing defaults.
- Remaining Stage 6 *cleanup* (none of it blocking, all listed at the end of
  the Stage 6 entry): bisect the unconditional `GPU_RENDER_POSSIBLE`, and
  optionally the DX12/Metal orchestration.

### How the gate was reached (2026-07-30)

- **AE gate open + the CUDA kernel port, same day.**

  *Gate open (v1.0.0.34):* AE hands us `what_gpu = CUDA` at PreRender and calls
  `PF_Cmd_SMART_RENDER_GPU` at 4K. **The mechanism is the documented AE-SDK F32
  path**, not the MediaCore GPUVideoFilter mechanism an earlier revision of
  these docs claimed — GPUVideoFilter is *Premiere's* path. The full, corrected
  route is `../../docs/AE_GPU_Effects.md`; read it before GPU work on any plugin.

  What actually unblocked it, after seven failed hypotheses: **`GPU_DEVICE_SETUP`
  must re-affirm `out_data->out_flags2 = PF_OutFlag2_SUPPORTS_GPU_RENDER_F32`**
  (a plain assignment) for each device. Without it AE completes device setup and
  then offers `what_gpu = NONE` forever. Three supporting changes: restore
  `SUPPORTS_DIRECTX_RENDERING`, drop `USE_OUTPUT_EXTENT` (PiPL now
  `0x02000400 / 0x2A001400`), and install to **AE's** Plug-ins folder rather
  than MediaCore.

  Ground truth came from building Adobe's own AE GPU sample,
  `<AE SDK>\Examples\Effect\SDK_Invert_ProcAmp`, and instrumenting it — it
  GPU-renders on this box, which proved the F32 path works for third parties.

  *Kernel port (this round):* the GPU now runs the REAL pipeline on CUDA, not a
  passthrough — see the Stage 6 entry below for the pass list, the memory
  layout, and what is still unported (DX12 host orchestration, Metal). The
  temporary `ER_GPU_VISIBLE_MARKER` is deleted; the effect's own output is the
  proof it ran. **Not yet AE-verified — the AE gate is the next action** (test
  protocol in the Stage 6 entry).

  `src/gpu/ER_GPUFilter.cpp` (the GPUVideoFilter module) is inert in AE and
  built OFF by default; keep it only if Premiere support is wanted.
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
   the Gaussian pass. See ER_Composite.h / ER_Grain.h.)
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

Range **0–250 px**, default **5 px**. **0** = hard (Composited: snaps at core;
Distance Field: solid band with a hard cut). Fixed px rather than a % since
2026-07-22 — px is predictable, it doesn't rescale when Extend/Erode change.
This is NOT edge-blur/feather softness (that is ModernEdgeBlur's job and remains
a removed concept, below) — it reshapes distance-band WEIGHTS only; sample
points and the distance field are unchanged.

> **This geometry is deliberately unchanged — do not re-anchor it again without
> testing in AE first.** Two attempts were built and rejected on 2026-08-24
> (v1.0.0.37 and .38); the reasoning and the measurements are recorded under
> *Softness / Extend anchoring* below. The **Extend** re-anchoring from the same
> pass was kept.

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
    fade. Exact Euclidean distance, identical across 8/16/32bpc. **Unchanged —
    two re-anchorings were rejected in AE, see below.**
  - **Degenerate frames.** A frame with NO alpha edge anywhere (all-opaque or
    all-transparent across the inflated source) renders **black** — there is
    nothing to measure from. Both `ER_DistViewRow` and `ER_ViewDist` gate on
    that explicitly. Before 2026-08-24 they fed `edgeDist = 0` through the ramp
    instead, which reads as "right at the edge" and painted an all-opaque frame
    fully WHITE at any Erode > 0.
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

(Both survived the 2026-08-24 anchoring pass unchanged — see below.)
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
  (`src/core/ER_Common.h/.cpp`: pack/unpack, `ForEachRow` over
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
  `src/core/ER_DistanceField.h` (AE-free EDT + brute-force reference),
  `src/core/ER_CoreMatte.h` (threshold + signed erode via distance-to-
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
  (v1.0.0.6). `src/core/ER_PullPush.h` (AE-free, Gortler hole-filling
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
  Result mode. Per-pixel formulas live in `src/core/ER_Composite.h` — the
  documented CPU/GPU lockstep contract:
  - Blend: `w(d) = 1 - smoothstep(0, Extend, d)`; in the band,
    `out = lerp(low, ext, w) + lerp(1, detail, w) * hi` in straight space,
    where `low` = alpha-weighted Gaussian split (`src/core/ER_Gaussian.h`,
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
     premultiply internally only (`ER_PullPush.h` contract updated + its
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
  `ER_Composite.h`).

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
  (`src/core/ER_Grain.h`): deterministic zero-mean per-pixel luma noise,
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

  **EXTEND RE-ANCHORED; SOFTNESS TRIED, REJECTED, REVERTED (2026-08-24,
  v1.0.0.37 -> .39).** Two user reports from AE opened this: *"the softness
  radius seems anchored on the original alpha edge, not the inward eroded edge —
  higher values effectively shrink the erosion back to the original edge"*, and
  *"if Erode goes to high values I have to increase Extend to reach beyond the
  original alpha"*. The diagnosis was that everything was measured OUTWARD FROM
  THE ERODED CORE, so the erode gap — the contaminated rim Erode exists to carve
  off — was the first thing every other knob spent itself on. With `d` = the core
  distance field (0 at the core boundary, alpha edge at `d = Erode`):

  - *Extend spent itself crossing the gap.* `outer = 1 - smoothstep(0, Extend, d)`
    started at the core, so the band died `Extend - Erode` px past the alpha
    edge. Erode 10 + Extend 32 reached only **22 px** past the matte, and raising
    Erode silently shortened the extension.
  - *The inner ease ate the gap.* `innerW = smoothstep(0, Softness, d)` ramped up
    going OUTWARD from the core, i.e. straight through the gap. At Erode 10 /
    Extend 32, Softness 32 dropped the whole gap under `w = 0.18` and moved the
    band's peak to `d = 16`, six px OUTSIDE the matte; at Softness >= Extend it
    degenerated into a global "turn the effect down" knob.
  - *The Distance Field matte could only lose coverage.* `plateau = gap -
    Softness` carved the ramp OUT of the gap, reach pinned at Erode: Softness 0
    -> 10 px of white, 5 -> 7.5, 10+ -> 5 and clamped.

  **KEPT — the Extend fix.** `BlendWeight` gained a `gap` argument: the erode gap
  is a full-weight plateau and the roll-off is measured from the ALPHA EDGE,
  `outer = d <= gap ? 1 : 1 - smoothstep(0, Extend, d - gap)`. Outward reach past
  the matte is now `Extend` px whatever Erode is, and the two knobs are
  independent. Erode 0 is bit-identical to before (the gap term vanishes).

  **REVERTED — the softness geometry, after two failed attempts.** Both were
  built, tested in AE, and rejected:

  1. **v1.0.0.37 — feather running INWARD from the core boundary** over the full
     Softness width, on a SIGNED distance field (negative inside the core). It
     kept the gap fully repaired, but pushed the repair up to 250 px into the
     subject: *"this feels quite aggressive — the softness doesn't just soften
     the inner edge, it extends it significantly"*. It also cost a second EDT:
     measured at 4K, core+field went **396.6 -> 589.9 ms (1.49x)** — *"it also
     seems to be very slow to respond"*. (That cost was avoidable: for Erode > 0
     `BuildCoreMatte` already runs the exterior EDT and the core boundary is its
     `Erode` level set, so the depth is `edgeDist - Erode`, an O(n) subtraction —
     measured back down to **400.9 ms, 1.01x**. Recorded here in case a signed
     field is ever wanted again; it is not in the tree.)
  2. **v1.0.0.38 — ModernEdgeBlur's Blend Radius model**, from the user's own
     framing: *"it should behave with the same kind of softness as the modern
     edge blur, but the edge width is the erode amount"*. MEB blurs the eroded
     mask symmetrically, 50% contour staying on the eroded edge, so the feather
     was centred on the core boundary with width `min(Softness, Erode)` — softens
     in place, cannot move or grow the edge, cannot exceed the band. Verdict:
     *"this doesn't feel right at all. I feel like we were closer before we
     started all this."*

  **v1.0.0.39 restores the pre-.37 softness exactly** — inner ease anchored at
  the core and measured outward, Distance Field ramp carved out of the gap —
  with the Extend fix retained. Field cost is back to baseline (`ER_CoreMatte.h`
  is byte-identical to its pre-pass state; no signed field is built anywhere).

  **DO NOT re-anchor the softness again without testing in AE first.** Three
  geometries have now been tried and the original won. The diagnosis above is
  arguably still correct in the abstract — Softness genuinely does trade gap
  coverage for smoothness — but it is what the look was tuned around, and every
  "more correct" alternative read worse on real footage.

  Also fixed in this pass and KEPT: a frame with no alpha edge at all
  (all-opaque or all-transparent across the inflated source) used to render the
  Distance Field view fully WHITE at any Erode > 0, because `edgeDist = 0` was
  fed through the ramp and read as "right at the edge". Both `ER_DistViewRow`
  and `ER_ViewDist` now gate on "this frame has an alpha edge at all" and render
  black.

  Touched: `ER_Composite.h` / `ER_KernelMath.h` (`BlendWeight` / `ER_BlendWeight`
  are now 4-arg, `(d, gap, extend, inner)` — the 3-arg form is GONE so no call
  site can silently keep the old Extend anchoring; the 2-arg hard curve stays as
  the tests' reference shape), `EdgeRepair.cpp` (`ER_ResultRow` gap wiring,
  `ER_DistViewRow` haveEdge gate), `EdgeRepair_GPU.cpp` (the composite pass
  passes the gap in `p.erode`). Inflation is unchanged and still correct:
  `Erode + Extend + |Choke| + blur` already covers the new outward reach.
  Tests: `test_composite.cpp` `blendweight-gap-extend` (gap plateau at full
  weight; band ends at `gap + extend`; equal weight at equal distance from the
  alpha edge across gaps 0..50; original inner-ease shape preserved; Softness 0
  reduces to the 2-arg curve), `test_gpuparity.cpp` `blendweight` and
  `distance-field view`.


  NOTE for Stage 5: the Gaussian/EDT/fill run single-threaded and allocate
  per frame — Result at 4K float will be slow (seconds/frame); that is Stage
  5's whole job, judge Stage 4 on correctness only.
- **Stage 5 — CPU performance.** DONE (v1.0.0.14), gate re-defined and PASSED
  2026-07-22. All work is BYTE-IDENTICAL to Stage 4 output — proven, not
  asserted (see tests below). What landed:
  - **Injected parallel-for** (`src/core/ER_Parallel.h`): `er::ParallelFor`
    with a pluggable backend — serial (deterministic) in the headless tests,
    `iterate_generic` inside AE (`er::MakeIterateGenericBackend` in
    `ER_Common.cpp`; installed for the render scope via `ScopedParallelBackend`
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
- **Stage 6 — GPU path. DONE — GATE PASSED 2026-08-02 (v1.0.0.36),
  user-confirmed on real footage.** AE gate opened 2026-07-30 and the CUDA
  kernel port landed the same day. See "Current status" at the top of this file
  and `../../docs/AE_GPU_Effects.md` before any further GPU work on any plugin.

  ### Kernel port — what landed (2026-07-30, v1.0.0.36)

  The CUDA backend now runs the whole repair on the GPU, mirroring
  `SmartRender`'s CPU pipeline pass for pass. All four View Modes render on
  GPU; there is no "GPU does something different" mode left except the two
  documented divergences below.

  - **Shared math, four targets — as designed.** Every per-pixel formula moved
    into `src/gpu/ER_KernelMath.h`, which compiles as HLSL, CUDA, Metal AND
    host C++. Each function names its `MM_*.h` twin in a comment; the lockstep
    contract at the top of that file is now load-bearing rather than aspirational.
  - **19 CUDA entry points** in `src/gpu/EdgeRepairKernels.cu` (thin wrappers —
    entry decorations and buffer plumbing only): `ER_Passthrough`, `ER_Zero`,
    `ER_ExtractAlpha`, `ER_Classify`, `ER_MakeCore`, `ER_JfaInitAlpha`,
    `ER_JfaInitMask`, `ER_JfaStep`, `ER_JfaResolve`, `ER_PPInit`, `ER_PPPull`,
    `ER_PPPush`, `ER_PPResolve`, `ER_GrainPartial`, `ER_GrainFinal`,
    `ER_Composite`, `ER_ViewCore`, `ER_ViewDist`, `ER_ViewExt`. Names are
    resolved by string at device setup; **a missing entry declines the whole
    CUDA backend** rather than failing mid-render (the classic stale-PTX
    symptom, and there is no CPU fallback once worlds are device memory).
  - **Pass sequence** (`ER_RenderCUDA`, EdgeRepair_GPU.cpp): alpha plane →
    interior → [erode field] → core matte → core distance field → pull–push
    fill → grain amplitude → [alpha-choke field] → composite/view. Same
    host-side branch decisions as the CPU (`earlyOut`, `doBand`, the three
    `BuildCoreMatte` modes), computed from the same params.
  - **Host-side branching WITHOUT host readback.** The CPU asks questions
    between passes ("empty core?", "did the fill find a seed?", "is there an
    alpha edge at all?") and branches. The GPU can't — a readback would mean
    synchronizing AE's stream. So the producing kernels `atomicOr` a bit into a
    tiny device flags buffer (`ER_FLAG_CORE/FILL/CHOKE/INTERIOR/EXTERIOR`) and
    the consuming kernels test it. Every CPU early-out has a matching device
    flag; `test_gpuparity` covers the degenerate cases explicitly.
  - **Memory.** ONE cached device allocation per GPU device, sub-divided per
    render (planes, two jump-flood seed buffers, the pull–push mip chain,
    partials, flags) and grown only when a bigger frame arrives — the device-side
    analogue of Stage 5's `ER_Scratch`, so a steady-state render allocates
    nothing. Only what the current view needs is reserved: a Core Matte view at
    Erode 0 pays for neither the chain (~4/3 × 16 B/px) nor the distance plane.
    Freed at `GPU_DEVICE_SETDOWN`.
  - **Concurrency.** `renderLock` now guards the whole enqueue AND the shared
    scratch. That is sufficient *because* every launch goes on AE's single
    per-device stream, which is FIFO: serializing the enqueue serializes
    execution, so render A's passes finish before render B's touch the scratch.
    AE still owns stream synchronization — we never sync.
  - **BGRA, not ARGB.** GPU worlds are `PF_PixelFormat_GPU_BGRA128`; the CPU
    path's `UnpackRow` ARGB convention does not apply. Kernels read `.z/.y/.x`
    for R/G/B and `.w` for alpha. Scratch planes are tightly packed at
    pitch == W, which is NOT the worlds' pitch — mixing the two is the easiest
    bug to write here.
  - **Params** are read in `SmartRender` BEFORE the CPU/GPU fork and passed to
    `ER_SmartRenderGPU`; the launch block is one 112-byte struct that must stay
    field-identical across `ER_GPU_PassParams` (C++) / `ERPassParams` (HLSL) /
    `ER_PassParamsCu` / `ER_PassParamsMtl`. A `static_assert` pins the size.
  - **`ER_GPU_VISIBLE_MARKER` deleted** — it existed only because the milestone
    passthrough rendered bit-identically to the CPU.

  ### Two accepted divergences (why the gate is "within tolerance")

  1. **Distance = Jump Flood (JFA+2), not the exact EDT.** User-accepted
     2026-07-23. Measured by `tests/test_gpuparity.cpp` against
     `er::EuclideanDistanceTransform` on five seed topologies (single seed,
     disc, sparse noise, thin line, two corners): **worst error 0.0000 px** —
     exact on those shapes. The test asserts a 0.5 px tolerance rather than
     locking in "happens to be exact".
  2. **Grain amplitude sums in float, not double.** The CPU accumulates
     `|luma neighbour diff|` serially in `double`; the GPU uses a fixed
     256×256 grid, block-reduces in shared memory, and finalizes serially over
     256 partials. Measured relative difference on a synthetic plate: **0.0**,
     with the pair COUNT matching exactly (the test asserts both).

  Everything else is **bit-identical**, and proven so rather than asserted —
  see the parity harness below.

  ### `tests/test_gpuparity.cpp` — the parity harness (headless, no GPU)

  Runs the GPU's own functions on the CPU (that is what the host-C++ mode of
  `ER_KernelMath.h` is for) and diffs them against the `MM_*.h` originals:
  blend weight, smoothstep, band composite, both choke directions, the grain
  hash + amplitude conversion, the core-matte decision across all three erode
  branches + degenerate seed sets, the Distance Field view value, the
  pull–push value helpers, world/plane addressing, and seed packing — all
  `==`, no epsilon. Plus three composition-level checks that validate the HOST
  wiring, not just the formulas: the whole pull–push chain built exactly as
  `ER_RenderCUDA` schedules it vs `er::PullPushFill` (worst 6e-7), the grain
  reduction in its real grid shape, and the JFA schedule vs the exact EDT.
  Runs in `tests/build_tests.ps1` alongside the rest (all 7 files green).

  ### NOT ported — deliberately, with reasons

  - **DX12 host orchestration** is still the passthrough milestone. Nothing on
    the test box exercises it (AE offers CUDA only; the Premiere GPUVideoFilter
    module is built OFF by default), so porting it would mean ~400 lines of
    multi-pass D3D12 descriptor plumbing that no gate can check. The MATH is
    already shared, so finishing it is orchestration only: mirror the CUDA
    entries in `EdgeRepairKernels.hlsl` and issue the same pass sequence
    through `DXShaderExecution`. fxc still compiles the HLSL every build and
    type-checks the whole translation unit — which already caught one
    C++/CUDA-only construct in the shared header (a ternary over a struct
    type), so the header stays honest about HLSL's rules for free.
  - **Metal** likewise: param struct kept in sync, entries not mirrored, no Mac
    to gate on.
  - **The unconditional `GPU_RENDER_POSSIBLE` in PreRender** stays for this
    round on purpose — changing it in the same build as the kernel port would
    put two variables into one AE test. Bisecting it back to the gated form
    (`what_gpu != NONE && gpu_data`) is the next cleanup AFTER the kernel gate
    passes.

  ### *Stage 6 kernel gate — PASSED 2026-08-02*

  The protocol below is what the user ran, and all of it checked out. Keep it:
  it is the regression checklist for any future change to the GPU path, and the
  difference render (step 1) is the only way to tell a real divergence from a
  plausible-looking one.

  With Project Settings → Mercury GPU Acceleration = CUDA:

  0. **The numeric A/B, and the one that actually proves parity.** One frame,
     Composited, defaults but Detail 0. Render Queue to **32-bit EXR** on CUDA
     (`A`), switch to Software Only, render again (`B`). Comp `A` over `B`,
     blend mode **Difference**, Levels input-white pulled to ~0.004 (≈250x
     amplification). Expect essentially black. Structure confined to the band =
     a real math divergence; non-black everywhere, or a shifted ghost =
     addressing/pitch/offset, not math. Repeat at Detail 50 to bring the grain
     amplitude reduction in. Expected residual is ~1e-7 on band edges only —
     anything visible without amplification is a bug.
  Then, as stage isolation (each narrows a failure to one kernel group):
  1. Apply Edge Repair to a keyed plate. **The result must now MATCH the CPU
     render** (Mercury → Software Only), not pass through — that is the whole
     point of this round. Compare at Erode 2 / Extend 32 / Detail 50.
  2. Sweep every param, and all four View Modes on both settings. Distance
     Field and Extended Colour are opaque deliverables; Composited keeps the
     original alpha unless Alpha Choke is touched.
  3. Alpha Choke 0 must leave alpha untouched; non-zero must choke/grow the
     same way the CPU does.
  4. Extend 0 + Choke 0 = the effect does nothing (early-out passthrough).
  5. RAM preview + Render Queue, 8/16/32 bpc, no errors or crashes.
  6. `%TEMP%\EdgeRepair_gpu.log` should show `GPU plan view=… scratch=…MB`
     lines and no `cuLaunchKernel(...) FAILED`.
  If GPU rendering stops engaging entirely (log shows `PRERENDER what_gpu=0`),
  that is a regression in the AE gate, not the kernels — the flag experiment
  above is the first thing to look at.

  ### Earlier Stage 6 design decisions (still standing)

  Settled with the user 2026-07-23,
  grounded in the actual SDK headers (AE 2026 exposes FOUR frameworks —
  OpenCL/Metal/CUDA/DirectX — via `PF_GPU_Framework`):
  - **Shared-kernel layout** (the SDK samples' pattern): all per-pixel math
    lives ONCE in `src/gpu/ER_KernelMath.h`, which compiles as HLSL, CUDA,
    Metal, OpenCL AND host C++ (so headless tests can call the exact GPU
    functions). Backend wrapper files (`EdgeRepairKernels.hlsl/.cu/.metal`)
    hold ONLY entry decorations + buffer plumbing. This is what makes the
    later Metal build a toolchain step, not a port (explicit user
    requirement).
  - **Backends:** DirectX 12 built + shipped now (works on any Windows GPU,
    incl. the NVIDIA test box; Adobe's own `${AE_SDK_ROOT}/Util/DirectXUtils` helper
    targets it). CUDA source maintained but NOT built — no CUDA toolkit on
    the build machines (no nvcc); enabling it later = install toolkit + add
    the nvcc/PTX step + the CUDA branch in `EdgeRepair_GPU.cpp`. Metal
    source maintained, builds on a Mac later. OpenCL declined. Unsupported
    frameworks are declined QUIETLY at `PF_Cmd_GPU_DEVICE_SETUP`
    (`PF_Err_NONE`, null `gpu_data`) → clean CPU fallback, no error dialog.
  - **Distance on GPU = Jump Flood (JFA+2), approximate** — user-accepted
    2026-07-23, so the Stage 6 parity gate is **within tolerance**, NOT
    byte-identical (deliberate break from the CPU stages' discipline; JFA+2
    error is sub-pixel).
  - **Plumbing milestone — BUILT (v1.0.0.24), AWAITING AE GATE:**
    `PF_OutFlag2_SUPPORTS_GPU_RENDER_F32` + `SUPPORTS_DIRECTX_RENDERING`
    (PiPL OutFlags_2 now `0x2A001400` — must keep matching GlobalSetup);
    `PF_Cmd_GPU_DEVICE_SETUP/SETDOWN` in `src/EdgeRepair_GPU.cpp` (wraps
    AE's ID3D12 device/queue in Adobe's `DXContext`, loads precompiled
    kernels); PreRender raises `PF_RenderOutputFlag_GPU_RENDER_POSSIBLE`
    only when the framework is supported AND device setup succeeded;
    SmartRender forks to `ER_SmartRenderGPU` on `what_gpu != NONE` (fork
    sits BEFORE the pixel-format check — GPU worlds are
    `PF_PixelFormat_GPU_BGRA128` and must never reach the CPU unpack; no
    CPU fallback after a GPU error since CPU code cannot address device
    memory). The output→source offset (srcOX/srcOY) is computed once,
    pre-fork, from the PreRender rects and fed to kernels as launch params.
    First kernel = `ER_Passthrough`, a bit-exact raw copy of the output
    rect from the inflated source — proves device setup, `.cso`/`.rs`
    loading, world addressing incl. the inflation offset, dispatch, and
    synchronization before any real math ports. Kernels are compiled at
    build time by fxc (Windows SDK, globbed in CMake) into
    `DirectX_Assets/EdgeRepair_*.cso` + `.rs` DEPLOYED NEXT TO THE .AEX
    (both the AE plugins folder and `dist/` — `dist/DirectX_Assets/` is now
    part of the deliverable). Binding ABI fixed by
    `${AE_SDK_ROOT}/Util/DirectXUtils.cpp`: root param 0 = CBV table, 1 = UAV table,
    2 = SRV table, raw ByteAddressBuffers; a per-device mutex serializes
    the shared D3D12 command list (AE does not promise GPU renders are
    serialized). Param structs must stay field-identical across
    `ERPassParams` (HLSL) / `ER_PassParamsCu` / `ER_PassParamsMtl` /
    `ER_GPU_PassParams` (C++).
  - ***Milestone gate:*** (1) effect loads clean (PiPL/flags match); (2) with
    Project Settings → Mercury GPU Acceleration ON, the effect renders the GPU
    passthrough — i.e. it shows the SOURCE FOOTAGE (params do nothing): that
    is the success signal that AE took the GPU path end-to-end; (3) toggling
    GPU acceleration OFF restores the full CPU repair (regression check);
    (4) no errors/crashes across RAM preview + render queue at 8/16/32bpc.

  - **Stage 6 investigation state (2026-07-23/24) — READ BEFORE THEORIZING.**
    The gate is BLOCKED: AE never engages the GPU path. All findings below are
    measured via `ER_GPU_LOG` diagnostics (`EdgeRepair_GPU.cpp`, logs to
    `%TEMP%\EdgeRepair_gpu.log`; first-N rate limits per event class).
    - **CUDA backend now exists and initializes successfully in AE.** AE 2026
      Win/NVIDIA offers effects `what_gpu=CUDA` ONLY (measured; it never
      probes DIRECTX even with `SUPPORTS_DIRECTX_RENDERING` set — that flag
      appears to serve Premiere hosts; the Mercury dropdown on the test box
      lists ONLY Software/CUDA). CUDA host side runs through a runtime
      `nvcuda.dll` driver-API shim (`src/gpu/ER_CudaShim.h`, no toolkit needed
      to compile); the kernel ships as PTX (`CUDA_Assets/EdgeRepair_Kernels
      .ptx`, `compute_75`) built by nvcc from `EdgeRepairKernels.cu`. Build
      needs CUDA toolkit 12.8 + the `-ccbin` 14.4x toolset + `--use-local-env`
      + PATH/INCLUDE/VCToolsInstallDir overrides in CMakeLists (CUDA 12.8's
      nvcc cannot digest VS 2026's newest STL; the older side-by-side 14.4x
      toolset is used for the PTX step only).
    - **The failure signature:** `DEVICE_SETUP what_gpu=3 → OK (CUDA backend
      ready)` at AE launch, then `PRERENDER what_gpu=0, gpu_data=null` on
      EVERY frame, and `PF_Cmd_SMART_RENDER_GPU` NEVER called. Effect renders
      correctly on CPU throughout. DEVICE_SETUP firing proves AE parses our
      PiPL GPU flags and registers the effect as GPU-capable.
    - **Ruled out empirically (do NOT re-test):** `SUPPORTS_DIRECTX_RENDERING`
      present vs absent (currently ABSENT, flags2 `0x0A001400`); other effects
      on the layer (tested with EdgeRepair as the only effect); project bit
      depth (8/16/32 — 32 confirmed in-log); preview resolution (Full,
      `ds=1/1` in-log); Fast Previews Off / Final Quality; Render Queue final
      render; raising `PF_RenderOutputFlag_GPU_RENDER_POSSIBLE`
      unconditionally (currently in the code, `raised=YES` confirmed in-log);
      handling `PF_Cmd_SMART_RENDER_GPU` in EffectMain (was genuinely missing
      — the enum name inverts the word order, `SMART_RENDER_GPU`, which the
      original SDK survey grep missed — but adding it changed nothing);
      **checkout-request inflation** (`ER_GPU_DIAG_NOINFLATE=1` forced
      inflate=0 — `what_gpu` stayed NONE, hypothesis dead; toggle restored
      to 0).
    - **KEY GROUND TRUTH (2026-07-24): AE's GPU effect pipeline WORKS on this
      machine for NATIVE effects.** Animated Fractal Noise (GPU-enabled) RAM-
      previews significantly faster with Mercury CUDA than Software Only. So
      the refusal is SPECIFIC to our (third-party) effect — the investigation
      target is what distinguishes native GPU eligibility from ours in AE
      2026, NOT machine/driver/project config.
    - **Third-party GPU effects DO work in AE 2026 generally:** an Adobe bug
      report (March 2026) shows BorisFX Sapphire WarpBubble2 GPU-RENDERING in
      AE 2026 on NVIDIA (crashing mid-GPU-render on driver 579.79, fixed by
      595.71) — so the official third-party GPU path functions in this AE
      version. Test box driver is 576.28 (functional: native CUDA accelerates,
      our PTX loads; updating to latest Studio driver is cheap hygiene).
    - **SPECIMEN DIFF DONE (2026-07-24) — PRIME SUSPECT FOUND:
      `PF_OutFlag_USE_OUTPUT_EXTENT`.** A PiPL parser (PE-resource walk +
      property decode, kept as `tools/pipl_dump.py` + `tools/pipl_sweep.py`;
      validated byte-exact against our own .aex first) swept EVERY .aex under
      both plugin roots on the test box: 218 PiPLs, 66 declaring
      SUPPORTS_GPU_RENDER_F32 (1<<25).
      - Deep Glow and RSMB — the originally planned specimens — do NOT set
        GPU_RENDER_F32: both drive the GPU out-of-band (RSMB links OpenCL.dll
        directly; Deep Glow via helper). Their diffs prove nothing; discarded.
      - REAL official-path specimens found installed instead: Red Giant
        Optical Glow / Chromatic Displacement (eGLO 0x02008021, eGL2
        0x0A101409, spec 13.25), RG Spot Clone (0x06008023/0x0B121489), the
        whole Artlist family (typ. 0x06000430/0x0A901401, spec 13.28),
        Jawset Postshot (0x00008410/0x02A00402, spec 13.28).
      - **Per-bit named diff vs ours (0x02000440/0x0A001400): the ONLY flag
        we set that NO working F32 declarer sets is USE_OUTPUT_EXTENT (eGLO
        bit 6). There is NO flag that every working declarer sets and we
        lack.** (Union across all 63 third-party F32 declarers — zero have
        bit 6. The only .aex on the machine combining GPU_RENDER_F32 with
        USE_OUTPUT_EXTENT are our own EdgeRepair, MM_FreqSep, MM_Qualify —
        worth asking whether those two also fail to GPU-render.)
      - Spec-version gating is DEAD: working declarers ship 13.25 and 13.28,
        so 13.29 is not required and older isn't excluded.
      - Framework confirmation: Optical Glow's binary imports nvcuda.dll and
        uses cuModuleLoad/cuLaunchKernel — the SAME CUDA driver-API pattern
        as our ER_CudaShim. Our framework choice is right.
      - **EXPERIMENT 1 — USE_OUTPUT_EXTENT removal: TESTED, NEGATIVE
        (2026-07-24).** Removed from `src/EdgeRepair.cpp` GlobalSetup AND
        `resources/EdgeRepairPiPL.r` (eGLO 0x02000440 → 0x02000400), deployed
        PiPL verified by re-parse, user relaunched AE: log still shows
        `DEVICE_SETUP OK` then `PRERENDER what_gpu=0` every frame.
        USE_OUTPUT_EXTENT is ruled out as the (sole) gate. The removal is
        KEPT (behavior-neutral under SmartFX — extent hints are a
        classic-render mechanism — and it aligns us with every working GPU
        declarer; do not reintroduce). Failed-run log preserved as
        `%TEMP%\EdgeRepair_gpu.extent-fail.<stamp>.log`.
      - Full property-level dump of working specimens (Optical Glow, Artlist
        ZoomBlur, Postshot): `aeFL` (AE_Reserved_Info) = 0 for all, same as
        ours; property sets structurally identical to ours (kind/name/catg/
        entry-point/ePVR 2.0/eSVR/eVER/eINF/eGLO/eGL2/eMNA/aeFL). After the
        extent removal our PiPL is a strict clean subset of the working
        declarers' — **the PiPL angle is EXHAUSTED except OutFlags_2 bit 0.**
      - **EXPERIMENT 2 — SUPPORTS_QUERY_DYNAMIC_FLAGS: DEPLOYED (fixed
        handler), AWAITING CLEAN AE TEST.** The last declaration-level
        difference (RG + all Artlist set it; Postshot doesn't, so it only
        gates us if Postshot is also secretly non-GPU). Added bit 0 (eGL2
        0x0A001400 → 0x0A001401 in .r + GlobalSetup), plus a
        `PF_Cmd_QUERY_DYNAMIC_FLAGS` case in EffectMain and `ER_GPULogEvent`
        in EdgeRepair_GPU.cpp. **First attempt answered the query by calling
        GlobalSetup (re-asserting ALL static flags) — AE raised error
        25::228 "effect cannot change non-dynamic flag bits during
        PF_Cmd_QUERY_DYNAMIC_FLAGS" on every apply, unsuppressable. LESSON:
        during QDF, only the dynamic-queryable subset may be written
        (OutFlags: WIDE_TIME_INPUT, NON_PARAM_VARY, PIX_INDEPENDENT,
        I_USE_SHUTTER_ANGLE; OutFlags2: I_USE_3D_CAMERA/LIGHTS,
        DOESNT_NEED_EMPTY_PIXELS, REVEALS_ZERO_ALPHA,
        DEPENDS_ON_UNREFERENCED_MASKS, OUTPUT_IS_WATERMARKED) — OR them into
        out_data->out_flags/2; never assign the full static set. Fixed
        handler ORs in PIX_INDEPENDENT only.** The errored run still proved
        two things (log preserved as `%TEMP%\EdgeRepair_gpu.qdf-err228.*`):
        (a) AE sends QUERY_DYNAMIC_FLAGS 8+ times immediately at
        effect-apply once the bit is declared; (b) that run's
        `what_gpu=0` is INCONCLUSIVE for the experiment since the QDF
        error may itself have disqualified GPU — retest with the fixed
        build before judging.
    - **EXPERIMENT 2 RESULT (clean retest after handler fix): NEGATIVE —
      what_gpu still 0. QDF ruled out. The declaration angle is DEAD.**
    - **ROOT CAUSE FOUND (2026-07-29, high confidence) — AE 2026 routes
      effect GPU rendering through the MediaCore "GPUVideoFilter"
      mechanism (`xGPUFilterEntry` export), NOT the AE-SDK
      SUPPORTS_GPU_RENDER_F32 path we implemented.** Evidence chain, all
      verified on this box:
      - AE's plugin registry is literally the Windows registry:
        `HKCU\Software\Adobe\After Effects\26.2\PluginCache\en_US\` (one key
        per .aex + `<file>_<loader-uuid>`, values Ignore/DoNotCache/date;
        the loader's "Loading from the registry" lines in
        `%APPDATA%\Adobe\After Effects\26.2\Plugin Loading.log` refer to
        this). 445 entries.
      - **82 entries carry a `GPUVideoFilter.0` subkey** (fields:
        GPUVideoFilter.MatchName = `AE.<AE-effect-match-name>`,
        InterfaceVersion = 2, Plugin Factory Guid, cached Pipl blob). These
        82 = every Adobe native GPU effect (**FractalNoise.aex — the effect
        that provably accelerates here — plus Transform, DirectionalBlur,
        Lumetri, Levels2, Bilateral, all VR effects**) + every GPU
        third-party (Red Giant OpticalGlow/ChromaDisp/SpotClone, all
        Artlist, CLEngine).
      - **Export-table check: FractalNoise, Transform, DirectionalBlur,
        Lumetri ALL export `xGPUFilterEntry`** — the MediaCore/Premiere GPU
        filter module entry point — alongside their AE entry points. So do
        OpticalGlow + Artlist. Adobe's own AE effects are GPU-rendered as
        GPUVideoFilter modules bound to the AE effect by match name
        ("AE." prefix).
      - The only SUPPORTS_GPU_RENDER_F32-only plugins on the machine (ours
        and Jawset Postshot) have NO GPUVideoFilter record — and neither
        shows any evidence of GPU rendering. Every fact of our failure
        signature is explained: AE probes GPU_DEVICE_SETUP from the PiPL
        flag (AE-SDK side) but its render scheduler only offers GPU
        (what_gpu != NONE / lightning-bolt badge) to effects with a
        GPUVideoFilter registration.
      - Sapphire GPU-rendering in AE 2026 (Adobe tracker) is consistent:
        BorisFX ships Premiere GPU support = xGPUFilterEntry.
      - CAVEAT (honesty): Adobe's AE SDK still documents the F32 path, and
        one forum thread (dkt0, "GPU API") describes SmartRenderGPU firing
        on an unknown AE version — so the F32 path may work on some
        AE builds or with some undocumented extra. On THIS box, nothing
        renders GPU without a GPUVideoFilter record. If a cheap
        counter-test is ever wanted: build Adobe's own SDK_Invert_ProcAmp
        (needs full AE SDK zip; sdk/ here is Headers+Util only) and see if
        IT gets what_gpu — if yes, the F32 path works and the diff hunt
        reopens; if no, the GPUVideoFilter conclusion is airtight.
    - **NEW STAGE 6 GPU ROUTE — `xGPUFilterEntry` module: SKELETON LANDED
      2026-07-29 (v1.0.0.26, deployed, AWAITING FIRST AE TEST).**
      `src/gpu/ER_GPUFilter.cpp` is a MediaCore GPUVideoFilter module
      (PrGPUFilterBase subclass + `DECLARE_GPUFILTER_ENTRY`), CUDA-only,
      passthrough render (raw `cuMemcpyDtoDAsync` input→output; in-place =
      no-op — SDK_ProcAmp's assumption; added `cuMemcpyDtoDAsync_v2` to
      `ER_CudaShim.h`). Binding: `outMatchName` left null → "default to the
      module's pipl" (PrSDKGPUFilter.h) — the host binds to "MM Edge Repair"
      and should record `AE.MM Edge Repair`, InterfaceVersion 2. Initialize
      declines non-CUDA frameworks / missing nvcuda → software render =
      our CPU SmartRender. Deployed .aex export-verified: `EffectMain,
      PluginDataEntryFunction2, xGPUFilterEntry` — the exact trio the
      working GPU effects ship.
      - Premiere Pro 26.0 C++ SDK facts (source of the API): user's copy at
        `E:\OFX DEV\AE SDK\Premiere Pro 26.0 C++ SDK Windows\`; needed
        headers copied to `sdk/Premiere/Headers` (all of Examples/Headers) +
        `sdk/Premiere/Utils/PrGPUFilterModule.h` (gitignored via sdk/;
        non-redistributable; CMake FATAL_ERRORs with instructions if
        missing). Only ER_GPUFilter.cpp sees these headers
        (per-source INCLUDE_DIRECTORIES + PRWIN_ENV define in CMakeLists).
        GOTCHA: PrGPUFilterModule.h uses PF_TransitionSuite without
        including it — include `PrSDKAESupport.h` first. Render contract:
        inFrames[0] = current-time input, render into outFrame or in place;
        pixel formats PrPixelFormat_GPU_BGRA_4444_32f / _16f (BGRA! —
        kernel port must handle order + optional half float); params via
        `GetParam(index, inClipTime)` (index-1 shift: "GPU filters do not
        include the input frame"); frame deps via GetFrameDependencies
        (base returns NotImplemented = current frame only). Adobe's
        Vignette example is the AE-style hybrid reference (its comment:
        works "in After Effects and Premiere Pro, including CUDA/OpenCL").
      - **INSTALL LOCATION IS PART OF THE MECHANISM (learned 2026-07-29,
        first module test).** Test 1 with the .aex in AE's own
        `Support Files\Plug-ins\` FAILED: no GPUVideoFilter registry record,
        no GPUVF log events, identical render on CUDA and Software Only —
        i.e. AE loaded the effect but never looked for its GPU module.
        AE's `Plugin Loading.log` shows why: our .aex was "recognized by
        loader 7a7d3cd3-…" (the plain AE-effect loader) while every GPU
        third party in `Common\Plug-ins\7.0\MediaCore\` loads through the
        MediaCore path and gets cached WITH a `GPUVideoFilter.0` subkey.
        **EdgeRepair now installs to
        `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\` with
        `CUDA_Assets\` + `DirectX_Assets\` NEXT TO IT** (CMake:
        `MEDIACORE_PLUGINS_DIR` + per-target `MEB_INSTALL_DIR_<TARGET>`;
        ModernEdgeBlur still installs to the AE folder, untouched). The
        Premiere SDK readme states the same rule for its own kernel assets.
        Keep exactly ONE copy of EdgeRepair.aex installed — a stale copy in
        the AE folder would double-register the match name.
      - **TEST 2 (from MediaCore) — REGISTRATION SUCCEEDED, RENDER DID NOT.**
        AE created the record:
        `EdgeRepair.aex_74697f7d-…\GPUVideoFilter.0` with
        `MatchName = AE.MM Edge Repair`, `InterfaceVersion = 2`, HasPipl=1 —
        structurally identical to ZoomBlur's. **So the install-location fix
        WORKED and the module is registered.** But no `EVENT GPUVF …` lines
        appeared and CUDA/Software rendered identically: AE registered the
        module and never called CreateInstance/Render. (Note the loader UUID
        changed to 74697f7d-… — the MediaCore loader, same one Deep Glow
        uses — confirming the different load path.)
      - **MISSING DECLARATION FOUND: "PF Pixel Format Suite".** A binary
        string scan of suite names across the specimens: Optical Glow
        acquires PF Pixel Format Suite + GPU Device + Video Segment + GPU
        Image Processing + Memory Manager + PPix; EdgeRepair had every one
        EXCEPT PF Pixel Format Suite. Per PrSDKAESupport.h, an effect calls
        `ClearSupportedPixelFormats` / `AddSupportedPixelFormat` **during
        PF_Cmd_GLOBAL_SETUP** to declare what it accepts — without a GPU
        format declared, the host has nothing to hand the GPU module.
        FIXED: `ER_DeclareGPUPixelFormats()` (EdgeRepair_GPU.cpp, called
        from GlobalSetup) declares `PrPixelFormat_GPU_BGRA_4444_32f` then
        `_16f`. Our binary's suite profile now matches Optical Glow's
        exactly. NOTE ZoomBlur does NOT acquire this suite, so it is one
        working recipe, not provably the only one.
        GOTCHA: those GPU labels are defined in the PREMIERE headers, which
        must not enter the AE-SDK TU — but they are built from
        `MAKE_ADOBE_PRIVATE_PIXEL_FORMAT_FOURCC`, which our AE SDK's own
        PrSDKPixelFormat.h provides. Use Adobe's macro; never hand-roll the
        FourCC (the private prefix is `'@'`, byte order is REVERSED, and
        16f is lowercase `'a'` vs 32f's `'A'` — all three are easy to get
        wrong, and a wrong value fails silently as "host never offers that
        format").
      - **TEST PROTOCOL (next AE launch, now from MediaCore):** (1) apply
        Edge Repair; (2) does
        `HKCU\...\26.2\PluginCache\en_US\EdgeRepair.aex_*\GPUVideoFilter.0`
        appear (check from a shell — decisive); (3) lightning-bolt badge?;
        (4) `%TEMP%\EdgeRepair_gpu.log` for `EVENT GPUVF Initialize: OK
        (CUDA)` + `EVENT GPUVF Render (passthrough milestone)`; (5)
        EXPECTED VISUAL with Mercury CUDA: the effect APPEARS TO DO NOTHING
        (passthrough) — that IS the success signal; Software Only restores
        the real CPU render. **"Works the same in CUDA and Software Only"
        means the GPU module was NOT engaged** (that was test 1's result).
        Until the kernel port lands, GPU and CPU results intentionally
        differ — keep Mercury on Software for real work.
      - After the gate opens: port the four kernel groups into the module
        (JFA+2 distance, pull–push, composite; BGRA + 16f aware), wire
        params via GetParam, then bisect/clean the AE-SDK-side experiment
        scaffolding per the standing rule.
    - Experiment-state note: extent removal (eGLO 0x02000400), the QDF bit +
      fixed handler (eGL2 0x0A001401), ER_GPU_LOG, and unconditional
      GPU_RENDER_POSSIBLE all stay in place — harmless, match working
      specimens, and the standing bisect-after-success rule still applies
      if the F32 path ever fires.
    - Diagnostic scaffolding still in place on purpose until the gate passes:
      `ER_GPU_LOG=1`, unconditional GPU_RENDER_POSSIBLE (marked EXPERIMENT in
      `PreRender`), `ER_GPU_DIAG_NOINFLATE` define (now 0). Once GPU renders,
      bisect the unconditional flag back to the gated form.
  - **DONE 2026-07-30 (v1.0.0.36), GATED 2026-08-02 — see "Kernel port"
    above:** the four kernel groups are in `ER_KernelMath.h` + the CUDA wrapper
    (threshold/erode core matte; JFA+2 distance; pull–push mip chain; composite
    = grain + inner-ease blend + choke, each mirroring its `MM_*.h` twin), and
    the headless CPU-vs-GPU parity harness is `tests/test_gpuparity.cpp`.

  ### Remaining Stage 6 cleanup — OPTIONAL, none of it blocking

  Stage 6 is complete and gated; these are tidy-ups, not open defects.
  1. **Bisect the unconditional `GPU_RENDER_POSSIBLE`** in `PreRender` (marked
     EXPERIMENT there) back to the gated form `what_gpu != NONE && gpu_data`.
     It was kept through the kernel gate on purpose — one variable per AE test.
     It costs an AE round-trip to verify, so it waits for a session with time
     for one. If GPU rendering ever stops engaging, this is the first suspect.
  2. **DX12 host orchestration + Metal entries** — see "NOT ported" above.
     Speculative until a host exists that can gate them.
  3. **A long GPU soak** (the CPU path has `test_soak.cpp`; the GPU path has no
     equivalent). The device scratch is grow-only and freed at setdown, so the
     risk is low, but it is unmeasured.

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
  (`ER_DistanceField.h`, `ER_CoreMatte.h`, and future pull–push /
  blend-weight modules) — the brief requires the pure algorithms be
  unit-tested headlessly.
