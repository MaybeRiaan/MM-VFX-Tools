# Modern Edge Blur

An After Effects plugin that detects high-contrast alpha transitions and applies a weighted blur to soften matte edges without blurring the interior of the image. See [Architecture.md](Architecture.md) for design details.

Status: early development. Builds and loads in AE 2026; effect logic is in progress.

## Prerequisites

- Windows 10/11 with Visual Studio 2022 or 2026 (C++ workload)
- CMake 3.20+
- Adobe After Effects Plug-in SDK (download from [Adobe Developer](https://developer.adobe.com/after-effects/))

## Setup

1. Clone the repo.
2. Copy the AE SDK contents into `sdk/`:
   - `sdk/Headers/` from the SDK's `Examples/Headers/`
   - `sdk/Util/` from the SDK's `Examples/Util/`
3. Copy `PiPLtool.exe` from the SDK's `Examples/Resources/` into `resources/` (as `PiPLTool.exe`, capital T).

These paths are gitignored because Adobe's SDK is not redistributable.

## Build

```powershell
mkdir build
cd build
cmake .. -A x64
cmake --build . --config Release
```

The post-build step installs `ModernEdgeBlur.aex` to `C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\Plug-ins\` — this requires the build to run with Administrator privileges and AE must be closed.

The plugin appears in AE under **Effect → MM → Modern Edge Blur**.

## Versioning

Versions are defined in [src/ModernEdgeBlur.h](src/ModernEdgeBlur.h) and must be kept in sync with [resources/ModernEdgeBlurPiPL.r](resources/ModernEdgeBlurPiPL.r). A mismatch produces AE error 8800b at load time. The `STAGE_VERSION` define is especially load-bearing — the C++ side uses the `PF_Stage_*` enum and the PiPL .r file uses literal integers. The enum values are: `PF_Stage_DEVELOP = 0`, `PF_Stage_ALPHA = 1`, `PF_Stage_BETA = 2`, `PF_Stage_RELEASE = 3` (the STAGE field is only 2 bits wide).
