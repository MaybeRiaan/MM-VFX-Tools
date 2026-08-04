# Modern Edge Blur — Next Steps

> **STATUS 2026-08-03 — GPU (CUDA) port landed, code-complete, not yet run in AE.**
>
> The whole pipeline now exists on CUDA, pass for pass, in
> `src/ModernEdgeBlur_GPU.cpp` + `src/gpu/`. **The CPU pipeline is unchanged** —
> same phases, same threading, same `MEB_Pool`, same numbers. What did change in
> `ModernEdgeBlur.cpp`: its per-pixel formulas moved verbatim into the new
> AE-free `src/core/MEB_CoreMath.h` (so the parity gate can diff against the real
> code rather than a transcription), and the GPU plumbing was added — GlobalSetup
> flags, PreRender's `GPU_RENDER_POSSIBLE`, the `what_gpu` fork in SmartRender,
> and three new `EffectMain` cases. PiPL `out_flags2` → `0x2A001400`.
>
> Full write-up: [Architecture.md → GPU path](Architecture.md#gpu-path-added-2026-08-03).
>
> **NEXT, in order:**
> 1. **Build it.** Nothing here has been compiled yet. The nvcc PTX step and the
>    fxc type-check step are both new in CMakeLists.txt and both skip gracefully
>    if their tool is missing.
> 2. **Run the parity gate:** `.\tests\build_tests.ps1`. Headless, no AE, no GPU.
>    It must pass before any in-AE testing is worth doing.
> 3. **Spot-check CPU perf** on the 4.6K comp against the numbers below
>    (TOTAL avg 351 ms, worst 580 ms). The extraction into `MEB_CoreMath.h` is
>    inline + `__restrict` and should be free, but the blocked vertical loops are
>    exactly the ones whose auto-vectorisation earned that 4x, so confirm rather
>    than assume.
> 4. **First in-AE GPU render.** Check `%TEMP%\ModernEdgeBlur_gpu.log` FIRST:
>    `what_gpu=3` at PreRender and `SMART_RENDER GPU` lines mean the gate is open.
>    `what_gpu=0` at every PreRender means it is shut — go to
>    `../../docs/AE_GPU_Effects.md` §2.2 before theorising. Then A/B the same
>    frame against Mercury → *Software Only*; target max-abs ≤ 1e-5.
> 5. Only then: consider the perf levers noted in Architecture.md (shared-memory
>    blur tiles; fusing the vertical blur with the composite to save a full
>    frame of bandwidth).

> **STATUS 2026-07-06, end of session — v1.3 core is DONE and verified. Blur Amount and Blend Radius are correct, halo-free, and ~4x faster on worst-case frames. All work committed to main.**
>
> **This session completed (all user-verified in AE):**
> 1. **Black halo RESOLVED** — three stacked root causes (blur input not honestly premultiplied; output handed back premultiplied when AE's effect pipeline is STRAIGHT alpha on both sides; Phase 4 blend mixing the zeroed-black cliff via plain lerp). Golden rule for all future Phase 2/4 work: **premultiply → blur → unpremultiply internally, output straight colour, never multiply RGB by alpha in the output.** Full writeup: "Black halo — RESOLVED" under Priority 3.
> 2. **Blend Radius dark line RESOLVED** — alpha-weighted composite blend (sharp term drops out exactly where sharp coverage is 0).
> 3. **Blur width remapped** — kernel sigma radius×0.25 → ×0.5 with 3σ support; user-confirmed the blur shape looks right (old mapping was eyeball-tuned against the halo bug's fake extra width).
> 4. **Performance restructure** — fused threaded ingest/composite passes, tiled vertical blur, thread_local buffer pool. Measured: TOTAL avg 597→351ms, worst case 2413→580ms on a 4.6K comp; visually verified unchanged. Details under Priority 3.
>
> **NEXT SESSION — start here, in rough priority order:**
> 1. **Shift Edge (Priority 2):** fix the slider not appearing in AE's effect UI, validate the restructured shift path (untested — it now derives buffers post-shift via `MEB_DeriveRow`), then implement the planned **method dropdown**: "genuine min/max" (current exact circular `MorphPlane` — needs threading at minimum, it's O(radius²) and serial) vs "fast choke" (octagon/running-min, whichever benchmarks fastest). See Priority 2 for details.
> 2. **8/16bpc verification** of the unpremult pipeline (commit `7ad1164` once flagged unpremult instability across bit depths; halo fix was only confirmed at project depth).
> 3. **Red Giant Edge Blur A/B** at matched slider values — fine-tune the sigma ×0.5 factor only if the comparison demands it.
> 4. **Before any release build:** strip the debug logging + EDGE/PHASE4 dump diagnostics (see the debug-logging note under Priority 3), and remember build.ps1's deploy silently fails while AE is open — check timestamps before trusting a test (see "Testing pitfalls").
> 5. Backlog when relevant: erosion-shape dropdown (Box/Octagon/Circle), box-chain "fast blur" option, SIMD in blur inner loops, Ingest thread-fanout curiosity, EdgeRepair plugin (Priority 4).

## Priority 1 — Blend Radius erosion of the sharp/blurred blend weight — DONE (2026-07-03)

Fixed: `weight` (the sharp-vs-blurred RGB blend factor) is now built by eroding the sharp alpha inward by Blend Radius and then Gaussian-feathering the eroded result. As Blend Radius increases, the region showing the sharp/top layer at full strength shrinks, revealing more of the blurred base layer at the edge; at extreme radius the sharp layer is fully eroded away and only the blurred layer shows.

**Scope note:** `out_alpha` was deliberately left unchanged — it still comes from Blur Amount's blurred alpha, same as v1.2. This fix is scoped purely to the sharp/blurred reveal, not the matte's transparency shape. See `src/ModernEdgeBlur.cpp` Phase 3/4 comments.

## Priority 2 — Shift Edge

Intentionally on the shelf for now. `ShiftEdgeAlpha` is implemented (circular brute-force min/max morphological filter, unchanged — its "simple choker" behavior was a deliberate design choice, not a quality gap) and, as of 2026-07-03, actually runs whenever `shift_edge` is moved off 0 (not disabled — "stubbed" here just meant "not yet validated/prioritized," see below).

**Ordering already handled (2026-07-03):** `SmartRender` now does Shift Edge → explicit zero-RGB-where-alpha-is-0 pass → Blur Amount / Blend Radius, in that order. This matters because Shift Edge changes the alpha shape (grow/shrink), so the zeroing has to key off the *post-shift* alpha — zeroing first would leave a grow's newly-revealed pixels black (already-zeroed RGB) or leave a shrink's newly-hidden ring un-zeroed (leftover colour). The zero-pass no longer depends on `preserve_rgb_of_zero_alpha` at the checkout level either (see Known working state) — it's enforced explicitly in code, so there's nothing left to change here for that reason. **Do not** carry over the octagon erosion from Priority 3 to Shift Edge unless explicitly asked — Shift Edge keeps its own simple choker method by design.

**Still open:** the Shift Edge slider reportedly isn't showing up in the AE effect UI at all during testing (separate from the ordering issue above) — noted but deprioritized for now.

**Planned (user request, 2026-07-06): a Shift Edge method dropdown** instead of silently picking one algorithm — options along the lines of:
- **Genuine min/max** — the current `MorphPlane` brute-force circular filter (exact shape, O(radius²), currently also single-threaded — at radius 20 that's ~1,250 taps/pixel, will be the slowest thing in the plugin once validated; at minimum thread it per-row via `iterate_generic` like everything else, which preserves the exact result).
- **Fast choke** — whatever translates to fastest: octagon decomposition (the `DirectionalMinFilter` infra already generalizes to min OR max — box = H+V passes, octagon = 4 passes) or a van Herk/Gil-Werman running-min/max (O(1) per pixel regardless of radius, exact for box shape).
This supersedes the earlier "do not carry octagon over to Shift Edge" note — the user has since asked for the choice to be exposed rather than made for them. Pairs naturally with the erosion-shape dropdown already in the Backlog (same dispatch structure, possibly the same UI pattern).

## Priority 3 — Threading + performance

**Done (2026-07-03):** Blend Radius's erosion step now uses `OctagonErode` — a 4-direction (horizontal/vertical/both diagonals) decomposition that approximates the circular min-filter via Minkowski-sum composition (exact algebra, not a hack: eroding by 4 line segments in sequence equals eroding once by their combined octagon shape). This changes the cost from O(radius²) to O(radius) and is threaded per-row via AE's `iterate_generic` (`MEB_DirMinRow`), since each directional pass reads from one buffer and writes to a separate one — no shared scratch state needed, unlike the old pre-rebuild Gauss/min-filter threading scaffold. `MorphPlane` (brute-force circular) is untouched and still used by `ShiftEdgeAlpha` (Priority 2), by design.

**Done (2026-07-03):** `GaussBlurPlane` (single-channel; used for the Blend Radius feather pass only, see below) is now threaded via `iterate_generic`, using `thread_local` scratch buffers per row/column callback — not a shared pool indexed by `thread_indexL` (that design was tried first and caused a heap-overflow-style corruption artifact; see git history / conversation for the diagnosis) and not a fresh heap allocation per callback either (safe but measurably slow under concurrent threads — allocators tend to serialize on an internal lock). `thread_local` gives each OS thread one persistent, reused buffer: safe regardless of AE's actual thread count, no repeated allocation.

**Done (2026-07-03) — native AE convolve dropped entirely:** Measured via the timing log on a 4608x3164 frame: `in_data->utils->convolve` took ~400-660ms for the horizontal pass and ~950-1250ms for the vertical pass, for a 9-tap kernel over 14.5M pixels — should be tens of milliseconds. It's a generic public plugin-SDK utility, not necessarily the same code AE's own shipped Gaussian Blur effect uses internally, and the ~2x H-vs-V gap is the classic signature of a cache-unfriendly, non-vectorized implementation. Our own threaded blur is consistently faster. First disabled behind a `MEB_TRY_NATIVE_CONVOLVE` flag, then the whole native-convolve code path (`GaussBlurWorld`, the `PF_EffectWorld`-based wrapper it lived in) was deleted outright once it became unreachable dead code (see next item) — it no longer fits the current architecture anyway, since native convolve assumed premultiplied input and the current pipeline deliberately doesn't premultiply before blurring (see Known working state).

**Done (2026-07-03) — merged 4-channel blur for Blur Amount:** The old `GaussBlurWorld` fallback called `GaussBlurPlane` once per channel (4 sequential single-channel passes: extract channel → blur → write back), which measured ~3-5x slower than Blend Radius's blur despite Blend Radius doing more total work (erosion + feather) — because a Gaussian blur is per-channel-independent, blurring 4 channels sequentially costs ~4x the `iterate_generic` dispatch/join overhead plus the extract/reinsert copies, even though the raw arithmetic is identical either way. Added `GaussBlurRGBA` (own `MEB_BlurRGBARefcon`/`MEB_HBlurRowRGBA`/`MEB_VBlurColRGBA` scaffold, mirroring `GaussBlurPlane`'s but operating on all 4 interleaved channels per row/column callback in one pass) — `SmartRender`'s Phase 2 now calls it directly on a plain float buffer. `GaussBlurPlane` itself is completely untouched and used only by Blend Radius's `weight` blur; the two code paths don't share a function, by design (per explicit request not to touch the Blend Radius blur while doing this).

**Done (2026-07-03) — stripped the `PF_NewWorld` round trip for Blur Amount:** Since native convolve is gone, there's no reason to build a real `PF_EffectWorld` for Phase 2 anymore. `SmartRender` now calls `GaussBlurRGBA` directly on a plain float buffer — no `PF_NewWorld`/`PackRow`/`UnpackWorld`/`PF_DisposeWorld` round trip, saving another ~80-260ms/frame on a 4608x3164 test comp on top of the wins above.

**Black halo — RESOLVED (2026-07-06, user-confirmed in AE, all sources, halo gone, blur shape good):**

**The three root causes, in pipeline order — all three had to be fixed; any one alone left a visible artifact:**

1. **Blur input must be HONESTLY premultiplied** (`rgb *= alpha` at every pixel), not merely hard-zeroed at the alpha=0 cliff. Zero-cliff-only data lets a partial-alpha edge pixel contribute its full-strength colour to the kernel sum while zeroed neighbours contribute black — coverage-disproportionate mixing = dark contamination at every soft edge.
2. **Output colour must be STRAIGHT, never premultiplied.** AE's effect pipeline is straight (unmatted) alpha on BOTH sides: the EDGE DUMP proved the input side (a=0.78 and a=1.0 pixels carry identical RGB), and the output is symmetric — AE multiplies our RGB by our alpha itself at composite time. Returning `colour×alpha` made AE display `colour×alpha²`: a dark fringe on **every** source *including the transparency-grid checkerboard* (that checkerboard observation was the decisive clue — no background colour to blame), plus a visibly "shrunken" blur. So Phase 2 is premultiply → blur → **unpremultiply** (`rgb /= max(blurred_alpha, 1e-4)`) — exactly the divide technique from `BuildCleanSource` (84e713f). The divide is provably safe *on honestly premultiplied input*: `blur(c·a)/blur(a)` is a convex weighted average of true colours, bounded, no amplification — the earlier "glitchy out-of-bounds" divide failure was dividing *straight* (non-premultiplied) data. Note the internal blur data was verified correct via PHASE4 DUMP (constant colour/alpha ratio ~0.19 across the whole edge from a=0.07 to a=1.0) while the screen still showed a halo — the bug was in the handback convention, not the math.
3. **Phase 4's blend must be alpha-weighted, not a plain lerp by w:** `out_rgb = (sh_rgb·w·sh_a + bl_rgb·(1-w)·bl_a) / (w·sh_a + (1-w)·bl_a)` (guard denom ≤ 1e-6 → pass blurred colour). With a plain lerp, Blend Radius's feathered mask gives w a small nonzero value just outside the sharp matte where sharp RGB is the zeroed cliff — mixing black in at weight w = a thin dark line that grows with Blend Radius. The alpha weighting drops the sharp term exactly where its coverage is 0 and reduces to the plain lerp in the interior. (This was chosen over a "1px inner offset" workaround — it's exact at every radius.)

Also fixed: `MakeBlurKernel` sigma mapping radius×0.25 → radius×0.5 with support widened to ceil(3σ) (clipped tails make a blur read smaller than its sigma). The old mapping had been hand-tuned while the halo bug was darkening edges, which faked extra visible width; honest colour exposed it as far too small next to Red Giant Edge Blur.

**Testing pitfalls that burned this session twice — check BEFORE debugging "the fix didn't work":** (a) AE holds a file lock on the deployed `.aex`, so the build's `Copy-Item` deploy step fails (non-fatally, buried in build output) whenever AE is open — **always compare timestamps of `build/ModernEdgeBlur.aex` vs the Plug-ins copy before trusting a test result**; (b) when copying manually, the linker also emits `ModernEdgeBlur.exp` right next to the `.aex` — one manual copy grabbed the wrong file. Both times the "failed fix" was actually a stale build.

---

**Debugging history below kept for reference (how we got there):** Reported symptom: workflow is a layer with 2x Color Key effects (extracting a white cyc) + a Simple Choker (erode a few px so no white survives) + precompose, then Modern Edge Blur on the precomp; later also reproduced on a plain mask and a raw shape layer.

Three attempts, in order:
1. Blur premultiplied RGBA directly (assumed honest premult everywhere). Produced a dark ring on the keyed source, since Color Key + Simple Choker only touch alpha and leave real leftover colour behind alpha=0 (confirmed via pixel dump: alpha exactly `0.0000` with RGB still 0.29-0.77, not zero).
2. Blur straight RGB (no division) then multiply by blurred alpha once. Reverted after the halo reproduced even on a shape layer — the reasoning at the time was that this must double-premultiply an honestly-premultiplied source.
3. "Proper" unpremultiply: divide RGB by alpha before blurring, then re-premultiply after. This produced a *new* symptom — "glitchy edges, values feel out of bounds" — which is exactly what dividing by a small alpha does (e.g. `0.86 / 0.02 ≈ 43×` amplification).

**The actual root cause**, found via the same EDGE DUMP pixel diagnostic on a plain shape layer: AE hands effects **straight/held colour at partial-alpha pixels, not honestly premultiplied data** — confirmed by an alpha=0.78 pixel and an alpha=1.0 pixel showing the *identical* RGB value (not the former scaled to ~78% of the latter). This was true all along, for every source tested, not just the keyed one — the "shape layers/masks are honestly premultiplied" assumption behind attempt 2's revert was itself wrong. Dividing this straight data by alpha (attempt 3) isn't recovering anything — it's actively corrupting already-correct values (0.86/0.78 ≈ 1.1, already over 1.0 before the near-zero cases blow up further).

4. Attempt 2 retested after a clean cache purge (ruling out the stale-cache confound): **halo confirmed on both the shape layer and the keyed source.** This meant the actual bug was never about which premult variant Phase 2 used — all three attempts still forced RGB to exactly 0 wherever alpha=0 (the zero-RGB pass), and then blurred that zeroed data with a flat, alpha-unaware spatial average. That average necessarily mixes real edge colour with hard black from the zeroed region — the same contamination problem the un-zeroed white background caused originally, just black instead of white. Multiplying by blurred alpha afterward (attempts 2/3's structure) doesn't fix this, it compounds it (darkens the estimate further, since it's attenuating an already-contaminated value a second time).

**Fix attempt 4 (2026-07-06):** blur the zeroed data as before, but **divide** the blurred RGB by the blurred alpha instead of multiplying — i.e. `blur(colour×alpha) ÷ blur(alpha)`. For a locally-constant true colour, this recovers that colour exactly regardless of how much of the blur kernel's support falls in the zeroed region, because the division cancels the coverage-dependent attenuation instead of compounding it. This leaves `blurred_buf`'s RGB as the blurred layer's TRUE colour (not premultiplied) rather than a premultiplied value — so Phase 4's composite changed too: it now blends the sharp layer's true colour and the blurred layer's true colour by `w` first, and premultiplies **once**, at the very end, by `out_alpha`. Previously each layer was scaled by its own alpha before blending, which darkens each layer individually wherever its own alpha is low, before the blend even happens — that per-layer premultiply-before-blend was itself part of the bug.

Retested: **improvement, but not fully resolved.** User reports (a) the blur's visible spread is now tighter to the edge than before, and (b) some black fringing still remains. *(Superseded — attempt 4's divide-then-remultiply was later shown to be an algebraic no-op: Phase 4 multiplied by the same blurred alpha the divide had just removed, cancelling back to the raw contaminated value everywhere except near the epsilon clamp. That's why tuning it changed so little. See RESOLVED above for what actually fixed it.)*

**Prior art found in git history, worth reading before re-designing anything:** this exact technique (blur premultiplied colour and alpha separately, divide to recover true colour, guarded epsilon) was already designed (`Architecture.md §4.2`, old `NEXT_STEPS.md` "Priority 1 — Phase 3: Color Dilation") and implemented (`BuildCleanSource` in commit `84e713f`) in a prior session, specifically to kill black fringe, with an explicit acceptance test ("black fringe around the matte boundary should disappear"). It was deleted in the clean-slate rebuild (`7dcceb5`) along with unrelated EdgeExtend/EdgeRepair code, not because it didn't work. Their version only applied the divide-based recovery *outside* the original matte (interior left untouched) — that was a deliberate scoping choice for a spill-cleanup feature and doesn't directly transfer to Blur Amount (which needs to actually soften colour near the interior edge too), but the core math is the same and worth reading (`git show 84e713f:src/ModernEdgeBlur.cpp`, function `BuildCleanSource`).

Separately, commit `7ad1164` ("stable premult blur... ") explicitly removed an earlier unpremult/repremult attempt from the *main* blur pipeline, citing instability "across all bit depths" — that diff doesn't show the actual reverted code (likely never committed), so the exact cause is unknown, but it's a real caution. **Test 8bpc and 16bpc specifically once 32-bit looks right**, since that's the exact axis flagged as broken before.

Also still relevant regardless of outcome: pixel-level inspection (via the temporary EDGE DUMP diagnostic in `SmartRender`, right after `UnpackWorld`) showed the Color-Key+Choker source's leftover data isn't a flat hidden background colour but a ~10px-wide *gradient* — a fossil of the Color Key's own antialiased edge, orphaned by the Choker zeroing alpha for the outer half of it while leaving that gradient's RGB untouched. If the divide-based fix doesn't fully resolve the keyed case specifically (as opposed to the shape-layer case), this gradient is the next thing to dig into.

**Debug infrastructure available for the next session** (all still in the code, see `DebugLog`/`DebugLogTiming`/`DebugLogFlush` near the top of `ModernEdgeBlur.cpp`): an `EDGE DUMP` diagnostic (dumps raw alpha/RGB across the first soft edge found on the middle row, right after `UnpackWorld` — proved the straight-vs-premultiplied question conclusively) and a `PHASE4 DUMP` diagnostic (dumps `w`, sharp/blurred alpha+RGB, and final composited RGBA at that same tracked pixel, after Phase 4 — written to check `w`-vs-`out_alpha` consistency, not yet actually needed since the simpler contamination bug was found first). Both write to `ModernEdgeBlur_debug.log` next to the `.aex`. Useful starting point for the next session rather than re-instrumenting from scratch.

**Debug logging (intentionally kept, 2026-07-03):** `DebugLog`/`DebugLogTiming`/`DebugLogFlush` write a per-phase timing breakdown plus native-convolve-path diagnostics to `ModernEdgeBlur_debug.log`, written next to `ModernEdgeBlur.aex` itself (falls back to `%TEMP%` if that folder isn't writable at runtime) — same location regardless of which Windows user/machine is running AE, so a build can be handed to another PC and still produce a log in a predictable place. Buffered per-thread in memory and flushed once per `SmartRender` call (at the `cleanup:` label) rather than one file open per log line, since repeated file I/O can itself be slow under antivirus/real-time-scanning software and was polluting the very timings being measured. This has proven useful enough during v1.3 testing that it's being kept for now rather than stripped — revisit before an actual release build.

**Done (2026-07-06, second optimisation pass — pipeline restructure, no visual change intended):** Profiling across 97 renders showed ~245ms/frame of *serial* full-buffer memory passes (unpack 64ms, composite 63ms, zero-RGB 25ms, plus untimed memcpy/premult/unpremult/alpha-extract/clamp walks) around ~300ms of actual threaded filtering. Three structural fixes:
1. **Fused threaded ingest/composite** (`MEB_IngestRow`/`MEB_DeriveRow`/`MEB_CompositeRow`/`MEB_PackRow_cb`, dispatched via `MEB_ForEachRow`): unpack + zero-RGB + premultiplied-blur-copy + alpha-plane extraction are now ONE threaded per-row pass; clamp + alpha-weighted blend + pack are another. Unpremultiply is folded into the tail of the blur's own vertical pass (`unpremult` flag on `MEB_BlurRGBARefcon`) — zero extra walks. Shift Edge frames split ingest into unpack-only followed by a post-shift derive pass (the zeroing must key off post-shift alpha, see Priority 2).
2. **Vertical blur passes tiled into column blocks** (`MEB_VBlurColsRGBA` 16 px/block, `MEB_VBlurCols` 64 cols/block): the old one-column-per-callback pattern strode a full row pitch per kernel tap (every tap a cache/TLB miss — the same disease that made native convolve 10-20x slow). Blocks gather into compact row-major scratch; inner loops are contiguous and auto-vectorize.
3. **`thread_local` buffer pool** (`MEB_Pool`: sharp/blurred/weight/alpha + OctagonErode's scratch): SmartRender was allocating ~640MB of fresh pages per 4.6K frame, paying OS commit+zero costs smeared across every phase timing. Grow-once, reused per render thread. Trade-off noted in code: ~0.6GB retained per MFR render thread.
Timing labels changed: `Unpack`→`Ingest`, `ZeroRGB` gone (fused), `Composite` now includes pack. **Perf-verified same day (20 renders, same 4.6K comp): TOTAL avg 597→351ms, worst case 2413→580ms (~4x); BlendRadius 237→134, BlurAmount 166→109 (now inclusive of premult/unpremult), Ingest 62 (was ~110+ split across Unpack/ZeroRGB/hidden copies), Composite 60→46.** Note the two samples aren't perfectly like-for-like (old avg included light frames; all new samples were heavy) — the worst-case ratio is the truer picture. PHASE4 dump confirmed the math is unchanged (flat recovered colour across the edge, no darkening), and the user visually verified in AE same day (halo still gone, blend line still gone, blur width unchanged). Open curiosities, non-blocking: Ingest 62ms looks high for a threaded pass (possibly bandwidth/format-conversion-bound, possibly iterate_generic not fanning out — `MEB_ForEachRow` falls back to serial silently); Shift Edge path restructured but untested (slider still missing from UI, see Priority 2). Next perf levers if ever needed: SIMD intrinsics in the blur inner loops, box-chain blur (see Backlog).

**Backlog (not urgent, future version):** add a dropdown parameter to pick the erosion shape — Box (cheapest) / Octagon (default — current implementation) / Circle (most expensive, exact). Applies to both Blend Radius and, later, Shift Edge. Circle option would need the exact row-decomposition algorithm discussed in conversation (not yet implemented — currently only the brute-force circular and the octagon exist).

## Priority 4 — EdgeRepair suite plugin

A second plugin (`EdgeRepair.aex`) was prototyped in an earlier session but removed in the clean-slate rebuild. It will be reintroduced as a separate `add_meb_plugin` entry in CMakeLists.txt once ModernEdgeBlur v1 is fully validated. The `add_meb_plugin` CMake function already supports multiple targets.

## Priority 5 — GPU scratch budget gate in PreRender (TODO, 2026-08-03)

**Deferred deliberately.** Planned for the session that also revisits MM_FreqSep,
because that plugin already has the thing to copy.

**The problem.** The GPU path asks AE for one scratch block sized to the frame
and the active parameters — 11 floats/pixel at the defaults, 12 with Shift Edge
active (per-buffer table in [Architecture.md](Architecture.md) → "Device memory"):

| Frame | default | + Shift Edge | passthrough |
|---|---|---|---|
| HD 1920×1080 | 91 MB | 100 MB | 8 MB |
| UHD 3840×2160 | 365 MB | 398 MB | 33 MB |
| 4.6K 4608×3164 | 641 MB | 700 MB | 58 MB |

If `AllocateDeviceMemory` refuses, `MEB_RenderCUDA` returns
`PF_Err_OUT_OF_MEMORY` and **the frame fails** — it does not fall back to the CPU
pipeline. It cannot: by the time `PF_Cmd_SMART_RENDER_GPU` runs, AE has already
handed over input and output worlds that live in device memory, and CPU code
cannot address them. The decision has to be taken earlier, in PreRender, where
declining costs nothing.

**The fix.** What MM_FreqSep already does and calls its "budget gate"
(`../../docs/gpu-kernel-rules.md` §6): compute the worst-case scratch size in
PreRender and raise `PF_RenderOutputFlag_GPU_RENDER_POSSIBLE` only when it fits a
budget. Infeasible frames then render on the CPU — slower, never wrong. Two
properties of that version are worth copying rather than reinventing:

- **The gate and the render must run the SAME size calculation**, so they cannot
  disagree about whether a frame fits. Here that means calling `MEB_BuildLayout`
  from PreRender too — it already takes nothing but `W`, `H`, four booleans and
  the two kernel sizes, so it is a lift, not a rewrite.
- MM_FreqSep budgets **¼ of dedicated VRAM, clamped to [256 MB, 1 GB]**. That
  fraction is a judgement call rather than a measurement, which is the main
  reason this is deferred instead of guessed at now.

**Measure before picking a fraction:** whether AE's `AllocateDeviceMemory`
already degrades gracefully under pressure, or whether it competes with AE's own
frame cache. The answer decides whether ¼ of VRAM is generous or reckless.

**Why it is not urgent.** On the test box (RTX 3070 Ti, 8 GB) even 4.6K with
Shift Edge is ~700 MB, comfortable. It matters for 8K comps, laptop GPUs, and
comps stacking several GPU-accelerated effects.

## Known working state (v1.2 / v1.3-in-progress)

- Blur Amount: matches Red Giant Edge Blur A/B at equivalent values. As of 2026-07-03, blurs a plain float buffer directly via `GaussBlurRGBA` (`iterate_generic` + `thread_local` scratch, all 4 channels in one pass) — no AE world, no native `convolve` call at all (removed; measured 10-20x slower, see Priority 3).
- Blend Radius: visibly softens edge, linear response, 0.01 precision. Erodes the sharp/blurred blend weight inward (Priority 1, done); output alpha shape itself is still driven only by Blur Amount.
- Shift Edge: slider present in UI (reportedly not visible in AE's effect panel during testing — open issue, see Priority 2), runs when non-zero.
- Premult handling (2026-07-06, **CONFIRMED WORKING in AE — halo gone on all sources incl. transparency grid**): `sharp_buf` stays straight/held colour with RGB explicitly zeroed only wherever alpha is exactly 0 (right after Shift Edge). Blur Amount takes a copy, **premultiplies it honestly** (`rgb *= alpha`, every pixel), blurs via `GaussBlurRGBA`, then **unpremultiplies** (`rgb /= max(blurred_alpha, 1e-4)`) — blurred layer ends up as straight colour. Phase 4 does the **alpha-weighted blend** `(sh·w·sh_a + bl·(1-w)·bl_a)/(w·sh_a + (1-w)·bl_a)` and outputs straight colour + blurred alpha, with **no alpha multiply anywhere** — AE applies alpha itself at composite time. See Priority 3 "Black halo — RESOLVED" for the three root causes and why every earlier variant failed.
- Position shift: still fine (no known issue). Dark halo: **RESOLVED** (2026-07-06, user-confirmed).
- Blur kernel: `sigma = radius × 0.5`, support `ceil(3σ)` (2026-07-06). User-confirmed "blur shape looks good"; fine-tune the ×0.5 factor only against a Red Giant Edge Blur A/B if needed.
- Passthrough at Blur=0 / Blend=0 preserves held RGB.
- 8/16/32 bpc clean (predates the 2026-07-06 unpremult pipeline — **re-verify 8bpc and 16bpc specifically**: commit `7ad1164` once reverted an unpremult attempt citing instability "across all bit depths", so that axis is the one to watch; halo fix was confirmed at project bit depth only).
- Loads and runs on 6K comps without crashing.
