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

**Where things stand (verify with `git fetch`):**
- Stages 0-4: DONE, all AE gates user-confirmed.
- **Stage 5 (CPU performance): DONE, gate re-defined + PASSED 2026-07-22
  (v1.0.0.14).** Everything is BYTE-IDENTICAL to Stage 4 output — proven by
  headless tests, not asserted. Landed: an injected parallel-for
  (`src/common/MM_Parallel.h`, `iterate_generic` backend in AE, serial in
  tests) threading the EDT / Gaussian / pull-push; zero per-frame allocation
  (grow-only `thread_local` scratch everywhere, MFR-safe, no global state);
  and a band-box-limited Detail Gaussian (`GaussianBlurH/V_Rect` — blur only
  the blend band's bounding box + kernel margin, clamp source to the full
  image so used pixels are bit-identical). The original <40ms/4K gate was
  re-defined (unreachable on the 8c/8t i7-9700K with correctness-preserving
  CPU work; 4K-realtime is Stage 6/GPU's job) to: big multiplicative speedup
  (~1150ms→~300ms in AE) + zero alloc churn + clean soak + byte-identical.
  A box-blur approximation was tried and REJECTED (2.3x slower — the blur is
  memory-bandwidth-bound at 4K, not compute-bound). Full detail + the exact
  test list is in `docs/EdgeRepair.md`'s Stage 5 entry.

**NEXT — a KNOWN, SEPARATE issue surfaced at the Stage 5 gate (NOT a Stage-5
regression; proven identical with the band-box on and off via the
`EDGEREPAIR_FULLBLUR` A/B toggle, since removed):**
On **premultiplied-alpha input footage**, raising Detail re-introduces a light
edge rim. LIVE keyed footage (true straight edge RGB) is clean. Root cause: the
plugin's locked STRAIGHT-alpha convention — a premultiplied export has
multiplied semi-transparent edge RGB toward the matte colour, so
`low = gaussianBlur(origStraightRGB)` and `hi = orig - low` (in
`ER_ResultRow`, `src/EdgeRepair.cpp`) capture a spurious high-frequency step at
the alpha boundary, and Detail (`out += w*detail*hi`) amplifies it into a rim.
The `hi` guard currently zeroes only at `a <= 1e-6` (fully transparent), NOT
across the semi-transparent band where the rim lives. This is an input-
convention interaction, same failure SURFACE as the Gate-4 round-2
alpha-weighted-blur bug but a different trigger. **Do NOT just start coding a
fix — first confirm with the user how they want premultiplied input handled**
(options to weigh, none chosen yet: detect & unpremultiply-on-input then
re-premult; attenuate `hi` by an alpha-gradient / coverage term across the
band; document it as a footage-prep requirement (straight in) and add no code;
or a combination). The brief forbids new user params without an explicit ask,
so a "premult input" knob is off the table unless the user requests it.

**Build/test mechanics — do not relearn these the hard way:**
- Build with `build_edgerepair.ps1`, NEVER `build.ps1` (that bumps
  ModernEdgeBlur's version, which the user explicitly does not want). It bumps
  `BUILD_VERSION` in BOTH `src/EdgeRepair.h` AND `resources/EdgeRepairPiPL.r`
  together; if they drift AE shows a misleading "version mismatch" — check both
  files match before blaming AE cache.
- **The deploy-copy to the AE plugins folder FAILS if After Effects is open**
  (the `.aex` is file-locked). The compile/link still succeed and `build/`
  still updates — just close AE and re-copy (`cp build/EdgeRepair.aex` to the
  plugins folder, or re-run the script).
- After building, copy `build/EdgeRepair.aex` to `dist/EdgeRepair.aex` and
  commit it (`.gitignore` has a `!dist/*.aex` override).
- New/changed AE-free algorithm code lives in `src/common/MM_*.h` (NO PF/SDK
  types) so it stays headlessly unit-testable via `tests/build_tests.ps1`
  (plain `cl.exe`, copies into `%TEMP%` first to dodge spaces-in-path). Any
  refactor of those headers MUST keep the existing `tests/test_*.cpp` green —
  that is how we prove the math didn't change. The Stage 5 guardrails
  (`test_bandblur.cpp`, `test_bandcomposite.cpp`, `test_soak.cpp`) are the ones
  that prove byte-identity + zero-alloc; keep them green.
- Headless perf bench: `tests/run_perf.ps1` (serial-vs-threaded bit-compare +
  timings). AE per-frame timing: set `ER_PERF_LOG 1` in `EdgeRepair.cpp`,
  rebuild, RAM-preview, read with `tests/read_ae_perf.ps1` (writes
  `%TEMP%\EdgeRepair_perf.log`). Note: `pwsh` is NOT installed on the 9700K box
  — use `powershell` or call the `.ps1` directly with `&`.
- Stop and report at each stage gate; wait for user confirmation before Stage 6
  (GPU). Do not add parameters, view modes, or scope beyond `docs/EdgeRepair.md`
  without asking first.
