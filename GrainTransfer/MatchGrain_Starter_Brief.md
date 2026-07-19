# Starter Brief: Grain Transfer Plugin for After Effects

STATUS: **Starter brief.** The core method below is a strong recommendation, not a fully-locked spec like the EdgeExtend brief. Two decisions are marked **[VALIDATE EARLY]** — build a quick spike to confirm them before committing to the full staged plan. Do not treat this as final until those gates pass.

## Context

- Part of an existing AE plugin suite (ModernEdgeBlur and an Edge Colour Repair plugin). **Do not modify ModernEdgeBlur's plugin code.** Use it as the reference for repo conventions: CMake structure, `build.ps1`, PiPL sync discipline, disk-ID rules, 8/16/32bpc via `UnpackRow`/`PackRow` with float32 internal math, MFR-safe patterns (no mutable globals).
- Target: C++17, AE SDK SmartFX, Windows first, matching the existing build system.
- FFT dependency: vendor a small permissively-licensed FFT (kissfft, MIT — do NOT use FFTW, its licence is unsuitable for a commercial plugin).

## Suite protection — read before touching any file

Other plugins in this suite are shipped or mid-build in **separate sessions**. Breaking them from this session is the failure mode to prevent.

**One-time boundary read (do this once, at the start — not per edit).** Before writing code, inspect the repo and produce a short explicit list: which files/directories are **plugin-specific** (belong to ModernEdgeBlur, Edge Colour Repair, or this grain plugin) versus which are **shared** (a common library, shared build files, shared headers). State this list once. This is a single upfront cost, not a per-file check on every edit — do not re-scan the tree repeatedly.

**Hard rules:**
- **Do not modify the plugin-specific code of any existing suite plugin** — not ModernEdgeBlur, not Edge Colour Repair, not any other. This grain plugin adds new files; it does not edit theirs.
- **Shared code is additive-only.** You MAY add new functions/files to a shared library. You MAY NOT change the signature or behaviour of anything an existing plugin already calls, and MAY NOT rename/move/delete shared symbols, without stopping to ask first. (Same discipline as disk IDs: append, never mutate.)
- If a change to shared code seems necessary beyond additive, **stop and ask** — surface exactly what you'd change and which plugins call it, and wait for confirmation.
- Shared build files (e.g. top-level CMake): add this plugin's target additively; do not alter how existing targets build.

**Git is the real backstop.** The instructions above are requests, not enforced locks. Commit this plugin's work on its own branch or in small, clearly-scoped commits so any unintended cross-plugin edit is visible in a diff and revertable. Never run destructive git operations (reset --hard, clean, force-push, branch deletion) without explicit confirmation.

## What this plugin is

A **fast grain-transfer primitive.** It replaces AE's Match Grain, which is wildly slow because it re-analyzes and re-synthesizes grain heavily. This tool inverts the cost model: analyze the source *once*, precompute grain tiles *once*, and make the per-frame render loop trivial (stamp + modulate + add).

Suite philosophy — same as the rest of the toolset: **real measured detail over synthetic models; paring knife, not multitool.** One job: take the grain character of a source plate and lay ghost-free, seam-free grain onto a target element. Everything else (blend modes, masking, further grading) is the compositor's job downstream.

## The core problem and the method

Naive frequency separation (`grain = plate − blur(plate)`) does NOT isolate grain — the high-frequency residual contains grain **plus** all content structure (edges, detail). Transferring it ghosts the source's shapes onto the target. Simple re-tiling relocates the ghost and adds seams + visible periodicity.

**Solution — phase-randomized grain (RPN, random phase noise):**
- Content position is encoded in the Fourier **phase**; grain character (size/frequency distribution) is encoded in the Fourier **magnitude**.
- Keep the real measured **magnitude spectrum** of the source residual; **randomize the phase**; inverse-FFT.
- Result: a noise field with the *exact grain character of the real plate*, but with all content structure destroyed (no ghosting, by construction) and inherently periodic (seamless tiling, by construction).
- This is regeneration from a *real measured spectrum*, not parametric synthesis. It honours the "real detail, not modeled" philosophy.

Implementation note: to get a real-valued output from the inverse FFT, enforce Hermitian (conjugate) symmetry on the phase-randomized spectrum. Otherwise you get complex garbage.

Known limitation: RPN output is Gaussian; real grain is slightly non-Gaussian (highlight skew). Usually invisible. If a validation gate shows it, the cheap fix is histogram-matching the RPN tile to the source residual's amplitude histogram (restores non-Gaussianity, keeps decorrelation). Build WITHOUT this first; add only if needed.

## How DasGrain does it — and why we don't inherit its dependencies

The main Nuke community equivalent (DasGrain) attacks the identical core problem — reuse real grain without ghosting or repetition — but in the spatial domain, and its dependencies are exactly what we're avoiding:

- It is **difference-based**: it extracts grain as `plate − degrained_plate`, so it hard-requires a **good degrain of the exact shot** as a second input. Its own docs stress a good degrain is crucial. Degraining well is a whole separate expensive problem and quality risk — an entire other plugin's worth. **Our RPN method needs only a grain *sample* (a flat region or a source whose spectrum we measure); it does NOT require the user to produce a clean degrain.** This is a real simplification in our favour.
- It decorrelates grain from content via an **animated Voronoi scatter (an STMap warp)**, which requires a per-shot animated Voronoi map sequence and depends on the STMap/Voronoi nodes. Its public bug history is dominated by exactly this: STMap crashes across Nuke 13.1/15.x, GPU-dependent kernel errors, inputs disconnecting on reload, and hard failure on Nuke Indie/NC. **Phase randomization replaces the entire scatter with a self-contained FFT — no external map, no STMap dependency, no per-shot Voronoi sequence to manage.** Same problem solved, none of the fragile machinery.

Net: DasGrain validates that real-grain-reuse + decorrelation is the right family. We keep its quality benefit (real measured grain, ghost-free) and drop its two heaviest liabilities — the mandatory degrain and the animated-Voronoi STMap.

**Back-pocket fallback (explicitly deferred, NOT built in v1):** a procedural/noise synthesis path for the *no-source* case (graining a grainless CG element with no plate to sample). This is the AE-Match-Grain use case and is deliberately out of scope — if a user needs to generate grain from nothing, the answer is "use AE Match Grain, or add a plate." Noted here only so it is a known, documented escape hatch we could add later as a separate mode, never as a reason to make the have-a-source path heavier.

## Float-native — non-negotiable through the entire chain

AE Match Grain's inability to work properly in 32-bit is a headline limitation to beat, and beating it is free if we never break the float chain. Every stage — FFT, magnitude spectrum, RPN tile, luminance modulation, working-space encode/decode, final add — operates on float32 and must never round-trip through an integer or clamped buffer "for convenience." Grain in a 32-bit scene-referred comp must sit correctly on values well above 1.0; a float-native add handles this where an integer-internal path clips or crushes highlights. This is a structural property of the pipeline, not a feature to add later. The 8/16bpc input/output paths convert at the boundary only; all internal math stays float.

## Cost model — the whole point

1. **Analyze once** (on source change): compute the average power spectrum of the source high-frequency residual (average over several patches/frames for stability), and a luminance→grain-amplitude curve (bucket source luminance into ~5 bands, measure grain sigma per band).
2. **Precompute once**: bake a bank of N (~8–16) seamless RPN grain tiles from the spectrum, each with a different phase seed.
3. **Per frame (cheap)**: tile the target from the bank with a per-frame random offset + flip + tile selection (breaks periodicity, gives temporal variation so grain doesn't freeze), modulate amplitude by target local luminance via the sampled curve, scale by Amount, add to target. No per-frame FFT, no per-frame analysis.

Grain must vary per frame — static grain reads as dirt on the lens. Per-frame re-randomization of offset/flip/tile handles this at near-zero cost.

## Colour management & working space — FIRST-CLASS REQUIREMENT

This is critical to the toolset and must be designed in from the start, not bolted on. Getting it wrong produces a subtle "grain looks right in mids but wrong in highlights" bug that is very hard to chase later.

**Physical principle (non-negotiable):** film grain is roughly constant in *density* — i.e. in *log* — and only becomes exposure-dependent when viewed in linear. Therefore the grain math — BOTH extraction of the sample AND application onto the target — must happen in a **log/density-encoded space**. Pick the space, do everything (extract, decorrelate, apply) in it, convert in/out only at the boundaries.

**What the incoming pixels actually are (important correction):** in a colour-managed AE project, footage is already converted into the project **working space** by the time the effect samples it — so per-footage interpretation/IDT is irrelevant to us; we need exactly one fact, the working space's transfer function. But note: **AE working spaces are display-gamma-encoded or linear — never log.** AE's default is Rec.709 Gamma 2.4; wide-gamut options like ACEScg are linear; AE doesn't even offer ACEScc/ACEScct as a *working* space. So log is our internal *destination* for the grain math, never something we receive. The real question is therefore **"are the incoming pixels linear or display-gamma-encoded?"** — and then we convert into our log apply-space accordingly. Three incoming realities:

- **Scene-linear (ACEScg / linearized working space):** values well past 1.0, linear light. Applying grain without encoding is dramatically wrong (grain explodes in highlights, vanishes in shadows). Convert linear → log-density (e.g. an ACEScct-style log curve) → apply → back. **This is where getting it wrong is most glaring** — as you predicted, ACES/linear makes the error obvious where display-encoded would partly hide it.
- **Display-gamma-encoded (Rec.709/sRGB working space):** roughly 0–1, gamma curve. Gamma is already a rough perceptual/log-ish curve — which is *why* "add grain in a Rec.709 project" kind-of-works. Apply with little/no additional encoding.
- **Colour management bypassed / Preserve-RGB (legitimate legacy AE workflow):** the user has deliberately opted out of colour management; the incoming pixels carry no reliable transfer-function signal the plugin can trust. This is the case that forces a manual override to exist at all — the user tells us, because AE won't.

**Detection: query the API, do NOT infer from pixel values.** You cannot reliably recover the transfer function from pixel values — a dim linear image with no highlights above 1.0 is range-indistinguishable from a display-encoded image; the same mid-gray number means different light in different encodings. So value-sniffing is a fallback guess, never the source of truth. The correct source of truth is the AE colour-management API: recent SDKs (the knowledge base referenced is current to AE 26.0, newer than the assistant's training) expose a suite described as accessing **colour profiles, OCIO configuration, and colour transforms.** Acquire that suite, query the working space, and derive the incoming transfer function from it.

**The gate becomes: auto-query first, manual override for the bypass case.** So the parameter's role shifts — it is NOT the primary mechanism, it is the override:
- **Auto (default):** query the working space via the colour/OCIO suite; pick linear-encode or display-encode path accordingly.
- **Manual (Display-Bypassed · Scene-Linear · ACES):** for the CM-off/Preserve-RGB case where there is no reliable signal, and as a safety net if the query proves unreliable on a given AE version.

This still justifies the single gate parameter as load-bearing (the bypassed workflow genuinely needs it), but demotes it from "how we normally know" to "the override when the API can't tell us."

**SDK behaviours to verify at Stage 3 (spike, don't assume):**
1. The exact suite name and what the working-space query returns — a named space, an ICC profile, or an OCIO colorspace string — and how to map that return onto "linear vs display-gamma" reliably.
2. **Whether the query reports usefully when colour management is OFF / Preserve-RGB**, or just returns "None". This specific behaviour is the entire reason the manual override must exist — confirm it against the running SDK, not a doc summary. State findings explicitly.

**Interaction with the Response curve (resolve at a validation gate):** applying correctly in log-density *already* makes grain exposure-dependent in the physically correct way — which overlaps with what the manual Response (luminance modulation) curve does. Doing both naively double-counts exposure dependence. Likely resolution: Response becomes a fine-tune *trim* on top of the physically-correct baseline, or the manual fallback specifically for the bypassed/display case where there is no scene-linear encoding to lean on. Do not guess this now; resolve it empirically once working-space handling and the Response curve both exist.

## Parameters — proposed, tight. Confirm before locking disk IDs.

| # | Param | Type | Range | Default | Disk ID |
|---|-------|------|-------|---------|---------|
| 1 | Grain Source | layer param | — | none | 101 |
| 2 | Amount | float % | 0–200 | 100 | 102 |
| 3 | Size | float % | 25–400 | 100 | 103 |
| 4 | Response | float % (luminance modulation trim) | 0–100 | 100 | 104 |
| 5 | Seed | int | 0–9999 | 0 | 105 |
| 6 | Working Space (Apply In) | popup: Auto / Display-Bypassed / Scene-Linear / ACES | — | Auto | 106 |
| 7 | Preview Grain Only | checkbox | — | off | 107 |

Notes:
- **Grain Source is a legitimate layer param** — unlike EdgeExtend, this is a *match* operation that inherently needs two inputs (target = applied layer, source = this). This is not bloat; it is the nature of the task.
- **Size** rescales the grain tile — needed when target and source differ in resolution/scale.
- **Response** — luminance modulation trim; see the colour-management section for why it is a *trim* on top of physically-correct log-space application rather than the sole exposure-dependence mechanism. Its exact role is resolved at a validation gate.
- **Working Space** is the colour-management override (see the dedicated section). Default **Auto** queries the project working space via the colour/OCIO suite and picks the linear-vs-display encode path automatically; the manual settings exist for the CM-bypassed/Preserve-RGB workflow (where AE gives no reliable signal) and as a version safety net. Most users never touch it.
- **Preview Grain Only** shows the generated grain layer alone — debug/trust aid.
- Assumption: source and target share stock/ISO (the "grain my CG to match this plate in the same shot" case). Do not add controls to reconcile different stocks — that is scope creep.

Seven params, of which two (Grain Source, Working Space) are essential inputs to correctness rather than options, and one (Preview) is a debug aid. Still a paring knife.

## Non-goals — do not implement

- No parametric/procedural grain model (Gabor, sparse convolution, etc.). Use the real measured spectrum.
- No grain *removal*/degrain (different tool).
- No blend-mode / masking / grading controls — compositor's job downstream.
- No multi-stock reconciliation, no per-channel grain modeling beyond what the sampled spectrum gives for free.
- No GPU before the CPU path is validated and fast.
- No per-shot external maps, degrain inputs, or STMap/Voronoi machinery (the DasGrain dependencies we are explicitly avoiding).
- No colour-management controls beyond the single Working Space gate — no per-channel colourspace knobs, no OCIO config exposure, no display-transform options. The gate picks the apply-space; that is the whole surface.
- No parameters beyond those above without stopping to ask.

## Staged plan with validation gates

**Stage 0 — Skeleton.** New plugin target alongside the suite. Passthrough SmartFX, repo conventions, shared foundation reused. Vendor kissfft and confirm it builds.
*Gate:* loads in AE, byte-identical passthrough at 8/16/32bpc; FFT round-trip (forward then inverse) reconstructs a test image within float tolerance.

**Stage 1 — [VALIDATE EARLY] RPN proof of concept (headless, no AE).** From a still grain plate, extract residual, compute magnitude spectrum, phase-randomize with Hermitian symmetry, inverse-FFT to a tile. Tile it across a blank canvas.
*Gate:* the tiled RPN grain (a) visually matches the source grain's size/character, (b) shows NO ghosting of source content, (c) shows NO visible seams or periodicity at reasonable tile size. **If ghosting or Gaussian-look fails here, resolve the method before proceeding** — this is the make-or-break gate for the whole approach. If RPN's Gaussian character is visibly wrong, evaluate the histogram-matching fix here.

**Stage 2 — [VALIDATE EARLY] Luminance response (headless).** Sample source luminance→grain-amplitude curve; apply to a target of varying brightness.
*Gate:* grain is correctly louder/quieter across exposure to match the source's behaviour; grain sits "in" a test gradient rather than floating on top.

**Stage 3 — AE integration, params, layer input, colour management.** Wire the seven params, Grain Source layer checkout, Preview Grain Only. Analyze-once / precompute-bank / cheap-per-frame architecture in place. Per-frame offset/flip/tile randomization + Seed. Implement working-space encode/decode (extract and apply in log-density space). For the Working Space gate: **acquire the colour/OCIO suite, query the project working space, and map its return to a linear-vs-display encode path — do not infer encoding from pixel values.** Spike and report the two SDK behaviours flagged in the colour-management section (exact query return/mapping; behaviour when CM is OFF/Preserve-RGB). Manual override handles the bypass case and any version where the query is unreliable.
*Gate:* real source plate → real target in AE; grain matches; temporally varies (no frozen grain); Preview Grain Only shows clean decorrelated grain. **Grain matches correctly in all three colour contexts: a scene-linear/ACES comp, a display-gamma (Rec.709) comp, and a CM-bypassed/Preserve-RGB comp — with no highlight blow-out in the linear case.** Resolve the Response-vs-log-space double-counting here.

**Stage 4 — Bit depths + defaults + polish.** 8/16bpc via dispatch layer; tune defaults; confirm analysis re-runs only when source actually changes (cache/invalidorate correctly).
*Gate:* full-quality match on real footage at all bit depths; changing target frame doesn't re-trigger source analysis.

**Stage 5 — CPU performance.** Thread analysis and per-frame stamping; ensure the precompute is one-time and the render loop is allocation-free after warm-up. Benchmark.
*Gate:* per-frame cost dramatically below AE Match Grain on the same shot (measure and report the ratio); clean 10,000-frame soak with stable memory.

**Stage 6 — GPU path (only after 0–5 signed off).** Per-frame stamping + luminance modulation are trivially GPU-friendly (the FFT precompute can stay CPU). Mirror math exactly.
*Gate:* CPU/GPU parity within tolerance; soak repeated.

## Working style

- Unit-test the pure DSP (FFT round-trip, RPN decorrelation, luminance curve) headlessly — must not depend on AE.
- Stages 1 and 2 are deliberately headless and BEFORE AE integration, because the method risk lives there, not in the plumbing.
- Prefer ModernEdgeBlur's established patterns where the AE SDK is ambiguous; state assumptions explicitly.
- If anything conflicts with the repo, stop and ask rather than improvising.
