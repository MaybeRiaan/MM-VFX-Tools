# Handoff prompt — paste this to start the next session

---

Continuing the Edge Repair plugin build in this repo (After Effects "MM"
suite, alongside the already-shipped Modern Edge Blur — do not modify Modern
Edge Blur's plugin code; it is reference only).

**Read `docs/EdgeRepair.md` first** — it is the durable source of truth: the
full build brief, the LOCKED five-parameter spec (Erode/Extend/Detail/Alpha
Choke/View Mode, disk IDs 101-105 — never reorder or reuse; Softness and a
Premultiplied checkbox were REMOVED on 2026-07-06, do not reintroduce), the
AE SDK requirements, and the staged plan with per-stage gate status. Then
`git log --oneline -12` for recent history. **This file may be stale between
PCs — the work happens across two machines, so `git fetch` and reconcile
BEFORE doing anything** (this repo was found 11 commits behind once already).

**Where things stand (verify with `git fetch` — as of writing, origin/main
@ `86a35ed`, EdgeRepair v1.0.0.12):**
- Stages 0-3: DONE, all AE gates user-confirmed.
- **Stage 4 (composite + Detail + Alpha Choke): DONE, gate PASSED
  2026-07-22.** Rim gone at Detail 0; Alpha Choke=0 bit-identical alpha;
  Detail 0 vs 100 differs only inside the blend band. The same-scale
  contamination ring that survives at Detail 100 is CORRECT, not a bug
  (`out = orig + w*(ext - low)` is a purely low-frequency correction;
  frequency separation cannot separate a same-scale ring from real edge
  texture). To soften the out-of-box result the **Detail default was lowered
  100 -> 50** (`ER_DETAIL_DFLT` in `src/EdgeRepair.h`; range unchanged).
- OPEN DESIGN NOTE (do NOT act on without an explicit ask): whether Detail
  should roll off in the innermost band, or a separate rim-suppression
  control. Per the brief, no new params without the user asking first.

**Next stage: Stage 5 — CPU performance.** The algorithms are correct and
frozen; this stage is pure optimization and MUST NOT change any output
values (byte-identical Result before/after is the discipline — the headless
tests in `tests/` are the guardrail; keep them green). Scope, per the brief:

1. **Parallelize the two O(n) hot passes** using the existing
   `mm::ForEachRow` / `iterate_generic` primitive (already used for the
   final composite row-pass, `src/common/MM_Common.cpp`):
   - **Euclidean distance transform** (`src/common/MM_DistanceField.h`,
     `EuclideanDistanceTransform`): the column pass (`for x`, ~line 110) and
     row pass (`for y`, ~line 117) are independent per line — thread them.
     The `DT_1D` scratch (`f/v/z`) must become per-thread, not shared.
   - **Separable Gaussian** (`src/common/MM_Gaussian.h`, `GaussianBlurH/V`):
     rows independent in H, columns independent in V — thread each.
   - **Pull–push** (`src/common/MM_PullPush.h`): parallelize within each mip
     level (levels are sequential/dependent; texels within a level are not).
2. **Kill per-frame allocation.** SmartRender currently `std::vector`-
   resizes fresh buffers every frame: `srcBuf` (~L711), `alphaPlane/core/
   dist` (~L767-769), Extended-view `ext` (~L814), Result `ext/low`
   (~L846-850), Gaussian `plane/tmp` (~L872), `chokeField` (~L896) in
   `src/EdgeRepair.cpp`. Reuse scratch across frames WITHOUT reintroducing
   mutable global/static state (MFR-safe is non-negotiable — the plugin must
   stay reentrant). Options to weigh: a render-scoped scratch arena checked
   out per render, or `sequence_data`. Pick one and document the choice.
3. **Early-out passthrough** when `Extend <= 0 AND Alpha Choke == 0` (Result
   view) — no field, no fill, no Gaussian; bitwise copy. Partially present
   already (`doBand`, and the Choke==0 skip) — make the whole-frame trivial
   case a clean single early return and confirm it allocates nothing.

**Stage 5 gate:** <~40ms/frame at 4K float on a modern desktop CPU; zero
allocation churn per frame after warm-up; clean 10,000-frame soak with
stable memory (no leak, no growth). Stage 4 correctness is already banked —
do not re-open it. Measure BEFORE and after (get a baseline first) and
report the numbers at the gate.

**Build/test mechanics — do not relearn these the hard way:**
- Build with `build_edgerepair.ps1`, NEVER `build.ps1` (that one bumps
  ModernEdgeBlur's version, which the user explicitly does not want). It
  bumps `BUILD_VERSION` in BOTH `src/EdgeRepair.h` AND
  `resources/EdgeRepairPiPL.r` together (`Update-BuildVersion`); if they
  drift AE shows a misleading "version mismatch. Code version is 1.0 and
  PiPL version is 1.0" — check both files match before blaming AE cache.
- **The deploy-copy to the AE plugins folder FAILS if After Effects is
  open** (the `.aex` is file-locked). The compile/link still succeed and
  `dist/` still updates — just close AE and re-copy (or re-run the script).
- After building, copy `build/EdgeRepair.aex` to `dist/EdgeRepair.aex` and
  commit it (`.gitignore` has a `!dist/*.aex` override).
- New/changed AE-free algorithm code lives in `src/common/MM_*.h` (NO
  PF/SDK types) so it stays headlessly unit-testable via
  `tests/build_tests.ps1` (plain `cl.exe`, copies into `%TEMP%` first to
  dodge spaces-in-path). Any perf refactor of those headers must keep the
  existing `tests/test_*.cpp` green — that is how we prove Stage 5 didn't
  change the math.
- Stop and report at the stage gate; wait for user confirmation before
  Stage 6 (GPU). Do not add parameters, view modes, or scope beyond
  `docs/EdgeRepair.md` without asking first.
