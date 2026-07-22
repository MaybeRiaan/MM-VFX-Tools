# Handoff prompt — View-mode deliverables pass

**Run this in a FRESH session, and ONLY AFTER Stage 5 (CPU performance) is
merged to `origin/main`.** It edits `src/EdgeRepair.h` / `src/EdgeRepair.cpp`
— the same files Stage 5 refactors — so running it concurrently would
conflict across the two PCs this project is built on.

---

Continuing the Edge Repair plugin (After Effects "MM" suite) in this repo.
**Do not modify Modern Edge Blur's plugin code** (reference only). This work
spans two PCs — **`git fetch origin` and reconcile BEFORE touching
anything.** Then confirm Stage 5 is already merged to `origin/main`
(`git log --oneline -15`, look for the Stage 5 / CPU-performance commits). If
Stage 5 is NOT in yet, STOP and tell me — this task must sit on top of it,
not race it.

**Read `docs/EdgeRepair.md` first** (locked spec + staged plan). The
parameter set is LOCKED at five; disk IDs 101-105 are permanent. **This task
adds no parameters, no new popup items, and does not reorder or renumber
anything** — it changes view-mode *labels* and *render semantics/quality*
only.

## What the user wants

Promote the diagnostic View Mode entries into real, deliverable-quality
outputs, and rename the default for clarity. Popup string is `ER_VIEW_ITEMS`
in `src/EdgeRepair.h` (currently `"Result|Core Matte|Distance Field|Extended
Colour"`; order and disk IDs LOCKED). Per-view render code is the
`ER_*ViewRow` / `ER_ResultRow` functions in `src/EdgeRepair.cpp`.

### Per view

1. **Rename "Result" → "Composited".** Change ONLY the label text in
   `ER_VIEW_ITEMS`. The enum `ER_VIEW_RESULT`, its value (=1), and its disk
   ID stay identical so existing `.aep` projects still resolve to the same
   view. Grep for other user-facing "Result" strings and update for
   consistency; leave code identifiers alone. **The Composited render output
   itself must NOT change** — same math, same alpha (original layer alpha,
   passthrough except Alpha Choke). Label-only.

2. **Extended Colour — first-class clean-plate deliverable. Fully opaque
   (alpha = 1) by design** — edge-extend has no transparency; it is the
   filled colour source an artist mattes themselves downstream. It already
   renders opaque filled RGB (`ER_ExtViewRow` sets `dst[0]=1.f`), so the work
   is QUALITY, not plumbing: verify it is the clean unpremultiplied
   straight-RGB pull–push fill, colour-correct at 8/16/32bpc, no
   blockiness/bleed, genuinely usable as a render output. Decide + document
   the empty-core fallback (currently black).

3. **Distance Field — first-class MATTE deliverable. Fully opaque (alpha = 1)
   across the whole frame**, the normalized distance encoded as luminance so
   the view can be used directly as a **luma/track matte for downstream edge
   work (light wraps, etc.).** The softness of the matte IS the distance
   falloff — no separate blur needed. Currently: 0 at the core edge → black,
   brightening outward, white at `Extend` px (`invMaxDisplay = 1/Extend`,
   fallback 64px when Extend≈0). Two things to settle at the gate, do not
   guess:
   - **Polarity.** For a light-wrap matte the artist usually wants WHITE at
     the alpha/core edge fading OUT to black over the band — i.e. possibly
     the inverse of the current 0-at-core mapping. Ask the user which
     polarity they want (or expose both only if they explicitly ask — that
     would be a param and needs sign-off).
   - **White-point mapping.** Confirm "white at `Extend` px" is the right,
     predictable reach (it ties the matte width to the Extend slider, which
     is probably desirable), and document it. Make the normalization stable
     and identical across 8/16/32bpc.

4. **Core Matte — leave AS-IS, diagnostic only.** The user treats the layer's
   own alpha as the real matte. Do NOT delete it (disk IDs are permanent);
   just don't invest in it.

## Constraints

- **No new parameters.** If anything seems to need one (e.g. Distance Field
  polarity as a toggle), STOP and ask — it is a brief amendment, not a
  unilateral add.
- Keep the headless `tests/` green.
- The **Composited** output must be byte-identical to before (label only).
- Build with `build_edgerepair.ps1` (NEVER `build.ps1`). It bumps
  `BUILD_VERSION` in BOTH `src/EdgeRepair.h` AND `resources/EdgeRepairPiPL.r`
  together — drift makes AE cry "version mismatch."
- **Close After Effects before building**, or the deploy-copy to the plugins
  folder fails (the `.aex` is file-locked; compile/link still succeed and
  `dist/` still updates, but the AE folder won't refresh).
- After building, copy `build/EdgeRepair.aex` → `dist/EdgeRepair.aex` and
  commit it (`.gitignore` has `!dist/*.aex`).
- Update `docs/EdgeRepair.md`: the spec table's View Mode row (new label
  "Composited") and a short "View modes" section recording that **Extended
  Colour** (opaque clean plate) and **Distance Field** (opaque luma matte)
  are now supported real-world deliverables, with the Distance Field polarity
  + white-point decision documented.
- **Stop at a visual gate** and let the user confirm each view in AE before
  committing as final. Do not proceed to any further scope without asking.
