# Modern Edge Blur — Next Steps

Organised follow-ups across feature work, cleanup, and known smells. Each item carries enough context to start cold. Phases referenced here map to [Architecture.md §5](Architecture.md).

## Priority 1 — Phase 3: Color Dilation

The biggest user-visible feature still missing. Currently the `Enable Color Dilation` checkbox and `Dilation Amount` slider are wired up (`ParamsSetup`, `ReadParams`, `InflationPixels`) but `SmartRender` never reads them.

**Algorithm** (Architecture.md §4.2): un-premultiply input RGB by alpha, run two separable Gaussians (`RGB*α` and `α` independently, both at sigma = `Dilation Amount`), divide to recover dilated straight RGB, then re-premultiply by the output alpha. Goal: when alpha falls off at the matte edge, the RGB shown beneath it isn't black-multiplied but instead colour-matched to the nearest opaque interior.

**Where it fits in the render pipeline**: between the existing input checkout and the current Step 1 (blurred RGBA base) in `SmartRender`. The dilation pass produces a colour-corrected version of the source, then the existing Gaussian blur runs on *that* instead of the raw input. The erosion compose at the end is unchanged.

**Reuse**: `GaussBlurFloat` is the workhorse. `UnpackWorldToFloat` already gives us a packed `[H*W*4]` float buffer for 32-bit. The 8/16-bit paths can dilate inside the same packed-float intermediate to avoid writing a second native-typed version of the algorithm.

**Acceptance test**: side-by-side against EXTENDO or Red Giant's spill-suppression on a green-screen plate with dark RGB in the keyed area. Black fringe around the matte boundary should disappear when dilation is on.

**Open question**: ε in `dilated_RGB = blurred_RGB / max(blurred_alpha, ε)` — pick a value (1e-6 is conventional) and confirm no division artefacts in fully-transparent areas.

## Priority 2 — Phase 4: Polish & Validation

- **Performance profile on 4K plate**, now that threading is in. The `GaussBlurWorld` deinterleave/reinterleave loop (4 channels × full image, each pass) is the most likely remaining hot spot. Worth profiling before deciding to optimise.
- **Render-region correctness test**: render a small region preview vs full-frame render of the same composition, difference the overlap. Should be exactly zero. Tests the `InflationPixels` math.
- **HDR test**: 32-bit float plate with super-white RGB (values > 1). Confirm RGB never gets silently clamped by the coverage-only alpha clamp. Failing case would show as crushed highlights at the edge.
- **About box content** is currently a generic skeleton template; rewrite to mention the actual plugin name, author, version, link to GitHub.
- **Version bump** to 1.0.0 once Phase 3 ships and the validation tests pass.

## Priority 3 — Small Cleanups

### Code

- **`BuildEdgeWeightMap` is dead code** — `SmartRender` builds the erosion map inline at the same code site, applying the `pow(., 3)` shaping that `BuildEdgeWeightMap` doesn't. Either delete it, or refactor `SmartRender` to call it and apply the cube outside. Leaving it as-is grows confusion over time.
- **`SampleAlpha8/16/Float` use `world->data` arithmetic** that doesn't honour `world->rowbytes` correctly in all cases — actually it does (`reinterpret_cast<const char*>(world->data) + y * world->rowbytes`), but it's worth re-reading after Phase 3 lands to make sure the inflated-input-region path still works for non-contiguous worlds.
- **`StrID_Gain_Param_Name` / `StrID_Color_Param_Name`** were removed from `ModernEdgeBlur_Strings.{h,cpp}` during the audit. If a future param needs a localised display name, add it to the string table instead of inline literals in `ParamsSetup`. Don't reintroduce the dead skeleton entries.

### Build

- **Spurious `'pwsh.exe' is not recognized` warning** on every build. The actual post-build deploy uses `powershell.exe` and succeeds. Source of the `pwsh.exe` reference is unclear — likely a VS-internal MSBuild rule, not our CMake. Track down and silence or accept as cosmetic.
- **`build.ps1`** auto-increments `BUILD_VERSION` and auto-commits, which makes iterative builds noisy in git history. Used with `-NoBuildIncrement` during dev. Consider making no-increment the default and requiring a flag to bump, or removing the auto-commit entirely.
- **`build/` and `.aider.chat.history.md`** are in the working tree but probably should be in `.gitignore`. Check `.gitignore` next time you commit.

### Architecture / docs

- **Architecture.md §3 still lists `Blur Type` semantics for kernel construction** even though the popup was removed. Wording is now self-consistent (says "Gaussian" everywhere) but a future reader might wonder where Box went. Mentioned in §5 Phase 2; consider adding a one-liner to §3 to close the loop.

## Roads Not Taken — do not re-attempt without explicit user re-validation

These were tried, considered, or designed and explicitly rejected. Recording them here so a future session doesn't rediscover them as "good ideas."

- **Sobel-gradient edge weight + Edge Offset param** (Architecture.md §6). Original Phase 1 design. Replaced with `pow(GaussBlur(alpha, edge_radius), 3)` which is simpler, parameter-free, and produces visually equivalent or better results across hard and soft mattes. Don't add Edge Offset back; the cube exponent does the same job implicitly.
- **Box blur path / `Blur Type` popup**. Original Phase 2 plan had Box as the default and Gaussian as Phase 3. Code went straight to Gaussian-only; the popup is gone. Box doesn't add quality at this scale and the second code path is maintenance cost.
- **Invoking AE's built-in Gaussian Blur via `PF_invoke_effect_on_layer`** (Architecture.md §6). Considered as a way to skip writing our own convolution. Rejected: we'd still need the compose pass, some built-ins don't support MFR, and match-name pinning is fragile across AE versions.
- **`iterate_generic` with a fixed-size stack scratch buffer** in the H-pass callback (`float tmp[512];`). Caused a stack overflow on any layer wider than 512 px. The shipped design uses one heap-allocated `n_workers * max(W,H)` pool indexed by `thread_indexL`. See [memory/threading_iterate_generic.md] for the full crash post-mortem; don't reintroduce stack scratch.
- **Hardcoding `max_threads = 64`** for the per-thread scratch allocation. The shipped design probes via `iterate_generic(PF_Iterations_ONCE_PER_PROCESSOR, ...)` and uses the real worker count AE returns. Defensive clamp at 256.
- **Calling-convention annotations on iterate callbacks** (`__cdecl` explicit, custom `PF_CDECL` macro). The SDK has zero `__cdecl` annotations anywhere, and MSVC's default `/Gd` makes both sides `__cdecl` automatically. Adding explicit annotations doesn't fix anything; the shipped callbacks use `extern "C" static` for C linkage + internal linkage, which is sufficient.
- **GPU path (CUDA/Metal/OpenCL via `PF_GPU_Effect_Suite`)**. Out of scope for v1.0 per Architecture.md §1.

## Open Questions (no decision yet)

- **`output_alpha` semantics**: currently `output_alpha = blurred_alpha`, so the matte itself softens. Some users may want `output_alpha = max(input_alpha, blurred_alpha)` to preserve the original opaque interior exactly. Worth surfacing as a param once we have real-footage validation showing it matters.
- **Threading granularity**: H-pass is per-row, V-pass is per-column. For very tall, thin layers (e.g. 256w × 4320h) the H-pass is the bottleneck and we get poor parallel utilisation. Worth measuring before deciding whether to switch to a tiled approach.
- **Per-channel deinterleave cost** in `GaussBlurWorld` — runs 4 times per native-world blur. A packed-float intermediate (like the 32-bit path) might be faster overall by amortising the deinterleave. Measure before refactoring.
