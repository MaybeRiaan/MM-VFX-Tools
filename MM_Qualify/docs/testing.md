# MM Qualify — Testing Process

Two layers, in order of authority — the same discipline as MM FreqSep
(`../docs/gpu-kernel-rules.md` §5). Automated tests are the regression gate;
the in-AE checklist is a development-time confidence pass that needs a live
AE host and GPU.

---

## Layer 1 — Automated, AE-free (the regression gate)

```powershell
cd tests
.\run_tests.ps1      # exit 0 = all pass
```

Compiles and runs three suites against the vendored SDK headers (for the
`A_long`/`PF` typedefs only) — no AE host, no GPU device, **no CUDA toolkit**,
no Mac required.

### `core_test.cpp` — shared per-pixel math (`src/qk_core.h`)
95 assertions over the exact functions the CPU render **and** the GPU
kernels compile. Focus = the plan's stated risks (§6):

- **Hue wraparound across the 0°/360° seam**, including `In > Out` ranges
  (reds), full-circle (`In == Out`), and softness trapezoids that straddle
  the seam. This is risk #1 in the plan; it has dedicated coverage.
- Linear gates (sat/luma): hard bounds, per-side softness, reversed-bound
  robustness, HDR bounds above 1.0.
- `RGB→H/S` for HSV and HSL, negatives clamped to finite output.
- Qualify-In transforms: monotonic and finite across `[-0.5, 8]` for As-Is /
  Gamma 2.4 / Log / ACEScct; ACEScct 0.18→0.413 (Resolve's log midtone).
- Clean/Clip identity at neutral settings + endpoint behavior; In/Out Ratio
  identity + shrink/grow with pinned endpoints.
- Temporal IIR: asymmetric attack/release, one-step magnitude, convergence,
  warm start.
- Output-mode compose: keyed = premult·matte, matte/inverted/raw views,
  highlight (grey when unselected, image when selected).

### `kernel_test.cpp` — the GPU kernel BODIES
23 cases, one or more per entry point. This does not *transcribe* the kernels,
it **compiles them**: `src/gpu/cuda/qk_kernel_args.h` pulls the real `.kernel`
files through `src/gpu/kernels/qk_prologue_host.h` — the same vocabulary
`qk_prologue_cuda.h` defines with CUDA spellings — so
`qkk_<entry>::Args{...}.run(x, y)` executes the shipped kernel source on the
CPU. A hand transcription can rot silently against the kernel it mirrors; this
cannot, because it *is* the kernel.

Covers all nine entry points: `qk_load_tile` / `qk_store_tile` (BGRA↔ARGB
swizzle with a row pitch deliberately wider than the width — deriving pitch
from width is the classic AE-GPU-world bug), `qk_fill_ones`, both Gaussian
passes against the real `blur.cpp` (including a support wider than the plane,
where the skip-don't-clamp rule of `../../docs/gpu-kernel-rules.md` §3.2 is the
only thing holding parity), `qk_matte`, `qk_garbage` against the real
`qk::SampleGarbage` under a non-identity Fit mapping on all four channels,
`qk_iir` seeded from a deliberately **poisoned** state plane, and `qk_compose`
in all five output modes.

Tolerance is **exact (0.0)**, not an epsilon: same source, same arithmetic,
same order. The ≤ 1e-5 in `gpu-kernel-rules.md` §3.6 is for real silicon,
where fma contraction legitimately differs; it has no business here.

### `pipeline_test.cpp` — tiled-vs-untiled parity
12 end-to-end cases. The whole chain (denoise → gates+cleanup → temporal IIR →
matte blur → ratio+compose) is run two ways on the same synthetic multi-frame
input:

- an **untiled** CPU reference — the real `qk::GaussianBlurARGB`,
  `qk::GaussianBlurF1`, `qk::SampleGarbage` and `qk_core.h`, whole frame at
  once; and
- the **tiled GPU pipeline**: the **real kernels** (as above), driven by the
  **real** tile planner (`qkgpu::QK_PlanTiles`), in the exact pass sequence and
  plane schedule `qk_gpu_cuda.cpp` and `mm_gpu_dx.cpp` enqueue — BGRA world
  load/store through a wide pitch, the denoise A→T→Q plane roles, history
  frames streamed oldest-first through one IIR state plane (pre-filled with
  garbage), and the matte-blur ping-pong that keeps `in != out` on every pass.

Cases sweep denoise/blur/temporal/garbage on-off, single-axis blur on **each**
axis (the ping-pong picks a different destination plane in each case, which is
where an aliasing bug would hide), garbage under a non-identity Fit mapping,
all five output modes, an interior + world-edge band split, and HDR
(superwhite) input.

**Current result: 128/128 pass — 93 core + 23 kernel + 12 pipeline. Every GPU
case at `max-abs = 0.000e+00`.** Together, `kernel_test` and `pipeline_test`
close the loop `../../docs/AE_GPU_Effects.md` §4.3 asks for: the per-pixel
formulas *and* their composition. Consequence: band margins, offsets, compose
addressing, the swizzle, the matte stages, the garbage mapping and every
kernel body are *proven* before AE runs — so an in-AE mismatch is backend
plumbing (module load, bindings, launch config) by elimination.

Known gap (accepted, same as MM FreqSep): layer 1 does not exercise the CUDA
driver / D3D12 API layers themselves — that needs a live AE-hosted device
(layer 2).

---

## Layer 2 — In-AE, hands-on (development-time)

Build + deploy first:

```powershell
.\build.ps1          # configure + build + deploy .aex to AE 2026 plugins
```

Restart After Effects. The effect appears as **Effect ▸ MM ▸ Qualify**.

### 2.0 — Smoke test (loads + passthrough)
1. New comp, any footage layer (an 8-bit still is fine to start).
2. Apply **MM Qualify**. Default Output is *Selection Overlay*; with default
   gates (Hue Width 360 full circle, Sat/Luma open) everything is selected,
   so the overlay shows **the full image at true colour** (nothing dimmed).
   If AE rejects the plugin on load, the outflags literals in
   `resources/MM_QualifyPiPL.r` disagree with `GlobalSetup()` — they are
   documented to match (`0x06000442` / `0x2A001400`).

### 2.1 — Gates pull a key (the Phase 1 milestone)
Footage with a clear color region (sky, skin, a colored product).
1. Leave Output on **Selection Overlay** (default) — selected pixels stay full
   colour, unselected desaturate + dim, so you see the selection live without
   leaving the view. (Output ▸ Matte shows the raw matte if you prefer.)
2. **Eyedrop the "Sample" swatch** on the target colour. The three gates
   recentre on it and that colour pops in the overlay.

   **SAMPLING CAVEAT (current build).** AE's eyedropper samples the *composited
   effect output* — i.e. the overlay, not the original pixels. For the first
   **Sample** pick that's usually fine (the whole frame is selected, so the
   overlay is true colour). But for **Sample +** / **Sample −**, picking a new
   colour out of an already-dimmed region reads the *dimmed* value and mis-keys.
   Until the custom-UI eyedropper lands (which will read the true underlying
   pixel by click position), use the reliable workflow: **double-click the
   layer to open the Layer panel and eyedrop there** — the Layer panel reads
   the raw input *before* effects, so add/subtract picks are always correct.
3. Use **Sample +** to click a neighboring shade — the selection should *grow*
   to include it. **Sample −** should carve a sampled colour back out (a
   full-circle hue gate becomes "everything except" that neighborhood).
4. Toggle each gate's **Enable**; drag **Hue Center / Hue Width / Soft In /
   Soft Out** (and the Sat/Luma **In / Out / Soft In / Soft Out**) by hand —
   soft edges should feather, not step.
5. **Hue seam check:** find a red/magenta subject and set **Hue Center ≈ 0°**
   with a moderate **Width**. The red should key cleanly with no hole at 0°.
   Animate Hue Center from −30° through +30° across a colour sweep — the range
   must track *through* red, not swing backward (the seam-free win).
6. **Keyframe a gate** (the headline differentiator): set a keyframe on Hue
   Center/Width or any Sat/Luma bound, move in time, change it — it animates.
   Confirm an expression can drive it too.

### 2.2 — Matte finesse holds a moving key (the Phase 2 milestone)
Moving footage where a raw key sizzles. Chain order: **Denoise → Blur Radius →
In/Out Ratio → Black/White Clip** (+ Temporal). NOTE most finesse controls
reshape the matte's *soft ramp*, so pull a key first (they do nothing on a flat
all-selected matte).
1. **Denoise** (pre-filter) up a little — matte edge noise should calm at the
   source, while the *image* stays sharp (it is untouched; only the
   qualification path is blurred).
2. **Blur Radius** to soften the matte edge, then **In/Out Ratio** to choke
   (negative) or spread (positive) that softened edge. Ratio needs the ramp
   Blur creates — with Blur at 0 it has little to move.
3. **Black Clip / White Clip** to harden the endpoints: Black Clip lifts the
   transparent floor to solid 0, White Clip pulls the opaque ceiling to 1.
4. **Temporal Stabilize** on: Attack/Release ungrey. Scrub — the matte should
   sizzle less frame-to-frame. Raise **Release Smoothing** for a stubborn edge
   letting go; **Attack** governs a new selection appearing. (Requires
   multi-frame footage; on a still it is a no-op by design.)
5. **Qualify Raw** output shows the matte *before* cleanup — flip between it
   and *Matte* to see what each finesse control bought you.
6. **Garbage Matte:** drop a mask/layer into *Garbage Layer* — the key should
   be confined to it (intersected before cleanup). Try each *Garbage Channel*.

### 2.3 — GPU vs CPU parity (the important one)

**Step 0, before looking at a single pixel: read the log.** The GPU failure
mode is *silent* — a declined device renders correctly on the CPU with no error
anywhere, so the frame tells you nothing (`../../docs/AE_GPU_Effects.md` §5).
Apply the effect, scrub a frame, then open:

```
%TEMP%\MM_Qualify_gpu.log
```

What you want to see:

```
[MM_Qualify] DEVICE_SETUP what_gpu=3 device_index=0
  -> OK (CUDA ready, 9 kernels, scratch budget ... MB)
[MM_Qualify] PRERENDER what_gpu=3 gpu_data=... out=WxH need=...MB budget=...MB -> GPU_RENDER_POSSIBLE=YES
[MM_Qualify] SMART_RENDER GPU what_gpu=CUDA src=... bands=... scratch=...MB
```

`what_gpu`: `0 = NONE, 1 = OPENCL, 2 = METAL, 3 = CUDA, 4 = DIRECTX`.

- **`what_gpu=0` at every PreRender ⇒ the gate is shut.** Go to
  `AE_GPU_Effects.md` §2.2 first — the re-affirm in `GPU_DEVICE_SETUP` is the
  one that has cost this suite the most.
- **`DEVICE_SETUP` says "PTX missing/unreadable"** ⇒ `CUDA_Assets/` did not
  travel next to the `.aex`, or the build machine had no CUDA toolkit.
- **`cuModuleGetFunction(...) FAILED`** ⇒ the deployed PTX is stale against the
  binary. Rebuild; the backend declines rather than failing mid-render.
- **No `SMART_RENDER GPU` line at all** while PreRender says YES ⇒ AE chose the
  CPU for that frame anyway (normal for thumbnails and heavy draft downsamples).

Then the A/B:

1. Project Settings ▸ Video Rendering and Effects ▸ **Mercury GPU
   Acceleration (CUDA)**. Note render; then switch to **Software Only** and
   compare the same frame across several parameter states (a keyed result, a
   *Matte* view, denoise+blur+temporal all on).
2. Target **max-abs ≤ 1e-5** per channel — the real-silicon tolerance of
   `gpu-kernel-rules.md` §3.6, which covers fma contraction. Layer 1 already
   proves every kernel body and the whole tiled composition bit-exact, so a
   mismatch here points at the CUDA host (`src/gpu/qk_gpu_cuda.cpp`) — module
   load, launch config, plane offsets — not the algorithm.
3. **6K+ / large-radius stress** (the usage profile): a 6K comp with Denoise
   and Blur Radius high. Confirm it renders (the PreRender budget gate may
   route pathological tiles to the CPU — slower, never wrong; that is by
   design, not a bug; the log says `-> GPU_RENDER_POSSIBLE=no` when it does).
4. **Temporal Stabilize on, multi-frame**: scrub *and* render a range with
   Multi-Frame Rendering enabled. The IIR state plane is per-render by
   construction (see `qk_gpu_cuda.cpp`'s header), so a frame must look the same
   whether reached by scrubbing or by a batch render.

> If AE renders on the CPU no matter what, note that **DirectX is not the AE
> path on this hardware** — AE offers CUDA and never probes DIRECTX
> (`AE_GPU_Effects.md` §1.1). `src/gpu/mm_gpu_dx.cpp` is kept and kept
> building for Premiere; it is not what AE calls.

### 2.4 — Depth + alpha
- Run 8 / 16 / 32-bit (float) project depths. Float should carry HDR
  (superwhite) luma into the Luma gate (bounds settable to 8.0). 8/16-bit
  clamp only on the final pack.
- Straight vs premultiplied source alpha: the key should track the visible
  color; feathered layer edges must not fringe into the selection (the
  denoise + gate path renormalizes by blurred alpha).

---

## What "pass" means at each phase (from the plan)

- **Phase 1:** pulls keys as confidently as Resolve's qualifier on the same
  footage — *"works" parity, not pixel matching.*
- **Phase 2:** a moving-footage key (skin, sky, product color) that holds
  without sizzle.
- **Phase 3:** 6K float perf on a mid GPU, GPU==CPU, presets, docs.

Layer 1 is green now. Layer 2 §2.0–§2.4 is the manual pass to run in AE 2026.
