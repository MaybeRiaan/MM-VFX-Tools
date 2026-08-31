# MM VFX Tools

A suite of After Effects plugins (Effect → MM → …). Fast, simple, intuitive —
no bloated feature sets. Windows / MSVC / CMake.

| Plugin | Folder | Status |
|---|---|---|
| Grain Transfer | `GrainTransfer/` | Functional in AE; CUDA host built, kernel parity 18/18 exact; grain-analysis passes ongoing; **in-AE GPU rows not yet run** |
| HSL Key (MM Qualify) | `MM_Qualify/` | Built + tested (105 headless tests); **CUDA GPU render verified in AE 2026-08-11** |
| MM FreqSep | `MM_FreqSep/` | Phases 1–5 done, kernels bit-exact; **CUDA GPU render verified in AE 2026-08-12** |
| Modern Edge Blur | `ModernEdgeBlur/` | Shipped/working; **CUDA GPU render verified in AE 2026-08-20**; rename planned |
| Edge Repair | `EdgeRepair/` | All six stages done; **CUDA GPU render verified in AE 2026-07-30**; the suite's GPU reference; rename planned |

All five plugins live here, one folder each. Modern Edge Blur and Edge Repair
shared a folder until 2026-08-02 — they were separated once it was established
they shared a build system but no code. See
[docs/MIGRATION.md](docs/MIGRATION.md) for the full history of how this repo
was assembled.

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
| [docs/gpu-validation-checklist.md](docs/gpu-validation-checklist.md) | **In-AE validation** — the hands-on loop, the per-plugin traps that make a sweep hollow, and the log decoder. Its §0 table is the live record of which plugins AE has actually GPU-rendered. |
| `docs/gpu-validation-tracker.xlsx` | The row-by-row evidence behind that table. Read and write it with `tools/gpu-validation/read-tracker.ps1` / `write-tracker.ps1` — it is the only live copy. |
| [docs/MIGRATION.md](docs/MIGRATION.md) | How this repo was assembled from four independent repos, and the recipe if it ever needs repeating. |

`tools/` holds suite-wide tooling: `pipl_dump.py` (read a built `.aex`'s PiPL —
match name, version, outflags), `pipl_sweep.py`, and `gpu_status.ps1`.

> **GPU direction (decided 2026-08-02): CUDA only, EdgeRepair is the reference.**
> EdgeRepair's GPU path is the one written after the AE host mechanism was
> *measured* rather than inferred. **That re-examination is finished.** All five
> plugins now ship a CUDA host, and After Effects has GPU-rendered four of them
> on this box: EdgeRepair (2026-07-30), MM_Qualify (2026-08-11), MM_FreqSep
> (2026-08-12), Modern Edge Blur (2026-08-20). Grain Transfer's host is built
> and its kernel is parity-tested headlessly, but its in-AE rows are still to
> run — [docs/gpu-validation-checklist.md](docs/gpu-validation-checklist.md) §0
> is the live status.
>
> MM_FreqSep's and MM_Qualify's DX12 hosts accept `DIRECTX`, which AE never
> offers on this hardware, so they silently render on the CPU. Those hosts are
> **parked, not deleted** — their kernels are bit-exact, they remain the asset,
> and Premiere is the host that would use them.
>
> **Metal is unwritten.** No Metal host exists in any plugin, no
> `*_prologue_metal.h` exists, and the only `.metal` file in the repo mirrors a
> single EdgeRepair entry point. The kernel math is already Metal-shaped by
> construction — see [docs/AE_GPU_Effects.md](docs/AE_GPU_Effects.md)
> §1.1–1.2 and [docs/gpu-kernel-rules.md](docs/gpu-kernel-rules.md) §4 for
> what a port still has to settle on-device.

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
