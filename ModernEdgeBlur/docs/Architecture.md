# Modern Edge Blur — Architecture

## Overview

A single After Effects effect (`ModernEdgeBlur.aex`, match-name `MM Modern Edge Blur`) that applies a soft Gaussian blur to the edges of a keyed/masked layer while preserving the sharp interior. Replacement for Red Giant's discontinued Edge Blur plugin.

Built from scratch 2026-06-04. All prior EdgeExtend/EdgeRepair/meb_common code has been removed.

## Pipeline (per frame)

```
Input (premultiplied RGBA, preserve_rgb_of_zero_alpha = FALSE)
  │
  ├─[Shift Edge = 0, skip] Morphological grow/shrink on alpha (stubbed, wired up in v2)
  │
  ├─[Blur Amount = 0, skip]
  │   Gaussian blur full RGBA → blurred base layer (A)
  │   Via AE's built-in convolve (in_data->utils->gaussian_kernel + convolve)
  │   Fallback: hand-rolled separable Gaussian (sigma = radius * 0.25)
  │
  ├─[sharp_buf] Original unpacked pixels (B)
  │
  ├─[Blend Radius = 0, skip]
  │   Gaussian blur sharp alpha only → weight w
  │   w = clamp(blurred_alpha, 0, 1)  [linear, no pow curve]
  │   Hand-rolled separable kernel (sigma = radius, support = ceil(radius*3))
  │
  └─ Composite:
       out_rgb   = lerp(A_rgb, B_rgb, w)   [w=1 inside=sharp, w=0 edge=blurred]
       out_alpha = A_alpha                  [blurred alpha = soft feathered edge]
```

**Passthrough:** when both Blur Amount = 0 and Blend Radius = 0, input is copied directly to output. Held RGB (preserve_rgb_of_zero_alpha) is returned intact in passthrough mode only.

## Parameters

| Slider | Disk ID | Default | Range | Precision |
|--------|---------|---------|-------|-----------|
| Shift Edge | 1 | 0.0 | −50 → +50 | 0.01 |
| Blur Amount | 2 | 4.0 | 0 → 100 | 0.01 |
| Blend Radius | 3 | 4.0 | 0 → 100 | 0.01 |

Disk IDs are permanent — never reorder or reuse.

## Premultiplied alpha handling

- `preserve_rgb_of_zero_alpha = FALSE` on checkout — AE delivers honest `(0,0,0,0)` outside the matte.
- All processing operates on premultiplied data. Because outside-matte pixels are `(0,0,0,0)`, the Gaussian kernel naturally contributes zero colour from transparent pixels — no dark halo.
- Passthrough path (blur=0, blend=0) uses `preserve_rgb_of_zero_alpha = TRUE` implicitly — AE returns the original world untouched, held RGB preserved.

## Gaussian kernels

Two separate kernels, tuned independently:

**MakeBlurKernel** (Blur Amount backplate):
- Used only as fallback when AE's convolve is unavailable
- `sigma = radius * 0.25`, `half = ceil(radius) + 1`
- Gentle feel, matches AE's native blur character at equivalent values

**MakeBlendKernel** (Blend Radius alpha weight):
- Always used (alpha-only, no AE world needed)
- `sigma = radius`, `half = ceil(radius * 3)`
- Aggressive — needs to visibly erode the alpha edge band

## AE integration

**SmartRender / PreRender:**
- No input inflation — src_world is exactly output size. Border clamping at layer edges is invisible in practice (alpha is 0 at layer boundaries).
- `PF_OutFlag_USE_OUTPUT_EXTENT` set — AE manages render region.

**Bit depths:** 8, 16, 32 bpc via `UnpackRow` / `PackRow` dispatch. All internal processing in float32.

**MFR safety:** No mutable globals. All per-render state on the stack of SmartRender.

**Threading:** Blur Amount uses AE's `convolve` (internally threaded). Blend Radius alpha blur is serial — fast enough at typical values, threading via `iterate_generic` is the v2 upgrade path.

## Version / PiPL sync rules

`MAJOR_VERSION`, `MINOR_VERSION`, `BUG_VERSION`, `STAGE_VERSION`, `BUILD_VERSION` must match between:
- `src/ModernEdgeBlur.h`
- `resources/ModernEdgeBlurPiPL.r`

`STAGE_VERSION`: `PF_Stage_DEVELOP = 0`. PiPL literal must be `0`. Mismatch → AE error 8800b.

`AE_Effect_Global_OutFlags` / `AE_Effect_Global_OutFlags_2` in the PiPL must match the bitmasks set by `GlobalSetup` at runtime. Current values:
- `out_flags  = 0x02000440` (DEEP_COLOR_AWARE | PIX_INDEPENDENT | USE_OUTPUT_EXTENT)
- `out_flags2 = 0x2A001400` (FLOAT_COLOR_AWARE | SUPPORTS_SMART_RENDER | SUPPORTS_THREADED_RENDERING | SUPPORTS_GPU_RENDER_F32 | SUPPORTS_DIRECTX_RENDERING)

The PiPL declares the **superset**: `GlobalSetup` only ORs the two GPU bits in
for non-Premiere hosts, and AE tolerates the PiPL being wider than the runtime
flags. It does not tolerate the reverse — any narrower and AE rejects the plugin
at load with *"global outflags mismatch"* (25::16).

## GPU path (added 2026-08-03)

The same pipeline, pass for pass, on CUDA. After Effects picks per frame; when
it picks the CPU, everything above runs unchanged. **The CPU pipeline is
untouched by the port** — same phases, same threading, same buffer pool, same
numbers.

### Why CUDA only

AE 2026 on Windows/NVIDIA hands effects `what_gpu = CUDA` and never probes
DirectX, even with `SUPPORTS_DIRECTX_RENDERING` declared — measured on this box
2026-07-23 while bringing up Edge Repair. See [`AE_GPU_Effects.md`](../../docs/AE_GPU_Effects.md)
§1.1–1.2, which makes CUDA the backend this suite maintains. DirectX is
**declined** at `GPU_DEVICE_SETUP` rather than served a passthrough host: a
passthrough would render the effect as a silent no-op in Premiere, which is
worse than falling back to the correct CPU path.

Metal has no wrapper yet, but the shared headers carry its shim spellings so a
Mac build is a toolchain step rather than a port.

### Layout

```
src/core/MEB_CoreMath.h            CPU pixel math, AE-free  ← lifted out of ModernEdgeBlur.cpp
src/gpu/MEB_KernelMath.h           GPU formulas   (CUDA / HLSL / Metal / host C++)
src/gpu/MEB_KernelPasses.h         GPU pass bodies (CUDA / host C++; see note)
src/gpu/ModernEdgeBlurKernels.cu   CUDA entry points — 11 kernels, thin wrappers
src/gpu/ModernEdgeBlurKernels.hlsl HLSL passthrough entry — build-time type-check only
src/gpu/MEB_CudaShim.h             runtime nvcuda.dll driver surface (no toolkit needed)
src/ModernEdgeBlur_GPU.{h,cpp}     device lifecycle + the GPU pass sequence
tests/test_gpuparity.cpp           the gate: CoreMath == KernelMath, exact
```

`MEB_KernelPasses.h` is split from `MEB_KernelMath.h` because its functions take
`const float*` parameters, which HLSL has no equivalent for. Bringing DX12 or
Metal up means adding `MEB_RBUF`/`MEB_LOAD`/`MEB_STORE` macros to the one shim
block and respelling the buffer accesses — no math work.

The `.hlsl` file has no host behind it and its `.cso` is **not** deployed. It is
compiled every build because fxc type-checks the whole translation unit, so one
entry point validates the entire shared math header against HLSL's rules
(AE_GPU_Effects.md §4 item 5).

### Pass sequence

| Pass | Runs when | CPU twin |
|---|---|---|
| `MEB_ExtractAlpha` | always | the alpha-plane half of ingest |
| `MEB_Morph` | Shift Edge ≠ 0 | `ShiftEdgeAlpha` / `MorphPlane` |
| `MEB_Passthrough` | Blur = 0 and Blend = 0 | the passthrough pack |
| `MEB_Derive` | otherwise | `MEB_DeriveRowInline` |
| `MEB_BlurHRGBA` / `MEB_BlurVRGBA` | Blur Amount ≥ 0.5 | `GaussBlurRGBA` (+ unpremultiply) |
| `MEB_DirMin` ×4 / `MEB_CopyPlane` | Blend Radius ≥ 0.5 | `OctagonErode` |
| `MEB_BlurHPlane` / `MEB_BlurVPlane` | Blend Radius ≥ 0.5 | `GaussBlurPlane` |
| `MEB_Composite` | otherwise | `MEB_CompositeRow` |

No host-side readback anywhere: unlike Edge Repair, no pass here asks a question
between passes, so no device flags buffer is needed. Every branch is a slider
decision the host already knows.

### Two shape differences from the CPU path (both exact, neither visible)

1. **No `sharp_buf`.** The CPU materialises the sharp layer into a full-frame
   RGBA plane at ingest. The GPU recomputes it from the source world plus the
   alpha plane at the two points that consume it — same values, one fewer
   16-bytes-per-pixel plane resident.
2. **Ping-pong instead of in-place.** A dispatch cannot read and write one plane
   at once, so each blur pass reads one buffer and writes another. Tap order,
   clamped edge sampling and accumulation type are identical.

The one thing that *would* change numbers — the Gaussian weights — is not
recomputed on the device at all. `MakeBlurKernel` / `MakeBlendKernel` build them
on the host for both paths and the float array is uploaded verbatim
([`gpu-kernel-rules.md`](../../docs/gpu-kernel-rules.md) §3.1).

Note also that Modern Edge Blur **clamps** edge taps (the weight sum stays 1.0
at the border), which is the opposite of MM_FreqSep's skip-and-renormalise rule
in gpu-kernel-rules.md §3.2. That is this plugin's own convention and the GPU
mirrors it exactly — it is not a bug to be "fixed".

### Device memory

One `AllocateDeviceMemory` block per device, sub-divided, grow-only, cached
across frames, freed at `GPU_DEVICE_SETDOWN`. Only what the current parameters
need is reserved:

| Buffer | floats/px | Reserved when |
|---|---|---|
| `alpha` | 1 | always |
| `alphaShift` | 1 | Shift Edge ≠ 0 |
| `planeA` | 4 | anything but passthrough |
| `planeB` | 4 | Blur Amount ≥ 0.5 (the blur's ping-pong) |
| `weight` | 1 | anything but passthrough |
| `tmp` | 1 | Blend Radius ≥ 0.5 |

So **11 floats/px at the defaults** (Shift Edge 0, Blur and Blend both on), 12
with Shift Edge active:

| Frame | default | + Shift Edge | passthrough |
|---|---|---|---|
| HD 1920×1080 | 91 MB | 100 MB | 8 MB |
| UHD 3840×2160 | 365 MB | 398 MB | 33 MB |
| 4.6K 4608×3164 | 641 MB | 700 MB | 58 MB |

Same order as the CPU path's thread-local pool, and per *device* rather than per
render thread — so Multi-Frame Rendering does not multiply it. A mutex serialises
scratch growth and the whole enqueue sequence; AE's stream is FIFO, so
serialising enqueue serialises execution (AE_GPU_Effects.md §4).

#### The budget gate (PreRender)

If the allocation is refused inside `SMART_RENDER_GPU` the **frame fails** — the
worlds are already device memory, so there is no CPU to fall back to. So the
decision is taken in PreRender instead, where declining costs only speed:
`MEB_GPUBudgetAllows` sizes the frame and leaves `GPU_RENDER_POSSIBLE` unraised
when it will not fit. Infeasible frames render on the CPU: slower, never wrong.

- **One planner, both callers.** `MEB_PlanFrame` makes every host-side decision
  — which passes run, the Gaussian weights, the morphology geometry — and ends
  in the single `MEB_BuildLayout` call. PreRender's gate and `MEB_RenderCUDA`
  both go through it, so the size approved and the size allocated cannot drift.
- **Worst case on both axes.** The gate sizes the full output *request*
  (the render's world is clipped into it) against the *currently held* scratch
  (grow-only, so it can only be larger by render time).
- **Already-held scratch always passes.** After the first frame of a given size
  the render allocates nothing, so no budget can refuse it — a card whose free
  memory AE's cache has since eaten keeps GPU-rendering rather than silently
  reverting to the CPU.
- **Budget = ¼ of FREE VRAM** (`cuMemGetInfo`), floored at 256 MB while ≥512 MB
  is actually free, **no upper clamp**, 384 MB if the query is unavailable.

Two deliberate deviations from MM_FreqSep's version of this gate
(`../../docs/gpu-kernel-rules.md` §6), both because its budget feeds a *tile
planner* — where the number only picks a band height — while here it is a hard
feasibility threshold:

1. **No 1 GB ceiling.** Harmless there; here it would mean "never GPU-render
   above ~5K", on any card, forever — and 8K is what this gate is for.
2. **Free VRAM, re-measured per gated PreRender**, not a fraction of dedicated
   VRAM cached at device setup. AE's frame cache is cold exactly when a device
   is set up, which is the least representative moment to measure.

The ¼ fraction itself is inherited rather than measured — see
[NEXT_STEPS.md](NEXT_STEPS.md) Priority 5 for the in-AE measurement that settles
it, and the log lines that take it.

### Validation

- **Automated gate:** `tests/build_tests.ps1` compiles both math headers as host
  C++ and diffs them with `==` — per-formula, per-pass, and over the whole
  multi-pass schedule for 13 parameter combinations × 4 source images. Run it
  after **any** change to a formula in either header. A formula changes in both,
  in the same commit, or not at all.
- **In AE:** log check first (`%TEMP%\ModernEdgeBlur_gpu.log`, `MEB_GPU_LOG`) —
  `what_gpu=3` at PreRender and `SMART_RENDER GPU` entries mean the gate is
  open; `what_gpu=0` at every PreRender means it is shut, and
  AE_GPU_Effects.md §2.2 is the first place to look. `PRERENDER budget … TOO BIG
  - CPU` means the gate is open but this frame was deliberately sent to the CPU.
  Then A/B the same frame
  against Mercury → *Software Only*. Target max-abs ≤ 1e-5 per channel: fma
  contraction on real silicon lands well inside that.
- **Not yet done:** the in-AE A/B. Everything above it is code-complete and
  gated AE-free.

### The six things that make AE dispatch at all

All six are in place; missing any one fails **silently** (the effect renders on
the CPU, correctly, with no error):

1. `GlobalSetup` declares `SUPPORTS_GPU_RENDER_F32 | SUPPORTS_DIRECTX_RENDERING`
   for non-Premiere hosts.
2. `GPU_DEVICE_SETUP` **re-affirms** `out_flags2 = SUPPORTS_GPU_RENDER_F32` —
   a plain assignment, not an OR. This is the one that cost Edge Repair seven
   failed experiments.
3. `PreRender` raises `GPU_RENDER_POSSIBLE`. Unconditionally except for frames
   the budget gate can *prove* will not fit (see "The budget gate" above) — it
   never withholds the flag on a frame it could not size, because AE offers
   `what_gpu = NONE` until it has seen the flag and a gate that demanded a known
   device would deadlock itself shut.
4. `PF_Cmd_SMART_RENDER_GPU` is handled — it is a **separate command** from
   `PF_Cmd_SMART_RENDER`.
5. The `.aex` and `CUDA_Assets/` install to **AE's** Plug-ins folder, not
   MediaCore.
6. The PiPL's `AE_Effect_Global_OutFlags_2` matches (as a superset) what
   `GlobalSetup` sets — `0x2A001400`.

## Known limitations / v2 roadmap

- **Shift Edge** slider is wired up but stubbed — the morphological grow/shrink function exists but is not called (default=0). Implementing it requires `preserve_rgb_of_zero_alpha = TRUE` on checkout, then zeroing held RGB after the morph and before the blur.
- **Blend Radius erosion** is purely alpha-based (Gaussian blur of alpha → weight). It does not yet erode the alpha channel itself inward — increasing Blend Radius softens the blend but does not shrink the matte. True inward erosion is the next improvement.
- **Blend Radius threading** — currently serial. Candidate for `iterate_generic` in v2.
- **Edge Repair suite plugin** — a second plugin, `EdgeRepair.aex`, is now being built from scratch as a separate `add_meb_plugin` CMake target (an earlier prototype of the same name was removed in the clean-slate rebuild and is unrelated/dead). See [docs/EdgeRepair.md](docs/EdgeRepair.md) for its full build brief, locked parameter list, and staged-plan status — do not modify ModernEdgeBlur's own plugin code for its sake.
