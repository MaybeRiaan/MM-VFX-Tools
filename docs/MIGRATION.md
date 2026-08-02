# MM VFX Tools — Unified Repo Migration (handoff document)

**Started:** 2026-07-29
**Status:** COMPLETE — all five plugins migrated. GrainTransfer, MM_Qualify,
MM_FreqSep on 2026-07-29; ModernEdgeBlur + EdgeRepair on 2026-08-02 (the
deferred migration, run once EdgeRepair's Stage 6 GPU work landed).

## Goal

Five After Effects plugins were built over several months in four independent
git repos, at different levels of completion. This repo unifies them into one
project structure **without regressing anything**:

- Code stays per-project (no shared/common code yet — deliberate).
- Every project's full commit history, progress logs, and discoveries migrate.
- Each project keeps its own CMakeLists, build.ps1, docs/, tests/ unchanged.

## Source inventory (state at migration time)

| Plugin | Old path | Old remote | Branch | Commits | New folder |
|---|---|---|---|---|---|
| Grain Transfer | `e:\OFX DEV\GrainMatch` | MaybeRiaan/GrainTransfer | main | 8 | `GrainTransfer/` |
| HSL Key (MM Qualify) | `e:\OFX DEV\HSL KEY\MM_Qualify` | MaybeRiaan/HSLQualifier | master | 9 | `MM_Qualify/` |
| FreqSep (MM FreqSep) | `e:\OFX DEV\AE FS\MM_FreqSep` | MaybeRiaan/SkinRetouch | master | 6 | `MM_FreqSep/` |
| Modern Edge Blur + Edge Repair | `e:\OFX DEV\ModernEdgeBlur` | MaybeRiaan/ModernEdgeBlur | main | 51+ | `ModernEdgeBlur/` (**deferred**) |

Notes captured at migration time:

- **GrainMatch** had uncommitted work (analysis cache keyed on render pixel
  format, v1.0.0.13). Committed in the source repo as `113bbcd` before
  migration, per owner decision.
- **ModernEdgeBlur** repo holds *two* independent plugins (Modern Edge Blur +
  Edge Repair) sharing one CMake project and `src/common/`. It had significant
  uncommitted EdgeRepair GPU work (`src/gpu/`, `EdgeRepair_GPU.cpp/.h`,
  DX/CUDA dist assets) that the owner is actively iterating on — migration of
  this repo is deferred until that work reaches a natural commit point.
- **MM_FreqSep** local `master` was 3 commits ahead of its old remote
  (SkinRetouch). Those commits are captured in this repo; the old remote was
  deliberately left as-is (owner decision: old repos untouched).

## Decisions (owner-confirmed 2026-07-29)

1. **History**: full history rewrite into subfolders via `git filter-repo
   --to-subdirectory-filter`, merged with `--allow-unrelated-histories`.
   Commit *messages, dates, authors, and diffs* are preserved and browsable at
   the new paths; commit *hashes change*. The old repos keep the original
   hashes — if you ever need to cross-reference an old hash, look it up there
   (messages match 1:1).
2. **ModernEdgeBlur + EdgeRepair**: deferred, owner actively working in the
   old repo. Do **not** start editing edge-tool code in this repo until the
   deferred migration below has been run.
3. **Modern Edge Blur rename**: happens *after* migration, as its own change.
   Constraint: the PiPL **match name is load-bearing** — existing AE projects
   reference effects by match name, so only the display name, menu strings,
   and folder may change. Same constraint applies to every plugin here
   (see also GrainTransfer's locked names/disk IDs).
4. **Remotes**: new GitHub repo for this suite; the four old GitHub repos are
   left exactly as they were (not archived, not pushed to). They are the
   frozen historical record with original hashes.

## Repo layout

```
MM VFX TOOLS/
├── README.md            suite overview + status
├── docs/MIGRATION.md    this document
├── GrainTransfer/       self-contained project (was GrainMatch/)
├── MM_Qualify/          self-contained project
├── MM_FreqSep/          self-contained project
└── ModernEdgeBlur/      (arrives with the deferred migration)
```

Each project folder is byte-identical to its old repo's tree, including its
own `.gitignore` (still valid — gitignore patterns are directory-scoped).

## Procedure used (repeatable — this is also the deferred-migration recipe)

Requires `git-filter-repo` (`pip install --user git-filter-repo`, invoke as
`python -m git_filter_repo`). Run from Git Bash; adjust paths for PowerShell.

```bash
SCRATCH=/tmp/migrate   # any scratch dir

# 1. Fresh clone (filter-repo refuses to run on a non-fresh clone; the
#    original repo is never touched)
git clone --no-local "/e/OFX DEV/GrainMatch" "$SCRATCH/GrainTransfer"

# 2. Rewrite history so every commit lives under the target subfolder
cd "$SCRATCH/GrainTransfer"
python -m git_filter_repo --to-subdirectory-filter GrainTransfer

# 3. Merge into the unified repo
cd "/e/OFX DEV/MM VFX TOOLS"
git remote add mig "$SCRATCH/GrainTransfer"
git fetch mig
git merge mig/main --allow-unrelated-histories \
    -m "Merge GrainTransfer history (from MaybeRiaan/GrainTransfer)"
git remote remove mig
```

Repeated for `MM_Qualify` (branch `master`) and `MM_FreqSep` (branch
`master`). Branch names differ per source repo — check before merging.

### Untracked build inputs — superseded by SDK consolidation (2026-07-29)

At migration time, MM_Qualify and MM_FreqSep vendored a gitignored `sdk/`
subset plus `resources/PiPLTool.exe`, which were copied over by hand. Later
the same day both projects were switched to GrainTransfer's convention — the
shared SDK install referenced by the `AE_SDK_ROOT` CMake cache var (default
`E:/OFX DEV/AE SDK/AfterEffectsSDK_25.6_61_win/.../Examples`), with
`PiPLtool.exe` taken from the SDK's own `Resources/`. The vendored copies
were verified **byte-identical** to the full SDK before the switch, then
deleted; both projects clean-rebuilt and passed their full test suites
against the shared SDK. **A fresh clone now needs nothing restored by hand**
— just an SDK install (override path with `-DAE_SDK_ROOT=...` / the
`AE_SDK_ROOT` env var for the test runners).

Not carried from the old folders: `build/`, `tests/out/`, GrainTransfer's
`tests/ae_smoke_frame*.png` (all regenerable), and MM_FreqSep's untracked
`AE FS.code-workspace` (encodes old paths; recreate a workspace at the suite
root if wanted).

## Verification results (no-regress gate)

Recorded after building **from the new locations**:

All three projects built and tested from the new locations on 2026-07-29,
using each project's own `build.ps1` / test scripts, unmodified:

| Project | Build | Tests |
|---|---|---|
| GrainTransfer | ✅ `GrainTransfer.aex` v1.0.0.13 (built with `-NoBuildIncrement`) | ✅ all headless core gates (FFT, RPN, response, determinism, 6K perf) + real-footage smoke, 0 failures |
| MM_Qualify | ✅ `MM_Qualify.aex`, deployed to AE | ✅ 93/93 core math + 10/10 tiled-vs-untiled pipeline parity |
| MM_FreqSep | ✅ `MM_FreqSep.aex`, deployed to AE | ✅ 21/21 core (identity/guided/matte) + 27/27 GPU kernel parity |

One non-fatal note: GrainTransfer's post-build deploy could not overwrite the
`.aex` in the AE plugins folder because After Effects was open with the plugin
loaded (file lock). The build script treats this as non-fatal; the next build
with AE closed will deploy normally.

### ModernEdgeBlur + EdgeRepair (2026-08-02)

| Project | Build | Tests |
|---|---|---|
| ModernEdgeBlur + EdgeRepair | ✅ clean `build.ps1 -NoBuildIncrement -Clean` — both `.aex` targets, PiPL, fxc DirectX assets, nvcc CUDA PTX, deployed to AE | ✅ 7/7 headless test files, incl. `test_gpuparity` ("All parity checks passed") and `test_soak` |

**Content parity was proven at the git-object level, not just by eyeball:**
the migrated `ModernEdgeBlur/` subtree hash equals the old repo's root tree
hash exactly (`9617867`), and all 56 tracked blobs match 1:1. Note that a
plain `cmp` of the *working trees* reports ~32 files as differing — that is
`core.autocrlf=true` re-checking-out CRLF where the old working tree happened
to hold LF. The committed content is identical; `git status` is clean in both.
Compare blobs/trees, not working files, when verifying a migration here.

## Deferred migration: ModernEdgeBlur + EdgeRepair — RUN 2026-08-02

Gate condition (in-flight EdgeRepair GPU work at a commit point) was met:
old-repo HEAD `b6bf98a` "docs(GPU): generalize the CPU/GPU lockstep discipline
into the suite-wide doc", clean tree, in sync with origin, all six EdgeRepair
stages done. 59 commits rewritten and merged; suite went 29 → 89 commits.
**Edge-tool development now happens here, in `ModernEdgeBlur/`.**

This is the procedure as actually run:

```bash
# 0. In the OLD repo: commit everything (git add -A; git commit) so the
#    migration captures it. Check `git status --ignored --short` for
#    build-required ignored files.

# 1-3. Same recipe as above:
git clone --no-local "/e/OFX DEV/ModernEdgeBlur" "$SCRATCH/ModernEdgeBlur"
cd "$SCRATCH/ModernEdgeBlur"
python -m git_filter_repo --to-subdirectory-filter ModernEdgeBlur
cd "/e/OFX DEV/MM VFX TOOLS"
git remote add mig "$SCRATCH/ModernEdgeBlur"
git fetch mig
git merge mig/main --allow-unrelated-histories \
    -m "Merge ModernEdgeBlur + EdgeRepair history (from MaybeRiaan/ModernEdgeBlur)"
git remote remove mig

# 4. DONE — parity build first, consolidation second (deliberate order: a
#    byte-identical tree proves the MOVE, before any change proves itself).
#    The vendored sdk/ + resources/PiPLTool.exe were verified `diff -rq`
#    clean against the shared install, then copied to the new location.
# 5. DONE — build + tests green from the new location (see Verification).
# 6. DONE — development moved here.
```

Notes specific to this repo:

- `dist/` (including `EdgeRepair.aex`) **is tracked** there — it migrated with
  history; no hand-copy needed.
- Two plugins, one CMake project — migrated as a single `ModernEdgeBlur/`
  folder, as planned. The eventual split is now a *post*-migration option, not
  a migration concern: `src/common/` turns out to be included **only** by
  `EdgeRepair.cpp` (`ModernEdgeBlur.cpp` pulls in nothing but its own header
  and the STL), so the two plugins share the CMake helper and the SDK glue —
  not code. Splitting is therefore mechanical; its real cost is duplicating
  `add_meb_plugin()`. Do it as its own change, after the rename.
- A `pre-dilation` tag came across with the history and now points into the
  rewritten commits.

## Post-migration follow-ups (open items)

1. ~~**GitHub remote**~~ — DONE: `origin` =
   `https://github.com/MaybeRiaan/MM-VFX-Tools.git`.
2. **Rename Modern Edge Blur** — display name / menu strings / folder only;
   PiPL match name stays (see Decisions #3). Unblocked now that the migration
   has landed; owner is doing this next, across the suite, ahead of release.
3. **SDK consolidation for ModernEdgeBlur** — still vendors `sdk/`; the AE
   subset is verified `diff -rq` clean against the shared install, so the
   switch to `AE_SDK_ROOT` is a `CMakeLists.txt:9` change. Caveat: `sdk/Premiere/`
   is the **Premiere** SDK, which has no shared install — it needs its own
   cache var or stays vendored. Low urgency: `EDGEREPAIR_PREMIERE_GPU` is OFF
   by default and AE never uses that module.
4. **Old local folders** — left in place as frozen backups with original
   hashes. Untracked `_MOVED.md` markers dropped in each migrated one so
   nobody accidentally resumes work there. Delete only after the suite has
   been building from here for a while. `ModernEdgeBlur/Specimen/` (an
   untracked third-party `Deep Glow.aex` kept for reference) was deliberately
   **not** carried over: nothing in the tree references it, and the PiPL-sweep
   investigation it served is recorded as a wrong turn in `AE_GPU_Effects.md`.
   It stays in the old folder if ever wanted.
5. **Parent repo hygiene** — `e:\OFX DEV` itself is a git repo (SkinRemapOFX).
   `MM VFX TOOLS/` was added to its `.gitignore` so the nested repos don't
   fight. The other project folders were already untracked there.

## Gotchas & discoveries (for future maintainers)

- **SDK consolidated 2026-07-29** (was: two coexisting strategies). All three
  migrated projects now reference one shared SDK install via the
  `AE_SDK_ROOT` cache var; the vendored `sdk/` copies were verified
  byte-identical to the full SDK, then removed. ModernEdgeBlur arrived
  2026-08-02 still vendoring its `sdk/` — consolidation pending, follow-up #3.
- **`PiPLTool.exe` was never a separate tool** — it is the SDK's own
  `Examples/Resources/PiPLtool.exe` (identical md5), so the builds now take
  it straight from `${AE_SDK_ROOT}/Resources/` and nothing is hand-copied.
- **Branch names are mixed** (`main` vs `master`) across the old repos; this
  repo standardizes on `main`.
- **Old repo names ≠ plugin names** in places: folder `GrainMatch` held the
  GrainTransfer plugin; remote `SkinRetouch` holds MM_FreqSep. The unified
  folders use the plugin names.
- Progress logs and design docs live inside each project's `docs/` (and
  README/Architecture/NEXT_STEPS files) and migrated with the history —
  e.g. the canonical FreqSep plan is `MM_FreqSep/docs/`, EdgeRepair's staged
  build log is `ModernEdgeBlur/docs/EdgeRepair.md` (after deferred migration).
