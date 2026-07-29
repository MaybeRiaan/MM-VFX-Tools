# MM Qualify — Testing Process

Two layers, in order of authority — the same discipline as MM FreqSep
(`docs/gpu-kernel-rules.md` §5). Automated tests are the regression gate;
the in-AE checklist is a development-time confidence pass that needs a live
AE host and GPU.

---

## Layer 1 — Automated, AE-free (the regression gate)

```powershell
cd tests
.\run_tests.ps1      # exit 0 = all pass
```

Compiles and runs two suites against the vendored SDK headers (for the
`A_long`/`PF` typedefs only) — no AE host, no GPU device, no Mac required.

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

### `pipeline_test.cpp` — tiled-vs-untiled parity
10 end-to-end cases. Two **independent** implementations of the whole chain
(denoise → gates+cleanup → temporal IIR → matte blur → ratio+compose) run on
the same synthetic multi-frame input:

- an **untiled** CPU reference (whole frame at once), and
- a **tiled** pipeline driven by the **real** tile planner
  (`qkgpu::QK_PlanTiles`) — the same code `mm_gpu_dx.cpp` uses to cut bands,
  compute per-band margins, and offset the compose addressing.

Both call the same `qk_core.h` math (the test proves the *tiling and data
movement*, which is where a hand-written GPU host diverges — not the pixel
math, which layer 1 already covers). Cases sweep denoise/blur/temporal/
garbage on-off, all five output modes, an interior + world-edge band split,
and HDR (superwhite) input.

**Current result: 105/105 pass. Every pipeline case at `max-abs = 0.000e+00`**
(identical float ops in identical order). Consequence: band margins, offsets,
the compose addressing, the matte stages and the garbage channel math are
*proven* before AE runs — so any in-AE GPU mismatch is D3D12 plumbing
(bindings, barriers, sync) by elimination, exactly as with MM FreqSep.

Known gap (accepted, same as MM FreqSep): layer 1 does not exercise the
D3D12 API layer itself — that needs a live AE-hosted device (layer 2).

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
1. Project Settings ▸ Video Rendering and Effects ▸ **Mercury GPU
   Acceleration (DirectX)**. Note render; then switch to **Software Only** and
   compare the same frame across several parameter states (a keyed result, a
   *Matte* view, denoise+blur+temporal all on).
2. They should match to display precision. Layer 1 already proves the tiling
   math is bit-exact; a visible mismatch here points at the D3D12 host
   (`src/gpu/mm_gpu_dx.cpp`) — bindings, barriers, or the BGRA↔ARGB swizzle —
   not the algorithm.
3. **6K+ / large-radius stress** (the usage profile): a 6K comp with Denoise
   and Blur Radius high. Confirm it renders (the PreRender budget gate may
   route pathological tiles to the CPU — slower, never wrong; that is by
   design, not a bug).

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
