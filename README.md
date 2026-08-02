# MM VFX Tools

A suite of After Effects plugins (Effect → MM → …). Fast, simple, intuitive —
no bloated feature sets. Windows / MSVC / CMake.

| Plugin | Folder | Status |
|---|---|---|
| Grain Transfer | `GrainTransfer/` | Functional in AE; grain-quality passes ongoing |
| HSL Key (MM Qualify) | `MM_Qualify/` | Built + tested (105 headless tests); in-AE validation pending; GPU host is DX12-only (see note below) |
| MM FreqSep | `MM_FreqSep/` | Phases 1–5 done incl. DX12 GPU host (48/48 AE-free); in-AE GPU pass **blocked** (see note below) |
| Modern Edge Blur | `ModernEdgeBlur/` | Shipped/working; rename planned |
| Edge Repair | `ModernEdgeBlur/` | All six stages done incl. CUDA GPU render; rename planned |

All five plugins now live here. Modern Edge Blur and Edge Repair share one
folder and one CMake project (two independent `.aex` targets); splitting them
is an open option, not a commitment — see
[docs/MIGRATION.md](docs/MIGRATION.md) for that reasoning and the full history
of how this repo was assembled.

## Structure

Each project is **self-contained**: its own `CMakeLists.txt`, `build.ps1`,
`docs/`, `tests/`, and `.gitignore`. There is deliberately no shared code
between projects yet.

```powershell
cd <ProjectFolder>
.\build.ps1        # configure + build (+ tests where wired)
```

Per-project docs, architecture notes, and progress logs are inside each
project's `docs/` and README — they are the authoritative record.

## Suite documentation (`docs/`)

Cross-cutting knowledge that no single plugin owns. A rule proven by one plugin
belongs here, not in that plugin's folder.

| Doc | What it answers |
|---|---|
| [docs/AE_GPU_Effects.md](docs/AE_GPU_Effects.md) | **Host contract** — how to make AE dispatch to your GPU code, which framework it actually offers, and how to tell whether it really ran. Read before any GPU work. |
| [docs/gpu-kernel-rules.md](docs/gpu-kernel-rules.md) | **Kernel contract** — how to write shader bodies so one math source serves DX12 / CUDA / Metal / host C++, and how to prove it against the CPU reference. |
| [docs/MIGRATION.md](docs/MIGRATION.md) | How this repo was assembled from four independent repos, and the recipe if it ever needs repeating. |

> ⚠️ **Open cross-plugin issue.** MM_FreqSep's and MM_Qualify's GPU hosts accept
> `DIRECTX` only, but AE hands effects `CUDA` on the test box — so those GPU
> paths never engage in After Effects and render on the CPU, silently and
> correctly. Their kernels are bit-exact and not in question; the backend choice
> is. See [docs/AE_GPU_Effects.md](docs/AE_GPU_Effects.md) §1.1.

## Fresh-checkout note

The Adobe AE SDK is not in this repo (its license prohibits redistribution).
One shared install serves every project: GrainTransfer, MM_Qualify and
MM_FreqSep reference the SDK's
`Examples/` dir via the `AE_SDK_ROOT` CMake cache var (test runners honour an
`AE_SDK_ROOT` env var), defaulting to
`E:\OFX DEV\AE SDK\AfterEffectsSDK_25.6_61_win\...\Examples`. On a new
machine, unzip the SDK and pass `-DAE_SDK_ROOT=<path>` (or set the env var)
if it lives elsewhere.

`ModernEdgeBlur/` is the one exception, pending consolidation: it still
expects a vendored `sdk/` subset plus `resources/PiPLTool.exe` (both
gitignored — copy them from the SDK per that project's README). Its AE-SDK
subset is verified identical to the shared install; the extra `sdk/Premiere/`
subset is only needed for `-DEDGEREPAIR_PREMIERE_GPU=ON`, which is OFF by
default and unused by After Effects.
