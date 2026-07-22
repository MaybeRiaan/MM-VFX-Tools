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
- **Stage 5 (CPU performance): DONE, gate re-defined + PASSED 2026-07-22.**
  Injected parallel-for (`src/common/MM_Parallel.h`, `iterate_generic` backend
  in AE, serial in tests) threading the EDT / pull-push; zero per-frame
  allocation (grow-only `thread_local` scratch everywhere, MFR-safe, no global
  state); early-out passthrough. Gate re-defined (original <40ms/4K unreachable
  on the 8c/8t i7-9700K with correctness-preserving CPU work; 4K-realtime is
  Stage 6/GPU's job): big speedup (~1150ms→~300ms in AE) + zero alloc churn +
  clean soak. Full detail in `docs/EdgeRepair.md`'s Stage 5 entry.
- **Detail method REPLACED + inner softness (2026-07-22, v1.0.0.17, AE-
  confirmed):** frequency separation was DARKENING the edge (a luminance edge
  and texture are the same frequency, so `hi = orig - blur(orig)` re-added a
  signed edge step). Detail is now **synthetic grain** (`src/common/
  MM_Grain.h`): zero-mean per-pixel luma noise, amplitude auto-estimated from
  the trusted core, `out = lerp(orig,ext,w) + w*detail*grain`. Cannot darken,
  fixed the premultiplied-input rim, dropped the Gaussian pass. Also added an
  **inner-band ease** (`BlendWeight` 3-arg, `inner = 0.15*Extend`) so the band
  blends back into the real core colour instead of leaving a hard line on wide
  light wraps. Both user-confirmed in AE. (The Stage-5 band-box Gaussian and
  its two tests were removed as moot once the Gaussian went away.)

**NEXT — View Mode deliverables (see `docs/HANDOFF_VIEWMODES.md`, which is
gated on Stage 5 being merged — now done).** Promote the diagnostic View Mode
entries into real deliverables per the user's intent: rename "Result" ->
"Composited" (label only), Extended Colour = opaque clean-plate colour source,
Distance Field = opaque distance-as-luminance usable as a luma/track matte
downstream (polarity + white-point mapping are GATE DECISIONS, not guesses),
Core Matte stays diagnostic. No new params; disk IDs 101-105 untouched. Read
that file and confirm the mapping choices with the user before coding.

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
