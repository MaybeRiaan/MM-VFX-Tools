# HSL Qualifier — Development Plan (v2, descoped)

**Working title:** MM Qualify (AE, C++ SmartFX, GPU-first)
**One-liner:** The DaVinci Resolve HSL qualifier, faithfully rebuilt inside After Effects.
**Scope philosophy:** Resolve parity, nothing more. If Resolve's qualifier panel doesn't have it, we don't build it (v1).

---

## 1. Qualifier

Three independent 1D gates, ANDed — any combination enableable. **Uniform control set per gate: In, Out, In Soft, Out Soft** (absolute bounds + per-side softness — Lumetri-style, muscle-memory friendly):

- **Hue** — In/Out in degrees with **wraparound allowed** (In > Out ⇒ range crosses 0°, so reds behave naturally); circular seam-free distance math
- **Saturation** — In/Out bounds 0–1+, per-side softness
- **Luma** — same, with selectable luma weights (Rec.709 / Rec.2020 / working primaries); float-safe, HDR-aware (bounds settable >1.0)

Per-gate: enable toggle. Global: handled by output selector (see §3).

**All gate parameters are standard AE float params → natively keyframeable and expression-drivable.** This is a headline differentiator over Resolve: animated qualifier ranges (sunset skies, exposure rides, expression-linked gates) at zero extra cost.

**Sampling:** eyedropper reads raw working buffer (SkinRemap pattern), sets In/Out + softness from sample. Drag-region sampling from mean ± n·stddev. Add-to / subtract-from selection modes (Resolve's +/− pickers).

**Color model:** HSV/HSL selectable; all math float.

**Qualify In popup — `As-Is / Gamma 2.4 / Log / ACEScct`:** transforms the *qualification path only* (image never touched — same principle as Denoise pre-filter). Default As-Is (respect input, WYSIWYG). ACEScct option gives ACEScg/linear-working-space users literally Resolve's qualification behavior (Resolve's ACES timeline is ACEScct): log-spread luma, sane saturation distributions, uniform gate softness, no primaries conversion. Transform is fused inline in the kernel — zero perf cost. Reuses SkinRemap's ACEScct code.

## 2. Matte Finesse (Resolve's exact control set)

1. **Denoise (pre-filter)** — blur/median on the *qualification input only*; image untouched. Kills sizzle at the source.
2. **Clean Black** — pushes near-black matte noise to solid black (soft threshold from below).
3. **Clean White** — pushes near-white core to solid white.
4. **Black Clip / White Clip** — hard levels endpoints with soft knee.
5. **Blur Radius** — matte blur (post morphology).
6. **In/Out Ratio** — biases the blurred edge inward or outward = shrink/grow. Classic implementation: blur then re-map midpoint.

Fixed chain order matching Resolve behavior so muscle memory transfers.

**Temporal Stabilize (differentiator, optional toggle):** MM IIR asymmetric attack/release module (shared with BlemishRepair) applied to the matte, last in chain. Resolve has nothing equivalent; this + keyframeable gates are the two "why not just use Resolve" answers.

## 3. Panel layout & UI strategy

**Effect Controls layout (top to bottom):**
1. **Qualifier** group — H / S / L subgroups, each: Enable · In · Out · In Soft · Out Soft
2. **Output** popup — `Matte` / `Matte Inverted` / `Original + Alpha`
3. **Matte Cleanup** group — Denoise · Clean Black · Clean White · Black Clip · White Clip · Blur Radius · In/Out Ratio · Temporal (toggle + attack/release)

**UI implementation strategy:**
- **v1: standard AE parameter controls only** (sliders, popups, groups — Lumetri / Hue-Sat style). Fast to ship, fully keyframeable, MFR-safe, zero custom event handling.
- **Later polish: custom ECW widget** (Drawbot-drawn hue spectrum strip + trapezoid handles, Hue/Sat-style). Handles are backed by the *same* standard float params, so keyframing keeps working and no rework is required — purely additive.

## 4. Output & UX

- **Output modes** (the popup above): Matte (luma) / Matte Inverted / Original + Alpha (keyed image, alpha replaced) — covers track-matte and direct-key workflows, no precomp dance.
- **Highlight preview** (Resolve-style flat color / checkerboard behind selection) and **Qualify Raw** (pre-cleanup) as additional view options for tuning.
- **Garbage mask input** (layer param) intersected before cleanup. (Resolve has windows; this is the AE equivalent, cheap to include.)

## 5. Phases

**Phase 0 — Scaffold:** SmartFX GPU + CPU fallback, MM color infra, straight/premult alpha audit, seam-free hue wraparound math + unit tests.

**Phase 1 — Gates + sampling:** three gates (In/Out/In Soft/Out Soft), eyedropper + region + add/subtract sampling, output popup, Highlight view.
*Milestone: pulls keys as confidently as Resolve's qualifier on the same footage — "works" parity, not pixel matching.*

**Phase 2 — Matte cleanup:** full control chain incl. temporal toggle, Qualify Raw view, garbage mask.
*Milestone: a moving-footage key (skin, sky, product color) that holds without sizzle.*

**Phase 3 — Polish & ship:** 4K float perf pass on mid GPUs, MFR audit, presets (Skin preset shared with BlemishRepair), custom ECW spectrum widget if time allows, docs + demo content leading with keyframeable gates + temporal stability.

## 6. Risks
| Risk | Mitigation |
|---|---|
| Hue wraparound bugs | Circular math from day one; unit tests across 0° seam incl. In>Out ranges |
| Matte sizzle on motion | Denoise pre-filter (cause) + Clean B/W + temporal module (symptom) |
| Straight vs premult alpha | Phase 0 audit; interpretation toggle |
| HDR/log values >1 | Unbounded L bounds, graceful behavior |
| Inconsistent input color spaces (vs Resolve's fixed timeline space) | Document recommended usage; optional working-space transform via MM color infra |

## 7. Deferred (explicitly out, revisit only on user demand)
- Standalone alpha-refinement mode / combine ops
- SDF engine, corner rounding, hole patching, despeckle
- 3D chroma qualifier, spill suppression

## Shared-engine ledger
- Qualifier core + Skin preset ↔ BlemishRepair constrain stage
- Raw-buffer eyedropper + color infra ↔ SkinRemap / MasterFilm
