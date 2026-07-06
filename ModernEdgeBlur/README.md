# Modern Edge Blur (MM plugin suite)

This repo builds a small suite of After Effects "MM" category plugins from one CMake project: **Modern Edge Blur** (documented below, shipped/working) and **Edge Repair** (in progress — edge extension/decontamination; see [docs/EdgeRepair.md](docs/EdgeRepair.md) for its full spec and staged-build status). The two are independent plugins that happen to share a build system and a small common helper lib (`src/common/`) — Modern Edge Blur's own plugin code is never modified for Edge Repair's sake.

## Modern Edge Blur

An After Effects plugin that softens the edges of keyed or masked layers with a Gaussian blur, while keeping the interior sharp. Replacement for Red Giant's discontinued Edge Blur plugin.

**Effect → MM → Modern Edge Blur**

## What it does

- Blurs a copy of the layer as a soft background plate (Blur Amount)
- Composites the original sharp layer on top
- Blend Radius controls how wide the soft edge transition is — eroding the sharp layer's alpha inward to reveal the blurred plate underneath
- At Blur Amount = 0 / Blend Radius = 0: pure passthrough, no processing

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

1. Clone the repo.
2. Copy AE SDK files into `sdk/`:
   - `sdk/Headers/` ← SDK `Examples/Headers/`
   - `sdk/Util/` ← SDK `Examples/Util/`
3. Copy `PiPLTool.exe` from SDK `Examples/Resources/` into `resources/`.

SDK files are gitignored (not redistributable).

## Build

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Post-build step installs `ModernEdgeBlur.aex` to:
`C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\Plug-ins\`

Grant write access once with:
```powershell
icacls "C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\Plug-ins" /grant "${env:USERNAME}:(OI)(CI)F"
```

AE must be closed during install (file lock).

## Version sync

`MAJOR/MINOR/BUG/STAGE/BUILD_VERSION` must match between `src/ModernEdgeBlur.h` and `resources/ModernEdgeBlurPiPL.r`. Mismatch → AE error 8800b at load. `PF_Stage_DEVELOP = 0` (not 1).

See [Architecture.md](Architecture.md) for full technical details.
