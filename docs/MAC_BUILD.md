# Building the suite on macOS

**What this gets you: all five plugins loading in After Effects on a Mac and
rendering on the CPU path.** Not a degraded build — the CPU pipeline is the
reference every GPU number in this suite is measured *against*, so a Mac render
is the correct render by definition. What it does not get you is GPU
acceleration: there is no Metal host yet (`gpu-kernel-rules.md` §4), and CUDA is
Windows/NVIDIA. Every plugin declines the device and takes the reference path.

> **Built clean on a Mac, 2026-08-31.** All five plugins compile and link, and
> produce universal (`x86_64` + `arm64`) `.plugin` bundles carrying a valid
> PiPL. It took three fixes to get there — two in the Rez invocation, one
> genuine portability error — all now in the tree and recorded in
> [§5](#5-first-build-what-actually-broke). Installing into AE needs write
> access to its `Plug-ins` folder, which a normal account does not have (§3).
>
> **The in-AE half is still unverified.** No build from this document has yet
> been loaded by After Effects, so §7 remains a plan, not a result. The machine
> it was built on had AE 2023/2024/2025 at the time; AE 2026 was installed
> 2026-11-09 and is now the deploy target.

---

## 1. Prerequisites

| | | |
|---|---|---|
| **Xcode command-line tools** | `xcode-select --install` | Supplies clang and **Rez**, which compiles the PiPL resource. **Full Xcode is not required** — the CLT alone carry `Rez`, and it lands on `PATH` at `/usr/bin/Rez`, satisfying the branch's `find_program(REZ_TOOL Rez REQUIRED)`. (A future Metal host would need full Xcode for the `metal` compiler; nothing in the CPU build does.) |
| **CMake ≥ 3.20** | `brew install cmake` | Same minimum the Windows build uses. |
| **After Effects SDK 25.6** | Adobe developer site | Not in this repo — its licence prohibits redistribution. The macOS zip is a **separate download** from the Windows one and arrives with the SDK still packed as a zstd tarball: run its bundled `./extractzstd.sh` before pointing `AE_SDK_ROOT` anywhere. (Its `Resources/PiPLtool.exe` is the Windows binary and is not used here.) |
| **After Effects** | | The deploy target. `AE_PLUGINS_DIR` defaults to **AE 2026** on both platforms (`/Applications/Adobe After Effects 2026/Plug-ins` on macOS). Pass `-DAE_PLUGINS_DIR=...` for any other version. |

The SDK path is a cache variable, exactly as on Windows:

```bash
# Note the doubled folder name and the inner ae*.AfterEffectsSDK dir - that is
# what the mac zip actually unpacks to, once extractzstd.sh has run.
export AE_SDK_ROOT="$HOME/AE SDK/AfterEffectsSDK_25.6_61_mac/AfterEffectsSDK_25.6_61_mac/ae25.6_61.64bit.AfterEffectsSDK/Examples"
```

There is no CUDA toolkit requirement on the Mac — the nvcc step lives inside
the Windows branch of each `CMakeLists.txt`.

## 2. Build

Whole suite, from the repo root:

```bash
cmake -S . -B build-mac -DAE_SDK_ROOT="$AE_SDK_ROOT" -DCMAKE_BUILD_TYPE=Release
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

The macOS default was briefly AE 2025 (the build Mac's AE was pinned while it
served as the Resolve server); it returned to 2026 on 2026-11-09 once that
machine was upgraded. The plugins are expected to stay backward compatible
across AE versions — install into an older AE with `--ae 2025` or
`-DAE_PLUGINS_DIR=...`.

**The deploy is non-fatal, matching the Windows behaviour.** AE's `Plug-ins`
folder under `/Applications` is `root:wheel`, so a normal user account cannot
write to it — and a hard `copy_directory` there would fail the *build*, not just
the install. The macOS branch therefore tests the folder first and prints

```
AE plugins folder missing or not writable; skipping deploy (pass -DAE_PLUGINS_DIR=<writable dir>, or copy by hand)
```

rather than failing. This mirrors why the Windows deploy is non-fatal (AE holds
a file lock on a loaded `.aex`); the cause differs, the consequence does not.
**A build that says "skipping deploy" succeeded** — check the message before
assuming the plugin is in AE.

To actually install, run `tools/mac-install.sh` after the build — one sudo
prompt for the whole set:

```bash
./tools/mac-install.sh                 # all five, AE 2026, from build-mac/
./tools/mac-install.sh EdgeRepair      # just one
./tools/mac-install.sh --ae 2024       # a different AE version
./tools/mac-install.sh --dry-run       # preflight only, no sudo
```

It preflights every bundle before asking for a password — executable present,
`Info.plist` present, and **PiPL non-empty** — so a build that would load in AE
and show nothing is caught before it is installed rather than after. It warns
if AE is running (AE reads its plugin list at launch and will keep using the
copy it already loaded), and re-checks the PiPL at the destination once the
copy is done. The alternative remains pointing `-DAE_PLUGINS_DIR` at a writable
folder and letting the build install itself.

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

## 5. First build: what actually broke

**The portability audit held.** All threading goes through AE's own
`iterate_generic` (no `std::thread`, no OpenMP), there are no SSE/AVX
intrinsics anywhere, and no MSVC-only pragmas in the render paths — and the
pixel code duly compiled. Predicted problems 2–4 below (`fopen_s`,
signed/unsigned width, case-sensitive includes) **did not materialise at all**:
the first build produced two errors in one file and four cosmetic
`'/*' within block comment` warnings, nothing else.

Three things did break, none of them in the algorithms.

1. **Rez could not create its output directory.** CMake's bundle generator
   makes `Contents/MacOS` and `Contents/Info.plist` but **not**
   `Contents/Resources`, and Rez will not create its own output dir — it dies
   with `### Rez - fnfErr (-43) during create of ".../<Target>.rsrc"`, which
   reads like a missing *input*. Every plugin that linked hit this. Fixed by a
   `${CMAKE_COMMAND} -E make_directory` command ahead of the Rez call.
2. **Rez wrote the PiPL to the resource fork, and exited 0 doing it.** This is
   the dangerous one: with the directory fixed, Rez succeeds, and leaves a
   **0-byte** `.rsrc` whose 634 bytes are in the `com.apple.ResourceFork`
   xattr. `copy_directory` does not carry that xattr, so the deployed bundle
   would load and never appear in the Effect menu — with a clean build log and
   an `.rsrc` file sitting right where it belongs. **`-useDF` is mandatory**,
   not a preference; it puts the resource in the data fork where AE reads it.
   Verify with `stat -f%z <bundle>/Contents/Resources/<Target>.rsrc` — a real
   PiPL is ~630 bytes, and `strings` on it shows `eFKT`, the effect name and
   `EffectMain`.
3. **`static_cast` from `PF_Handle`'s dereference (Grain Transfer, 2 sites).**
   `*(in_data->global_data)` is `void*` on Windows but `Ptr` (`char*`) on the
   Mac, and `static_cast` between unrelated object pointers is illegal —
   `error: static_cast from 'Ptr' (aka 'char *') to 'GT_GlobalData *' is not
   allowed`. `reinterpret_cast` is correct on both platforms and is the SDK's
   own idiom. Only the `*(handle)` form is affected: the sites that cast the
   `void*` from `host_lock_handle()` are fine as they are.

The PiPL work done on the Windows side was sound — dropping the forced
`AE_OS_WIN` from `EdgeRepairPiPL.r`, `ModernEdgeBlurPiPL.r` and
`GrainTransferPiPL.r` was necessary and sufficient; all five `.r` files compile
under Rez with no complaints.

### Rebuild after the macOS 27 / clang 21 update (2026-11-09)

4. **`size_t` no longer arrives transitively.** Apple clang 21's libc++ trimmed
   its internal includes, so `MM_FreqSep/src/matte.cpp` and
   `MM_Qualify/src/garbage.cpp` — which used `size_t` with only `<cmath>` or
   their own headers included — failed with `unknown type name 'size_t'`.
   Fixed by including `<cstddef>` explicitly. No pixel code changed.

5. **MM_Qualify's `Info.plist` was a copy of MM_FreqSep's** — executable,
   identifier and bundle name all FreqSep's. The bundle built, the binary was
   fine and signed, the PiPL was valid, and AE silently left it out of the
   Effect menu: with `CFBundleExecutable` naming a file that is not in
   `Contents/MacOS`, macOS treats the bundle as codeless (`codesign` reports
   it "not signed at all"). Found on first load in AE 2026 — 4 of 5 present.
   `tools/mac-install.sh` now refuses a bundle whose plist names any executable
   but its own.

Configure with `-DCMAKE_BUILD_TYPE=Release`: the two commands in §2 leave the
build type empty under the Makefile generator, which means an unoptimised build
— fine for checking that a plugin loads, far too slow to judge render speed.

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
