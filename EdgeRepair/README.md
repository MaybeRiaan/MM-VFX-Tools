# Edge Repair

Edge extension and decontamination for keyed/masked layers: rebuild plausible
colour outside the matte edge, so a soft or shrunken alpha does not drag the
background in with it.

**Effect → MM → Edge Repair** (match name `MM Edge Repair` — load-bearing, see
below).

Split out of the `ModernEdgeBlur` project on 2026-08-02. The two plugins shared
one CMake project from the day Edge Repair was started, but never shared code —
`src/common/` was only ever included by `EdgeRepair.cpp`. They are now fully
independent.

## Status

All six build stages done, including the CUDA GPU port: the full pipeline —
core matte, JFA+2 distance, pull–push fill, grain-amplitude reduction,
composite and all four view modes — runs on GPU, mirroring the CPU pipeline
pass for pass.

**Edge Repair is the suite's reference implementation for GPU work.** Its GPU
path is the only one written after the After Effects host mechanism was
*measured* rather than inferred, and the only one AE has actually GPU-rendered.
See [../docs/AE_GPU_Effects.md](../docs/AE_GPU_Effects.md) §1.1–1.2 for that
decision and what it means for the other plugins.

## Build

```powershell
.\build.ps1                    # bump build version, compile, deploy to AE
.\build.ps1 -NoBuildIncrement  # same without touching the version
.\build.ps1 -Clean             # wipe build/ first
```

Or build the whole suite from the repo root with `..\build.ps1`.

The AE SDK is not in this repo (its license prohibits redistribution). One
shared install serves the suite via the `AE_SDK_ROOT` cache var — pass
`-DAE_SDK_ROOT=<path>` if yours is elsewhere. Nothing needs restoring by hand.

## Tests

```powershell
.\tests\build_tests.ps1
```

Seven headless test files, no AE and no GPU required. The important one is
`test_gpuparity.cpp`: `src/gpu/ER_KernelMath.h` compiles as HLSL, CUDA, Metal
**and host C++**, so the tests call the exact functions the GPU runs and diff
them against their `MM_*.h` CPU twins with `==`, not an epsilon. It tests the
multi-pass *composition* too, not just per-pixel formulas — an off-by-one in a
mip loop sails straight through per-pixel checks.

## GPU backends

| Backend | State |
|---|---|
| CUDA | **The maintained path.** Full pipeline. What AE offers on Windows/NVIDIA. |
| DirectX 12 | Passthrough milestone; serves Premiere hosts. Kept partly because `fxc` type-checks the whole shared kernel header on every build. |
| Metal | Kernel source exists, not built (Mac-only toolchain). |

CUDA needs no toolkit at runtime (`src/gpu/ER_CudaShim.h` loads `nvcuda.dll`);
only the `.cu` → PTX build step needs `nvcc`, and CMake skips it gracefully when
absent — the plugin then declines CUDA at runtime and AE falls back to CPU.

The optional Premiere GPUVideoFilter module (`-DEDGEREPAIR_PREMIERE_GPU=ON`) is
OFF by default, unused by After Effects, and needs the separate Premiere SDK
under `sdk/Premiere/`.

## Version sync

`MAJOR/MINOR/BUG/STAGE/BUILD_VERSION` must match between `src/EdgeRepair.h` and
`resources/EdgeRepairPiPL.r`. AE packs both into one integer and rejects the
plugin on any mismatch, reporting only `MAJOR.MINOR` — so its error shows two
identical-looking versions. The suite CMake now catches this at configure time;
`build.ps1` keeps both files in step when it bumps.

**The PiPL match name is load-bearing.** Existing AE projects reference effects
by match name, so `MM Edge Repair` must not change — only display name, menu
strings and folder may.

## Docs

[docs/EdgeRepair.md](docs/EdgeRepair.md) is the authoritative record: full
spec, the staged build log, every measurement, and the negative results worth
not repeating.
