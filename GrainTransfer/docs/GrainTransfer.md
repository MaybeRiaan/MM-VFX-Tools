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
(Seed, frame) — grain never freezes, at near-zero cost. Channels are tied
together by a shared grain FIELD whose per-channel weights come from the
source's measured pairwise correlations (`GrainSpectrum.crossPair`), so
luma-dominant film grain reproduces as unified rather than chromatic; the
per-channel measured spectra and sigmas come free from the same analysis. The
correlation is measured on its own block grid by a content-robust estimator and
is **signed**, because debayered digital plates are commonly anti-correlated —
see *Cross-channel colour-grain fidelity* below.

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
                (per-plugin, namespace gtc — every plugin has its own now)
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
| 1c Cross-channel colour | measured correlation reproduced, both signs | **PASS** (realized within 0.003 of target; content bias removed) |
| 2 Response [VALIDATE EARLY] | σ(L) recovered; grain sits in gradient | **PASS** (bands ≤1.8%; strips ≤3.4%) |
| 3 AE integration | loads; matches; varies; preview clean; 3 colour contexts | **BUILT — needs in-AE verification** (checklist below) |
| 4 Bit depths + caching | all depths; no re-analysis on frame change | **BUILT** — depths via pack/unpack (headless-verified logic); cache fast-path implemented; verify in AE |
| 5 CPU perf | ≪ Match Grain; 10k-frame soak | Stamp threaded, allocation-free after warm-up; 6K single-thread 304 ms headless. **Benchmark + soak need AE** |
| 6 GPU | parity | **BUILT** — CUDA host landed, gated on the analysis cache; headless kernel-vs-CPU parity 18/18 exact. **In-AE G-rows not yet run** (G-01 blocked on a missing Grain Source layer) |

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

## Cross-channel colour-grain fidelity

The goal is to reproduce the source grain's **measured** cross-channel
correlation faithfully — not to force it to 1.0 (that would strip the plate's
real chroma-grain component).

### Method 2, common-factor spatial model — **DONE** (v1.0.0.17)

`MeasureGrainSpectrumRes` now keeps the three pairwise correlations separately
on `GrainSpectrum.crossPair` (`crossCorr` remains their mean, for the log and
the fallback). `SolveCrossChannel` factors them into a rank-1 common-factor
model and `SynthesizeTileBank` synthesizes it:

    grain_c = a_c·L + N_c

with `L` one shared grain field summed into all three channels, `N_c`
independent per channel, and `a_c` **signed** (see *Correlation is SIGNED*
below). Because tiles are normalized to unit sigma per channel
(absolute amplitude is reapplied at stamp time), the quantity reproduced is the
**correlation matrix**, not the covariance: `corr(i,j) = a_i·a_j` exactly, with
no phase-blend nonlinearity to undershoot and full per-pair asymmetry.

The spectral split `p_c = a_c²·p_L + p_Nc` requires `p_Nc ≥ 0`, which is what
bounds the model: the shared field can only live where **all three** channels
have power. Putting `p_L` at the per-bin channel minimum makes that bound both
explicit and maximal — its total is the spectral overlap `M`, and the reported
`achieved` is the fraction of the measured correlation the spectra permit. A
coarse blue against a fine red caps `achieved` below 1 and no method can beat
that; two fields with different power spectra cannot be perfectly correlated.

The per-channel magnitude is deliberately **not** re-imposed after the sum —
that would restore the exact spectrum and destroy the correlation the sum just
bought. Each component carries an exactly imposed magnitude; the total is
correct in expectation, fluctuating per bin by at most `2·a_c·√(p_L·p_Nc)`,
bounded rather than Rayleigh since only the phases are random.

Gated by `[Stage 1c]` in `tests/test_core.cpp` on a plate built with
deliberately asymmetric pairs (a = .9/.8/.6 → R–G .72, G–B .48, R–B .54):

| | R–G | G–B | R–B | total error |
|---|---|---|---|---|
| measured target | 0.748 | 0.523 | 0.573 | — |
| **common-factor** | 0.747 | 0.522 | 0.572 | **0.003** |
| legacy scalar | 0.299 | 0.297 | 0.299 | 0.950 |

The legacy row shows both failures the model removes: the undershoot (0.299
realized against a 0.615 mean target) and the collapse of three distinct pairs
into one number.

**Method 1 (closed-loop ρ calibration) was dropped**, not implemented: it
corrects the undershoot that method 2 removes by construction, so it would have
been throwaway work. The legacy scalar phase-blend remains as the fallback for
measurements that cannot be factored.

### The measurement, rebuilt — **DONE** (v1.0.0.17)

Method 2's synthesis was exact from the start, but it **declined on both real
plates**, because the measurement feeding it was corrupted by content leaking
into the high-pass residual. The old estimator pooled cross-products over the
same large FFT patches used for the spectrum, which fails three ways: a 6K plate
yields only ~6 patches at n=256; pooling weights each patch by its variance,
handing the answer to the most contaminated ones; and selection ranked patches
by *combined* sigma, which never catches a mid-sigma patch carrying a strong
coloured edge.

Isolating the cause on identical grain that is independent **by construction**
(true correlation 0) showed content is the whole story — and that its sign
depends on the content's colour:

| Plate composition | old pooled estimator | `MeasureCrossPairs` |
|---|---|---|
| grain only, no content | +0.000 / −0.002 / +0.001 | +0.002 / −0.005 / −0.000 |
| grain + **grey** content | **+0.118 / +0.098 / +0.083** | +0.002 / −0.004 / +0.001 |
| grain + **coloured** content | **+0.042 / −0.025 / −0.076** | +0.003 / −0.006 / −0.003 |

Grey edges appear in all three channels and drive every pair positive; coloured
edges drive pairs in *opposite* directions. `MeasureCrossPairs` removes both,
and the `[Stage 1c]` gate confirms it does not remove genuine signal — a plate
correlated by construction still measures 0.714/0.474/0.532 against a true
0.720/0.480/0.540.

It measures on its **own grid of 32×32 blocks**, not on the FFT patches, because
the two measurements want opposite things: the spectrum wants large patches for
frequency resolution and tolerates content that phase randomization discards
anyway, while correlation needs content excluded and benefits from having
hundreds of samples rather than six. Three defences, then a median:

1. **Small blocks**, so any one edge contaminates few of them.
2. **Flattest-first selection** — blocks ranked by combined residual sigma, and
   only the 10th–60th percentile band kept (edges inflate sigma; the bottom of
   the range is clipped or grainless).
3. **Winsorized correlation** inside each block at 2.5σ, stripping the leverage
   of surviving edge pixels without discarding the grain around them.
4. Per-block correlations combined by **median, never pooling** — pooling
   weights by variance, which re-admits exactly the blocks 1–3 excluded.

The Alexa plate went from 6 patches to 564 blocks, the DSLR from 11 to 1345.

### Correlation is SIGNED, and real plates use it

With the measurement clean, the real plates read consistently — and negatively:

| Plate | R–G | G–B | R–B | samples |
|---|---|---|---|---|
| Alexa Mini BT709 | −0.047 | −0.046 | −0.031 | 564 blocks |
| DSLR linear | +0.145 | **−0.725** | −0.200 | 1345 blocks |

This is not noise and not contamination: it is what debayered digital plates
actually do. Chroma reconstruction drives green against red and blue, so the
grain is genuinely **anti-correlated**. Clamping that to [0,1], as the code
originally did, threw away real structure.

A rank-1 model represents it fine, because `a_c` is **signed**: one negative
weight flips exactly two of the three pairs, which is the (+,−,−) pattern the
DSLR measures. A real solution exists iff every pair is non-negligible and
`r_RG·r_GB·r_RB ≥ 0` — all three positive, or exactly two negative. One negative
pair, or three, has no real solution and falls back. Gated both ways by
`[Stage 1c2]`.

The DSLR plate now solves to `a = 0.146 / 0.532 / −0.732` and **uses the
common-factor path**. The Alexa plate is all-three-negative, which has no rank-1
solution — but at |r| ≤ 0.05 it is independent for practical purposes, and the
scalar fallback is the right answer.

### Known conservatism: `overlap` is biased low

`overlap` (M) is the sum over bins of the per-bin channel minimum, and the min
of three *noisy* periodogram estimates sits below their common true value. On
the `[Stage 1c]` plate, whose three channels have identical spectra by
construction, M measures **0.852** rather than 1.0 — about 15% low on 24
patches, and worse on fewer. Since `achieved = min(1, M / max a²)`, this
under-delivers correlation whenever `max a²` approaches M: the DSLR plate
reports `achieved 0.537`, so it reproduces ~54% of its measured correlation.

Some of that ceiling is real (the channels' grain spectra genuinely differ) and
some is estimator noise. Smoothing the power spectra before forming p_L would
separate the two, but it also changes the magnitudes imposed during synthesis
and so touches grain character — a decision of its own, not folded in here.


### Still open

**Method 4. Per-frequency cross-spectral (3×3 Cholesky)** *(theoretical
ceiling; likely overkill).* Measure the full 3×3 cross-spectral density `S(f)`
per bin (accumulate `F_i(f)·conj(F_j(f))` over patches). Synthesize per bin as
`F(f) = L(f)·z`, `z` a 3-vector of independent unit complex Gaussians, `L(f)`
the Cholesky factor of `S(f) = L Lᴴ`; preserve Hermitian symmetry
(`S(−f) = S(f)ᴴ`). Captures *frequency-dependent* correlation (luma-correlated
lows, chromatic highs) that method 2 cannot. Costs ~6 floats/bin (Hermitian
3×3) of storage and a per-bin 3×3 factorization at synth; needs regularization
when `S(f)` is near-singular.

**There is now real evidence for it.** Method 2 ties channels with ONE shared
field, so the correlation it can carry is bounded by how similarly the channels'
spectra are shaped — the DSLR plate reproduces only ~54% of its measured
correlation for that reason. Method 4 has no such bound: it correlates
per-frequency, so differently-shaped channels can still be fully correlated
wherever they overlap. Before reaching for it, settle the `overlap` bias above,
since part of that 54% is estimator noise rather than a real ceiling.

**Method 6. Artistic override** *(optional; off by default).* A "Grain Unify"
gain/bias on the measured correlation so an artist can push toward
monochromatic — or the other way — for a look. Departs from the "measure and
match" principle and adds an eighth control against the locked-seven design, so
keep it opt-in with the default = faithful reproduction. It would also need to
enter `GT_AnalysisKey` (it changes the analysis product), though **not** the GPU
`PARAM_BLOCK`, since it folds into the tile bank at analysis time. Deferred as
a product decision, not a fidelity one.

*Considered and excluded:* opponent-space (Y/Cb/Cr) synthesis — an alternative
framing of method 2 that additionally fixes the luma-vs-chroma energy split but
touches the response-curve path.

### GPU impact

The tile bank is the firewall. Everything above changes how the bank is
**filled**, at analysis time, on the CPU, behind the compute cache. The GPU
kernel reads it as an opaque `count × 3 × n × n` float buffer, so no kernel
argument, `PARAM_BLOCK` field or per-frame math moves, and the G-row parity
tests are indifferent. Two exceptions: **G-02** (Preview Grain Only) inspects
the grain field itself, so its reference imagery must be re-taken after any
change here; and method 6, if ever built, would add a parameter that reaches the
PiPL and the cache key.

`GT_ANALYSIS_FORMAT` in `GrainTransfer.cpp` versions the analysis product into
the cache key — bump it whenever the meaning of `GrainSpectrum` or the synthesis
changes, so stale banks cannot outlive their algorithm.

## Luminance response — the flat-plate fix (v1.0.0.18)

Found by the GrainPlates split-screen test (4 film stocks, a 7-step grey chart
at 3840×2160, Preview Grain Only). Six of the seven bands rendered **1.7–2.6×
too grainy**; only the one band whose luminance happened to land inside the
plate's own range was right.

**Cause: the response curve was measuring its own grain.** `MeasureResponse`
binned pixels by their raw luminance, then measured residual MAD per bin — so
the binning variable *contained* the quantity being measured. On a locked grain
plate (which is what the Grain Source is for) there is no content at all, so
the plate's entire luminance variation IS its grain: the top bin is by
definition the pixels where grain landed brightest. The four plates span only
4–8 residual sigmas of luminance end to end, and the fitted curve came out
U-shaped — inflated at both ends, collapsed in the middle:

| Stock | band 0 | 1 | 2 | 3 | 4 | (× plate's flat sigma) |
|---|---|---|---|---|---|---|
| 35mm Ultra Fine | 1.87 | 0.83 | 0.58 | 1.38 | 2.30 | |
| 8mm Course | 1.44 | 0.76 | 0.64 | 1.55 | 3.24 | |

`EvalSigma` clamps outside the measured range, so every target luminance
outside the plate's narrow band picked up an inflated end value.

**Fix, in two parts, both free at runtime:**

1. **Bin on the BLURRED luminance** (`rgb − residual`). Both planes are already
   in hand — the residual is defined as their difference — so removing grain
   from the binning variable costs nothing. It is also the more correct
   question: the curve describes how grain scales with *scene* luminance, not
   with the grain-perturbed pixel value.
2. **An identifiability guard.** If the plate's luminance span is under ~10
   residual sigmas there is no recoverable luminance dependence, and fitting
   five bands across it produces noise that end-clamping then extrapolates.
   The curve collapses to `flatSigma` instead. The estimator is MAD-robust, not
   RMS — an RMS over a whole plate measures content EDGES and would wrongly
   declare a properly graded plate unidentifiable.

All four plates now return a flat curve at **1.00×** their own measured sigma at
every luminance, matching the plate to within a few percent:

| Stock | plate residual sigma | curve flatSigma |
|---|---|---|
| 8mm Course | 0.00728 | 0.00731 |
| 16mm Course | 0.01868 | 0.01795 |
| 35mm Medium | 0.01547 | 0.01548 |
| 35mm Ultra Fine | 0.01284 | 0.01314 |

Gated by `[Stage 2b]`; `[Stage 2]` (a plate WITH real luminance range) still
recovers its curve to ≤1.1% per band, confirming the debiased binning did not
cost real-content accuracy.

### What the same test revealed but did NOT fix

- **Residual-band truncation, now quantified on real stock.** Energy retained
  at the plugin's cutoff: 8mm Course **38.8%**, 16mm 60.2%, 35mm Medium 76.5%,
  35mm Ultra Fine 71.2%. The 8mm plate loses 61% of its grain energy before
  synthesis begins, and its grain size drops 3.70px → 3.09px in the output.
  `GT_BlurSigmaFor` scales the cutoff with RESOLUTION, not with the measured
  grain — so a coarse and a fine stock at the same frame size are split
  identically. Tracking the cutoff to the measured grain instead is the open
  question. (Caveat: some sub-cutoff energy is plate lighting non-uniformity
  rather than grain, which is likely why Ultra Fine sits below Medium.)
- **Coarse grain is not Gaussian.** 8mm Course measures skew **+0.78**, excess
  kurtosis **+1.95**; RPN is Gaussian by construction and reproduces +0.19 /
  −0.02. The other three stocks are near-Gaussian and match well. Coarse stock
  has sparse punchy blobs a random-phase Gaussian field cannot make —
  architectural, not a tuning matter.
- **These plates are monochrome** (35mm Ultra Fine has literally identical RGB
  channels; the others differ by a mean of ~8e-7). They read `crossPair`
  +1.000/+1.000/+1.000, which the common-factor model handles correctly by
  collapsing to one shared field — but they cannot exercise the cross-channel
  work at all. Evaluating that needs COLOUR grain plates.
- Temporal decorrelation passes cleanly: frame-to-frame |r| ≤ 0.01 across every
  stock and band, source and output alike.


## Non-goals (unchanged from the brief)

No degrain, no procedural/no-source synthesis (use AE Match Grain or add a
plate), no blend/mask/grade controls, no multi-stock reconciliation, no
per-shot external maps, no extra colour controls beyond the single gate, no
GPU until CPU is signed off, no parameters beyond the locked seven.
