# Modern Edge Blur — Architecture & Phased Plan

## 1. Objective

A production-quality edge-blur and edge-extend tool for After Effects, intended to replace Red Giant's Keying Suite "Edge Blur" plugin (long unmaintained) and provide functionality equivalent to Nuke's `EdgeExtend` / aescripts' EXTENDO.

The plugin produces a soft, eroded matte edge with the blurred image showing through beneath the sharp original, and (planned) can dilate (extend) RGB into semi-transparent edge pixels to eliminate the "black fringe" common to naive blurs.

### Non-goals (for v1.0)
- GPU kernels (CUDA/Metal/OpenCL via `PF_GPU_Effect_Suite`) — deferred to v2.0. v1.0 is CPU + MFR + iterate_generic.
- Mask channels other than the primary alpha.
- Sequence-data caching across frames. Every frame is rendered from its own params + input.

## 2. Standing rules

These apply across every phase. Violating any of them is a bug.

- **MFR-safety.** No mutable statics or globals. Per-render data lives on the stack of `SmartRender` (see `EdgeBlurRenderInfo`).
- **Premultiplied alpha.** AE delivers premultiplied. Any operation that touches RGB independently of alpha (notably color dilation) MUST un-premultiply → operate in straight space → re-premultiply. Skipping this is the *root cause* of the black-fringe artifact the plugin is supposed to fix.
- **Bit-depth fidelity.** 8 / 16 / 32-bit float paths produce visually identical results. The 32-bit float path does NOT clamp RGB values > 1 (HDR / additive layers). Alpha is clamped to [0,1] **only when used as a coverage weight** — specifically inside the erosion weight derivation and (Phase 4) as the kernel weight in dilation. The alpha *channel itself* passes through unclamped where it's the output value. This matches Nuke's EdgeExtend convention.
- **Render-region correctness.** `PreRender` always inflates the input request by `edge_radius + blur_amount + (dilate_en ? dilate_amt : 0)` plus 1 pixel of slop. Effect applied to a small crop must produce the same pixels as the same effect applied full-frame and then cropped.
- **Visual reference.** Each phase has an acceptance test that puts the plugin's output side-by-side with Red Giant Edge Blur (and EXTENDO where dilation is involved) on the same source. The reference is the existing tool, not a numerical tolerance.

## 3. Parameters

Display name and identifier (see [src/ModernEdgeBlur.h](src/ModernEdgeBlur.h)):

| Display | Identifier | Type | Default | Range |
| --- | --- | --- | --- | --- |
| Edge Radius | `EDGE_BLUR_EDGE_RADIUS` | Float | 3.0 | 0..50 |
| Blur Amount | `EDGE_BLUR_BLUR_AMOUNT` | Float | 5.0 | 0..50 |
| Enable Color Dilation | `EDGE_BLUR_DILATE_EN` | Checkbox | off | — |
| Edge Extend | `EDGE_BLUR_DILATE_AMT` | Float | 2.0 | 0..50 |
| Sample Distance | `EDGE_BLUR_SAMPLE_DIST` | Float | 0.0 | 0..50 |
| Sharp Matte Edge | `EDGE_BLUR_SHARP_ALPHA` | Checkbox | off | — |

Param semantics:

- **Edge Radius**: Gaussian radius (in pixels) used to blur the alpha when building the erosion weight map. Larger values produce a wider, softer eroded band; 0 disables erosion (sharp output).
- **Blur Amount**: Gaussian radius (in pixels) of the RGBA blur kernel applied to the source. Drives how much smoothing the blurred-base image gets before being composited beneath the eroded edge.
- **Enable Color Dilation / Edge Extend**: enables the dilation pass — `blur(premult_RGB) / max(blur(alpha), eps)` to recover straight RGB at the edge, then re-premult. `Edge Extend` is the Gaussian sigma of the dilation kernel in pixels. Eliminates the dark fringe a naive blur of premultiplied RGB produces. Also used as an "edge extend" feature when combined with `Sample Distance` and `Sharp Matte Edge`.
- **Sample Distance**: how far inward to erode the sampling silhouette before dilation runs. Implemented as a true morphological min-filter (per-pixel minimum alpha in an N-px radius), so it produces a hard inward choke with no rounding or two-way smear. Used to throw away motion-blur-contaminated soft pixels so the dilation pushes clean deep-interior RGB outward. Only meaningful when `Enable Color Dilation` is on.
- **Sharp Matte Edge**: when on, output alpha = original input alpha (matte is unchanged; dilation only fixes the RGB shown through it). When off, output alpha = blurred alpha (today's soft-erosion look — matte itself softens). Compose-time switch only.

Gaussian kernel construction (used by both Edge Radius and Blur Amount): `sigma = max(radius/2, 0.5)`, kernel size `2*ceil(radius)+1`. All three bit-depth paths share `MakeGaussianKernel` so kernel shape is bit-identical across depths.

## 4. Algorithm — high level

Per render call, given the (inflated) input layer and the parameters:

1. **Blurred RGBA base** — apply a separable Gaussian (H then V) of the full input *RGBA* with kernel radius `Blur Amount`. This is what shows through the eroded edge. For 32-bit float the blur runs in a packed `float[H*W*4]` buffer; for 8/16-bit it runs in a native scratch `PF_EffectWorld` (one channel at a time, deinterleaved → blurred → reinterleaved). Output alpha of the final compose comes from this blurred base, so the matte itself softens.
2. **Erosion weight map** — extract the input alpha as a float plane, Gaussian-blur it with kernel radius `Edge Radius`, then cube the result: `w = clamp(blurred_alpha, 0, 1)^3`. The cube concentrates the falloff inside the matte so the sharp original is eaten in from the boundary.
3. **Compose** — `output_RGB = lerp(blurred_RGB, sharp_RGB, w)`, `output_alpha = blurred_alpha`. Where `w ≈ 1` (deep interior) the sharp original passes through unchanged. Where `w → 0` (edge and outside) the blurred base shows through, with a soft alpha falloff to match.
4. **Optional dilation** (planned, Phase 4) — before the blur, replace the RGB of semi-transparent pixels with weighted-neighborhood RGB pulled from nearby opaque pixels (§4.2). Eliminates dark fringes when the original matte has dark RGB in transparent regions.

The model is alpha-driven erosion, not band-only softening. Compared to the more common "lerp(sharp, blurred, edge-band)" approach, erosion preserves the sharp interior naturally (no edge-detection needed) and produces a visually richer falloff because both RGB and alpha soften together at the boundary.

### 4.1 Why erosion instead of Sobel-gradient band detection

An earlier design called for a Sobel-style alpha gradient + spread + threshold-offset to identify the edge band, then lerp sharp→blurred across it. In practice the alpha-blur-and-cube approach produces better results with less code and fewer parameters: it doesn't require an Edge Offset slider (the cube exponent does the same job implicitly), it handles both hard 1-pixel mattes and wide pre-blurred mattes the same way, and it degrades gracefully when alpha is noisy or sparse. The Sobel approach is preserved here as an alternative if the erosion model ever proves insufficient for a real case.

### 4.2 Dilation: alpha-weighted neighbourhood (the EdgeExtend math)

Implemented as a **source preprocessing step** that produces a "clean RGBA source" replacing the raw input for all downstream pipeline stages. The downstream pipeline (Edge Radius erosion blur, Blur Amount colour-bleed, lerp compose) is unchanged.

Input is premultiplied (AE gives us premult buffers). The core algorithm:

```
# Optional: Sample Distance > 0 erodes the sampling silhouette inward
# via a true morphological min-filter, then hard-thresholds to a 0/1
# mask (so masked-out pixels contribute exactly zero to the dilation Gauss).
if Sample Distance > 0:
    eroded  = MinFilter(input_alpha, radius=SampleDist)
    mask    = (eroded > EPS) ? 1 : 0
    src_a   = input_alpha * mask    # 0 outside the eroded zone
    src_RGB = input_RGB   * mask    # 0 outside the eroded zone (premult RGB)
else:
    src_a   = input_alpha
    src_RGB = input_RGB

# Alpha-weighted neighbourhood (the dilation itself)
blur_a   = SeparableGaussian(src_a,   sigma=EdgeExtend)
blur_RGB = SeparableGaussian(src_RGB, sigma=EdgeExtend)
straight_RGB = blur_RGB / max(blur_a, EPS)
```

`straight_RGB` is colour pushed from confident-interior pixels into the wider neighbourhood, weighted by alpha. EPS = 1e-4 keeps the divide bounded.

Output composition depends on `Sharp Matte Edge`:

- **OFF**: `clean_alpha = input_alpha` (unchanged). Inside the original matte: `clean_RGB = input_RGB` (untouched). Outside: `clean_RGB = straight_RGB * input_alpha = 0` (premult-zero, since input_alpha is 0 there). The compose callback then swaps in `blurred_alpha` for the output alpha, letting the blurred RGBA bleed outward — Blur Amount drives the soft edge.
- **ON**: hard outward grow of the original alpha. `grown_a = (Gauss(input_alpha, dilate_k) > EPS) ? 1 : 0`, `clean_alpha = max(input_alpha, grown_a)`. NB: the Gauss is on **input alpha**, not the eroded one — Edge Extend's reach is independent of Sample Distance. Inside the original matte: `clean_RGB = input_RGB` (untouched). In the grown region: `clean_RGB = straight_RGB * clean_alpha`. Matte literally extends outward by ~Edge Extend pixels with a hard outer boundary and a clean-colour fill from the eroded interior.

EXTENDO, Nuke's `EdgeExtend`, and Furnace's `F_Erode` are variants of this. Faster than nearest-opaque search; better quality than naive RGB-blur.

## 5. Phased plan

Each phase ends with a working plugin that loads cleanly in AE and produces a visible result. Each phase has a side-by-side visual reference test before it merges.

### Phase 0 — Foundation (done, commit `0b17349`)
Smart Render scaffolding, MFR opt-in, 8/16/32 bpc dispatch, param read, render-region inflation, debug heat-map output.

### Phase 1 — Edge weight map (done, commit `bfc36f5`)
Implemented as Gaussian-blurred alpha cubed (see §4.1), not the Sobel-gradient approach originally specified. `Edge Radius` drives the blur sigma; the cube concentrates erosion inside the matte.

### Phase 2 — Blurred RGBA base + erosion compose (done, commit `bfc36f5`)
Separable Gaussian on RGBA (all bit depths), composed via `lerp(blurred, sharp, edge_weight)` with `output_alpha = blurred_alpha`. The Box-blur path and `Blur Type` popup from the original plan were dropped — Gaussian-only is simpler and quality-equivalent at this scale.

### Phase 2.5 — Threading (done, commit `2195018`)
Parallelised the inner separable Gaussian (`GaussBlurFloat`) via `in_data->utils->iterate_generic` — H-pass per row, V-pass per column, one shared per-thread scratch pool sized to the actual image. Worker count probed via `PF_Iterations_ONCE_PER_PROCESSOR`. Gated by `MODERN_EDGE_BLUR_THREADED` (default 1) with the serial body preserved as fallback. See [memory/threading_iterate_generic.md](.claude/projects/.../memory) for the stack-overflow bug that killed the first attempt.

### Phase 3 — Color dilation (done)
Implemented §4.2 by extending the existing `GaussBlurRGBA` and `GaussBlurWorld<T>` helpers with an optional `dilate_k` parameter. When non-null, alpha and the (already-premultiplied) RGB are blurred with `dilate_k`, RGB is divided by `max(blurred_alpha, 1e-4)` to recover straight RGB, then the existing `blur_k` pass runs on the dilated buffer for the colour-bleed falloff. Sequence is dilate→divide→blur because the divide step isn't linear and can't be folded into a single combined Gaussian. The compose at the end is unchanged. Gated on `Dilate En` and `Dilation Amount > 0`; when off, the helpers are bit-identical to the pre-Phase-3 path.

### Phase 4 — Edge Extend mode (done, corrective rewrite shipped)
First Phase 4 attempt grafted dilation into the blur helpers (`GaussBlurRGBA`/`GaussBlurWorld<T>`), which incorrectly transformed the *blurred base buffer* and broke every visible mode (matte ate inward, Edge Extend added blur, Sharp Matte Edge produced a black ring). Rewrote dilation as a **source preprocessing step** (`BuildCleanSource`): it transforms the input into a "clean RGBA source" that the existing erosion-blur and compose pipeline runs on. The lerp at compose is `lerp(blurred_clean, clean, w)`, both premult-consistent.

`Sharp Matte Edge` is now baked into the clean alpha at preprocessing time — when on, the clean alpha is the original input alpha hard-grown outward by `Edge Extend` pixels (threshold the dilation-blurred alpha at EPS), and the grown region is filled with dilated RGB from the eroded interior. When off, the clean alpha is the unmodified input alpha and only the RGB is replaced with the alpha-weighted-neighbourhood version.

`Sample Distance` (alpha-threshold erosion at 0.95) is applied before the dilation runs, so the colour push comes from confident deep-interior pixels rather than motion-blur-contaminated soft edges.

The bit-depth code structure also unified as part of the rewrite: all paths now go through `UnpackWorldToFloat → BuildCleanSource → GaussBlurRGBA (colour-bleed) → iterate compose`, with the 8/16-bit callbacks quantising to native on writeback. `GaussBlurWorld<T>` and `BuildEdgeWeightMap` were deleted (no longer reachable; `GaussBlurWorld<T>` was a casualty of the architecture change, `BuildEdgeWeightMap` was already dead). Memory cost at 8/16 bpc went up by one `H*W*4*sizeof(float)` allocation; perf impact in the noise.

Renamed `Dilation Amount` → `Edge Extend` (display only; disk ID preserved). With both new params at defaults (Sample Distance=0, Sharp Matte Edge=off) and Color Dilation off, output is unchanged from pre-Phase-3.

### Phase 5 — Polish & validation (next)
- Performance profile on 4K plate now that threading, dilation, and sample-erode are in. Identify any remaining per-pixel hot spots (channel deinterleave/reinterleave in `GaussBlurWorld` is a likely candidate).
- Validate render-region math on a small-region preview vs. full-frame render. Pixels in overlap region must match.
- 32-bit float HDR test: footage with super-white values; confirm no clamping leaks where the alpha-coverage clamp shouldn't be touching RGB.
- About box, plugin metadata, version bump to 1.0.0.

## 6. Alternatives considered

### Sobel-gradient edge weight (the original Phase 1 plan)

The simple formula `|alpha - 0.5|` only works when the matte is exactly antialiased and centered at 0.5. The Sobel-gradient approach was proposed to handle real keyed footage with wide partial-alpha regions: detect "how fast alpha is changing", spread that signal across `Edge Radius` pixels, and apply `Edge Offset` as a threshold shift to push the band inward/outward.

We tried this and landed on the simpler alpha-blur-and-cube model (§4.1) instead. The Sobel path may return if a real footage case proves the erosion model insufficient.

### Invoking AE's built-in blur instead of writing our own

AE exposes built-in effects to plugins via `PF_invoke_effect_on_layer` / the AEGP suite, so we could in principle run "ADBE Gaussian Blur 2" on the source and use the result. Decided against this for v1.0:

- We need to blur RGBA into a base layer that gets composited *under* the eroded edge — AE's blurs would also have to be composited the same way, so we save no work.
- Some AE built-ins (e.g. Camera Lens Blur) don't support MFR and would serialize our render thread.
- Pinning to a specific built-in match-name creates an implicit dependency on AE's effect catalog being stable across versions.
- A separable Gaussian blur is ~50 lines per bit-depth (less now that the per-channel deinterleave bridges via `GaussBlurFloat`). The hard parts of this plugin are the alpha-driven erosion compose and the un-premult/dilation math, neither of which AE provides as an invokable effect.

## 7. Risks & open questions

- **Performance on large `Dilation Amount`.** Two separable Gaussians at sigma=10 across a 4K frame is fine on CPU with threading; sigma=50 starts hurting. If users want >10px dilation we may need to add a downsample/upsample step (push-pull) in v1.1.
- **Smart Render render-region edge cases.** When AE asks for tiny regions during scrubbing, the inflated-input checkout may overshoot the layer bounds. The SDK clamps automatically but we should verify with a test.
- **HDR alpha (>1).** Unusual but possible in 32-bit ACES comps. Current rule says "clamp inside coverage math only" — we'll see if that holds when we have a real HDR test.
- **Float-to-PNG export drift in regression tests.** Bit-identical comparisons of the threaded vs serial blur should be done in-comp (Difference blend in 32-bit float) rather than via PNG export — PNG quantisation makes premultiplied edges look different even when the float values agree.
