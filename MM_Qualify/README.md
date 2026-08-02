# MM Qualify — "Qualify"

The DaVinci Resolve HSL qualifier, faithfully rebuilt inside After Effects
(C++ AE SDK, SmartFX, GPU-first). Part of the MM suite (Category **MM**,
effect name **Qualify**, match name `MM Qualify`).

Three independent 1D gates (Hue / Saturation / Luma), ANDed, each with a
uniform **In · Out · In Soft · Out Soft** control set — plus Resolve's
matte-finesse chain, a temporal-stabilize module, and a garbage matte.

Full plan: [docs/hsl_keyer_plugin_plan.md](docs/hsl_keyer_plugin_plan.md).
Testing process: [docs/testing.md](docs/testing.md).

## Why not just use Resolve

Two answers, both structural:

1. **Every gate parameter is a standard AE float/angle param** → natively
   keyframeable and expression-drivable. Animated qualifier ranges (sunset
   skies, exposure rides, expression-linked gates) at zero extra cost.
2. **Temporal Stabilize** — an asymmetric attack/release IIR on the matte.
   Resolve has nothing equivalent; it holds a moving key without sizzle.

## Features (v1 — Resolve parity, nothing more)

- **Qualifier:** Hue (seam-free wraparound — `In > Out` crosses 0° so reds
  behave naturally), Saturation, Luma (HDR-aware, selectable Rec.709/2020/
  601/Average weights). HSV or HSL model.
- **Qualify In** (`As-Is / Gamma 2.4 / Log / ACEScct`): transforms the
  *qualification path only* — the image is never touched. ACEScct gives
  linear-working-space users literally Resolve's ACES-timeline qualification
  behavior. Fused inline, zero perf cost.
- **Sampling:** eyedropper with **Set / Add / Subtract** range picking
  (Resolve's +/− pickers), through the same qualification math as the render.
- **Matte Finesse** (fixed Resolve chain order): Denoise pre-filter · Clean
  Black · Clean White · Black/White Clip · Blur Radius · In/Out Ratio.
- **Temporal Stabilize** (toggle + Attack/Release): warm-started IIR, stateless
  and MFR-safe.
- **Garbage Matte** (layer + channel): intersected before cleanup.
- **Output:** Original + Alpha (keyed) · Matte · Matte Inverted · Highlight
  (Resolve-style flat-grey preview) · Qualify Raw (pre-cleanup).
- **GPU (DirectX 12)** with CPU fallback; 32-bit float throughout, 8/16-bit
  promoted on unpack and clamped only on the final pack.

## Architecture

The load-bearing idea: **one shared per-pixel core** (`src/qk_core.h`) is
compiled by *both* the CPU render and the HLSL GPU kernels. Gates, transforms,
cleanup, IIR, garbage channel and compose are written once, so CPU/GPU parity
is a property of the build rather than a hand-kept promise. This extends MM
FreqSep's "platform-free kernels" rule (`../docs/gpu-kernel-rules.md` §0) one
step further — platform-free *and* language-shared.

```
MM_Qualify/
  docs/       hsl_keyer_plugin_plan.md, testing.md
  src/        main.cpp          entry points, GlobalSetup, all 45 params
              params.h          indices/disk IDs/ranges, popup→gates resolve
              smart_render.cpp  PreRender inflation + CPU pipeline + GPU job
              sampling.cpp      eyedropper Set/Add/Subtract range picking
              qk_core.h         SHARED per-pixel math (CPU + HLSL)
              qk_cpu_prologue.h C++ spellings of the qk_core vocabulary
              blur.{h,cpp}      separable Gaussian (ARGB premult + f1 matte)
              garbage.{h,cpp}   garbage-matte layer sampling (Fit-to-Layer)
              qk_common.*      row pack/unpack, ForEachRow (this plugin only)
              gpu/
                qk_gpu_host.h   platform-free tile planner + QK_GPUJob
                mm_gpu_dx.cpp   DX12 host (root sig, PSOs, dispatch, fence)
                kernels/*.kernel   platform-free kernel bodies (math only)
                kernels/qk_prologue_hlsl.h   HLSL vocabulary
                dx/*.hlsl       2-line shims: prologue + kernel, one per entry
  tests/      core_test.cpp, pipeline_test.cpp, run_tests.ps1
  resources/  MM_QualifyPiPL.r, Info.plist (mac)
  build.ps1   Windows build + deploy
  CMakeLists.txt
```

## SDK setup (required before building)

The Adobe After Effects SDK is **not included** — its license prohibits
redistribution. Download it (free) from the
[Adobe After Effects Developer Center](https://developer.adobe.com/after-effects/)
and unzip it anywhere. One shared install serves the whole suite (PiPLtool.exe
included — it ships in the SDK's `Resources/`); nothing is copied into the
project.

CMake expects the SDK's `Examples/` directory at the `AE_SDK_ROOT` cache
variable — default `E:\OFX DEV\AE SDK\AfterEffectsSDK_25.6_61_win\...\Examples`;
pass `-DAE_SDK_ROOT=<path>` if yours lives elsewhere. Built against
**AE SDK 25.6** (win, build 61).

## Build (Windows)

Visual Studio 2022+ with the C++ toolset (VS 2026 uses the bundled Ninja).
Complete **SDK setup** first.

```powershell
.\build.ps1            # configure + build + deploy .aex to AE 2026 plugins
.\build.ps1 -Clean     # wipe build/ first
```

Deploys to
`C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\Plug-ins`
(override with `-DAE_PLUGINS_DIR=...` at configure time). The GPU kernels are
compiled at build time by `dxc` (from the Windows SDK) into DXIL embedded as
C headers — no runtime shader compiler, no loose `.cso`, a single
self-contained `.aex`.

## Build (macOS)

The CMake `APPLE` branch produces a `.plugin` bundle with a Rez-compiled PiPL.
**Not compiled/verified on the Windows dev machine.** The Metal GPU host is
not yet written; a Mac build renders on the CPU reference path (the
platform-free kernels + `qk_gpu_host.h` are structured so a Metal host is a
mechanical port — see `../docs/gpu-kernel-rules.md` §4).

## Test

```powershell
cd tests
.\run_tests.ps1        # AE-free core + pipeline-parity tests; exit 0 = pass
```

105 tests: 95 core-math (incl. the hue seam), 10 tiled-vs-untiled pipeline
parity at `0.000e+00`. See [docs/testing.md](docs/testing.md) for the full
process including the in-AE checklist.

## Status

- **Phase 0 (scaffold)** — done: SmartFX GPU + CPU, MM color infra, seam-free
  hue math with unit tests.
- **Phase 1 (gates + sampling)** — done: three gates, eyedropper Set/Add/
  Subtract, Output popup, Highlight view. Builds, deploys, loads.
- **Phase 2 (matte cleanup)** — done: full finesse chain, temporal toggle,
  Qualify Raw view, garbage matte.
- **Phase 3 (polish & ship)** — GPU path implemented and parity-proven
  offline; remaining: the in-AE hands-on pass (`docs/testing.md` §2), presets,
  and the optional custom-ECW spectrum widget.

## Shared-engine ledger

- Qualifier core + Skin preset ↔ BlemishRepair constrain stage
- Raw-buffer eyedropper + color infra ↔ SkinRemap / MasterFilm
- ACEScct constants ↔ GrainMatch `GT_Colour.h`
- Tile planner + DX12 host template ↔ MM FreqSep `mm_gpu_host.h`
