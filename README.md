# MM VFX Tools

A suite of After Effects plugins (Effect → MM → …). Fast, simple, intuitive —
no bloated feature sets. Windows / MSVC / CMake.

| Plugin | Folder | Status |
|---|---|---|
| Grain Transfer | `GrainTransfer/` | Functional in AE; grain-quality passes ongoing |
| HSL Key (MM Qualify) | `MM_Qualify/` | Built + tested (105 headless tests); in-AE validation pass pending |
| MM FreqSep | `MM_FreqSep/` | Phases 1–5 done incl. DX12 GPU host (48/48 AE-free); in-AE GPU pass next |
| Modern Edge Blur | *(pending migration)* | Shipped/working; rename planned |
| Edge Repair | *(pending migration)* | In active development (GPU port in flight) |

Modern Edge Blur + Edge Repair still live in `e:\OFX DEV\ModernEdgeBlur`
until their in-flight work lands — see
[docs/MIGRATION.md](docs/MIGRATION.md) for the deferred-migration procedure
and the full history of how this repo was assembled.

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

## Fresh-checkout note

The Adobe AE SDK is not in this repo (its license prohibits redistribution).
One shared install serves every project: all three reference the SDK's
`Examples/` dir via the `AE_SDK_ROOT` CMake cache var (test runners honour an
`AE_SDK_ROOT` env var), defaulting to
`E:\OFX DEV\AE SDK\AfterEffectsSDK_25.6_61_win\...\Examples`. On a new
machine, unzip the SDK and pass `-DAE_SDK_ROOT=<path>` (or set the env var)
if it lives elsewhere. Nothing else needs restoring by hand.
