# MM FreqSep — "Skin Retouch"

Matte-driven frequency separation for After Effects (C++ AE SDK, SmartFX).
Splits a layer into low (tone/color) and high (texture/detail) bands,
processes each, recombines, and blends back over the source. Part of the MM
suite (Category **MM**, effect name **Skin Retouch**, match name
`MM Skin Retouch`).

Full plan: [docs/mm-freqsep-plan.md](docs/mm-freqsep-plan.md).
Testing & manual checklist: [docs/testing.md](docs/testing.md).

## Status

- **Phase 1 (scaffold)** — done: loads in AE, passthrough render, all 11
  params registered.
- **Phase 2 (CPU reference render)** — done: 32-bit-float internal pipeline,
  alpha-weighted separable Gaussian, split/recombine, downsample-scaled
  radius, View popup (Result / Low / High / Matte).
- **Phase 3 (matte driver)** — done: Matte Layer checked out in PreRender under
  its own checkout id, sampled per output pixel (bilinear), channel selection
  (alpha/luma ± invert, luma **coverage-gated** = `luma(RGB)·alpha`),
  Fit-to-Layer stretch mapped in **full-res layer space** (resolution- and
  tile-invariant; Center/Top-Left also implemented). No layer → matte = 1.0.
  Matte modulates the final blend (`out = lerp(src, proc, matte·mix)`) and
  drives the Matte view. (A "Use Layer Masks" option is deferred — the external
  Matte Layer with a masked driver covers the same workflow.)
- Phases 4+ (guided filter, Color Only, Working Curve, GPU) — not started; code
  is structured so they slot in (`curve.cpp` is a stub; `Separation Method`
  popup routes non-Gaussian to the Gaussian fallback).

## Layout

```
MM_FreqSep/
  docs/       mm-freqsep-plan.md, testing.md
  src/        main.cpp          entry points, GlobalSetup, ParamsSetup
              params.h          param indices/disk IDs/ranges, SDK includes
              smart_render.cpp  PreRender inflation + SmartRender pipeline
              blur.cpp/.h       separable Gaussian over ARGB float
              fs_core.h         AE-free band math (shared with tests)
              matte.cpp/.h      matte layer sampling: channel + Fit + bilinear
              curve.cpp/.h      Working Curve stub (identity)
              common/           MM_Common.* (row pack/unpack, ForEachRow)
  resources/  MM_FreqSepPiPL.r, Info.plist (mac)   [+ PiPLTool.exe, not in git]
  sdk/        AE SDK  (NOT in git — see "SDK setup" below)
  tests/      identity_test.cpp, run_tests.ps1
  build.ps1   Windows build + deploy
  CMakeLists.txt
```

## SDK setup (required before building)

The Adobe After Effects SDK is **not included** in this repo — its license
prohibits redistribution. Download it (free) from the
[Adobe After Effects Developer Center](https://developer.adobe.com/after-effects/)
and populate two paths from the SDK's `Examples/` folder:

1. Copy `Examples/Headers`, `Examples/Util`, and `Examples/Resources` into a
   local `sdk/` folder here, so you have `sdk/Headers`, `sdk/Util`,
   `sdk/Resources`.
2. Copy `Examples/Resources/PiPLtool.exe` to `resources/PiPLTool.exe` (Windows
   build only).

Both `sdk/` and `resources/PiPLTool.exe` are gitignored. This project was built
against **AE SDK 25.6** (win, build 61).

## Build (Windows)

Requires Visual Studio 2022+ with the C++ toolset (VS 2026 uses the bundled
Ninja automatically). Complete **SDK setup** above first.

```powershell
.\build.ps1            # configure + build + deploy .aex to AE 2026 plugins
.\build.ps1 -Clean     # wipe build/ first
```

The `.aex` deploys to
`C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\Plug-ins`
(override with `-DAE_PLUGINS_DIR=...` at configure time).

## Build (macOS)

The CMake `APPLE` branch produces a `.plugin` bundle with a Rez-compiled PiPL
(needs Xcode command-line tools). **Not compiled/verified on the Windows dev
machine** — wired up for a future Mac build.

## Test

```powershell
cd tests
.\run_tests.ps1        # AE-free core tests; exit 0 = pass
```
