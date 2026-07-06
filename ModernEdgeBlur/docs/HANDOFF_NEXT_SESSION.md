# Handoff prompt — paste this to start the next session

---

Continuing the Edge Repair plugin build in this repo (After Effects "MM"
suite, alongside the already-shipped Modern Edge Blur — do not modify
Modern Edge Blur's plugin code).

Read `docs/EdgeRepair.md` first — it's the full build brief, the locked
five-parameter spec (Erode/Extend/Detail/Alpha Choke/View Mode, disk IDs
101-105, amended 2026-07-06 — Softness and a Premultiplied checkbox were
REMOVED from an earlier draft, don't reintroduce them), the AE SDK
requirements, and the staged plan with gate status. `git log --oneline -10`
for recent history if useful.

**Where things stand:** Stages 0-2 are built, deployed, and committed
(053d9b5, 2076adf, 37482c2, cf6e593, 5ceb927, a712d17 — all pushed to
origin/main). Stage 2 added the core matte + Euclidean distance field
(`src/common/MM_DistanceField.h`, `src/common/MM_CoreMatte.h`), verified
headlessly against a brute-force reference (`tests/build_tests.ps1` — all
pass, worst diff = 0), and wired `View Mode` = Core Matte / Distance Field
to real visualizations. Result and Extended Colour are still passthrough.

**First thing to do:** the Stage 2 AE visual check was requested but never
confirmed by the user. Ask them to restart AE, apply Edge Repair (v1.0.0.5
is deployed), and check:
- **Core Matte** view: white = trusted core. Erode > 0 shrinks it inward,
  Erode < 0 grows it outward, both uniformly (no faceting — it's distance-
  based, not iterative).
- **Distance Field** view: black at the core, smoothly brightening outward
  to white at `Extend` px away, radially even, no banding/corners.

If that passes, move on to **Stage 3: pull-push fill.** Per the brief:
mip-chain pull of premultiplied RGBA + coverage, push fills only invalid
pixels (bilinear from the coarser level), unpremultiply at the end,
~2*log2(maxdim) passes. This is the ONLY fill engine — the distance field
never does colour fill itself, only drives erode/extend/choke/blend. Wire
`View Mode = Extended Colour` to show the unlimited (pre-`Extend`-clamp)
unpremultiplied fill result. Gate: compare against real keyed plates, watch
for blockiness or colour bleeding from zero-alpha garbage pixels (coverage
weighting has to be right).

**Build/test mechanics — don't relearn these the hard way:**
- Build with `build_edgerepair.ps1`, never `build.ps1` (that one bumps
  ModernEdgeBlur's version, which the user explicitly does not want).
- That script bumps `BUILD_VERSION` in BOTH `src/EdgeRepair.h` AND
  `resources/EdgeRepairPiPL.r` together (see `Update-BuildVersion` in the
  script) — they drifted once already and produced a misleading AE
  "version mismatch. Code version is 1.0 and PiPL version is 1.0" error
  (fixed in commit `2076adf`). If that error ever reappears, check both
  files' BUILD_VERSION match before assuming it's stale AE cache.
- After building, copy `build/EdgeRepair.aex` to `dist/EdgeRepair.aex` and
  commit it (the `.gitignore` has a `!dist/*.aex` override for this).
- New AE-free algorithm code (pull-push, blend-weight math, etc.) goes in
  `src/common/MM_*.h` headers with NO PF/SDK types, so it can get a
  `tests/test_*.cpp` headless unit test via `tests/build_tests.ps1` (plain
  `cl.exe`, no AE SDK, no spaces-in-path issues since it copies into
  `%TEMP%` first) — the brief requires the pure algorithms be
  unit-testable without AE.
- Stop and report at each stage gate; wait for confirmation before moving
  to the next stage. Don't add parameters, view modes, or scope beyond
  what's in `docs/EdgeRepair.md` without asking first.
