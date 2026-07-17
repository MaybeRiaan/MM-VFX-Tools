# MM FreqSep — Execution Plan
Matte-driven frequency separation effect for After Effects (C++ AE SDK, MM suite)

## 0. Product definition

**What it does:** Splits the source layer into low frequency (tone/color) and high frequency (texture/detail) bands, lets you process each band independently, recombines, and blends the result back over the source — with the affected region driven by an external matte layer selected via a layer dropdown.

**Key decisions up front:**
- **AE C++ SDK effect, SmartFX** (`PF_OutFlag2_SUPPORTS_SMART_RENDER`), not OFX — consistent with the rest of the MM suite.
- **Internal processing in 32-bit float**, accept 8/16/32 bpc input. FS math (`hi = src − lo`) is trivially correct in float; no Photoshop-style +128 offset hacks needed.
- **Internal masking is free:** AE's per-effect Compositing Options (effect masks) already let the user mask the effect within the layer. You don't build this — document it in the UI/manual. The plugin only needs the *external* matte layer param.
- **Multi-Frame Rendering safe** from day one (`PF_OutFlag2_SUPPORTS_THREADED_RENDERING`), no mutable globals.
- **Space-agnostic core + "Working Curve" popup (None / sRGB / ACEScct)** — Photoshop FS operates on gamma-encoded values, which is why texture/smoothing behave perceptually uniformly; None on gamma/log footage = exact PS behavior. On scene-linear pipelines (ACEScg working space), the popup internally encodes → processes → decodes so hi-band amplitude stays uniform instead of scaling with brightness (specular hammering). Curve applies to processing only; matte is sampled as-is.
- **GPU from day one** — kernels designed GPU-first (as with MasterFilm), with the mandatory CPU float path implemented as the reference the GPU output is validated against.
- Standalone repo/target now; folds into the MM monorepo later (Phase 8).

## 1. Parameter design

| # | Param | Type | Notes |
|---|-------|------|-------|
| 1 | Matte Layer | `PF_ADD_LAYER` | Default: None → effect applies everywhere |
| 2 | Matte Channel | Popup | Alpha / Alpha Inverted / Luma / Luma Inverted |
| 3 | Matte Fit | Popup | Fit to Layer (stretch) / Center / Top-Left — v1 can ship Fit only |
| 4 | Separation Radius | Float slider (px) | The frequency cut. Must scale by downsample factor |
| 5 | Separation Method | Popup | Gaussian / Bilateral / Guided Filter |
| 6 | Low Band: Smooth | Float 0–100 | Extra smoothing of tone band (second blur or edge-aware) |
| 7 | Low Band: Color Only | Checkbox | Smooth chroma, preserve luma structure |
| 8 | High Band: Texture | Float 0–200% | Gain on detail band |
| 9 | Mix | Float 0–100% | Final blend vs source |
| 10 | View | Popup | Result / Low Band / High Band / Matte — essential FS workflow tool |
| 11 | Working Curve | Popup | None / sRGB / ACEScct — internal encode→process→decode for linear pipelines |

Guided filter matters for the "very accurate / no halos" goal — plain Gaussian FS halos hard on high-contrast edges (jawlines, hairlines). Ship Gaussian first as the reference, add Guided in Phase 4.

## 2. Core math

```
src_f   = to_float(src)                  // unmultiply if premultiplied; alpha-weighted blur at edges
lo      = blur(src_f, radius, method)
hi      = src_f - lo                     // unbounded in float — fine
lo'     = process_low(lo)                // smoothing / color-only smoothing
hi'     = hi * texture_gain
proc    = lo' + hi'
m       = matte_sample * effect_mask     // effect_mask via AE compositing options, automatic
out     = lerp(src_f, proc, m * mix)
```

Edge correctness: blur must be alpha-weighted (normalize by blurred alpha) or fringes appear at layer edges/mask boundaries. This is the #1 "accuracy" bug in naive FS implementations.

## 3. Phased execution

### Phase 1 — Scaffold (standalone) ✅ DONE
- Clone AE SDK sample (SmartyPants/Skeleton) → rename to `MM_FreqSep`, MM vendor PiPL, match code signing / build setup from existing MM plugins.
- CMake or shared .props/.xcconfig consistent with future monorepo layout:
  ```
  MM_FreqSep/
    src/   main.cpp  smart_render.cpp  blur.cpp  matte.cpp  params.h
    build/ (VS + Xcode via CMake)
  ```
- Deliverable: plugin loads, passthrough render, all params visible.

### Phase 2 — CPU reference render ✅ DONE
- SmartFX PreRender: checkout input **expanded by ceil(3σ)** (blur support region); union result rect correctly. Get this right early — tiling/ROI bugs are miserable to retrofit.
- Separable Gaussian in float, multithreaded (iterate suites or manual row/column passes).
- Implement split → recombine with radius param only. Verify `hi + lo == src` bit-exact at Texture=100/Smooth=0 (identity test).
- **Downsample scaling:** `effective_radius = radius * downsample_x.num / downsample_x.den`. Test at half/quarter res previews.
- Deliverable: View popup shows correct Low/High bands at full and half res.

### Phase 3 — Matte driver ✅ DONE
- `PF_CHECKOUT_PARAM` on the layer param in PreRender (checkout_layer), request rect matching output.
- Matte channel extraction (alpha/luma, inversion), Fit-to-Layer sampling (bilinear).
- Blend: `out = lerp(src, proc, matte)`. Handle: no layer selected → matte = 1.0; matte layer smaller/larger than source; matte layer with its own masks (works automatically — masks bake into its alpha before you receive it, which is exactly the "draw masks on the driver layer" workflow you described).
- Deliverable: paint a white solid with masks as driver → effect confined to mask shapes.

### Phase 4 — Band processing quality
- Low band: extra smooth pass; Color Only mode (blur in a luma/chroma decomposition, keep luma from base lo).
- High band texture gain.
- Bilateral and Guided Filter separation methods (guided filter: O(1) via box-filter formulation, works well in float).
- Deliverable: side-by-side vs Photoshop FS retouch on test portrait — halos visibly reduced with Guided.

### Phase 5 — GPU render + performance
- GPU render path (`PF_OutFlag2_SUPPORTS_GPU_RENDER_F32`) immediately after CPU reference passes identity tests — DirectX/Metal, kernel structure shared with MasterFilm's compute-shader approach.
- Separable Gaussian, box-sum guided filter, and matte blend all map cleanly to compute kernels; validate GPU vs CPU within float tolerance per band (View popup makes this easy).
- CPU path: IIR recursive Gaussian (van Vliet/Deriche) for large radii — O(1) per pixel, matters at 4K with radius 40+.
- Profile MFR behavior on the CPU fallback, cache-friendly row-major passes.

### Phase 6 — Validation & QA
- Reference test: manual AE FS comp (duplicate + Gaussian Blur + subtract, 32bpc) must match plugin output at Working Curve = None within float tolerance; separate Photoshop side-by-side on gamma-encoded stills confirms the PS-accuracy target.
- Matrix: 8/16/32 bpc × full/half/quarter res × ROI crops × matte present/absent/mismatched size × premultiplied footage × rotated/scaled matte layer.
- MFR stress test, transparency edges, HDR (>1.0) sources.

### Phase 7 — UX polish & release
- Param grouping (Separation / Low Band / High Band / Matte / Output), sensible defaults (radius ~8px @ 1080p), tooltips.
- Docs note: "for masking within this layer, use Effect > Compositing Options > + mask".
- Presets (.ffx): Skin Soft, Skin Strong, Texture Boost.

### Phase 8 — Suite integration
- Move into MM monorepo: shared `mm_common` (blur kernels, matte sampling, param helpers — blur + matte code will be reused by EdgeExtend and grain plugins).
- Shared PiPL/versioning/build scripts, licensing hookup with the rest of the suite.

## 4. Known traps checklist
- [ ] Radius not scaled by downsample → previews look different from renders
- [ ] Input rect not expanded by blur support → seams at tile/ROI boundaries
- [ ] Blur not alpha-weighted → dark/bright fringing at edges
- [ ] Matte layer checkout at wrong time/rect → one-frame-late mattes
- [ ] Globals in render path → MFR crashes
- [ ] High band clamped before recombine → texture loss (never clamp intermediates)

## 5. Resolved decisions & v1 scope (locked 2026-07-16)

- Accuracy reference: **Photoshop manual FS workflow** on gamma-encoded values (Working Curve = None reproduces it; validated per Phase 6).
- Color space: space-agnostic core + **Working Curve popup (None / sRGB / ACEScct)**.
- **GPU from day one** — GPU path lands in Phase 5, immediately after CPU reference passes identity tests; CPU float path doubles as the mandatory fallback and validation reference.
- Standalone repo `MM_FreqSep` now; monorepo + `mm_common` extraction in Phase 8.
- v1 ships: Gaussian + Guided separation (Bilateral may slip to v1.1), Matte Fit = Fit-to-Layer only, params 1–11, presets, no paint/heal tools (parametric processing only — FS clone/heal is a stills workflow, out of scope for a temporal effect).

