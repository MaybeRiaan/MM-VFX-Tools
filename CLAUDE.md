# Working notes for Claude Code

Context that is **not** derivable from the code, the git history, or the docs —
current priorities, standing decisions, and the traps that have cost real time.
Everything factual lives in the docs this file points at; nothing here restates
them.

This file is checked in so it travels with the repo. Any machine that clones it
gets this context automatically.

---

## Current priority (as of 2026-08-31)

**Get all five plugins running on macOS.** Everything else is on hold.

"Running on macOS" means **loads in AE and renders correctly on the CPU path** —
it does **not** mean Metal. The CPU pipeline is the reference every GPU parity
number in this suite is measured against, so a Mac CPU render is the correct
render; Metal is a speed upgrade to add later, to plugins that already work. Do
not treat the Metal port as a prerequisite for Mac support.

Groundwork is in (commit `bd46240`): Mac targets for all five, PiPL fixes,
`Info.plist`s, GPU decline stubs. **None of it has been compiled on a Mac.**
Start at [docs/MAC_BUILD.md](docs/MAC_BUILD.md) — §5 lists the likely first-build
failures in the order they will hit.

**Grain Transfer is the only plugin with live development work.** The other four
are considered done for now. Their remaining backlog items are real and are
recorded in their own docs, but do not open work on them unasked:

- Modern Edge Blur — 8/16bpc re-verify, Shift Edge method dropdown
- MM_FreqSep — chunked sliding-window guided box, IIR CPU Gaussian
- EdgeRepair — bisect the unconditional `GPU_RENDER_POSSIBLE`, GPU soak test

---

## Standing decisions

**Modern Edge Blur's render pipeline is settled.** Its v1.3 structure is the
result of a long debugging campaign (the three-cause black-halo fix and the 4x
perf restructure, both documented in `ModernEdgeBlur/docs/NEXT_STEPS.md`) and is
user-verified in AE. Additive work is fine — a new backend, new files, new
tests. Restructuring `ModernEdgeBlur.cpp`'s phase order, threading, blocking or
buffer pooling is not: raise it as a question with the blast radius spelled out
before touching it.

**Look parameters get judged in AE before they get built.** When a control
governs *how something looks* rather than whether it is correct, do not
implement a full re-derivation from an analysis of the math. Land the
unambiguous correctness fix, then put the subjective part in front of the user —
ideally as a choice with concrete numbers — before writing the lockstep
CPU/GPU implementation. Precedent: EdgeRepair's Softness was re-anchored three
times across v1.0.0.37–.38, each version defensible from the distance-field
algebra and each built with full tests and docs, and all three were reverted in
.39. The genuine bug found in the same pass (Extend measured from the eroded
core rather than the alpha edge) was never in question and was kept. The tell is
feedback phrased as feel rather than as behaviour.

**GPU direction is CUDA-only on Windows, EdgeRepair is the reference.** The DX12
hosts in MM_FreqSep and MM_Qualify are parked, not deleted — they serve
Premiere, and their kernels are bit-exact. See the README's GPU-direction note
and `docs/AE_GPU_Effects.md` §1.1–1.2.

---

## Verification traps

**AE's frame cache defeats same-settings A/B tests.** Two *Save Frame As* of the
same frame, each preceded by *Purge All Memory & Disk Cache*, can produce **zero**
plugin renders — the log does not grow by a line. Both files then come from one
cached render and difference to exactly `0.000e+00`, which reads as a flawless
pass and is indistinguishable from a real result. **Count the `SMART_RENDER`
lines in the log before trusting any comparison.** A purge is not sufficient;
only an AE restart reliably invalidates. Where a genuine differential is needed,
change something the cache keys on — switching the Mercury GPU setting reliably
forces the other backend to render. Corollary: an apparent "no speed difference"
between two configurations usually means neither pass actually rendered.

**Several plugins have parameter sweeps that prove nothing.** Modern Edge Blur is
an exact identity on a fully opaque source; MM_FreqSep's Result view is an
identity at default params. `docs/gpu-validation-checklist.md` §4 documents each
one — read the relevant entry before designing a test, not after.

---

## Where the authoritative records live

| | |
|---|---|
| Per-plugin method, parameters, status | that plugin's `docs/` and `README.md` |
| AE GPU host contract | `docs/AE_GPU_Effects.md` |
| Kernel authoring contract | `docs/gpu-kernel-rules.md` |
| In-AE GPU validation, per-plugin traps | `docs/gpu-validation-checklist.md` (§0 = live status) |
| Row-by-row validation evidence | `docs/gpu-validation-tracker.xlsx`, via `tools/gpu-validation/read-tracker.ps1` and `write-tracker.ps1` — the only live copy |
| macOS build | `docs/MAC_BUILD.md` |
| Grain Transfer's open blocker | `GrainTransfer/docs/GrainTransfer.md`, *Cross-channel colour-grain fidelity* and *Luminance response* |

A user-facing documentation mirror exists in Notion (User + Developer sets under
a master "MM VFX Tools" page). **The repo is the ground truth**; the Notion
pages are a mirror to refresh when specs change, and they currently lag.

---

## Grain Transfer — the live work

The cross-channel and response-curve rework landed in `b38f42a` (v1.0.0.18).
Read the two doc sections above rather than re-deriving any of it.

**The open blocker:** the spectrum measurement is still content-contaminated.
`MeasureCrossPairs` and the response curve both gained content-robustness;
`MeasureGrainSpectrumRes` did not — it still takes large patches filtered only
by overall sigma and plain-averages their power. Real footage shows the
consequence: content residual is coarser and more directional than grain, so
measured grain comes out too coarse, too directional and too strong. The
proposed fix (not built) is gradient-based patch rejection on anisotropy and
non-stationarity, optionally a trimmed per-bin mean.

**This was paused deliberately** to source a wider, more varied set of reference
footage and formats first — that coverage gap is considered the bigger problem.
Do not resume the fix against the existing three clips alone.

Method that works and should be reused: source / denoised / GT triples give
ground truth, since `source − denoised` is the real grain, and measuring the
denoised plate alone isolates content contamination independently of how well
the denoiser worked. Watch the confound — Neat Video left 47%/87%/77% of the
grain in three "denoised" plates, so grain was being added on top of grain.

Also relevant: Resolve's film grain is a **match target**, not merely a
synthetic proxy — footage arriving for work often carries it, so reproducing it
is a product requirement, and a CG-plate test set (clean CG + Resolve grain,
where `grained − clean` is exact ground truth with no denoiser in the middle) is
both the controlled diagnostic and a real use case.

**GPU validation has not run** (16 in-AE G-rows; G-01 is blocked on a missing
Grain Source layer, which is a precondition, not a gate bug). The tile bank is a
firewall — the analysis rework changes how the bank is *filled*, and kernel
parity held 18/18 exact throughout — so only G-02 (Preview Grain Only) needs its
reference imagery re-taken. Grain Transfer's PreRender gate is the analysis
cache, not a memory budget, and its verdict splits across two logs:
`GrainTransfer_gpu.log` and `GrainTransfer_diag.log`.
