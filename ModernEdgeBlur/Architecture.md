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
- `out_flags2 = 0x08001400` (FLOAT_COLOR_AWARE | SUPPORTS_SMART_RENDER | SUPPORTS_THREADED_RENDERING)

## Known limitations / v2 roadmap

- **Shift Edge** slider is wired up but stubbed — the morphological grow/shrink function exists but is not called (default=0). Implementing it requires `preserve_rgb_of_zero_alpha = TRUE` on checkout, then zeroing held RGB after the morph and before the blur.
- **Blend Radius erosion** is purely alpha-based (Gaussian blur of alpha → weight). It does not yet erode the alpha channel itself inward — increasing Blend Radius softens the blend but does not shrink the matte. True inward erosion is the next improvement.
- **Blend Radius threading** — currently serial. Candidate for `iterate_generic` in v2.
- **EdgeRepair / Shift Edge suite** — a second plugin (`EdgeRepair.aex`) was prototyped but removed in the clean-slate rebuild. It will be reintroduced as a separate `add_meb_plugin` entry once ModernEdgeBlur v1 is fully validated.
