# Modern Edge Blur — Architecture & Phased Plan

## 1. Objective

A production-quality edge-blur and edge-extend tool for After Effects, intended to replace Red Giant's Keying Suite "Edge Blur" plugin (long unmaintained) and provide functionality equivalent to Nuke's `EdgeExtend` / aescripts' EXTENDO.

The plugin softens matte edges without blurring the interior, and can dilate (extend) RGB into semi-transparent edge pixels to eliminate the "black fringe" common to naive blurs.

### Non-goals (for v1.0)
- GPU kernels (CUDA/Metal/OpenCL via `PF_GPU_Effect_Suite`) — deferred to v2.0. v1.0 is CPU + MFR.
- Mask channels other than the primary alpha.
- Sequence-data caching across frames. Every frame is rendered from its own params + input.

## 2. Standing rules

These apply across every phase. Violating any of them is a bug.

- **MFR-safety.** No mutable statics or globals. Per-render data lives on the stack of `SmartRender` (see `EdgeBlurRenderInfo`).
- **Premultiplied alpha.** AE delivers premultiplied. Any operation that touches RGB independently of alpha (notably color dilation) MUST un-premultiply → operate in straight space → re-premultiply. Skipping this is the *root cause* of the black-fringe artifact the plugin is supposed to fix.
- **Bit-depth fidelity.** 8 / 16 / 32-bit float paths produce visually identical results. The 32-bit float path does NOT clamp RGB values > 1 (HDR / additive layers). Alpha is clamped to [0,1] **only when used as a coverage weight** — specifically inside gradient computation (Phase 1) and as the kernel weight in dilation (Phase 4). The alpha *channel itself* passes through unclamped to preserve downstream compositing semantics. This matches Nuke's EdgeExtend convention: coverage and channel value are separated.
- **Render-region correctness.** `PreRender` always inflates the input request by `edge_radius + blur_amount + (dilate_en ? dilate_amt : 0)` plus 1 pixel of slop. Effect applied to a small crop must produce the same pixels as the same effect applied full-frame and then cropped.
- **Visual reference.** Each phase has an acceptance test that puts the plugin's output side-by-side with Red Giant Edge Blur (and EXTENDO where dilation is involved) on the same source. The reference is the existing tool, not a numerical tolerance.

## 3. Parameters

Display name and identifier (see [src/ModernEdgeBlur.h](src/ModernEdgeBlur.h)):

| Display | Identifier | Type | Default | Range |
| --- | --- | --- | --- | --- |
| Edge Radius | `EDGE_BLUR_EDGE_RADIUS` | Float | 3.0 | 0..20 |
| Edge Offset | `EDGE_BLUR_EDGE_OFFSET` | Float | 0.0 | -10..10 |
| Blur Amount | `EDGE_BLUR_BLUR_AMOUNT` | Float | 5.0 | 0..50 |
| Blur Type | `EDGE_BLUR_BLUR_TYPE` | Popup | Box | Box \| Gaussian |
| Enable Color Dilation | `EDGE_BLUR_DILATE_EN` | Checkbox | off | — |
| Dilation Amount | `EDGE_BLUR_DILATE_AMT` | Float | 2.0 | 0..10 |

Param semantics nailed down:

- **Edge Radius**: width (in pixels) of the band around the matte boundary that gets blurred. Internally drives how far the alpha-gradient signal is spread before being used as the per-pixel blur weight.
- **Edge Offset**: positive shifts the edge band *outward* (into transparent area), negative shifts *inward* (into opaque area). Implemented as a threshold offset on the edge weight after spreading.
- **Blur Amount**: kernel radius in pixels for the blur convolution. Maps to `sigma = blur_amount / 3` for Gaussian.
- **Blur Type**: Box (mean kernel) or Gaussian (`sigma = blur_amount / 3`, kernel size `2*ceil(3*sigma)+1`). Both separable, both run as horizontal pass + vertical pass.
- **Enable Color Dilation / Dilation Amount**: enables the un-premult → weighted-neighborhood blur → re-premult pass. `Dilation Amount` is the Gaussian sigma of the weighting kernel in pixels.

## 4. Algorithm — high level

Per render call, given the (inflated) input layer and the parameters:

1. **Edge weight map** — derive a per-pixel scalar `w(x,y) ∈ [0,1]` from the input alpha. Peaks at the matte boundary, falls off across `Edge Radius` pixels. `Edge Offset` shifts it inward/outward.
2. **Spatial blur** — apply a separable two-pass blur (H then V) of the input *RGBA* with the chosen kernel and radius. This produces a fully-blurred image.
3. **Compose** — output = `lerp(input, blurred, w)`. Interior pixels (w≈0) pass through untouched; edge pixels (w≈1) take the blur. Sharp interior is preserved.
4. **Optional dilation** — if `Dilate En`, before the compose step, replace the RGB of semi-transparent pixels with weighted-neighborhood RGB pulled from nearby opaque pixels (un-premult / blur RGB·α and α independently / divide / re-premult). Eliminates dark fringes.

This is the "(a) blur once, lerp by edge weight" model, chosen over spatially-variant radius for performance with effectively identical visual output.

### 4.1 Edge weight derivation (the part Gemini's spec got wrong)

The simple formula `|alpha - 0.5|` only works when the matte is exactly antialiased and centered at 0.5. For real keyed footage with wide partial-alpha regions, gradient-based detection is required.

The edge weight is built from the **magnitude of the alpha gradient** — i.e. "how fast is alpha changing here". Computed via a Sobel-style derivative on the alpha channel. The raw gradient is then spread across `Edge Radius` pixels with a small blur, and `Edge Offset` applied as a threshold shift so users can push the band inward (into opaque) or outward (into transparent).

This is what lets the plugin work equivalently on hard 1-pixel mattes and on wide pre-blurred mattes.

### 4.2 Dilation: weighted-neighborhood algorithm

Working in straight (un-premultiplied) RGB:
```
blurred_RGB   = SeparableGaussian(RGB * alpha_mask,  sigma=DilateAmt)
blurred_alpha = SeparableGaussian(      alpha_mask,  sigma=DilateAmt)
dilated_RGB   = blurred_RGB / max(blurred_alpha, ε)
```
This pulls opaque-pixel colors into the transparent neighborhood weighted by their alpha. ε prevents divide-by-zero in fully transparent regions (those pixels keep their original RGB, which is irrelevant since their alpha is 0).

Then re-premultiply: `out_RGB = dilated_RGB * out_alpha`.

EXTENDO and similar plugins are variants of this. Faster than nearest-opaque search; better quality than naive RGB-blur.

## 5. Phased plan

Each phase ends with a working plugin that loads cleanly in AE and produces a visible result. Each phase has a side-by-side visual reference test before it merges.

### Phase 0 — Foundation (done)
Smart Render scaffolding, MFR opt-in, 8/16/32 bpc dispatch, param read, render-region inflation, debug heat-map output. Commit `0b17349`.

### Phase 1 — Real edge weight map
Replace the debug heat-map with the Sobel-gradient-based edge weight (§4.1). Output the weight as grayscale so it's directly inspectable. `Edge Radius` and `Edge Offset` become meaningful.

**Acceptance test:** apply to a keyed greenscreen layer. Visually compare the edge band against the layer's actual matte boundary. Edge Radius slider should widen/narrow the band; Edge Offset should shift it inward/outward.

### Phase 2 — Two-pass blur (Box only)
Implement separable box blur on RGBA. Allocate scratch `PF_EffectWorld`s via `PF_WorldSuite2::PF_NewWorld`. Compose: `output = lerp(input, blurred, edge_weight)`. Disable the grayscale debug output; the effect now actually softens edges. Gaussian path stays as a stub that falls through to Box for now.

**Acceptance test:** apply to keyed footage alongside Red Giant Edge Blur with matched settings. Edges should soften comparably (any residual fringing is expected — that's Phase 4's job). Interior pixels must be bit-identical to input.

### Phase 3 — Gaussian blur
Add the real Gaussian kernel (sigma = `blur_amount / 3`, precomputed weights per render call). Wire the `Blur Type` popup so users can choose Box or Gaussian.

**Acceptance test:** Gaussian visibly smoother than Box at the same radius. No banding or stepping artifacts at any bit depth (32-bit float carries enough precision that we don't need supersampling).

### Phase 4 — Color dilation
Implement §4.2. Un-premult / dual-Gaussian / divide / re-premult. Gate on `Dilate En`. Run *before* the blur so the dilated colors get softened together with the matte.

**Acceptance test:** side-by-side against EXTENDO on the same source. Black fringes that appeared in Phase 2/3 output should disappear when dilation is enabled. Test with both light- and dark-keyed plates.

### Phase 5 — Polish & validation
- Performance profile on 4K plate. Identify any per-pixel hot spots.
- Validate render-region math on a small-region preview vs. full-frame render. Pixels in overlap region must match.
- 32-bit float HDR test: footage with super-white values; confirm no clamping leaks.
- About box, plugin metadata, version bump.

## 6. Alternatives considered

### Invoking AE's built-in blur instead of writing our own

AE exposes built-in effects to plugins via `PF_invoke_effect_on_layer` / the AEGP suite, so we could in principle run "ADBE Gaussian Blur 2" on the source and use the result. Decided against this for v1.0:

- We need spatially-variant blending against an edge weight map; AE's blurs only accept a single global radius. We'd still have to write the compose step ourselves, so the saving is just the convolution.
- Some AE built-ins (e.g. Camera Lens Blur) don't support MFR and would serialize our render thread.
- Pinning to a specific built-in match-name creates an implicit dependency on AE's effect catalog being stable across versions.
- A separable box/Gaussian blur is ~50 lines per bit-depth. The hard parts of this plugin are the gradient-based edge detection and the un-premult/dilation math, neither of which AE provides as an invokable effect.

Worth keeping as a fallback if Phase 2 hits an unexpected performance wall.

## 7. Risks & open questions

- **Performance on large `Dilation Amount`.** Two separable Gaussians at sigma=10 across a 4K frame is fine on CPU; sigma=50 starts hurting. If users want >10px dilation we may need to add a downsample/upsample step (push-pull) in v1.1.
- **Smart Render render-region edge cases.** When AE asks for tiny regions during scrubbing, the inflated-input checkout may overshoot the layer bounds. The SDK clamps automatically but we should verify with a test.
- **HDR alpha (>1).** Unusual but possible in 32-bit ACES comps. Current rule says "clamp inside gradient math only" — we'll see if that holds when we have a real HDR test.
- **Param ranges.** Defaults are guesses inherited from the skeleton. Will likely be retuned based on what feels right against real footage in Phase 2 testing.
