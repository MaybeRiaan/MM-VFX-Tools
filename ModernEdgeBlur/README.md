# Modern Edge Blur

An After Effects plugin that erodes matte edges with a soft, alpha-driven blur, revealing a blurred copy of the source beneath the sharp interior. Designed as a modern replacement for Red Giant's discontinued "Edge Blur" plugin and a CPU-side alternative to Nuke's `EdgeExtend`. See [Architecture.md](Architecture.md) for the design and [NEXT_STEPS.md](NEXT_STEPS.md) for the open work list.

Status: alpha. Renders in 8/16/32 bpc, MFR-safe, blur passes parallelised via AE's `iterate_generic`. Color dilation (the dark-fringe fix) is still on the roadmap.

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

The post-build step installs `ModernEdgeBlur.aex` to `C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\Plug-ins\`. The current user must have write access there (a one-time `icacls` grant covers this — no elevation needed afterwards), and AE must be closed (else the .aex is file-locked and the copy fails).

To build with the inner Gaussian blur forced single-threaded — useful for A/B regression — pass `-DMODERN_EDGE_BLUR_THREADED=0` at configure time. Switch back with `=1`.

The plugin appears in AE under **Effect → MM → Modern Edge Blur**.

## Versioning

Versions are defined in [src/ModernEdgeBlur.h](src/ModernEdgeBlur.h) and must be kept in sync with [resources/ModernEdgeBlurPiPL.r](resources/ModernEdgeBlurPiPL.r). A mismatch produces AE error 8800b at load time. The `STAGE_VERSION` define is especially load-bearing — the C++ side uses the `PF_Stage_*` enum and the PiPL .r file uses literal integers. The enum values are: `PF_Stage_DEVELOP = 0`, `PF_Stage_ALPHA = 1`, `PF_Stage_BETA = 2`, `PF_Stage_RELEASE = 3` (the STAGE field is only 2 bits wide).
