# Test automation — plan of record

**Status as of 2026-11-09:** Layer 0 (provenance) is built, release-grade and
wired into all five plugins; identity is verified end-to-end, per-render
attribution exists for Grain Transfer only. Everything below Layer 0 is
planned, not built, and Layer 1 is waiting on the asset bank.

## The goal

The user specifies intent, parameter semantics, and what "correct" means.
Claude runs the tedious matrix, reports results with numbers, and proposes
fixes. Human eyes enter at roughly 90% confidence — reviewing a report and a
short list of flagged frames rather than driving AE by hand.

This document records how that is meant to work, what it deliberately does not
attempt, and why.

---

## 1. Division of labour

### Mechanizable — no human in the loop

| Area | Where it runs | Existing basis |
|---|---|---|
| CPU/GPU parity | offline | `*/tests/*gpuparity*`, 18/18 exact on GT |
| Bit-depth consistency (8/16/32bpc) | AE | checklist rows |
| Regression vs stored references | both | `tools/gpu-validation/exr-diff.ps1` |
| Structural health (loads, renders, finite, no leak) | both | `GrainTransfer/tests/test_realfootage.cpp` |
| Performance | offline | `EdgeRepair/tests/perf_pipeline.cpp`, `run_perf.ps1` |
| Host integration (effect present, params wired, smart-render taken) | AE | `GrainTransfer/tests/ae_smoke.jsx` |
| **Grain character where ground truth exists** | offline | see §4 |

That last row is the underrated one. On a clean CG plate plus the same plate
with Resolve film grain, `grained − clean` is *exact* ground truth with no
denoiser in the middle. "Does the measured spectrum match the real grain" then
stops being a matter of taste and becomes a residual with a number on it. That
is the single biggest lever for moving work out of the human column, and it is
why the CG test set matters more than its "synthetic proxy" framing suggests.

### Not mechanizable — stays with the user

- **Look calls.** The standing decision in `CLAUDE.md` holds unchanged: land
  the unambiguous correctness fix, then put the subjective part in front of the
  user before building the lockstep CPU/GPU implementation. The EdgeRepair
  Softness episode — three defensible geometries, each fully built and tested,
  all three reverted — is what this rule exists to prevent. An automated loop
  that cannot tell a correctness fix from a look change will reproduce that
  waste *faster*. The harness therefore needs an explicit stop-and-ask branch.
- **Whether a result holds up across varied real material.**

### Claude's actual visual capability — stated precisely

Claude can view rendered PNGs. This reliably catches gross failure: black
frames, inversion, seams, tiling, obvious directionality, halos. It is a useful
first-pass filter that raises where the 90% line sits. It is **not** a
substitute for a trained eye on subtle artefacts or on grain character. Treat
it as the thing that removes embarrassing cases before the user looks.

---

## 2. Layer 0 — provenance (BUILT)

Everything else depends on this, because without it an automated report is
*worse than no report*: it carries authority it has not earned.

Two documented traps produce a PASS indistinguishable from a real result:

1. **Stale binary.** AE holds a file lock on a loaded `.aex`, so a build that
   ran while AE was open compiles fine and silently does not install (the
   `mm_publish` note in `CMakeLists.txt` records this as observed behaviour).
   The version quad in the log is then correct and the binary is still old.
2. **Cache-served render.** AE can answer a *Save Frame As* entirely from its
   frame cache, so an A/B can pass with **zero** plugin renders and difference
   to exactly `0.000e+00`.

Neither is visible from the log alone, and no stamp derived from the *source*
can catch either one. The fix identifies the binary AE actually **loaded**.

### `common/mm_build_id.h`

Header-only, dependency-free, self-contained, wired into all five plugins. At
first call it locates the module containing its own code (`GetModuleHandleEx` on
Windows, `dladdr` on macOS), stats it, and derives a 32-bit id from
path + size + mtime.

**It serves two roles, and the second is why it ships.** The suite is intended
for release, so provenance is not only a test aid: when a user reports a
problem, the first question is which binary they ran, and one request — send me
the build log from your temp folder — answers it for every plugin at once,
without trusting a version number read off a UI.

Deliberate choices:

- **Unconditional.** `mmbuild::Announce()` is *not* behind any plugin's
  temporary diagnostic switch (`GT_DIAG_LOG`, `ER_PERF_LOG`). It writes exactly
  one line per plugin load, guarded by a once-flag that resets on reload — which
  is precisely the right granularity.
- **Self-contained.** The header owns its own temp-dir resolution and log
  writer, so a plugin needs no logging infrastructure of its own for
  provenance. That is what makes the per-plugin wiring two lines regardless of
  whether the plugin has a diag logger.
- **One shared log**, `<temp>/MM_VFX_Tools_build.log`, rather than five. A
  single support request then returns every plugin the user has loaded. The log
  self-caps at 256 KB (thousands of loads) and rewrites rather than growing.
- **Silent on every failure.** Provenance logging must never be able to affect
  a render.
- **Not a content hash.** A content hash would have to be reproduced
  bit-exactly on the harness side. Instead the `[build]` line reports the real
  loaded *path*, and the harness hashes that path itself with `Get-FileHash`.
  The id's only job is internal consistency between lines in one log.
- **Included by relative path** (`../../common/mm_build_id.h`), so every
  project still builds standalone from its own folder with no CMake change —
  the suite's standing contract.
- **An anchor function, not a variable**, so `/OPT:ICF` and `-dead_strip`
  cannot fold the address away.
- **Magic-static init**, because AE renders on many threads and the first
  caller may be a render thread rather than `GlobalSetup`.

**Wiring trap, cost real time on 2026-11-09:** "insert after the last
`#include`" is wrong. EdgeRepair's leading include block is followed by more
includes *inside* `#if ER_PERF_LOG` (which is `0`), so the include landed in
dead code and the call failed to compile. Any future rollout must check that
the include sits at conditional depth zero.

Log format, identical across all five plugins so one parser reads them all:

```
[build] plugin=GrainTransfer version=1.0.0.18 id=a1b2c3d4 time=2026-11-09T21:54:34 size=127488 mtime=1794254074 path=<the loaded plugin, absolute>
[render] id=a1b2c3d4 frame=0 cache=MISS depth=32bpc ...
```

`path` goes last and unquoted: AE plugin paths contain spaces, so last position
makes the harness regex "everything after `path=`" rather than a quoting
problem.

### `tools/verify-build-id.ps1`

Reads two files: `<temp>/MM_VFX_Tools_build.log` for identity (filtered to the
plugin asked about, most recent load) and `<temp>/<Plugin>_diag.log` for render
counting. Turns both traps into assertions:

- a `[build]` line exists for this plugin, else it never loaded — or the loaded
  binary predates stamping, which is itself a stale-binary finding
- the diag log's last load id equals the build log's, else the diag log is stale
- render count >= `-MinRenders`, else the imagery was probably cache-served
- every `[render]` id equals the `[build]` id, else the run spans two binaries
- the file at the loaded path still matches the logged size
- with `-ExpectAex`, SHA256 of the loaded file equals SHA256 of the binary just
  built — **this is the check that catches the file-lock trap**

Field parsing is `name=value` and tolerant, so fields can be added to the
`[build]` line later without breaking the reader. `-Reset` clears the logs
before a run. Exit 0 means the result is attributable.

**It never passes an assertion it could not make.** With no diag log and
`-MinRenders > 0` it fails and says why, rather than reporting green on an
unverified render count. That rule is the difference between this being
provenance and being decoration.

Self-tested 2026-11-09 against synthetic logs (happy path, zero renders, hash
mismatch, two binaries in one run, no build line) and end-to-end against a real
log written by a compiled probe calling `Announce` (identity match, identity
mismatch, unverifiable render count, once-guard). The zero-render case exposed
a real bug — `Get-Content` on a one-line file returns a bare string, so indexing
walked characters — which would have broken the single most important case.
`@()` around those reads is load-bearing.

### Where it is wired — all five plugins

| Plugin | `Announce` in `GlobalSetup` | per-render `id=` |
|---|---|---|
| GrainTransfer | yes | yes — `[render]`, uncapped |
| EdgeRepair | yes | not yet |
| ModernEdgeBlur | yes | not yet |
| MM_FreqSep | yes | not yet |
| MM_Qualify | yes | not yet |

Identity (trap 1, and the release support path) is complete for all five.
Per-render attribution (trap 2) exists only where a plugin already has an
uncapped per-render log line; Grain Transfer is the only one so far. The
verifier reports render count as `n/a` for the others and **fails rather than
passes** if asked to assert a count it cannot make.

Grain Transfer additionally repeats the `[build]` line into
`GrainTransfer_diag.log` and `GrainTransfer_gpu.log`, because the GPU verdict
splits across those two files and each must be self-identifying when read
alone.

**Finding worth recording:** the GPU log rate-limits with
`GT_GPU_LOG_FIRST(counter, limit, ...)` — `SMART_RENDER` lines cap at 8. So
*counting renders in the GPU log is unreliable past 8 frames*. Per-render
counting must use the diag log's uncapped `[render]` line. The GPU `[build]`
line is intentionally uncapped (`GT_GPU_LOGF`), because a device re-setup is
exactly when provenance matters.

---

## 3. Layer 1 — offline corpus harness

All five plugins already have AE-free test harnesses, varying in depth:
EdgeRepair is deepest (parity, soak, perf), ModernEdgeBlur thinnest (parity
only). `GrainTransfer/tests/build_tests.ps1` compiles with plain `cl.exe` and
no AE SDK, and `test_realfootage.cpp` already runs the full analyze/bake/stamp
chain on real EXRs.

What is missing is not architecture but output discipline:

- machine-readable per-case results (JSON/CSV), not PNG dumps for human review
- corpus-driven iteration over a footage manifest rather than hardcoded clips
- run-to-run diffing so a regression is a line in a report, not a noticed detail

**This is where the Grain Transfer blocker gets solved** — see §4. It needs
zero AE involvement and iterates in seconds.

### Assertions need preconditions

`docs/gpu-validation-checklist.md` §4 documents parameter sweeps that prove
nothing: Modern Edge Blur is an exact identity on a fully opaque source,
MM_FreqSep's Result view is an identity at default params. These must be
encoded as **guards**, so a test that cannot fail reports "not meaningful here"
rather than green.

This is the main way the whole plan fails while appearing to succeed: Claude
generates volume, and volume reads as confidence.

---

## 4. The Grain Transfer blocker, as an automation problem

Open issue (unchanged, see `GrainTransfer/docs/GrainTransfer.md`):
`MeasureGrainSpectrumRes` is still content-contaminated — it takes large
patches filtered only by overall sigma and plain-averages their power. Content
residual is coarser and more directional than grain, so measured grain comes
out too coarse, too directional and too strong.

Proposed fix, not built: gradient-based patch rejection on anisotropy and
non-stationarity, optionally a trimmed per-bin mean.

Work was **paused deliberately** to source wider and more varied reference
footage first; that coverage gap is considered the bigger problem, and the fix
is not to be resumed against the existing three clips alone.

Why this is an offline problem, not an AE one:

- `source − denoised` is the real grain, so triples give ground truth
- measuring the denoised plate alone isolates content contamination
  independently of denoiser quality
- the CG route is exact: clean CG + Resolve grain means `grained − clean` has no
  denoiser in the middle at all

Resolve's film grain is a **match target**, not merely a synthetic proxy —
footage arriving for work often carries it, so reproducing it is a product
requirement as well as the controlled diagnostic.

### The two corpora have different jobs (user decision, 2026-11-09)

Earlier framing treated the Neat Video residual — 47% / 87% / 77% of the grain
left in three "denoised" plates — purely as a confound to be eliminated. That
is only half right. **Neat will never do a perfect job, and it is the most
realistic use case**, so a denoised plate that still contains grain is a
condition Grain Transfer has to be *robust to*, not merely an artefact of a bad
test set. The two corpora therefore measure different things and neither
replaces the other:

| Corpus | Role | Question it answers |
|---|---|---|
| Clean CG + Resolve grain | **ground truth** | is the measurement correct? |
| Source / Neat-denoised / GT triples | **realistic usage** | does it hold up when the "clean" plate isn't? |

A pass requires both: correct against exact ground truth, and not fooled by
residual grain in a real-world denoised plate. Reporting them separately also
means a regression says *which* of the two broke.

### Acceptance criterion: AE's Match Grain (user decision, 2026-11-09)

The reference point is **to match or beat the accuracy of AE's own Match Grain**
(`ADBE Match Grain`). This is a better target than an absolute tolerance because
it is relative to something both parties can see, and it is the honest
competitive bar — Match Grain does the same job.

Mechanically, per test case: apply Match Grain to the clean plate with the
grained plate as its noise source, render, store that output as a reference.
Then require `error(GrainTransfer) <= error(MatchGrain)` against ground truth.

**The metric cannot be pixel-wise.** Grain is stochastic, so no method can
reproduce a specific grain realisation, and a pixel difference would score both
Grain Transfer and Match Grain as hopeless while saying nothing. The comparison
has to be statistical — and conveniently, the right statistics are exactly the
three quantities Grain Transfer already measures:

1. power spectrum (`MeasureGrainSpectrumRes` — the thing under repair)
2. sigma vs luminance, i.e. the response curve
3. cross-channel correlation (`MeasureCrossPairs`)

So the harness measures those three on the ground-truth residual, on Grain
Transfer's output residual, and on Match Grain's output residual, and compares.

**Consequence for the plan:** this pulls Layer 2 earlier than originally
ordered. Match Grain is an AE effect, so producing its reference renders needs
the AE driver — though only once per case, after which the references are
stored and iteration returns to being fully offline. To confirm: that Match
Grain still ships in AE 2026 and renders acceptably headless.

---

## 5. Layer 2 — the AE case driver

### Bridge

`afterfx.exe -r <full path to script.jsx>` runs a script **in the
already-running instance**; `afterfx -s "<code>"` runs inline code. That is the
entire transport — no CEP panel, no polling bootstrap, no `command.json`.

### Design: automate the restart, do not avoid it

An AE restart is the only cache invalidation `CLAUDE.md` calls reliable, and
60 seconds is only expensive while it is *attended*. The loop is therefore
`app.quit()` -> build -> relaunch -> `-r` the case script. Unattended, a
40-case matrix overnight does not care about a minute.

This also composes with the one step that may not script cleanly: the Mercury
GPU preference key is unconfirmed, but since a relaunch is already in the loop,
editing the prefs file between launches is a legitimate way to force the other
backend to render.

### Ceiling to design around

**ExtendScript has no pixel access.** The driver's job is "drive and capture",
never "measure". All numeric comparison happens outside AE on written files,
which is what `tools/gpu-validation/exr-diff.ps1` already does.

### Starting point and settled decisions (user, 2026-11-09)

`GrainTransfer/tests/ae_smoke.jsx` already applies the effect by match name
(`"MM Grain Transfer"`), sets Grain Source to a layer index, and writes frames
with `comp.saveFrameToPng`. It is most of the driver already. Generalising it
to a JSON case spec is the work.

- **No existing AE project for the G-rows; the driver generates comps from
  script.** This is the better answer anyway: reproducible, diffable, and
  immune to project-file drift.
- **G-01's missing Grain Source is synthesized to start**, pivoting to real
  source footage if the synthetic plate proves to be hiding something.
  `ae_smoke.jsx` already demonstrates the wiring.
- **Hard-killing AE is accepted.** AE needs Task Manager often enough that the
  driver should treat it as normal, not exceptional: timeout, kill, mark the
  case failed, relaunch. The residual risk is modal dialogs (see §8), not the
  kill itself.

### On third-party MCP servers

Three community AE MCP servers exist (HeroicSwan, Dakkshin, Tortoisepig). All
are built for motion-graphics authoring — create text layers, build edits — not
plugin QA, and all use a panel/polling bridge that `afterfx -r` makes
unnecessary. HeroicSwan's is the most complete and its `ae_run_jsx` escape
hatch can technically do anything, but the needed verb set here is small and
specific: open project, set a param by match name and index, purge, save a
frame, tail a log, kill and relaunch. A purpose-built server also lets the
verification discipline in §2 live *in code* rather than in habit.

---

## 6. Layer 3 — the run report

The artifact the user actually reads: what ran, the numbers, what changed since
the last run, and a short list of frames needing human eyes **with the reason
each was flagged**.

**Results must be files, not conversation.** Durable per-case JSON means any
session can resume and report, a long run does not depend on one context
surviving, and Claude is replaceable between sessions rather than a bottleneck.

---

## 7. Rejected: the proxy-shell hot reload

A shell plugin that `LoadLibrary`s the real binary from a separate folder, to
avoid AE restarts. Technically real; rejected for this suite.

- **The PiPL stays the shell's.** AE reads effect name, match name, version and
  every `PF_OutFlag*` / `PF_OutFlag2*` at scan time and re-checks at
  `GLOBAL_SETUP`. The shell must declare a superset and the real binary can
  never change any of it without a restart. Commit `c8a688c` fixed
  GrainTransfer's macOS PiPL claiming GPU flags the code did not set — a shell
  makes that class of mismatch permanent and invisible, because the flags under
  iteration are no longer in the file being rebuilt.
- **The param list freezes.** `PARAMS_SETUP` runs once per applied instance and
  AE stores values in the project by index. Swapping in a binary with a changed
  param list leaves the existing instance *misread, not errored*.
- **GPU lifecycle is the real blocker.** `GPU_DEVICE_SETUP` / `SETDOWN` are
  issued once per device at load. The CUDA context, loaded modules and device
  allocations all live in the DLL to be unloaded. Correct behaviour requires
  synthesising setdown-before-unload and setup-after, caching
  `PF_GPUDeviceSuite` device info by hand, every reload. For a suite whose GPU
  path *is the thing under validation*, that means trusting a bespoke
  device-lifecycle emulator inside the test rig.
- **Sequence data.** Flat data written by the old build is reinterpreted by the
  new one — silent corruption on any layout change without a forced
  setdown/resetup.
- **Two implementations**, `LoadLibrary` and `dlopen` with a different bundle
  layout, and macOS is a live target.
- **Decisive:** it adds "stale binary" to the same failure class as the cache
  trap, and fails the same way — silently, looking like success. Days of work
  whose primary risk is the thing that has already cost the most time.

### The Cache On Load suite — real, but a different problem

`PF_SetNoCacheOnLoad()` in `AE_CacheOnLoadSuite.h` is genuine, called from
`GLOBAL_SETUP`. It is a **Premiere Pro** mechanism: Premiere caches PiPL and
`GLOBAL_SETUP` results in the Windows registry / macOS plist to speed boot, and
this opts out. Adobe's own documentation states plainly that plugins cannot be
reloaded without restarting the host, and that the Shift-on-launch force-reload
applies to the *next* launch.

It saves nothing on an AE restart. It is still worth ~5 lines in MM_FreqSep and
MM_Qualify if the parked DX12 Premiere path matters, because a stale Premiere
plugin cache is exactly what makes a `c8a688c`-class PiPL/code mismatch
invisible.

---

## 8. Practical hazards for unattended runs

- **Modal dialogs block scripting** — missing footage, "save changes?", and
  especially the crash-recovery dialog after a hard kill. Since hard kills are
  an accepted part of the loop (§5), the post-kill recovery dialog is the
  single most likely cause of a dead overnight run. Clean `app.quit()` where
  possible, timeout-kill-and-mark-failed where not, and the recovery dialog
  needs an empirically tested suppression — clearing AE's auto-save/recovery
  state before relaunch is the likely route, but it must be *tested*, not
  assumed.
- **Permission prompts.** Builds and `afterfx.exe` invocations need
  allowlisting, or a matrix run stalls waiting for a human. The
  `/fewer-permission-prompts` skill derives this from transcripts.
- **Backslash mangling.** Heredoc-based editing through the Bash tool eats
  backslashes; C escape sequences in patch patterns must be constructed
  (`chr(92)`) or avoided. Cost real time on 2026-11-09.

---

## 9. Order of work

1. ~~Build-ID stamping, Grain Transfer~~ — done
2. ~~Roll identity stamping to all five plugins~~ — done, release-grade
3. Per-render `id=` for the other four (needs an uncapped per-render log line
   in each; only Grain Transfer has one today)
4. Corpus runner + machine-readable metrics in `GrainTransfer/tests`, built
   around the three statistics in §4 — **waiting on the asset bank**
5. Minimal AE driver, early, to batch-render the Match Grain references (§4)
6. Attack the spectrum blocker offline against ground truth and the Match
   Grain bar
7. Full AE case driver for the G-rows
8. Run report

Step 5 moved up from last position because the acceptance criterion is relative
to an AE effect, so the baseline cannot be produced offline. Everything after
it returns to the offline loop.

This ordering serves the standing priority (finish Grain Transfer, then Metal)
and the harness is also what the Metal port will want — a third backend that
must match the CPU reference is precisely a parity-matrix problem.
