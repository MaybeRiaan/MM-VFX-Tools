# GPU-accelerating an After Effects effect (AE 2026, Windows/NVIDIA)

**Audience: any plugin in this suite.** EdgeRepair paid the discovery cost;
nothing here is EdgeRepair-specific. Per-plugin status lives in that plugin's
own doc.

> **The suite has two GPU documents. This one is the HOST contract — how to get
> After Effects to dispatch to your GPU code at all, and what it hands you when
> it does.** Its companion,
> [`gpu-kernel-rules.md`](gpu-kernel-rules.md), is the KERNEL contract — how to
> write the shader bodies so one math source serves DX12 / CUDA / Metal / host
> C++. Read this one first: a perfectly portable kernel is dead code until the
> host gate in §2 is open. Where they overlap (§4 here vs §0–2 there) they
> agree in substance — one math body, platform difference quarantined, the
> shared source compiled as host C++ and diffed against the CPU original — but
> they were written from two plugins' experience and formalize it differently.
> §4.6 reconciles them.

Verified end-to-end on the test box (Win 10, i7-9700K, RTX 3070 Ti, driver
576.28, AE 2026 / 26.2, Mercury CUDA) on **2026-07-30**: EdgeRepair receives
`what_gpu = CUDA` at PreRender and AE calls `PF_Cmd_SMART_RENDER_GPU` at 4K.
Where something is *believed* rather than *measured*, it says so.

> ### Read this first if you saw the previous version
> An earlier revision of this document claimed the documented AE-SDK GPU path
> "did not work" and that AE GPU-renders through the MediaCore
> **GPUVideoFilter** (`xGPUFilterEntry`) mechanism. **That was wrong**, and it
> cost three days and seven failed experiments.
>
> **GPUVideoFilter is Premiere Pro's GPU path. The documented AE-SDK F32 path
> is After Effects'.** The old conclusion came from inferring host behaviour by
> sweeping installed third-party `.aex` files. Those binaries ship for *both*
> hosts, so their contents say nothing about which path AE drives.
>
> §8 records the wrong turns so nobody repeats them.

---

## 1. The mechanism

AE 2026 GPU-renders third-party effects through exactly the path the AE SDK
documents:

```
GLOBAL_SETUP        declare SUPPORTS_GPU_RENDER_F32 (+ DIRECTX_RENDERING)
      ↓
GPU_DEVICE_SETUP    once per device — build your context, AND re-affirm
                    out_flags2 = SUPPORTS_GPU_RENDER_F32   ← easy to miss
      ↓
SMART_PRE_RENDER    AE offers input->what_gpu; you raise GPU_RENDER_POSSIBLE
      ↓
SMART_RENDER_GPU    a separate command — worlds hold DEVICE pointers
      ↓
GPU_DEVICE_SETDOWN  tear down
```

`xGPUFilterEntry` / MediaCore / `GPUVideoFilter.0` registry records are **not
part of this** and are not needed for AE. Build a GPUVideoFilter module only if
you also want to ship into Premiere.

### 1.1 Which framework AE actually offers — pick your backend from this, not from the flags

**MEASURED 2026-07-23 on the test box (Win 10, RTX 3070 Ti, AE 2026 / 26.2,
Mercury GPU = CUDA): After Effects hands effects `what_gpu = CUDA` and never
probes DIRECTX** — even with `SUPPORTS_DIRECTX_RENDERING` declared. That flag
serves AE-SDK effects hosted in **Premiere Pro**, whose Windows GPU renderer is
DX12. See `EdgeRepair/src/EdgeRepair_GPU.cpp` (file header) and
`EdgeRepair/docs/EdgeRepair.md` Stage 6 for the measurement.

**Consequence, and it is not hypothetical.** A GPU host that accepts only
`PF_GPU_Framework_DIRECTX` declines the device in `GPU_DEVICE_SETUP`, and AE
then renders it on the CPU — correctly, at CPU speed, with no error anywhere.
That is precisely the silent failure this document opens by warning about. Two
plugins in this suite are in that position today:

| Plugin | GPU host accepts | Exercised by AE on the test box? |
|---|---|---|
| EdgeRepair | CUDA **or** DIRECTX (`EdgeRepair/src/EdgeRepair_GPU.cpp:264`) | ✅ yes, via CUDA |
| ModernEdgeBlur | CUDA only (`ModernEdgeBlur/src/ModernEdgeBlur_GPU.cpp`) | ⏳ built to this decision; in-AE A/B still pending |
| MM_FreqSep | DIRECTX only (`MM_FreqSep/src/gpu/mm_gpu_dx.cpp:179`) | ❌ no — declines, falls back to CPU |
| MM_Qualify | DIRECTX only (`MM_Qualify/src/gpu/mm_gpu_dx.cpp:159`) | ❌ no — declines, falls back to CPU |

Modern Edge Blur (ported 2026-08-03) is the first plugin written against §1.2
from the start: CUDA only, DIRECTX **declined** rather than served a passthrough
host — a passthrough would render the effect as a silent no-op in Premiere,
which is worse than falling back to the correct CPU path. Its HLSL wrapper still
exists and is still compiled every build, purely for the §4 item 5 type-check.

Both DX12 hosts are validated bit-exact against their CPU references in their
AE-free harnesses, so **the kernels are not in question — the backend choice
is.** Their remaining "first in-AE GPU validation" step cannot pass on this
hardware as built.

### 1.2 DECISION (2026-08-02): CUDA is the backend this suite maintains

**EdgeRepair is the ground truth for GPU work across the suite, past and
future.** It is the only plugin here whose GPU path was written *after* the
host mechanism was measured rather than inferred, and it is the only one AE has
ever actually GPU-rendered. Every other GPU effort in this repo was built under
the assumption this document's own earlier revision got wrong (§8), and
inherited it.

Consequences, and they are deliberate:

- **CUDA is the only GPU backend under active maintenance.** New GPU work
  targets CUDA, structured the way EdgeRepair structures it.
- **The DX12 hosts in MM_FreqSep and MM_Qualify are parked, not deleted.** They
  are correct code solving a problem AE does not pose on this platform. Nothing
  about them is thrown away, and their bit-exact kernel validation remains
  valuable — the kernels are the asset, the host is the part aimed at the wrong
  target.
- **All four non-EdgeRepair plugins get re-examined through this lens once the
  suite unification is finished** — not before, so the re-examination happens
  against a settled structure.
- Until then, treat "GPU" in those plugins' docs as meaning "validated
  AE-free, not reached by AE."

Scope honestly, because this decision rests on one measurement: one box, one
GPU vendor, one Mercury setting. It has not been tested on AMD/Intel (where
OpenCL is the plausible offer) or on macOS (Metal). "AE offers CUDA on
Windows/NVIDIA" is measured; "AE never offers DirectX anywhere" is an untested
inference. The decision above is the right call for the hardware this suite is
developed and shipped on today — revisit it if that changes, and re-measure
rather than re-infer.

---

## 2. The checklist

Six things. Miss any one and the failure is **silent**: the effect renders on
the CPU with no error and looks perfect.

### 2.1 Declare the GPU flags — and guard the Premiere branch

Mirror Adobe's own AE GPU sample (`SDK_Invert_ProcAmp`, §7):

```cpp
out_data->out_flags  = PF_OutFlag_PIX_INDEPENDENT | PF_OutFlag_DEEP_COLOR_AWARE;
out_data->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE
                     | PF_OutFlag2_SUPPORTS_SMART_RENDER
                     | PF_OutFlag2_SUPPORTS_THREADED_RENDERING;

if (in_data->appl_id == 'PrMr') {
    // Premiere wants pixel formats instead (see §6)
} else {
    out_data->out_flags2 |= PF_OutFlag2_SUPPORTS_GPU_RENDER_F32
                          | PF_OutFlag2_SUPPORTS_DIRECTX_RENDERING;
}
```

- **`SUPPORTS_DIRECTX_RENDERING` is correct for AE.** It does not force the
  effect onto DirectX; Adobe sets it unconditionally for non-Premiere hosts and
  still receives `what_gpu = CUDA`.
- **Do not set `USE_OUTPUT_EXTENT`.** Adobe's AE GPU sample declares
  `eGLO 0x02000400` — `PIX_INDEPENDENT | DEEP_COLOR_AWARE` only.
- **Do not declare `SUPPORTS_QUERY_DYNAMIC_FLAGS`** unless you genuinely have
  dynamic flags — see §6.

### 2.2 Re-affirm GPU support in `GPU_DEVICE_SETUP` ← **the one that bit us**

After building your per-device context, **assign** (not OR):

```cpp
out_data->out_flags2 = PF_OutFlag2_SUPPORTS_GPU_RENDER_F32;
```

Adobe's sample does this in *every* successful backend branch (CUDA, OpenCL,
DirectX, Metal). Without it, AE completes `GPU_DEVICE_SETUP` happily and then
offers `what_gpu = NONE` at **every** PreRender, forever. That is a perfect
imitation of "AE doesn't support third-party GPU effects", and it is why seven
other hypotheses were chased first.

If you decline a device (unsupported framework, missing driver), just return
`PF_Err_NONE` without setting `gpu_data` — AE falls back to CPU cleanly.

### 2.3 Raise `GPU_RENDER_POSSIBLE` in PreRender

```cpp
extra->output->flags |= PF_RenderOutputFlag_GPU_RENDER_POSSIBLE;
```

Unconditional is fine (Adobe's sample does it before anything else). AE still
decides per frame — expect `what_gpu = NONE` for thumbnails and heavily
downsampled draft renders even when GPU is working.

### 2.4 Handle `PF_Cmd_SMART_RENDER_GPU` — it is a **separate command**

```cpp
case PF_Cmd_SMART_RENDER:     err = SmartRender(...); break;
case PF_Cmd_SMART_RENDER_GPU: err = SmartRender(...); break;   // easy to omit
case PF_Cmd_GPU_DEVICE_SETUP:    err = GPUDeviceSetup(...);    break;
case PF_Cmd_GPU_DEVICE_SETDOWN:  err = GPUDeviceSetdown(...);  break;
```

Inside, branch on `extra->input->what_gpu != PF_GPU_Framework_NONE`. **Never
fall back to the CPU path on a GPU error** — CPU code cannot address device
memory.

### 2.5 Install to **AE's** Plug-ins folder

```
C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\Plug-ins\
```

with any kernel assets (`CUDA_Assets\`, `DirectX_Assets\`) **next to the .aex**.
Adobe's sample build files say the same: *"Set it to your After Effects
plug-ins folder."*

**Not** `Common\Plug-ins\7.0\MediaCore\` — that is the Premiere/MediaCore host
folder. Keep exactly **one** installed copy; a second copy elsewhere
double-registers the match name, and deleting from `Program Files` needs an
elevated shell.

### 2.6 PiPL must equal the runtime flags

`AE_Effect_Global_OutFlags`/`_2` in the `.r` file must match what `GlobalSetup`
sets, or AE rejects the plugin with a misleading *"global outflags mismatch"*
(**25::16**). For the recipe above that is:

```
AE_Effect_Global_OutFlags   { 0x02000400 }
AE_Effect_Global_OutFlags_2 { 0x2A001400 }
```

The PiPL declares the **superset** (including the Premiere-only branch's bits);
AE tolerates that. Mirror your version bump into both files.

---

## 3. Working with GPU frames

- `extra->input->gpu_data` is the blob you stored in `GPU_DEVICE_SETUP`.
  Sanity-check `gpu_data->framework == extra->input->what_gpu`.
- Get device pointers via `PF_GPUDeviceSuite1::GetGPUWorldData(effect_ref,
  world, &mem)`. For CUDA this **is** the `CUdeviceptr`.
- Pixel layout is **BGRA**, 4 × f32 (16 bytes/px) — *not* the ARGB float the AE
  CPU path uses. Derive pitch from `world->rowbytes`, never from width.
- Allocate through `PF_GPUDeviceSuite1` only — never raw `cudaMalloc`.
- **Launch on AE's stream** (`device_info.command_queuePV`) and do **not**
  synchronize: AE owns synchronization, and that stream also serializes
  concurrent Multi-Frame Rendering launches, which is what makes a *stateless*
  kernel lock-free. Share any device scratch between renders and that stops
  being true — see §4.
- 16f (`_16f`) formats are declared on the Premiere path; a kernel that may see
  them must handle half floats.

---

## 4. Porting the math without the two paths drifting

Everything above gets AE to *dispatch* to your GPU code. This section is about
what you dispatch to — and it is where the long-term cost lives. You now have
two implementations of the same formulas, and the GPU one is the one you cannot
step through. Drift between them is silent, arrives months later as "the GPU
looks slightly different", and is miserable to bisect.

EdgeRepair's Stage 6 port (`EdgeRepair/src/gpu/`,
`EdgeRepair/tests/test_gpuparity.cpp`) is the worked example; the rules
below are not specific to it. MM_FreqSep and MM_Qualify solve the same problem
with a stricter file layout — see §4.6.

**1. One math body, N compile targets.** Every per-pixel formula goes in ONE
header written in the common subset of HLSL / CUDA / Metal / host C++
(`ER_KernelMath.h`). The per-backend wrappers hold **only** entry decorations
and buffer plumbing — never arithmetic. This is also what makes a later Metal
build a toolchain step instead of a port.

**2. A formula changes in both places in the SAME commit,** or not at all. Say
so in both headers, and have each shared function name its CPU twin in a
comment. "I'll port it after" is how drift starts.

**3. Compile the shared header as host C++ and test it against the CPU
original.** That is the whole point of keeping it in the common subset: the
headless tests call *the exact functions the GPU runs*, with no GPU in the
loop. Compare with `==`, not an epsilon — it is the same arithmetic in the same
order, so anything looser hides a real divergence.

Test the **composition**, not just the formulas. Per-pixel helpers passing
individually says nothing about whether the host wired them together right; an
off-by-one in a mip-level loop or a swapped fine/coarse sails straight through.
Re-run the multi-pass schedule on the CPU and diff the whole result.

**4. Where the paths legitimately differ, MEASURE it and write the number
down.** An approximation accepted up front (an iterative distance transform
instead of an exact one, a float reduction instead of double) should ship with
its measured error and expected magnitude *in the doc*, so a later session
reading a faint difference render knows whether it is looking at the accepted
divergence or a new bug. "Sub-pixel" is a hope; "worst 0.0000 px across five
seed topologies, tolerance 0.5" is a fact.

**5. Keep at least one HLSL entry point in the build even if you never ship
DX12.** fxc type-checks the *whole translation unit*, so a single compiled
entry validates the entire shared header against HLSL's rules on every build.
This caught a construct CUDA and C++ both accept and HLSL rejects, months
before anyone would have compiled it for real.

### Branching when you cannot read back

CPU pipelines routinely ask a question between passes — "did the matte come out
empty?", "did the fill find a seed?" — and branch on the host. On the GPU that
answer costs a stream synchronization, which you must not do (§3).

The fix: have the **producing** kernel `atomicOr` a bit into a small device
flags buffer, and have the **consuming** kernel test it. Same decisions, no
round-trip. Zero the buffer in a tiny kernel at the top of each frame. Make
sure every CPU early-out has a matching flag, and cover the degenerate cases
(empty mask, no edge at all) in the parity test — those are exactly the ones
that never come up in normal footage and break in a customer's project.

### Multi-pass scratch, and when the path stops being lock-free

§3 notes that AE's per-device stream serializes concurrent MFR launches. That
makes a *stateless* kernel lock-free — but the moment you share device scratch
between renders, serializing **enqueue** becomes load-bearing: take a mutex
around the whole pass sequence. Because the stream is FIFO, that is sufficient
— render A's passes all execute before render B's touch the buffers.

Allocate scratch **once per device**, sub-divided per render and grown only
when a bigger frame arrives (the device-side analogue of a per-thread scratch
struct), and free it in `GPU_DEVICE_SETDOWN`. Reserve only what the current
parameters need — intermediates like a mip chain can dwarf the frame itself.

### Language differences that actually bite

Found the hard way writing one header for four compilers:

- **HLSL has no ternary over struct types** (`cond ? structA : structB`) — use
  an `if`. fxc reports it as "type mismatch between conditional values".
- **Out-parameters differ**: `out T` (HLSL), `T&` (CUDA/C++), `thread T&`
  (Metal, which needs the address space). Hide it behind one macro.
- **cs_5_1 has no float atomics.** A reduction that uses `atomicAdd` on CUDA
  needs a different shape to stay portable — a fixed grid writing per-block
  partials, then a short serial finalize, works everywhere and has the bonus of
  being independent of scheduling.
- **Wrap `sqrt`/`abs`/`min`/`max`/`floor` in macros.** The spellings and
  overload sets differ per language; picking them up implicitly compiles fine
  on one backend and fails on the next.
- Keep the backend `#ifdef`s in **one block at the top**. If they spread, the
  "one math body" property is gone and you are back to N implementations.

### 4.6 Reconciling with `gpu-kernel-rules.md`

Two plugins arrived at this problem independently, so the suite carries two
write-ups. They **agree on the substance** — one math body, platform difference
quarantined, the shared source compiled as host C++ and diffed against the CPU
original, composition tested and not just per-pixel formulas. They differ in
how far the quarantine is formalized:

| | EdgeRepair (this doc) | MM_FreqSep / MM_Qualify (`gpu-kernel-rules.md`) |
|---|---|---|
| Kernel source | one shared header (`ER_KernelMath.h`), backend `#ifdef`s in a single block at the top | `.kernel` bodies with **zero** `#ifdef`, all spellings in a separate platform *prologue* header |
| Shader delivery | loose `CUDA_Assets/` PTX + `DirectX_Assets/` `.cso` next to the `.aex` | DXIL compiled at build time, embedded as C headers — self-contained `.aex`, no runtime compiler |
| Parity tolerance | `==` (exact) | `==` (exact) in the AE-free harness; ≤ 1e-5 for real-GPU vs CPU |

None of these is a contradiction:

- The prologue split is the **same idea taken one step further** — an `#ifdef`
  block at the top of a shared header *is* a prologue, just not in its own
  file. `gpu-kernel-rules.md` §0 is the stricter statement and it is the one to
  follow for **new** kernel work; two plugins already do. Do not churn
  EdgeRepair's working single-header form to match without a reason.
- Delivery differs by necessity, not taste: CUDA PTX is produced by `nvcc` and
  loaded at runtime, so EdgeRepair ships it as a file; DXIL can be baked into
  the binary. (PTX is text and could be embedded too — an option, not a rule.)
- The tolerances measure different things. `==` is right when the *same source*
  is compiled as host C++ — same arithmetic, same order, so any difference is a
  real defect. `≤ 1e-5` is right when comparing against a **real GPU**, where
  fma contraction legitimately differs. Both docs use both, at their own layer.

---

## 5. How to tell whether it is actually working

The failure mode is silent. Use these, in this order.

### 5.1 Log inside the GPU entry points

Log `what_gpu` in PreRender and log entry to `SMART_RENDER_GPU`. This is the
decisive signal:

```
PRERENDER what_gpu=3 gpu_data=0x… -> GPU_RENDER_POSSIBLE=YES
SMART_RENDER GPU what_gpu=3 out=3840x2160 src=3840x2160
```

`what_gpu`: `0 = NONE, 1 = OPENCL, 2 = METAL, 3 = CUDA, 4 = DIRECTX`.
**`what_gpu=0` at every PreRender ⇒ the gate is shut — go to §2.2 first.**

### 5.2 Make the milestone kernel **visibly destructive**

> A passthrough kernel makes the CUDA-vs-Software A/B **worthless** — output is
> bit-identical whether or not the GPU ran. We wasted several test rounds on
> exactly this, twice.

Zero the top eighth of the output world, then check it disappears under
Mercury → *Software Only*:

```cpp
cu.MemsetD8Async(dstPtr, 0, rowBytes * (height / 8), stream);
```

Expect it to read as **transparent**, not black: zeroing BGRA sets alpha to 0.
That is also a free check that pointer, pitch and channel layout are right.

Keep it behind a compile switch (`ER_GPU_VISIBLE_MARKER`) and delete it when
real kernels land.

### 5.3 Don't bother with the registry

`GPUVideoFilter.0` under
`HKCU\Software\Adobe\After Effects\26.2\PluginCache\en_US` is the **Premiere**
mechanism's record. Its presence or absence says nothing about AE GPU. (An
earlier revision presented it as the decisive check. It is not.)

Still useful: `%APPDATA%\Adobe\After Effects\26.2\Plugin Loading.log` shows
which loader picked up each plugin.

---

## 6. Gotchas

- **`PF_Cmd_QUERY_DYNAMIC_FLAGS` — don't declare it unless you need it.**
  `SUPPORTS_QUERY_DYNAMIC_FLAGS` is not part of the GPU recipe, and getting the
  handler slightly wrong raises an unsuppressable **25::228** on *every* apply.
  AE hands you `out_flags` **pre-populated**, so even OR-ing one dynamic bit
  leaves non-dynamic bits set and counts as changing them. If your flags are
  static, simply don't advertise the capability.
- **Never acquire suites with `AEFX_AcquireSuite` in `GLOBAL_SETUP`.** On
  failure it sets `PF_OutFlag_DISPLAY_ERROR_MESSAGE` (`0x100`) on
  `out_data->out_flags` and writes `return_msg`
  (`${AE_SDK_ROOT}/Util/AEFX_SuiteHelper.c:51-57`). That silently corrupts your reported
  outflags and AE refuses to load the plugin (**25::16**, `0x2000500` vs
  `0x2000400`). Use `in_data->pica_basicP->AcquireSuite` and restore
  `out_flags`/`out_flags2` on the way out.
- **The pixel-format suite is Premiere-only.** `kPFPixelFormatSuite` returns
  `kSPSuiteNotFoundError` (`'S!Fd'`) in AE for *both* versions — correct, not a
  defect. Guard the call with `appl_id == 'PrMr'`.
- **FourCC trap** (Premiere path only): build `PrPixelFormat_GPU_*` with
  `MAKE_ADOBE_PRIVATE_PIXEL_FORMAT_FOURCC`. Never hand-roll — the private
  prefix is `'@'`, byte order is reversed, and 16f is lowercase `'a'` against
  32f's `'A'`. Each mistake fails silently.
- **SDKs are never committed.** `sdk/` is gitignored. Have CMake `FATAL_ERROR`
  with instructions when headers are missing.

---

## 7. The reference sample — build it when stuck

`SDK_Invert_ProcAmp` in the **AE** SDK is a complete, working AE GPU effect
(CUDA + OpenCL + DirectX + Metal). It is the ground truth; reading and building
it settled in minutes what days of black-box probing got wrong.

```
<AE SDK>\Examples\Effect\SDK_Invert_ProcAmp\      the sample
<AE SDK>\Examples\GPUUtils\                       PrGPU kernel-support headers
```

Building it here needed three workarounds, all reusable:

1. **Toolset** — its `.vcxproj` targets `v142`/`v143`. Skip MSBuild; drive
   `cl`/`link` directly through `vcvarsall.bat`, as our own Ninja build does.
2. **Boost** — `KernelWrapper.h` (which the `GF_KERNEL_FUNCTION` macro needs)
   includes Boost.Preprocessor. If you don't have boost, hand-expand the kernel
   into a plain `__global__` function; the macro only builds the signature and
   the thread-index preamble.
3. **nvcc vs VS 2026** — `cudafe++` crashes against the newest STL. Point nvcc
   at a side-by-side 14.4x toolset with `-ccbin`, plus `--use-local-env`,
   `-allow-unsupported-compiler`, and a rebuilt `INCLUDE`/`VCToolsInstallDir`.
   The exact recipe is in `EdgeRepair/CMakeLists.txt`.

Instrument its `PreRender` / `SmartRenderGPU` with a file log before testing —
the effect renders correctly on CPU *and* GPU, so **looking at the frame tells
you nothing**.

Other samples, and what they are good for:

| Sample | SDK | Use for AE? |
|---|---|---|
| `SDK_Invert_ProcAmp` | **AE** | **Yes — the AE GPU reference** |
| `Vignette` | Premiere | Partly — AE+Premiere hybrid, but GPU side is Premiere's |
| `SDK_ProcAmp` | Premiere | **No** — CPU render is `'PrMr'`-gated and GPU init has no CUDA branch, so in AE it renders a **black frame by design** |
| `SDK_CrossDissolve` | Premiere | **No** — transitions are a Premiere concept |

---

## 8. What we got wrong (so you don't repeat it)

Seven hypotheses were tested against live AE, each costing a restart, all
negative — because the real problem (§2.2) was in none of them:

| # | Hypothesis | Verdict |
|---|---|---|
| 1 | `SUPPORTS_DIRECTX_RENDERING` confuses AE-on-CUDA | Wrong — Adobe sets it; removing it changed nothing |
| 2 | `SUPPORTS_QUERY_DYNAMIC_FLAGS` gates GPU | Wrong — and caused 25::228 |
| 3 | Must install to MediaCore | **Backwards** — that's Premiere's folder |
| 4 | Must declare GPU pixel formats | Wrong — Premiere-only suite |
| 5 | `USE_OUTPUT_EXTENT` must be absent | Wrong both ways; irrelevant |
| 6 | `SUPPORTS_SMART_RENDER` disqualifies GPU | Wrong |
| 7 | Missing classic `PF_Cmd_RENDER` handler | Wrong — AE never called it |

**The single lesson.** Every wrong turn came from inferring host behaviour by
examining shipping third-party binaries. A shipping `.aex` tells you what *that
plugin* does — and it usually ships for both AE and Premiere. Only Adobe's
samples tell you what *the host* requires. When a mechanism question comes up:
**read the sample, build the sample, instrument the sample.**

---

## 9. Realistic payoff

For a memory-bandwidth-bound effect at 4K (EdgeRepair's profile):

- CPU today: ~300 ms/frame at 4K on 8 threads.
- Expected GPU: **~15–40 ms/frame**, i.e. roughly **8–20×**, mostly from the
  ~10× DRAM bandwidth ratio (≈35–40 GB/s across 8 cores vs ≈350–450 GB/s).
- Frames handed to `SMART_RENDER_GPU` are **already GPU-resident**, so there is
  no PCIe round-trip per frame on this path.
- Second-order win: moving work off the CPU frees cores for Multi-Frame
  Rendering, so comp throughput usually improves by more than the single-frame
  numbers suggest.

Estimates until a real kernel runs; the passthrough milestone exists to make
the first measurement possible.

---

## 10. Tools in this repo

- `tools/pipl_dump.py <file.aex>` — parse a plugin's PiPL off
  disk (PE resource walk): OutFlags, OutFlags_2, spec version, match name,
  entry point.
- `tools/pipl_sweep.py <dir> …` — sweep `.aex` files and report
  GPU declarers.
- `tools/gpu_status.ps1` — read the three GPU signals off a
  build (§5).

Both are useful for checking *your own* build. Per §8, be careful about drawing
conclusions from other vendors' binaries.
