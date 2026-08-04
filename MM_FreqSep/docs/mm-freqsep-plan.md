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
- **GPU from day one** — kernels designed GPU-first, with the mandatory CPU float path implemented as the reference the GPU output is validated against.
- Standalone repo/target now; folds into the MM monorepo later (Phase 8).

## 1. Parameter design

| # | Param | Type | Notes |
|---|-------|------|-------|
| 1 | Matte Layer | `PF_ADD_LAYER` | Default: None → effect applies everywhere |
| 2 | Matte Channel | Popup | Alpha / Alpha Inverted / Luma / Luma Inverted |
| 3 | Matte Fit | Popup | Fit to Layer (stretch) / Center / Top-Left — v1 can ship Fit only |
| 4 | Separation Radius | Float slider (px) | The frequency cut. Must scale by downsample factor |
| 5 | Separation Method | Popup | ~~Gaussian / Bilateral / Guided Filter~~ → **Gaussian / Guided Filter** (Bilateral cut) |
| — | **Edge Preservation** | Float 0–100 | **Added Phase 4.** Guided filter's ε (exponential map); greyed for Gaussian. Sits under Method. |
| 6 | Low Band: Smooth | Float 0–100 | Extra smoothing of tone band (second Gaussian) |
| 7 | ~~Low Band: Color Only~~ | ~~Checkbox~~ | **CUT** — moved to standalone MM Skin Color (§6) |
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
- Low band: extra smooth pass ✅ (Phase 2 second Gaussian). **Color Only mode — CUT.** It was built (low-band luma/chroma split, unit-tested) then removed: as a *low-band-only* operation it only reaches low-frequency color variation, which underwhelms on real skin (blotchiness is mid/high frequency, in the high band it never touches). More importantly it doesn't use the frequency split at all — it's a whole-image "keep luma, smooth chroma" operation — so it belongs in its own effect, not as a checkbox inside an FS plugin. Spinning out as a standalone **MM Skin Color** (edge-aware chroma evening via the shared `guided.h`); see §6.
- High band texture gain ✅ (Phase 2).
- **Guided Filter** separation method ✅ — O(1) box-filter formulation, self-guided (guide == input), per-channel, alpha-weighted (`src/guided.h`/`.cpp`, unit-tested in `tests/identity_test.cpp` against a brute-force reference + edge-preservation/smoothing-extreme controls). Kept **alongside** Gaussian (different looks: Gaussian creamy/soft, Guided edge-aware/no-halo). Separation Radius maps to a variance-matched box radius so the slider means the same smoothing scale on either method.
- **Edge Preservation** slider ✅ — the guided filter's ε, exposed 0–100 with an exponential slider→ε map (`FS_EdgeToEps`). Greyed out (PF_PUI_DISABLED) when Method = Gaussian via `PF_OutFlag_SEND_UPDATE_PARAMS_UI` + USER_CHANGED_PARAM / UPDATE_PARAMS_UI → `PF_UpdateParamUI`.
- **Bilateral cut** — removed from the Method popup entirely (redundant with Guided). Popup is now Gaussian | Guided Filter.
- Deliverable: side-by-side vs Photoshop FS retouch on test portrait — halos visibly reduced with Guided.

### Phase 5 — GPU render + performance
- GPU render path (`PF_OutFlag2_SUPPORTS_GPU_RENDER_F32`) immediately after CPU reference passes identity tests — DirectX (HLSL) + Metal (MSL), via AE's `PF_GPUDeviceSuite1`.
- **Architecture: kernel math authored ONCE in a shared header, thin per-platform shims.** DX and Metal both accept a common C-like subset, so each kernel body lives in one place and cannot drift between platforms; only host plumbing (device/buffer/dispatch) is written per platform. This split is the deliverable, not just the working DX build — it becomes the template for taking the rest of the MM suite cross-platform.
- **DirectX is the validation target; Metal is deferred, not written blind.** Revised 2026-07-18: rather than committing untestable Metal host code, Phase 5 builds DX12 for real while authoring every kernel body platform-free, against a documented ruleset (`../docs/gpu-kernel-rules.md`) derived from Metal's actual requirements. Metal then becomes a mechanical port (prologue + host plumbing) rather than a re-derivation.
- **Kernel math is validated WITHOUT a GPU.** Because `.kernel` bodies carry no platform syntax, their arithmetic is transcribed into C in `tests/gpu_parity_test.cpp` and diffed against the real `blur.cpp` / `fs_core.h` — no DX12 device, no AE host, no Mac. Algorithm error is caught here; anything left when the DX12 host lands is unambiguously plumbing. **Status: Gaussian + compose parity green (Gaussian bit-exact).**

**Phase 5 progress:**
- [x] `../docs/gpu-kernel-rules.md` — the suite-wide ruleset (also the ModernEdgeBlur / EdgeRepair reference)
- [x] `src/gpu/kernels/fs_prologue_hlsl.h` — HLSL vocabulary layer
- [x] `src/gpu/kernels/fs_gauss.kernel` — separable Gaussian, parity-tested bit-exact
- [x] `src/gpu/kernels/fs_compose.kernel` — recombine + View modes, parity-tested to ~1ulp
- [x] `src/gpu/kernels/fs_guided.kernel` — guided filter, 5 stages, parity-tested **bit-exact** (incl. r=24, anisotropic, eps floor)
- [x] **All kernel math complete and validated.** 40/40 tests green with no GPU, no AE host, no Mac.
**Usage profile (Riaan, 2026-07-18) — the constraints that shape the host design:**
- Footage: **6K+ / mixed.** One float ARGB plane at 6K ≈ 340MB — memory, not
  compute, is the binding constraint. GPU host must be **internally tiled**
  (horizontal bands + blur-support overlap, reusing the PreRender inflation
  math) from day one, not retrofitted.
- Radius: "2–30 feels right" — but radius is in pixels and that instinct is
  1080p-calibrated; the same look at 6K needs ~3×. Real use will hit 60–90.
  Keep FS_RADIUS_MAX 250. Big-radius perf is the main case, not an edge case.
- Felt pain: CPU slow only at 4K/big radius → GPU is solving render-time pain;
  favor doing it right over doing it fast. User is explicitly not in a rush.
- Fidelity: **identical GPU/CPU results, no silent approximations.** A
  quarter-res "fast guided filter" (He) exists and is visibly fine but NOT
  identical — permissible only as a future opt-in Draft toggle, never default.
- GPU guided scratch: reduce 5 planes → 3 by writing the A/B coefficient stage
  in place over s1/s2 (stage is pointwise — verified safe). ~1GB at 6K.

- [x] **DX12 host** (`src/gpu/mm_gpu_dx.cpp`, 2026-07-18) — device acquire via
  `PF_GPUDeviceSuite1`, PSOs from DXIL embedded at **build time** (dxc per
  entry point — no runtime compiler, single self-contained .aex), root
  constants + root UAV descriptors (no descriptor heaps), **internally tiled**
  in horizontal bands with blur-support overlap (planner in the platform-free
  `mm_gpu_host.h`), one submission + fence wait per render, per-render objects
  only (MFR-safe). Host design decisions logged in `../docs/gpu-kernel-rules.md` §6.
- [x] `GPURender` wiring — `PF_OutFlag2_SUPPORTS_GPU_RENDER_F32` +
  `SUPPORTS_DIRECTX_RENDERING` in `main.cpp` AND the PiPL (0x2A001400);
  `PF_Cmd_GPU_DEVICE_SETUP/SETDOWN/SMART_RENDER_GPU` routed; PreRender sets
  `GPU_RENDER_POSSIBLE` only when the worst-case tile plan fits the VRAM
  budget — infeasible frames (radius ~250 at 6K on small cards) fall back to
  the CPU reference automatically. Devices without shader doubles (rules
  §3.7) never get the GPU path at all.
- [x] **Matte driver on GPU** (`fs_matte.kernel`) — beyond the original
  milestone: Fit mapping + bilinear + channel extraction transcribed from
  `matte.cpp`, so matte-driven renders (the actual product workflow) run on
  the GPU too instead of silently falling back.
- [x] **Tiling proven AE-free**: `tests/gpu_parity_test.cpp` now runs the
  complete banded pipeline (load/swizzle → separation → smooth → matte →
  compose → store, driven by the real `FS_PlanTiles`) against the untiled CPU
  reference — multi-band, world-edge-clipped margins, HDR, all View modes —
  **bit-exact (0.000e+00)**. 48/48 tests green. Any in-AE mismatch is now
  D3D12 plumbing by elimination.
- [x] **CUDA host** (`src/gpu/mm_gpu_cuda.cpp`, 2026-08-03) — the backend AE
  actually offers on Windows/NVIDIA (`../../docs/AE_GPU_Effects.md` §1.1–1.2).
  Written as a line-for-line translation of the DX12 host's pass sequence, over
  the same `FS_GPUJob` and the same `FS_PlanTiles` band plan, dispatching the
  same `.kernel` bodies via PTX. Runtime `nvcuda.dll` shim
  (`src/gpu/fs_cuda_shim.h`, from EdgeRepair) so the host side needs no CUDA
  toolkit to build or run; only the `.cu` → PTX step needs nvcc, and without it
  the plugin still builds and AE renders on the CPU.
  - Two deliberate divergences from the DX host, both documented in the file
    header: scratch is **cached per device** (grow-only, freed at setdown,
    `renderLock` around the enqueue sequence — EdgeRepair's arrangement) rather
    than allocated per render, because FreqSep's scratch reaches ~1GB at 6K and
    a per-frame allocation of that size would eat the win; and there is no
    shader-doubles device gate, because every CUDA-capable NVIDIA GPU has fp64.
  - `GPU_DEVICE_SETUP` re-affirms `out_flags2 = SUPPORTS_GPU_RENDER_F32` by
    plain assignment (`AE_GPU_Effects.md` §2.2) — the omission that cost the
    suite seven failed experiments. The DX host never had to face it, having
    never been called by AE.
- [x] **Entry-point brackets** (`KERNEL_BEGIN`/`KERNEL_END`) — the kernel
  vocabulary was HLSL-shaped and could not express CUDA *or* Metal, both of
  which bind buffers as kernel arguments. See `../../docs/gpu-kernel-rules.md`
  §2.1. Two added lines per entry point, nothing existing moved; 7 of 10 DXIL
  blobs are byte-identical afterwards and the other 3 differ only in an
  auto-generated cbuffer name and its hash (same size, same `Offset: 0`, same
  `cb0`). All 27 parity tests still 0.000e+00.
- [ ] **In-AE validation** (hands-on, needs eyes on a monitor): View-popup
  band diff GPU vs Software-Only per `docs/phase6-validation.md`, now on CUDA.
  Milestone 1 = Gaussian end-to-end. **Nothing here has yet rendered a frame in
  After Effects** — until it does, treat "CUDA host" as written-and-compiling,
  not working.
- [ ] **Guided box: chunked sliding window** — sequenced deliberately AFTER the
  in-AE Gaussian validation, for the same reason the IIR CPU Gaussian is
  deferred below: landing it mid-port would make every GPU-vs-CPU mismatch
  ambiguous between a host bug and a kernel bug. `fs_guided.kernel`'s O(r)
  direct sum is ~110 taps at the radii real use reaches and accumulates in
  `double`, which GeForce runs at 1/64 rate — together that makes the GPU box
  pass slower than the CPU sliding window it replaces (`gpu-kernel-rules.md`
  §3.7). Fix: chunked sliding window, one thread per run of ~128–256 px,
  restoring O(1) per pixel at full occupancy. Bit-exactness survives because the
  sums are stored as `float` and a `double` accumulation over a ~6000-element
  row carries ~1e-12 relative error, three orders below float's resolution — any
  summation order rounds to the same float, which is why the existing test
  already finds direct-sum and sliding-window identical. Benefits DX equally.
- [ ] Metal prologue + host (deferred until Mac access) — now a smaller job
  than it was: the brackets mean the argument-struct shape Metal needs already
  exists and is proven by two backends.
- [ ] IIR recursive Gaussian on CPU (**deferred** — see tradeoff note below;
  stays sequenced AFTER the in-AE GPU validation for the same reason)

> **Tradeoff logged:** the IIR (van Vliet/Deriche) CPU Gaussian is deliberately
> deferred past the GPU work. It *approximates* a true Gaussian, so landing it
> mid-phase would make every GPU-vs-CPU mismatch ambiguous — kernel bug, or IIR
> approximation error? The GPU path is validated against the exact FIR reference
> first; the CPU large-radius path is swapped afterward against a trusted second
> implementation.

> **NOT a reference: MasterFilm.** Repeatedly mis-cited in earlier planning as the
> GPU template. It is not applicable — it's an **OpenFX plugin for Resolve**, not
> AE SDK; it uses **fragment shaders + fullscreen quads** (GLSL `#version 410`),
> not compute kernels; it has **no DirectX path at all** (OpenGL on Windows); and
> its **Metal path is an unbuilt stub** (`shaders/metal/` is empty, it loads a
> `.metallib` that is never authored, and no math is shared between its GL and
> Metal sides). Its OFX host model (`kOfxActionCreateInstance`) does not transfer
> to AE's `PF_SmartRenderExtra`/`PF_GPUDeviceSuite1`. MM FreqSep **defines** the
> suite's dual-target pattern; MasterFilm is a future consumer of it, and its own
> Mac port is a larger job than its directory layout suggests. Do not scaffold on it.
- Separable Gaussian, box-sum guided filter, and matte blend all map cleanly to compute kernels; validate GPU vs CPU within float tolerance per band (View popup makes this easy).
- CPU path: IIR recursive Gaussian (van Vliet/Deriche) for large radii — O(1) per pixel, matters at 4K with radius 40+.
- Profile MFR behavior on the CPU fallback, cache-friendly row-major passes.

### Phase 6 — Validation & QA
**Protocol + checklist: `docs/phase6-validation.md`** (written 2026-07-18; the
automated half — kernel/tiling/matte parity, HDR, plan invariants — already
runs in `tests/run_tests.ps1`; the doc scripts the hands-on AE half).
- Reference test: manual AE FS comp (duplicate + Gaussian Blur + subtract, 32bpc) must match plugin output at Working Curve = None within float tolerance; separate Photoshop side-by-side on gamma-encoded stills confirms the PS-accuracy target.
- Matrix: 8/16/32 bpc × full/half/quarter res × ROI crops × matte present/absent/mismatched size × premultiplied footage × rotated/scaled matte layer.
- MFR stress test, transparency edges, HDR (>1.0) sources.
- GPU-specific additions: GPU↔CPU band diff (View popup) per method; band-seam
  hunt at multi-tile radii on 6K; budget-fallback sanity at radius 250.

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
- [ ] Renumbering param **disk IDs** (even pre-release) orphans saved values in existing comps → params silently read as default and look "stuck". RULE: disk IDs are **append-only, leave gaps** — never renumber to close a hole. (Bit us removing Color Only: View shifted 111→110, its saved value orphaned, all View modes appeared dead until the effect was re-applied.)

## 5. Resolved decisions & v1 scope (locked 2026-07-16)

- Accuracy reference: **Photoshop manual FS workflow** on gamma-encoded values (Working Curve = None reproduces it; validated per Phase 6).
- Color space: space-agnostic core + **Working Curve popup (None / sRGB / ACEScct)**.
- **GPU from day one** — GPU path lands in Phase 5, immediately after CPU reference passes identity tests; CPU float path doubles as the mandatory fallback and validation reference.
- Standalone repo `MM_FreqSep` now; monorepo + `mm_common` extraction in Phase 8.
- v1 ships: **Gaussian + Guided separation** (Bilateral cut — redundant with Guided), Matte Fit = Fit-to-Layer only, 11 params, presets, no paint/heal tools (parametric processing only — FS clone/heal is a stills workflow, out of scope for a temporal effect).
- **Low Band: Color Only cut** (2026-07-17) — see Phase 4 note; moved to a standalone sibling effect (§6).

## 6. Sibling effect: MM Skin Color (chroma evening) — planned

Spun out of the cut Color Only checkbox. A standalone MM effect, NOT frequency
separation: it flattens skin **color** (redness, blotchiness) while preserving
**luminance** structure, across all frequencies — the operation most "skin color
evening" tools actually perform.

Why standalone, not a checkbox here:
- It doesn't use the frequency split (whole-image luma/chroma), so it would
  ignore Separation Radius / Method / Texture / Mix — confusing inside an FS UI.
- It wants its OWN radius + edge control, ideally **edge-aware chroma smoothing
  via the guided filter** (`guided.h`, already built + tested here) so color
  evens within the face but doesn't bleed across the jaw into the background.
- The MM suite already plans `mm_common` sharing (`guided.h`, luma/chroma
  helpers) in Phase 8 — this is exactly that reuse.

Design sketch (to flesh out before any code): Amount, Chroma Radius, Edge-Aware
toggle (+ its own edge/ε), the same Matte driver as this plugin, luma/chroma in
Rec.709. Guided-chroma core is reusable from here. Status: **design item, not
started** — the user has a related tool in mind it should pair with.

