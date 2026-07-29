# MM VFX Tools — Unified Repo Migration (handoff document)

**Started:** 2026-07-29
**Status:** GrainTransfer, MM_Qualify, MM_FreqSep migrated. **ModernEdgeBlur + EdgeRepair deferred** (active development in the old repo — see "Deferred migration" below for the turnkey procedure).

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

### Untracked build inputs carried by hand

Gitignored files the builds need, copied from the old folders after the
history merges (they are in no commit, by design):

| Project | Item | Why |
|---|---|---|
| MM_Qualify | `sdk/` (Headers/Util/Resources subset of AE SDK) | vendored, `CMakeLists` uses `${CMAKE_CURRENT_SOURCE_DIR}/sdk` |
| MM_Qualify | `resources/PiPLTool.exe` | PiPL resource compiler, required by build |
| MM_FreqSep | `sdk/`, `resources/PiPLTool.exe` | same |
| GrainTransfer | — nothing — | SDK referenced by **absolute path** `E:/OFX DEV/AE SDK/...` (cache var `AE_SDK_ROOT`), still valid from the new location |

Not carried: `build/`, `tests/out/`, GrainTransfer's `tests/ae_smoke_frame*.png`
(all regenerable), and MM_FreqSep's untracked `AE FS.code-workspace` (encodes
old paths; recreate a workspace at the suite root if wanted).

## Verification results (no-regress gate)

Recorded after building **from the new locations**:

<!-- VERIFICATION -->
_(pending — filled in below after builds/tests run)_

## Deferred migration: ModernEdgeBlur + EdgeRepair

Run this when the in-flight EdgeRepair GPU work reaches a commit point.
**Until then, all edge-tool work continues in `e:\OFX DEV\ModernEdgeBlur`.**

```bash
# 0. In the OLD repo: commit everything (git add -A; git commit) so the
#    migration captures it. Check `git status --ignored --short` for
#    build-required ignored files (sdk/ at minimum; its sdk has Headers+Util).

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

# 4. Copy untracked build inputs: sdk/ (and anything new in --ignored output)
# 5. Build BOTH plugins from the new location; run tests; compare .aex output
#    against the old repo's dist/ before switching development over.
# 6. Only then: stop working in the old folder (drop a MOVED marker in it).
```

Notes specific to this repo:

- `dist/` (including `EdgeRepair.aex`) **is tracked** there — it migrates with
  history; no hand-copy needed.
- Two plugins, one CMake project — migrate as a single `ModernEdgeBlur/`
  folder. Do not attempt to split them; they share `src/common/` and the
  README documents that Modern Edge Blur's code is never modified for Edge
  Repair's sake. The folder gets renamed together with the plugin rename,
  post-migration.

## Post-migration follow-ups (open items)

1. **GitHub remote** — `gh` CLI is not installed, so this is a manual step:
   create an empty repo on github.com (suggested: `MaybeRiaan/MM-VFX-Tools`,
   private), then:
   ```
   git remote add origin https://github.com/MaybeRiaan/MM-VFX-Tools.git
   git push -u origin main
   ```
2. **Rename Modern Edge Blur** — display name / menu strings / folder only;
   PiPL match name stays (see Decisions #3). Do after the deferred migration.
3. **Old local folders** — left in place as frozen backups with original
   hashes. Untracked `_MOVED.md` markers dropped in each migrated one so
   nobody accidentally resumes work there. Delete only after the suite has
   been building from here for a while.
4. **Parent repo hygiene** — `e:\OFX DEV` itself is a git repo (SkinRemapOFX).
   `MM VFX TOOLS/` was added to its `.gitignore` so the nested repos don't
   fight. The other project folders were already untracked there.

## Gotchas & discoveries (for future maintainers)

- **Two SDK strategies coexist.** GrainTransfer points at the full AE SDK by
  absolute path (`AE_SDK_ROOT` cache var, overridable with `-D`);
  MM_Qualify / MM_FreqSep / ModernEdgeBlur each vendor a trimmed, gitignored
  `sdk/` copy inside the project. Left as-is deliberately (no-regress rule).
  Unifying to one shared SDK reference is a possible later cleanup — touch it
  only with full rebuild + test verification per project.
- **`PiPLTool.exe` is gitignored but build-required** in MM_Qualify and
  MM_FreqSep. A fresh clone of this repo needs it (and `sdk/`) restored by
  hand — see the table above. Worth revisiting whether these should be
  tracked or fetched by a setup script.
- **Branch names are mixed** (`main` vs `master`) across the old repos; this
  repo standardizes on `main`.
- **Old repo names ≠ plugin names** in places: folder `GrainMatch` held the
  GrainTransfer plugin; remote `SkinRetouch` holds MM_FreqSep. The unified
  folders use the plugin names.
- Progress logs and design docs live inside each project's `docs/` (and
  README/Architecture/NEXT_STEPS files) and migrated with the history —
  e.g. the canonical FreqSep plan is `MM_FreqSep/docs/`, EdgeRepair's staged
  build log is `ModernEdgeBlur/docs/EdgeRepair.md` (after deferred migration).
