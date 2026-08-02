# MM VFX Tools

A suite of After Effects plugins (Effect → MM → …). Fast, simple, intuitive —
no bloated feature sets. Windows / MSVC / CMake.

| Plugin | Folder | Status |
|---|---|---|
| Grain Transfer | `GrainTransfer/` | Functional in AE; grain-quality passes ongoing |
| HSL Key (MM Qualify) | `MM_Qualify/` | Built + tested (105 headless tests); in-AE validation pending; DX12 GPU host parked (see note below) |
| MM FreqSep | `MM_FreqSep/` | Phases 1–5 done, kernels bit-exact (48/48 AE-free); DX12 GPU host parked pending CUDA re-examination (see note below) |
| Modern Edge Blur | `ModernEdgeBlur/` | Shipped/working; rename planned |
| Edge Repair | `ModernEdgeBlur/` | All six stages done incl. CUDA GPU render; rename planned |

All five plugins now live here. Modern Edge Blur and Edge Repair share one
folder and one CMake project (two independent `.aex` targets); splitting them
is an open option, not a commitment — see
[docs/MIGRATION.md](docs/MIGRATION.md) for that reasoning and the full history
of how this repo was assembled.

## Building

Two entry points, and both stay valid — the suite one is additive.

**Whole suite**, from the repo root — builds all five `.aex` and deploys them
to After Effects for testing:

```powershell
.\build.ps1                       # build + deploy everything
.\build.ps1 -Project MM_FreqSep   # just one
.\build.ps1 -Versions             # version table; verifies header/PiPL agree
.\build.ps1 -Publish              # re-deploy what's built, no recompile
.\build.ps1 -Clean                # wipe the suite build tree first
```

**Single plugin**, from its folder — unchanged, and still the tool for working
on one plugin, because these own their version bumps and per-project quirks:

```powershell
cd <ProjectFolder>
.\build.ps1        # configure + build (+ tests where wired)
```

Note the difference: a project's own `build.ps1` **auto-increments** its build
number (ModernEdgeBlur's also auto-commits it); the suite script never touches
version numbers — it builds what is committed.

### What the suite layer adds

- One command to build and deploy the whole suite.
- `-DAE_PLUGINS_DIR=<path>` at the root redirects **every** deploy at once —
  a second AE install, or a sandbox folder instead of Program Files.
- A **configure-time check that each plugin's header and PiPL versions agree.**
  Both files carry the same five defines; AE packs them into one integer and
  refuses to load on any mismatch, reporting only `MAJOR.MINOR` — so its error
  shows two identical-looking versions as the reason. Now it fails at configure
  time naming both files and both quadruples.
- `mm_publish` — reinstall without recompiling, for when a build ran while AE
  held the `.aex` lock (that copy fails non-fatally and installs nothing).

## Structure

Each project is **self-contained**: its own `CMakeLists.txt`, `build.ps1`,
`docs/`, `tests/`, and `.gitignore`, and each still configures standalone if
lifted out of the suite (`cmake/MMSuite.cmake` is included `OPTIONAL`). There is
deliberately no shared *plugin* code between projects yet — only the build-level
helpers in `cmake/`.

Per-project docs, architecture notes, and progress logs are inside each
project's `docs/` and README — they are the authoritative record. Cross-cutting
knowledge lives in the suite `docs/` below.

## Suite documentation (`docs/`)

Cross-cutting knowledge that no single plugin owns. A rule proven by one plugin
belongs here, not in that plugin's folder.

| Doc | What it answers |
|---|---|
| [docs/AE_GPU_Effects.md](docs/AE_GPU_Effects.md) | **Host contract** — how to make AE dispatch to your GPU code, which framework it actually offers, and how to tell whether it really ran. Read before any GPU work. |
| [docs/gpu-kernel-rules.md](docs/gpu-kernel-rules.md) | **Kernel contract** — how to write shader bodies so one math source serves DX12 / CUDA / Metal / host C++, and how to prove it against the CPU reference. |
| [docs/MIGRATION.md](docs/MIGRATION.md) | How this repo was assembled from four independent repos, and the recipe if it ever needs repeating. |

> **GPU direction (decided 2026-08-02): CUDA only, EdgeRepair is the reference.**
> EdgeRepair's GPU path is the one written after the AE host mechanism was
> *measured* rather than inferred, and the only one AE has actually GPU-rendered.
> MM_FreqSep's and MM_Qualify's DX12 hosts accept `DIRECTX`, which AE never
> offers on this hardware, so they silently render on the CPU. Those hosts are
> **parked, not deleted** — their kernels are bit-exact and remain the asset.
> All four non-EdgeRepair plugins get re-examined through this lens once suite
> unification is finished. See [docs/AE_GPU_Effects.md](docs/AE_GPU_Effects.md)
> §1.1–1.2.

## Fresh-checkout note

The Adobe AE SDK is not in this repo (its license prohibits redistribution).
**One shared install serves every project** — all of them reference the SDK's
`Examples/` dir via the `AE_SDK_ROOT` CMake cache var (test runners honour an
`AE_SDK_ROOT` env var), defaulting to
`E:\OFX DEV\AE SDK\AfterEffectsSDK_25.6_61_win\...\Examples`. On a new
machine, unzip the SDK and pass `-DAE_SDK_ROOT=<path>` (or set the env var)
if it lives elsewhere. `PiPLtool.exe` comes from the SDK's own `Resources/`.
**Nothing needs restoring by hand.**

One optional extra: the **Premiere** SDK is a separate download with no shared
install, needed only for `-DEDGEREPAIR_PREMIERE_GPU=ON`. It stays vendored
(gitignored) at `EdgeRepair/sdk/Premiere/`, overridable with
`-DPREMIERE_SDK_ROOT=<path>`. That module is OFF by default, unused by After
Effects, and parked under the CUDA-only decision above.
