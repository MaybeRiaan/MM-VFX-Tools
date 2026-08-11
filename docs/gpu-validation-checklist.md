# In-AE GPU validation — quick checklist

**Scope:** MM_Qualify, Modern Edge Blur, MM_FreqSep — and Grain Transfer once
it is ported. The first three have a CUDA host behind kernels already proven
AE-free; what none of them has is a frame rendered by After Effects on the GPU.
That is the only step left, and it is hands-on.

Written 2026-08-04 against the test box (Win 10, RTX 3070 Ti, AE 2026 / 26.2,
Mercury GPU = CUDA). Contracts: [`AE_GPU_Effects.md`](AE_GPU_Effects.md) (host),
[`gpu-kernel-rules.md`](gpu-kernel-rules.md) (kernels).

---

## 0. Where things stand (verified 2026-08-04)

| | Built + deployed | AE-free tests | In-AE GPU |
|---|---|---|---|
| **MM_Qualify** ("Qualify") | ✅ .aex + PTX | ✅ 128/128, all GPU cases `0.000e+00` | ⏳ **ready to test** |
| **Modern Edge Blur** | ✅ .aex + PTX | ✅ 84 216 checks, 0 failures | ⏳ **ready to test** |
| **MM_FreqSep** ("Skin Retouch") | ✅ .aex + PTX | ✅ 27/27, all `0.000e+00` | ⏳ **ready to test** |
| **Grain Transfer** | ✅ .aex + PTX | ✅ 18/18 kernel parity `0.000e+00`, DSP gates pass | ⏳ **ready to test** |
| *EdgeRepair* (reference) | ✅ .aex + PTX | ✅ | ✅ **verified 2026-07-30** |

All five suite plugins build clean and every `CUDA_Assets/*.ptx` is deployed
next to its `.aex` in AE's own Plug-ins folder.

**Grain Transfer was ported 2026-08-04** and is the only plugin here that was
CUDA-only from its first GPU commit — it was written after §1.2 was measured,
so it was never aimed at DirectX. One kernel (`gt_stamp`, plus a trivial
`gt_copy` for passthrough), no tile planner, and its per-pixel math extracted
into `src/gt_core.h`, which the CPU path now compiles too. That refactor was
proven output-identical to the previous revision: all 15 deterministic test
renders byte-for-byte unchanged against `HEAD`, in both encodes, on real
footage.

**Its gate is the analysis cache, not a memory budget** — the one place in this
suite where PreRender legitimately withholds `GPU_RENDER_POSSIBLE`. See §4.

---

## 1. The four things AE requires — audited across the suite

Miss any one and the failure is **silent**: the effect renders on the CPU,
correctly, with no error anywhere. Audited 2026-08-04, all green:

| | GlobalSetup declares `SUPPORTS_GPU_RENDER_F32` | `GPU_DEVICE_SETUP` re-affirms `out_flags2` (§2.2) | PreRender raises the flag without demanding a known device (§2.3) | `SMART_RENDER_GPU` handled |
|---|---|---|---|---|
| MM_Qualify | ✅ | ✅ both hosts | ✅ `if (fits)` | ✅ |
| Modern Edge Blur | ✅ | ✅ | ✅ `if (gpuPossible)` | ✅ |
| MM_FreqSep | ✅ | ✅ both hosts | ✅ `if (fits)` | ✅ |
| EdgeRepair | ✅ | ✅ | ✅ `if (gpuPossible)` | ✅ |
| Grain Transfer | ✅ | ✅ | ✅ `if (cached)` | ✅ |

Re-run this audit after any GPU change — it is four greps and it is the
difference between a useful AE session and a wasted one.

**Grain Transfer's column 3 is the one entry that is not `if (it fits)`.** Read
it carefully before copying it anywhere: `cached` is the result of a key-only
compute-cache probe, i.e. a fact the host KNOWS about work already done. It is
not a device capability, and the gate never consults `what_gpu` or `gpu_data` —
doing so is the deadlock this column exists to prevent, not a stricter version
of it.

### The deadlock that column 3 exists to prevent

**MM_Qualify and MM_FreqSep both shipped a PreRender that raised
`GPU_RENDER_POSSIBLE` only when `FrameworkSupported(what_gpu) && gpu_data`.
That gate can never fire.** AE offers `what_gpu = NONE` with `gpu_data = null`
at every PreRender until it has *seen* the flag at least once, so the flag is
never raised and a device is never offered — it deadlocks itself shut. Both
were fixed on 2026-08-04, and it is worth naming because it does not look like
§2.2 at first glance and it survives an otherwise perfect CUDA port.

Not a theory. `EdgeRepair/src/EdgeRepair.cpp:859-867` — the only plugin AE had
ever GPU-rendered — records it verbatim:

> *"The original gate (what_gpu != NONE && gpu_data) never fired because AE
> offered what_gpu=NONE at every prerender"*

**The rule:** a PreRender gate may decline a frame it can *prove* does not fit
(run the same tile planner the render runs). It must never withhold the flag on
a frame it could not size — no framework offered yet, no `gpu_data`, no budget
reading, degenerate rect. Withholding out of ignorance is the bug.

---

## 2. One-time AE setup

1. **Project Settings ▸ Video Rendering and Effects ▸ Mercury GPU Acceleration
   (CUDA).** If the dropdown offers DirectX instead, that is Premiere's path —
   these hosts decline it (AE_GPU_Effects.md §1.1) and you will measure nothing.
2. **Project depth: 32-bit float.** The GPU path is F32-only; 8/16-bit projects
   route to the CPU by design.
3. **Restart AE** after any build — plugins and PTX are loaded once.
4. Delete stale logs so you are reading this session. **Six files — Grain
   Transfer writes two of them:**
   ```powershell
   Remove-Item "$env:TEMP\MM_Qualify_gpu.log","$env:TEMP\ModernEdgeBlur_gpu.log",`
               "$env:TEMP\MM_FreqSep_gpu.log","$env:TEMP\EdgeRepair_gpu.log",`
               "$env:TEMP\GrainTransfer_gpu.log","$env:TEMP\GrainTransfer_diag.log" -ErrorAction SilentlyContinue
   ```
   Every log then opens at `DEVICE_SETUP` (the diag log at `frame=0`), which is
   how you confirm you are reading this session and not an appended older one —
   these are opened `"a"`, and no line carries a timestamp.
5. **Lift the log caps — `MM_GPU_LOG_ALL=1`.** Every log line in all five
   plugins is budgeted per call site (`QK_GPU_LOG_FIRST(counter, limit, …)` and
   its four siblings), and the counters are **process** statics: PreRender caps
   at 24 and the SMART_RENDER line at 6 or 8. A few seconds of scrubbing at
   default params exhausts them, after which the log is *silent* — a sweep run
   later leaves no trace at all, and the absence of a line proves nothing.
   ```powershell
   setx MM_GPU_LOG_ALL 1     # once; then launch AE from a NEW shell/session
   ```
   Unset it (`setx MM_GPU_LOG_ALL 0`) for normal work — uncapped logging writes
   a line per frame and reopens the file each time.
6. Comp: a 4K+ float layer with real detail. 6K if you have it — that is the
   usage profile and the only size that exercises multi-band tiling.

---

## 3. The loop — same three steps for every plugin

### Step 1 — Read the log FIRST. Do not look at pixels.

The failure mode is **silent**: a declined device renders correctly on the CPU
with no error anywhere, so the frame tells you nothing (AE_GPU_Effects.md §5).

Apply the effect, scrub one frame, then:

```powershell
Get-Content "$env:TEMP\MM_Qualify_gpu.log" -Tail 20
```

What "working" looks like — all three lines must be present:

```
DEVICE_SETUP what_gpu=3 device_index=0
  -> OK (CUDA ready, N kernels, scratch budget ... MB)
PRERENDER what_gpu=3 gpu_data=0x... -> GPU_RENDER_POSSIBLE=YES
SMART_RENDER GPU what_gpu=... out=3840x2160 ...
```

`what_gpu`: `0 = NONE · 1 = OPENCL · 2 = METAL · 3 = CUDA · 4 = DIRECTX`.

Every plugin also logs the other outcome:

```
SMART_RENDER CPU src=... out=... off=(...) ...
```

**A CPU line is a fact, not an inference.** Before this existed you could only
guess at a fallback from a *missing* GPU line, which the caps in §2.5 made
worthless. Now the two lines interleave and you can read exactly which frames
went where — and with `MM_GPU_LOG_ALL=1`, for every frame rather than the
first handful. Some CPU lines are correct (§5); what matters is that they are
visible.

**Stop here if the log is wrong.** A/B testing a CPU render against a CPU
render is how this suite lost several test rounds, twice.

### Step 2 — A/B against Software Only

Render/note a frame under Mercury CUDA, then switch **Mercury GPU Acceleration
→ Software Only** and compare the same frame.

- **Target: max-abs ≤ 1e-5 per channel** (gpu-kernel-rules §3.6 — this covers
  fma contraction on real silicon; the AE-free suites already hold at exactly
  `0.0`, so anything larger is host plumbing, not math).
- Practical method: render both to EXR and difference them, or stack the two as
  layers with blend mode *Difference* and crank Levels. Eyeballing at display
  gamma will not resolve 1e-5.
- Sweep at least: default params, one heavy setting, and each plugin's isolating
  view (§4).

### Step 3 — Stress + MFR

- **6K + large radius.** Confirm it still renders. The PreRender budget gate may
  route pathological frames to the CPU — the log says
  `GPU_RENDER_POSSIBLE=no` when it does. That is by design, not a bug.
- **Render a range with Multi-Frame Rendering on.** A frame must look identical
  whether reached by scrubbing or by a batch render. This matters most for
  MM_Qualify (temporal IIR) — see §4.

---

## 4. Per-plugin specifics

| | Effect (Effect ▸ MM ▸ …) | Log | Isolating view / param |
|---|---|---|---|
| MM_Qualify | **Qualify** | `MM_Qualify_gpu.log` | Output popup: *Matte* |
| Modern Edge Blur | **Modern Edge Blur** | `ModernEdgeBlur_gpu.log` | Blur Amount / Shift Edge |
| MM_FreqSep | **Skin Retouch** | `MM_FreqSep_gpu.log` | View popup: *Low Band* / *High Band* |
| Grain Transfer | **Grain Transfer** | **two:** `GrainTransfer_gpu.log` (GPU verdict) + `GrainTransfer_diag.log` (per-frame cache state) | *Preview Grain Only* checkbox |

Grain Transfer is the one plugin whose verdict is split across two files. The
`DEVICE_SETUP` / `PRERENDER` / `SMART_RENDER` lines are in `_gpu.log`; the
`cache` / `fullRes` / `hasSource` / `canStamp` state that explains *why* the
gate is shut is in `_diag.log`. Read the second before blaming the first.

### MM_Qualify — "Qualify"
Sweep these; each lights a different kernel:
1. **Output ▸ Matte** with the gates pulling a real key — isolates `qk_matte`
   and the gate block away from compose.
2. **Denoise** > 0 — `qk_gauss_rgba`.
3. **Blur Radius** > 0, then again with **In/Out Ratio** — `qk_gauss_f1` and
   the ping-pong plane schedule.
4. **Garbage Matte** layer connected, each *Garbage Channel* — `qk_garbage`
   and its Fit mapping.
5. **Temporal Stabilize on, over moving footage** — `qk_iir`. *This is the one
   to check under MFR*: the state plane is per-render by construction, so
   scrubbed and batch-rendered frames must match. A mismatch here is the only
   plausible MFR bug in the plugin.
6. **Output ▸ Qualify Raw** — the compose-side recompute path.

Full deep protocol: [`../MM_Qualify/docs/testing.md`](../MM_Qualify/docs/testing.md) §2.3.

### Modern Edge Blur
1. **Blur Amount** alone, then **+ Shift Edge**, then **+ Blend Radius** — the
   three passes of the schedule, added one at a time.
2. Watch for `PRERENDER budget ... -> TOO BIG - CPU` in the log at 4.6K+: MEB's
   scratch is all-or-nothing (~641 MB at 4.6K), so that line is expected on a
   busy card and means the CPU took the frame correctly.
3. `GPU scratch grown to ... MB` should appear **once** per frame size, then
   never again — the block is grow-only and cached.

### MM_FreqSep — "Skin Retouch"
1. **View ▸ Low Band** and **View ▸ High Band** separately — this is the whole
   point of that popup: it isolates the separation/guided kernels away from
   compose, which makes a mismatch diagnosable instead of merely visible.
2. **Separation Radius** low, then high. Note `gpu-kernel-rules.md` §3.7: the
   guided box pass is O(r), so at σ≈90 (r≈110) the GPU can land *slower* than
   the CPU. Expected, documented, and a perf issue — not a correctness one.
3. **Matte Layer** connected, each *Matte Fit* mode — `fs_matte`.

Full protocol: `MM_FreqSep/docs/phase6-validation.md`.

### Grain Transfer  🚧 *steps written in advance — see the [port brief](../GrainTransfer/GPU_Port_Starter_Brief.md)*

This one needs a different first question from the other three, because its
GPU path is gated on the analysis cache rather than on a memory budget.

1. **Analysis must be cached before the GPU can run at all.** Apply the effect,
   set a Grain Source layer, and let the **first** frame render at full
   resolution — that frame pays for the FFT analysis on the CPU and populates
   the compute cache. Only from the second frame on can PreRender raise
   `GPU_RENDER_POSSIBLE`. Expect the log to show a CPU frame, then GPU frames.
   *If every frame stays on the CPU, the cache gate is the first suspect, not
   §2.2.* Before blaming the gate, check `hasSource` and `fullRes` in
   `_diag.log`: `hasSource=0` means no Grain Source layer is connected, so
   analysis can never run and `analysis_cached=no` is the gate behaving
   correctly. Measured 2026-08-10 — 24 declines, all of them this.
2. **Change the Grain Source, the Seed, or the project bit depth** — each
   changes the cache key, so the next frame must drop back to CPU for one
   frame and then return to GPU. That round trip is the gate working; a
   permanent CPU fallback after a source change is the bug to look for.
3. **Reduced-resolution previews are CPU by design.** Analysis is full-res
   gated (at half res AE aliases the grain away and the measurement would
   read content, not grain). Half/quarter preview rendering on the CPU is
   correct, not a failure.
4. **A/B with *Preview Grain Only* ticked** — it renders mid-grey plus the
   modulated grain field, opaque. That is the isolating view: it shows exactly
   the field being added, with no plate underneath to mask a difference.
5. **Determinism is the real test here.** Grain is re-randomized per frame from
   `(seed, frame)`, so GPU and CPU must produce the *same* field for the same
   frame number. Render the same frame both ways and difference them —
   ≤ 1e-5. Then scrub back and forth: a frame must not change when revisited.
6. **Amount 0 must be an exact passthrough** on both paths, and **alpha must be
   untouched everywhere** (check a layer with a soft alpha edge — zero-alpha
   pixels pass through by contract).
7. Sweep both encodes: a Rec.709/sRGB project (*Display*) and a scene-linear /
   ACES project (*LogFromLinear*), including superwhite values. The log path
   round-trips through `LinToLog`/`LogToLin` and is where an `exp2`/`log2`
   mismatch between backends would show.

---

## 5. Log decoder — what each failure line means

| Line | Cause | Fix |
|---|---|---|
| `what_gpu=0` at every PRERENDER | The gate is shut | §1 above, then AE_GPU_Effects.md §2.2 (the `out_flags2` re-affirm in `GPU_DEVICE_SETUP` — assignment, not OR) |
| `PTX missing/unreadable` | `CUDA_Assets/` did not travel next to the `.aex`, or the build machine had no CUDA toolkit | Rebuild; check the Plug-ins folder |
| `cuModuleGetFunction(...) FAILED` | Deployed PTX is stale against the binary — the classic half-finished deploy | Rebuild + redeploy. The backend declines wholesale rather than failing mid-render |
| `nvcuda.dll unavailable` | Non-NVIDIA machine or no driver | Expected; CPU renders |
| `AE provided no CUcontext` | AE handed a CUDA device with no context | Driver/AE issue, not ours |
| `AllocateDeviceMemory(...) FAILED` | Card genuinely out of memory | Lower radius / resolution |
| `GPU_RENDER_POSSIBLE=no` with a real `need`/`budget` | Budget gate declined a frame it *proved* does not fit | Working as designed — CPU renders it |
| PRERENDER says YES but no `SMART_RENDER GPU` | AE chose the CPU for that frame anyway | Normal for thumbnails and heavy draft downsamples; try full res. Confirm with the `SMART_RENDER CPU` line rather than inferring it from silence |
| The log simply stops mid-session | A per-site cap was reached (§2.5) | `MM_GPU_LOG_ALL=1` and relaunch AE. Nothing after that point was recorded, so re-run the sweep — do not read the silence as a result |

---

## 6. If the log looks right but you distrust the A/B

A passthrough milestone proves nothing — output is bit-identical whether or not
the GPU ran, and this suite wasted several rounds on exactly that, twice
(AE_GPU_Effects.md §5.2). To prove the GPU is really producing the pixels, flip
the destructive marker on and rebuild:

- `MM_Qualify/src/gpu/qk_gpu_cuda.cpp` → `#define QK_GPU_VISIBLE_MARKER 1`
- `MM_FreqSep/src/gpu/mm_gpu_cuda.cpp` → `#define FS_GPU_VISIBLE_MARKER 1`

The top eighth of the frame must go **transparent** (zeroing BGRA zeroes alpha)
under Mercury CUDA and must come back under Software Only. Set it back to `0`
afterwards.

---

## 7. AE-free baselines — rerun any time, no GPU needed

Build everything, then run every suite — one pass, before any AE session:

```powershell
.\build.ps1                                   # whole suite, builds + deploys all 5

MM_Qualify\tests\run_tests.ps1                # 93 core + 23 kernel + 12 pipeline
MM_FreqSep\tests\run_tests.ps1                # 27
ModernEdgeBlur\tests\build_tests.ps1          # 84 216 checks
GrainTransfer\tests\build_tests.ps1           # DSP gates (FFT, RPN, response)
```

Or as one line that stops on the first failure:

```powershell
.\build.ps1; foreach ($t in @(
    "MM_Qualify\tests\run_tests.ps1",
    "MM_FreqSep\tests\run_tests.ps1",
    "ModernEdgeBlur\tests\build_tests.ps1",
    "GrainTransfer\tests\build_tests.ps1")) {
  & ".\$t"; if ($LASTEXITCODE -ne 0) { throw "FAILED: $t" } }
Write-Host "all suites green" -ForegroundColor Green
```

These are the regression gate and they cover everything except the CUDA driver
layer itself. Nothing in AE is worth investigating until they are green.

**Coverage caveat, worth knowing before you trust a green run:** Grain
Transfer's suite gates its *DSP* (FFT round-trip, RPN character/ghosting/seams,
the response law, determinism) and knows nothing about a GPU path — there is
none yet. Closing that is step 2 of its port brief, and until it is done a
green Grain Transfer run says nothing about GPU parity.
