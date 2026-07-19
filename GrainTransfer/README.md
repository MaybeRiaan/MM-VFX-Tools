# Grain Transfer (GrainMatch)

After Effects SmartFX plugin: transfers the measured grain of a source plate
onto the target layer — ghost-free and seam-free by construction, analyze
once, trivially cheap per frame. The lean replacement for AE Match Grain's
cost model.

- **Docs / architecture / status:** [docs/GrainTransfer.md](docs/GrainTransfer.md)
- **Original brief:** [MatchGrain_Starter_Brief.md](MatchGrain_Starter_Brief.md)

## Build

```powershell
./build.ps1            # bump build version, compile, deploy to AE 2026
./build.ps1 -Clean     # wipe build dir first
```

Requires VS 2022+ (VS 2026/Ninja detected automatically), CMake, and the AE
SDK at `E:\OFX DEV\AE SDK\...` (override with `-DAE_SDK_ROOT=`; the SDK is
referenced by path and never committed).

## Tests

```powershell
./tests/build_tests.ps1   # headless DSP gates + real-EXR smoke (no AE)
```

PNG diagnostics land in `tests/out/`. `tests/ae_smoke.jsx` is a 2-minute
in-AE smoke script (File > Scripts > Run Script File).

## Third-party

- `third_party/kissfft` — BSD-3-Clause (plugin)
- `tests/third_party` — stb_image_write, tinyexr, miniz (tests only, not
  linked into the plugin)
