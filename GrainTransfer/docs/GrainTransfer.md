# Grain Transfer — Architecture & Status

`GrainTransfer.aex`, match name **MM Grain Transfer**, category **MM**.
A fast grain-transfer primitive: take the grain character of a source plate,
lay ghost-free, seam-free grain onto the target layer. One job — the sharp
knife, not the multitool.

## Cost model (the whole point)

| Phase | When | Cost |
|---|---|---|
| Analyze | once per (source state, Seed, apply-space) | residual high-pass + patch power spectrum + 5-band response curve |
| Precompute | same trigger | bank of 8 phase-randomized seamless tiles (FFT, ≤256²) |
| Per frame | every frame | stamp: tile lookup + σ(L) modulate + add. ~304 ms at 6K single-thread, threaded across AE workers in practice |

The analysis product lives in AE's **Compute Cache** (`AEGP_ComputeCacheSuite1`),
keyed on the Grain Source layer param's `PF_GetCurrentState` hash + Seed +
resolved apply-space. A cache **hit skips the source frame entirely** — no
checkout, no render, no unpack. Only the first frame (or a genuinely changed
source) pays for analysis. If the suite is unavailable the plugin computes
inline per frame (correct, slower — never wrong).

**Use a still or locked-off plate as Grain Source.** The source is sampled at
the effect's current time; a moving source re-analyzes on first visit to each
frame (cached thereafter per frame).

## Method — phase-randomized noise (RPN)

`grain = plate − blur(plate)` does not isolate grain — the residual carries
content edges too. Re-tiling it ghosts content (validated: NCC 0.53 against
content structure in the gate tests). Instead:

- Measure the **magnitude spectrum** of the residual (Hann-windowed patches;
  patches whose residual σ is ≫ median are content-contaminated, ≪ median are
  grainless/clipped — both rejected).
- Give it the **phase of a fresh white-noise field**. A real field's spectrum
  is Hermitian by construction, so the phase-randomized spectrum inverts to an
  exactly real tile — no conjugate-symmetry bookkeeping.
- Result: exact measured grain character, zero content structure (gate: NCC
  0.047 vs the same content), inherently periodic (gate: wrap/interior
  gradient ratio 0.98 — seam-free tiling by construction).

Per frame, the stamp re-randomizes tile choice, offset, and flips from
(Seed, frame) — grain never freezes, at near-zero cost. Channels use
independent phases (film grain decorrelates across emulsion layers); the
per-channel measured spectra and sigmas come free from the same analysis.

**Known, quantified limitation:** frequency separation transfers the residual
band — ~72% of raw grain energy for fine grain (σ≈0.7 px correlation) with the
σ=2 px high-pass. The lowest-frequency component of grain is indistinguishable
from content and is deliberately excluded; DasGrain shares this property.
RPN output is Gaussian; source-vs-tile skew/kurtosis matched within noise in
gates, so the histogram-matching fallback from the brief was **not** needed.

## Colour management — apply space

Grain is ~constant in *density* (log); grain math must run in a log-like
space. Both extraction and application happen in the **apply space**:

- **Display** — incoming pixels are display-gamma (Rec.709/sRGB working
  space, or CM bypassed). Gamma is already perceptual-ish → identity.
- **LogFromLinear** — incoming pixels are scene-linear (ACEScg etc.) →
  ACEScct lin↔log both ways around the add. The ACEScct toe is linear, so
  negatives (wide gamut) and HDR (>1) survive exactly (gate: NaN-free over
  linear −0.01…10, grain σ in log space exact).

**Auto resolution order** (`QueryAutoEncode`):
1. `AEGP_IsOCIOColorManagementUsed` → if OCIO, `AEGPD_GetOCIOWorkingColorSpace`
   and parse the name: log-like (`cct`/`log`/`acescc`) → Display (already
   perceptual); else linear-like (`lin`/`aces`/`cg`/`scene`) → LogFromLinear.
2. Else classic ICC: working-space profile via
   `AEGP_GetNewWorkingSpaceColorProfile` (comp from
   `PFInterfaceSuite → LayerSuite`), `AEGP_GetColorProfileApproximateGamma`
   ≈ 1.0 → LogFromLinear, else Display.
3. **Any failure → Display** — which is also the correct behaviour for the
   CM-off / Preserve-RGB workflow. That workflow is exactly why the manual
   Working Space override exists.

The resolved mode is part of the analysis cache key AND mixed into the render
GUID, so flipping project colour settings invalidates both caches correctly.

### SDK spike findings (brief flagged two behaviours to verify)

Implemented against SDK 25.6 headers; confirmed present:
`AEGP_ColorSettingsSuite6` (version const 7, frozen AE 25.1) exposes the OCIO
queries and `AEGP_GetColorProfileApproximateGamma`. Effects obtain the needed
`AEGP_PluginID` via `AEGP_RegisterWithAEGP` at GLOBAL_SETUP (non-fatal on
failure — Auto degrades to Display).

**Still to verify against a running AE** (the two flagged behaviours):
1. The exact string `AEGPD_GetOCIOWorkingColorSpace` returns per config
   (assumed UTF-8 char; if it were UTF-16 the name heuristics fail closed →
   Display) and whether common config names hit the lin/log heuristics.
2. What the ICC path returns when colour management is **off** — expected:
   error or an sRGB-like profile, both of which resolve to Display (correct).
Verification steps are in the checklist below; the manual override covers any
version where the query misbehaves.

## Response curve vs log space (the double-counting question)

Resolved by construction: the log encode carries the *physically correct*
exposure baseline; the measured 5-band σ(L) curve captures only the source's
*deviation* from that baseline (because it is measured on apply-space-encoded
pixels, where a perfectly density-constant stock measures flat). The Response
param is a **trim**: 100% = full measured curve, 0% = flat (overall σ
everywhere), linear blend between. No double count at any setting.

## Parameters (locked, disk IDs forever)

| Param | ID | Range/Default | Notes |
|---|---|---|---|
| Grain Source | 101 | layer, none | the plate to match; still/locked plate intended |
| Amount | 102 | 0–200%, 100 | grain gain |
| Size | 103 | 25–400%, 100 | tile rescale (resolution/scale mismatch); ≠100% uses bilinear-wrap |
| Response | 104 | 0–100%, 100 | luminance-modulation trim (see above) |
| Seed | 105 | 0–9999, 0 | phase seeds + per-frame randomization stream |
| Working Space | 106 | Auto / Display-Bypassed / Scene-Linear / ACES | Auto queries the project; manual = bypass/override. Scene-Linear and ACES share the log path in v1 |
| Preview Grain Only | 107 | off | mid-grey + the exact modulated field being added, opaque |

Behavioural notes:
- No source connected → **byte-identical passthrough** (memcpy).
- Grain is added to unpremultiplied RGB and re-premultiplied; alpha is never
  modified; alpha=0 pixels pass through. Source alpha is ignored (plates are
  opaque; zero-alpha source regions are auto-rejected as grainless patches).
- Small grain samples: tile size falls back 256 → 128 → 64; below 64² the
  source is treated as absent (passthrough).

## Files

```
src/core/       AE-free DSP (gate-tested headlessly, no SDK dependency)
  GT_FFT        2D FFT wrapper over vendored kissfft (BSD-3-Clause)
  GT_Grain      residual, patch spectrum measurement, RPN tile bank
  GT_Response   5-band MAD-robust σ(L) measure + eval
  GT_Colour     ACEScct lin↔log, apply-space luma
  GT_Stamp      per-frame stamp (row-based; AE threads fan out over rows)
  GT_Random     SplitMix64 — deterministic, cross-platform
src/common/     GT_Common — row pack/unpack + iterate_generic wrapper
                (MM_Common semantics re-implemented; suite repo untouched)
src/            GrainTransfer.{h,cpp}, strings — AE plumbing only
resources/      PiPL (version + outflags in lockstep with the header)
third_party/    kissfft subset + licenses (committed)
tests/          headless gates + real-EXR smoke + ae_smoke.jsx; test-only
                third_party (stb_image_write, tinyexr, miniz) — not linked
                into the plugin
```

MFR safety: no mutable globals; global data = AEGP plugin id + registration
flags written once; per-render state in `pre_render_data` and on the
SmartRender stack; cache values are AE-owned and read-only after compute.

## Stage / gate status

| Stage | Gate | Status |
|---|---|---|
| 0 FFT + skeleton | round-trip < tol; builds+deploys | **PASS** (5e-7; v1.0.0.3 deployed) |
| 1 RPN [VALIDATE EARLY] | character match, no ghost, no seams | **PASS** (0.094 / NCC 0.047 vs 0.53 control / ratio 0.98) |
| 2 Response [VALIDATE EARLY] | σ(L) recovered; grain sits in gradient | **PASS** (bands ≤1.8%; strips ≤3.4%) |
| 3 AE integration | loads; matches; varies; preview clean; 3 colour contexts | **BUILT — needs in-AE verification** (checklist below) |
| 4 Bit depths + caching | all depths; no re-analysis on frame change | **BUILT** — depths via pack/unpack (headless-verified logic); cache fast-path implemented; verify in AE |
| 5 CPU perf | ≪ Match Grain; 10k-frame soak | Stamp threaded, allocation-free after warm-up; 6K single-thread 304 ms headless. **Benchmark + soak need AE** |
| 6 GPU | parity | **Not started — gated on 0–5 sign-off per brief** |

## In-AE validation checklist (manual, ~15 min)

1. **Load**: AE 2026 → effect appears under MM → apply to any layer: no
   error, passthrough identical (toggle effect on/off, difference mode = 0;
   check at 8, 16, 32 bpc).
2. **Smoke script**: run `tests/ae_smoke.jsx` — three PNGs: grain on the
   gradient, matching the plate, differing per frame, no seams.
3. **Real plates**: 6K plate as Grain Source, CG/flat target. Grain character
   matches at 100/100/100/0 defaults; Preview Grain Only shows clean
   decorrelated grain (no plate content visible).
4. **Colour contexts** (the Stage 3 gate): the same setup in (a) Rec.709 G2.4
   project, (b) ACEScg/linearized project — grain must sit correctly in
   highlights >1.0, no blow-out — and (c) CM off → set Working Space manually
   and confirm Auto behaved as Display. Report what Auto resolved in each
   (add a temporary diagnostic if needed).
5. **Cache**: scrub the target — analysis must not re-run (first frame after
   applying is the only slow one; watch for a one-time hitch only). Change
   Seed or the source layer → one re-analysis. RAM-preview a few hundred
   frames → stable memory.
6. **Benchmark**: same shot with AE Match Grain vs Grain Transfer; record
   per-frame ratio (expect an order of magnitude or more once cached).

## Non-goals (unchanged from the brief)

No degrain, no procedural/no-source synthesis (use AE Match Grain or add a
plate), no blend/mask/grade controls, no multi-stock reconciliation, no
per-shot external maps, no extra colour controls beyond the single gate, no
GPU until CPU is signed off, no parameters beyond the locked seven.
