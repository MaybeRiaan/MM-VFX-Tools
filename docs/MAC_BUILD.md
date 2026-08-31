# Building the suite on macOS

**What this gets you: all five plugins loading in After Effects on a Mac and
rendering on the CPU path.** Not a degraded build — the CPU pipeline is the
reference every GPU number in this suite is measured *against*, so a Mac render
is the correct render by definition. What it does not get you is GPU
acceleration: there is no Metal host yet (`gpu-kernel-rules.md` §4), and CUDA is
Windows/NVIDIA. Every plugin declines the device and takes the reference path.

> **Nothing in this document has been executed on a Mac.** It was written from
> the Windows side, against the SDK's own Xcode sample layout, after making the
> five projects capable of producing a Mac target. Treat the first build as a
> debugging session, not a formality — and see [First build: what to expect when
> it breaks](#first-build-what-to-expect-when-it-breaks).

---

## 1. Prerequisites

| | | |
|---|---|---|
| **Xcode** + command-line tools | `xcode-select --install` | Supplies clang and **Rez**, which compiles the PiPL resource. `Rez` must be on `PATH` — the CMake branch does `find_program(REZ_TOOL Rez REQUIRED)` and fails the configure if it is not. |
| **CMake ≥ 3.20** | `brew install cmake` | Same minimum the Windows build uses. |
| **After Effects SDK 25.6** (win/mac are the same download content) | Adobe developer site | Not in this repo — its licence prohibits redistribution. |
| **After Effects 2026** | | The deploy target. |

The SDK path is a cache variable, exactly as on Windows:

```bash
export AE_SDK_ROOT="$HOME/AE SDK/AfterEffectsSDK_25.6_61/Examples"
```

There is no CUDA toolkit requirement on the Mac — the nvcc step lives inside
the Windows branch of each `CMakeLists.txt`.

## 2. Build

Whole suite, from the repo root:

```bash
cmake -S . -B build-mac -DAE_SDK_ROOT="$AE_SDK_ROOT"
cmake --build build-mac -j
```

One plugin only:

```bash
cmake -S . -B build-mac -DAE_SDK_ROOT="$AE_SDK_ROOT" \
      -DMM_BUILD_GRAINTRANSFER=OFF -DMM_BUILD_EDGEREPAIR=OFF   # etc.
```

Each project also still configures standalone (`cmake -S EdgeRepair -B er-mac`),
the same property the Windows build has.

`build.ps1` is **Windows-only** — it drives MSVC and PiPLTool.exe. The two
commands above are its Mac equivalent; there is deliberately no `build_mac.sh`
yet, because a wrapper written blind would be one more thing to debug on the
first Mac session.

## 3. Where it installs

Each project's `POST_BUILD` copies its `.plugin` bundle to:

```
/Applications/Adobe After Effects 2026/Plug-ins
```

Override for every project at once, the same way the Windows build does:

```bash
cmake -S . -B build-mac -DAE_PLUGINS_DIR="$HOME/ae-sandbox/Plug-ins" ...
```

If AE lives elsewhere, or the copy hits a permissions wall, pass that flag and
copy the bundle by hand — the build itself is unaffected.

## 4. What each plugin produces

| | Bundle | Effect name in AE (Effect ▸ MM ▸ …) |
|---|---|---|
| EdgeRepair | `EdgeRepair.plugin` | Edge Repair |
| ModernEdgeBlur | `ModernEdgeBlur.plugin` | Modern Edge Blur |
| GrainTransfer | `GrainTransfer.plugin` | Grain Transfer |
| MM_Qualify | `MM_Qualify.plugin` | Qualify |
| MM_FreqSep | `MM_FreqSep.plugin` | Skin Retouch |

Universal (`x86_64;arm64`) in every case, so one build serves Apple Silicon and
Intel.

## 5. First build: what to expect when it breaks

The portable parts are genuinely portable and were audited before this document
was written: **all threading goes through AE's own `iterate_generic`** (no
`std::thread`, no OpenMP), there are **no SSE/AVX intrinsics anywhere**, and no
MSVC-only pragmas in the render paths. The pixel code should compile.

What is most likely to bite, in the order it will bite:

1. **`Rez` not found, or the PiPL failing to compile.** The three PiPLs that
   were Windows-only until now (`EdgeRepairPiPL.r`, `ModernEdgeBlurPiPL.r`,
   `GrainTransferPiPL.r`) used to force-define `AE_OS_WIN`, which would have
   emitted a Windows-only code descriptor and produced a bundle AE loads and
   ignores. They now follow the SDK's own `SmartyPantsPiPL.r` pattern, like
   MM_FreqSep and MM_Qualify always did. If a plugin builds but never appears
   in the Effect menu, this is the first suspect — check that
   `Contents/Resources/<Target>.rsrc` exists inside the bundle and is non-empty.
2. **`fopen_s` / `_wfopen_s` / `GetTempPathA`.** All the remaining ones live in
   the Windows-only GPU hosts, which are not compiled on the Mac. Grain
   Transfer's diagnostic log was the one in a CPU path and now branches on
   `TMPDIR` (`GrainTransfer_diag.log` lands in `$TMPDIR`, not `%TEMP%`).
3. **Signed/unsigned and `long` width.** MSVC and clang disagree about enough
   of this to produce a page of warnings; treat warnings as noise on the first
   pass and only chase errors.
4. **Case-sensitive `#include` paths.** Windows does not care; a case-mismatched
   include that has worked for a year fails here.

## 6. What is NOT wired up yet

- **Metal.** No host, no prologue, and the only `.metal` file in the repo
  mirrors a single EdgeRepair entry point. See `gpu-kernel-rules.md` §4 for the
  five plumbing questions a port has to answer on-device. The GPU host files
  compile to *decline stubs* off-Windows (`EdgeRepair_GPU.cpp`,
  `ModernEdgeBlur_GPU.cpp`), which is a real, correct behaviour and the single
  place to unpick when a Metal host lands.
- **The headless test suites.** Every runner is PowerShell + `cl.exe`
  (`tests/run_tests.ps1`, `tests/build_tests.ps1`). The test *sources* are
  portable C++ with no AE SDK dependency, so a clang runner is a small job, but
  it does not exist. Until it does, the regression gate runs on Windows only —
  which is fine, because the math it gates is the same math, but it means a Mac
  build cannot self-verify.
- **Code signing / notarisation.** Untouched. For local use on your own
  machine this normally does not matter; distributing the bundle to another Mac
  does.

## 7. First-session checklist in AE

Once it builds and installs, in this order — the first failure explains the
rest:

1. All five appear under **Effect ▸ MM**. If one is missing, its PiPL is the
   suspect (§5.1), not its code.
2. Apply each to a layer: no error dialog, and the default state renders
   (Grain Transfer needs a Grain Source layer before it does anything).
3. Set the project to **32-bit float** and check each plugin's isolating view —
   the per-plugin table in
   [gpu-validation-checklist.md](gpu-validation-checklist.md) §4 lists them
   (Qualify ▸ Matte, Skin Retouch ▸ Low/High Band, MEB ▸ Blur Amount over a
   masked layer, GT ▸ Preview Grain Only).
4. **Render the same frame on Windows and on the Mac and difference them.**
   This is the real acceptance test and it is worth the setup: both sides run
   the same CPU reference code, so the target is *bit-identical*, not a
   tolerance. Any difference is a portability bug and it will be in
   floating-point handling or in a struct layout, not in the algorithm.
5. Modern Edge Blur needs **a layer with a real alpha edge** or it is an exact
   identity and proves nothing — see the checklist §4 note, which cost a full
   validation round on Windows.
