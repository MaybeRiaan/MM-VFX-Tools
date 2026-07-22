# Handoff — after the view-mode deliverables + Softness + Erode pass

**Run in a FRESH session.** This work spans two PCs — **`git fetch origin` and
reconcile BEFORE touching anything.** Then `git log --oneline -6` to confirm
you're on top of `bfa496d` (EdgeRepair v1.0.0.23). If it's not in your history,
STOP and reconcile.

Continuing the **Edge Repair** plugin (After Effects "MM" suite) in this repo.
**Do not modify Modern Edge Blur's plugin code** (reference only).
**Read `docs/EdgeRepair.md` first** — it is the durable spec + staged plan and
is fully up to date as of this handoff.

---

## What just shipped (commit `bfa496d`, v1.0.0.23)

The View Mode entries are now real, deliverable-quality outputs, plus a new
Softness control and an Erode range change. All AE-gated by the user this
session. Headless tests green.

- **"Result" → "Composited"** — label only. `ER_VIEW_RESULT` (=1) and disk ID
  105 unchanged; render output byte-identical.
- **Extended Colour** — first-class opaque clean-plate deliverable (straight-RGB
  pull–push fill; empty-core = black). Verified, no plumbing change.
- **Distance Field** — now an **INWARD light-wrap luma matte of the erode gap**:
  WHITE at the original alpha edge → BLACK toward the eroded core over an
  `Erode`-wide band; opaque, black in exterior + core interior; Erode 0 → all
  black. Code: `ER_DistViewRefcon` / `ER_DistViewRow` + the
  `ER_VIEW_DISTANCE_FIELD` branch in `EdgeRepair.cpp`.
- **Core Matte** — untouched (diagnostic).
- **Softness (param #6, disk ID 106)** — fixed-**px** slider, 0–250, default 5.
  One knob drives both the Composited inner ease (`= Softness px` → BlendWeight)
  and the Distance Field inward ramp (`= Softness px`, clamped to the gap).
  Replaced the old fixed `ER_INNER_SOFT_FRAC` 15%-of-Extend constant.
- **Erode → 0–50** (was −50…50). Negative Erode (dilate) removed — no legitimate
  use for decontamination (verified from the fill code). The engine's
  `BuildCoreMatte` grow branch stays (still unit-tested) but is UI-unreachable.
  **Alpha Choke keeps its signed −50…50** (grow is meaningful there).

### Locked / invariant (do not change without a new ask)
- Param **disk IDs 101–106 are forever.** View Mode order is locked.
- Composited render must stay byte-identical (label-only for that view).
- Softness default 5px is *not* byte-identical to the old 15% look (~4.8px) —
  that was an accepted, visually-indistinguishable shift, not a regression.

---

## State of the staged plan (see `docs/EdgeRepair.md` for detail)

- Stages 0–4: DONE + AE-gated. Stage 5 (CPU perf): DONE. This pass sits on top.
- **Stage 6 — GPU path: NOT STARTED.** That's the next real stage: DX12/HLSL
  kernels mirroring the CPU math (threshold, distance, pull–push, composite),
  `PF_OutFlag2_SUPPORTS_GPU_RENDER_F32` + GPU device suites. Gate = CPU/GPU
  parity within tolerance + soak on the GPU path. **Only start after the user
  signs off that 0–5 + this pass are final.**

If the user wants to keep polishing the deliverable views instead of starting
Stage 6, the likely candidate is subjective Distance Field / Softness tuning on
more live plates (no new params without sign-off). Nothing else is outstanding
from this pass.

---

## Build / test mechanics (critical — get these right)

- Build with **`build_edgerepair.ps1`**, NEVER `build.ps1` (that bumps Modern
  Edge Blur's version). It bumps `BUILD_VERSION` in BOTH `src/EdgeRepair.h` AND
  `resources/EdgeRepairPiPL.r` together — drift → AE "version mismatch."
- **Close After Effects before building**, or the deploy-copy to the plugins
  folder fails (the `.aex` is file-locked; compile/link still succeed and the
  build/ output updates, but the AE folder won't refresh). The build script
  handles the AE-folder copy on success.
- After building, **copy `build/EdgeRepair.aex` → `dist/EdgeRepair.aex`** and
  commit it (`.gitignore` has `!dist/*.aex`). Verify with
  `cmp build/EdgeRepair.aex dist/EdgeRepair.aex`.
- Headless algorithm tests: `tests/build_tests.ps1` (plain cl.exe, no AE SDK) —
  keep green. AE-side view/render code is not headless-tested (same as the other
  view rows); the pure algorithm headers are.
- **Stop at a visual gate** and let the user confirm in AE before committing as
  final. This session's whole workflow was gate-driven.

---

## Housekeeping for whoever picks this up

- There is a stray **`image.png`** in the repo root (a screenshot from this
  session) and an untracked **`.claude/`** dir — both intentionally NOT
  committed. `image.png` can be deleted; `.claude/` is local session state.
- No SDK to GitHub ever (see memory / `.gitignore`).
