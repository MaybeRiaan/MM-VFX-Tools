# Modern Edge Blur

One plugin, one CMake project. Until 2026-08-02 this project also built **Edge
Repair**; the two now live apart, in [../EdgeRepair](../EdgeRepair). They shared
a build system but never shared code — Modern Edge Blur's sources include
nothing but their own header and the STL, which is what made the split
mechanical.

## Modern Edge Blur

An After Effects plugin that softens the edges of keyed or masked layers with a Gaussian blur, while keeping the interior sharp. Replacement for Red Giant's discontinued Edge Blur plugin.

**Effect → MM → Modern Edge Blur**

## What it does

- Blurs a copy of the layer as a soft background plate (Blur Amount)
- Composites the original sharp layer on top
- Blend Radius controls how wide the soft edge transition is — eroding the sharp layer's alpha inward to reveal the blurred plate underneath
- At Blur Amount = 0 / Blend Radius = 0: pure passthrough, no processing
- **GPU accelerated (CUDA)** when After Effects offers it, on the same pipeline —
  see [docs/Architecture.md](docs/Architecture.md#gpu-path-added-2026-08-03).
  AE chooses per frame; the CPU path is the always-correct fallback and is
  unchanged by the port.

## Parameters

| Slider | Default | Description |
|--------|---------|-------------|
| Shift Edge | 0 | Grow (+) or shrink (−) the alpha boundary before processing. Stubbed in v1 — reserved for v2. |
| Blur Amount | 4.0 | Gaussian blur radius of the background plate. Uses AE's internal blur. |
| Blend Radius | 4.0 | Width of the soft edge blend. Higher values erode the sharp layer further inward. |

## Prerequisites

- Windows 10/11
- Visual Studio 2022 or 2026 (C++ workload)
- CMake 3.20+
- Adobe After Effects 2026
- AE Plug-in SDK (from [Adobe Developer](https://developer.adobe.com/after-effects/))

## Setup

Nothing to copy. The build uses one shared AE SDK install for the whole suite
via the `AE_SDK_ROOT` cache var (default
`E:/OFX DEV/AE SDK/AfterEffectsSDK_25.6_61_win/.../Examples`); pass
`-DAE_SDK_ROOT=<path>` if yours lives elsewhere. `PiPLtool.exe` is taken from
the SDK's own `Resources/`. SDKs are never committed — the license prohibits
redistribution.

Optional for the GPU path: the **CUDA Toolkit** (nvcc), needed only to build the
PTX. Without it the build skips that step, `GPU_DEVICE_SETUP` declines quietly
at runtime, and AE renders on the CPU exactly as before. The host side needs no
toolkit — it loads `nvcuda.dll` (which ships with the NVIDIA driver) at runtime.

## Build

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The post-build step also installs `CUDA_Assets/ModernEdgeBlur_Kernels.ptx` next
to the `.aex` — AE loads kernel assets from beside the plugin, so a deploy that
copies only the `.aex` leaves a stale or missing PTX and the GPU path declines.

## Tests

```powershell
.\tests\build_tests.ps1
```

Headless, no AE SDK and no GPU: compiles both the CPU math (`src/core/`) and the
GPU kernel math (`src/gpu/`) as host C++ and diffs them with `==`. Run it after
any change to a formula in either — that is the only thing standing between a
one-line edit and a silent CPU/GPU divergence.

Post-build step installs `ModernEdgeBlur.aex` to:
`C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\Plug-ins\`

Grant write access once with:
```powershell
icacls "C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\Plug-ins" /grant "${env:USERNAME}:(OI)(CI)F"
```

AE must be closed during install (file lock).

## Version sync

`MAJOR/MINOR/BUG/STAGE/BUILD_VERSION` must match between `src/ModernEdgeBlur.h` and `resources/ModernEdgeBlurPiPL.r`. Mismatch → AE error 8800b at load. `PF_Stage_DEVELOP = 0` (not 1).

See [docs/Architecture.md](docs/Architecture.md) for full technical details, and
[docs/NEXT_STEPS.md](docs/NEXT_STEPS.md) for the working log — note that its
"next session" list dates from 2026-07-06 and predates later work.
