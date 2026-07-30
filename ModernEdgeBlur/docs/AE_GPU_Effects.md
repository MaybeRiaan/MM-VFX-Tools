# GPU-accelerating an After Effects effect (AE 2026, Windows/NVIDIA)

**Audience: any plugin in this suite.** EdgeRepair paid the discovery cost;
nothing here is EdgeRepair-specific. Per-plugin status lives in that plugin's
own doc.

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
> §7 records the wrong turns so nobody repeats them.

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

---

## 2. The checklist

Six things. Miss any one and the failure is **silent**: the effect renders on
the CPU with no error and looks perfect.

### 2.1 Declare the GPU flags — and guard the Premiere branch

Mirror Adobe's own AE GPU sample (`SDK_Invert_ProcAmp`, §6):

```cpp
out_data->out_flags  = PF_OutFlag_PIX_INDEPENDENT | PF_OutFlag_DEEP_COLOR_AWARE;
out_data->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE
                     | PF_OutFlag2_SUPPORTS_SMART_RENDER
                     | PF_OutFlag2_SUPPORTS_THREADED_RENDERING;

if (in_data->appl_id == 'PrMr') {
    // Premiere wants pixel formats instead (see §5)
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
  dynamic flags — see §5.

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
  concurrent Multi-Frame Rendering launches, which is what makes the path
  lock-free.
- 16f (`_16f`) formats are declared on the Premiere path; a kernel that may see
  them must handle half floats.

---

## 4. How to tell whether it is actually working

The failure mode is silent. Use these, in this order.

### 4.1 Log inside the GPU entry points

Log `what_gpu` in PreRender and log entry to `SMART_RENDER_GPU`. This is the
decisive signal:

```
PRERENDER what_gpu=3 gpu_data=0x… -> GPU_RENDER_POSSIBLE=YES
SMART_RENDER GPU what_gpu=3 out=3840x2160 src=3840x2160
```

`what_gpu`: `0 = NONE, 1 = OPENCL, 2 = METAL, 3 = CUDA, 4 = DIRECTX`.
**`what_gpu=0` at every PreRender ⇒ the gate is shut — go to §2.2 first.**

### 4.2 Make the milestone kernel **visibly destructive**

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

### 4.3 Don't bother with the registry

`GPUVideoFilter.0` under
`HKCU\Software\Adobe\After Effects\26.2\PluginCache\en_US` is the **Premiere**
mechanism's record. Its presence or absence says nothing about AE GPU. (An
earlier revision presented it as the decisive check. It is not.)

Still useful: `%APPDATA%\Adobe\After Effects\26.2\Plugin Loading.log` shows
which loader picked up each plugin.

---

## 5. Gotchas

- **`PF_Cmd_QUERY_DYNAMIC_FLAGS` — don't declare it unless you need it.**
  `SUPPORTS_QUERY_DYNAMIC_FLAGS` is not part of the GPU recipe, and getting the
  handler slightly wrong raises an unsuppressable **25::228** on *every* apply.
  AE hands you `out_flags` **pre-populated**, so even OR-ing one dynamic bit
  leaves non-dynamic bits set and counts as changing them. If your flags are
  static, simply don't advertise the capability.
- **Never acquire suites with `AEFX_AcquireSuite` in `GLOBAL_SETUP`.** On
  failure it sets `PF_OutFlag_DISPLAY_ERROR_MESSAGE` (`0x100`) on
  `out_data->out_flags` and writes `return_msg`
  (`sdk/Util/AEFX_SuiteHelper.c:51-57`). That silently corrupts your reported
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

## 6. The reference sample — build it when stuck

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
   The exact recipe is in this repo's `CMakeLists.txt`.

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

## 7. What we got wrong (so you don't repeat it)

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

## 8. Realistic payoff

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

## 9. Tools in this repo

- `tools/pipl_dump.py <file.aex>` — parse a plugin's PiPL off disk (PE resource
  walk): OutFlags, OutFlags_2, spec version, match name, entry point.
- `tools/pipl_sweep.py <dir> …` — sweep `.aex` files and report GPU declarers.

Both are useful for checking *your own* build. Per §7, be careful about drawing
conclusions from other vendors' binaries.
