# GPU-accelerating an After Effects effect (AE 2026, Windows/NVIDIA)

**Audience: any plugin in this suite.** This is the general, reusable
writeup — EdgeRepair happened to be the plugin that paid the discovery cost,
but nothing here is EdgeRepair-specific. Per-plugin status and the raw
experiment log live in `docs/EdgeRepair.md` ("Stage 6 investigation state").

Everything below was established empirically on the test box
(Win 10, i7-9700K, RTX 3070 Ti, driver 576.28, AE 2026 / 26.2, Mercury CUDA)
between 2026-07-23 and 2026-07-29. Where something is *believed* rather than
*measured*, it says so.

---

## 1. The headline: the documented AE-SDK GPU path did not work

The AE C++ SDK documents a GPU path built from
`PF_OutFlag2_SUPPORTS_GPU_RENDER_F32`, `PF_Cmd_GPU_DEVICE_SETUP`,
`PF_RenderOutputFlag_GPU_RENDER_POSSIBLE`, and `PF_Cmd_SMART_RENDER_GPU`.

**We implemented all of it, correctly, and AE never rendered a GPU frame.**
The signature: AE runs `GPU_DEVICE_SETUP` successfully at launch (so it *did*
parse our GPU flags), then offers `what_gpu = NONE` at every single PreRender
and never calls `PF_Cmd_SMART_RENDER_GPU`.

What actually drives GPU effect rendering in AE 2026 is the **MediaCore
GPUVideoFilter** mechanism — a Premiere-style GPU filter module exported as
`xGPUFilterEntry`, bound to the AE effect by match name.

Evidence (all verifiable on any machine with these plugins installed):

| Check | Result |
|---|---|
| Plugins on disk declaring `SUPPORTS_GPU_RENDER_F32` | 66 of 218 PiPLs |
| Adobe natives that GPU-accelerate (Fractal Noise, Transform, Directional Blur, Lumetri) | **all export `xGPUFilterEntry`** |
| Working 3rd-party GPU effects (Red Giant, Artlist, CLEngine) | **all export `xGPUFilterEntry`** |
| Plugins with the F32 flag but no `xGPUFilterEntry` (ours, Jawset Postshot) | **never get `what_gpu != NONE`** |
| AE's plugin registry (`HKCU\Software\Adobe\After Effects\26.2\PluginCache\en_US`) | 82 entries carry a `GPUVideoFilter.0` subkey — exactly the GPU-capable population |

**Caveat, stated honestly:** Adobe still documents the F32 path, and one
Adobe forum thread describes `SmartRenderGPU` firing for a developer on an
unstated AE version. It may work on other builds or need something
undocumented. On *this* configuration, nothing GPU-renders without a
GPUVideoFilter registration. A cheap future counter-test: build Adobe's own
`SDK_Invert_ProcAmp` sample from the full AE SDK and see whether it gets
`what_gpu`.

---

## 2. What a GPU-capable effect needs — the checklist

Five things. Miss any one and the failure is **silent** — the effect just
renders on the CPU with no error, and (this is the cruel part) *looks
completely fine*.

### 2.1 Export `xGPUFilterEntry` — a GPUVideoFilter module

Needs the **Premiere Pro C++ SDK** (free Adobe download; the AE SDK does not
contain these headers). Requires `PrSDKGPUFilter.h`,
`PrSDKGPUDeviceSuite.h`, `PrSDKGPUImageProcessingSuite.h`, and
`PrGPUFilterModule.h` (the last one lives in the SDK's
`Examples/Projects/GPUVideoFilter/Utils/`, not in `Examples/Headers/`).

Subclass `PrGPUFilterBase`, implement `Initialize` + `Render`, and export via
`DECLARE_GPUFILTER_ENTRY(PrGPUFilterModule<YourClass>)`. See
`src/gpu/ER_GPUFilter.cpp` for a working, commented skeleton.

The SDK's own reference implementations are worth reading:
`SDK_ProcAmp` (simplest), `Vignette` (AE-style hybrid — its header comment
explicitly says it works "in After Effects and Premiere Pro").

### 2.2 Install to the MediaCore folder, not AE's Plug-ins folder

```
C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\
```

with `CUDA_Assets\` / `DirectX_Assets\` **next to the .aex** (the Premiere
SDK readme states this rule for kernel assets).

From AE's own `Support Files\Plug-ins\`, the effect loads through the plain
AE-effect loader (`7a7d3cd3-…` in AE's `Plugin Loading.log`), AE never looks
for a GPU module, and **no `GPUVideoFilter.0` record is created**. From
MediaCore it loads through the MediaCore loader (`74697f7d-…`) and gets one.
Adobe's own natives are the exception that proves the rule — they're
special-cased inside AE's install.

Keep exactly **one** installed copy; a stale copy in the AE folder
double-registers the match name.

### 2.3 Declare GPU pixel formats — **Premiere only; NOT an AE step**

> **Corrected 2026-07-29.** An earlier revision of this document listed this
> as a required AE step and claimed its absence was why AE registers a GPU
> module but never instantiates it. **That was wrong**, and it cost several
> AE round-trips. It is recorded here so nobody repeats it.

Guard the call on the host. Both of Adobe's GPUVideoFilter samples do
exactly this — `SDK_ProcAmp_CPU.cpp:37` and `Vignette_CPU.cpp:92`:

```cpp
if (in_data->appl_id == 'PrMr') {          // Premiere ONLY
    ClearSupportedPixelFormats(effect_ref);
    AddSupportedPixelFormat(effect_ref, PrPixelFormat_VUYA_4444_32f);
    // ...formats in descending order of preference
}
```

In After Effects the suite genuinely **does not exist**: acquiring it returns
`kSPSuiteNotFoundError` (`'S!Fd'` = `0x53214664`) for *both* suite versions.
That is the correct, expected result in AE — not a defect to chase.

**Do not infer host behaviour from shipping binaries.** Finding the string
`"PF Pixel Format Suite"` inside `OpticalGlow.aex` (and 21 others) looks like
proof that AE vends the suite. It is not — it only shows the plugin
references it for its *Premiere* build, behind this same `'PrMr'` guard.
Adobe's samples are authoritative about the host; stripped third-party
binaries are not.

**Never acquire it with `AEFX_AcquireSuite`.** On failure that helper sets
`PF_OutFlag_DISPLAY_ERROR_MESSAGE` (`0x100`) on `out_data->out_flags` and
writes `return_msg` (`sdk/Util/AEFX_SuiteHelper.c:51-57`). Called from
`GLOBAL_SETUP` — where this call belongs — that silently corrupts the
reported global outflags and AE refuses to load the plugin with
*"global outflags mismatch ... code flags are 2000500 and PiPL flags are
2000400"* (**25::16**); `0x500` vs `0x400` is exactly that bit. Go through
`in_data->pica_basicP->AcquireSuite` directly so a miss is silent.

**FourCC trap** (still applies when you do this for Premiere). The
`PrPixelFormat_GPU_*` labels live in the *Premiere* headers, which shouldn't
be pulled into an AE-SDK translation unit. Build them with
`MAKE_ADOBE_PRIVATE_PIXEL_FORMAT_FOURCC` from the AE SDK's own
`PrSDKPixelFormat.h`. **Never hand-roll the constant** — the private prefix
is `'@'` (not `'A'`), byte order is reversed, and 16f is lowercase `'a'`
against 32f's `'A'`. Each mistake fails silently as "the host never offers
that format."

### 2.4 Keep the AE-side declarations (harmless, and they match the working set)

Red Giant and Artlist ship *both* the AE GPU flags and the GPU module. Ours
now declare `SUPPORTS_GPU_RENDER_F32`, `FLOAT_COLOR_AWARE`,
`SUPPORTS_SMART_RENDER`, `SUPPORTS_THREADED_RENDERING`.

Worth knowing before you copy that set: **neither Adobe sample declares
`SUPPORTS_GPU_RENDER_F32`.** `SDK_ProcAmp` sets `OutFlags 0x40 / OutFlags_2
0x100`; `Vignette` sets `0x440 / 0x1440`. Both GPU-render. That flag belongs
to the *other* (documented, non-functional here) path in §1, and is very
likely inert for the GPUVideoFilter mechanism. It appears harmless, so we
still ship it, but do not assume it is doing anything.

Three specific notes:

- **Do not declare `SUPPORTS_QUERY_DYNAMIC_FLAGS`** unless the effect really
  has dynamic flags — it is not part of the GPU recipe and it is an easy
  route to a per-apply AE error. See the 25::228 entry in §3.

- **`PF_OutFlag_USE_OUTPUT_EXTENT` — set it.** *(Reversed 2026-07-29.)* An
  earlier revision said never to set it, inferred from a sweep showing zero
  of 63 shipping third-party GPU declarers using it. **Adobe's own samples
  contradict that sweep**: `SDK_ProcAmp` declares `0x40` — that flag *alone*,
  nothing else — and `Vignette`, whose header says it runs in both AE and
  Premiere, declares `0x440`. Prefer samples over inference from stripped
  binaries.
- **Do not set `SUPPORTS_DIRECTX_RENDERING`** for AE. It declares the
  effect's framework to *be* DirectX; AE-on-Windows drives effects on CUDA.
  Set it conditionally for Premiere hosts (`in_data->appl_id`) if ever wanted.

### 2.5 Match the PiPL and the runtime flags exactly

`AE_Effect_Global_OutFlags`/`_2` in the `.r` file must equal what
`GlobalSetup` sets at runtime, or AE rejects the plugin with a misleading
"version mismatch" error. Our build script mirrors the version bump into both
files for this reason.

---

## 3. Hard-won gotchas

- **`PF_Cmd_QUERY_DYNAMIC_FLAGS` — do not declare it unless you truly need
  it.** `SUPPORTS_QUERY_DYNAMIC_FLAGS` is **not** part of the GPU recipe (it
  was tried as a gate experiment and did nothing), and getting its handler
  even slightly wrong raises an **unsuppressable AE error 25::228** —
  *"effect cannot change non-dynamic flag bits during
  PF_Cmd_QUERY_DYNAMIC_FLAGS"* — on **every apply of the effect**.
  We hit 25::228 twice: first by re-asserting the full static flag set (via
  `GlobalSetup`), then again by OR-ing a single dynamic bit into
  `out_data->out_flags`. AE hands you that struct **pre-populated**, so
  OR-ing still leaves non-dynamic bits set and AE counts that as changing
  them. Only these bits may be touched — OutFlags: `WIDE_TIME_INPUT`,
  `NON_PARAM_VARY`, `PIX_INDEPENDENT`, `I_USE_SHUTTER_ANGLE`; OutFlags2:
  `I_USE_3D_CAMERA/LIGHTS`, `DOESNT_NEED_EMPTY_PIXELS`, `REVEALS_ZERO_ALPHA`,
  `DEPENDS_ON_UNREFERENCED_MASKS`, `OUTPUT_IS_WATERMARKED`.
  **If your flags are all static (the normal case), simply don't declare the
  capability** — AE only sends the command to effects that advertise it, so
  the error becomes impossible. That is what EdgeRepair does now.
- **`PrGPUFilterModule.h` doesn't compile standalone** — it uses
  `PF_TransitionSuite` without including it. Include `PrSDKAESupport.h`
  first. (Adobe's examples get it via their effect header chain.)
- **Keep the Premiere headers in exactly one TU.** Per-source include dirs +
  `PRWIN_ENV` for the GPU-module file only; never mix them into AE-SDK files.
- **GPU frames are BGRA**, and may be 16f — not the ARGB float the AE CPU
  path uses. Any kernel port must handle channel order *and* half float.
- **`GetParam(index, time)` shifts by one** in a GPU filter: "GPU filters do
  not include the input frame."
- **SDKs are never committed.** Both the AE and Premiere SDKs are
  non-redistributable; `sdk/` is gitignored. CMake should `FATAL_ERROR` with
  instructions when the headers are missing, so the other build machine gets
  a clear message instead of a confusing compile failure.

---

## 4. How to tell whether it's actually working

The failure mode is silent, so use these three signals — in this order of
reliability. **Do not trust "it renders correctly" as evidence of anything.**

1. **The registry record** (decisive, checkable without AE running):
   ```powershell
   Get-ChildItem "HKCU:\Software\Adobe\After Effects\26.2\PluginCache\en_US" |
     Where-Object PSChildName -match 'YourPlugin' |
     ForEach-Object { Get-ChildItem $_.PSPath }
   ```
   A `GPUVideoFilter.0` subkey means AE registered the module. Its values
   should read `MatchName = AE.<Your Match Name>`, `InterfaceVersion = 2`.
   No subkey ⇒ problem is registration (§2.1/§2.2), not rendering.

2. **Your own log from inside the module.** Log in `Initialize` and `Render`.
   Registration without these lines means the host declined to instantiate
   (that's what §2.3 fixed). Ours writes to `%TEMP%\EdgeRepair_gpu.log`.

3. **A/B the Mercury renderer — but only with a *deliberately wrong* kernel.**
   CUDA and Software Only must look **different**. Identical output = the GPU
   module is not running. Better than timing, because a fast CPU path can mask
   a modest GPU win.

   **A passthrough kernel makes this test useless** — we ran it for days with
   a device-to-device copy, where "identical output" was guaranteed whether or
   not the module ran, and mistook that for evidence. Make the milestone
   kernel visibly destructive (e.g. `cuMemsetD8Async` zeroing the top eighth
   of the frame), confirm the difference, *then* write the real kernel.

Also useful: AE's `Plugin Loading.log`
(`%APPDATA%\Adobe\After Effects\26.2\`) shows which loader picked up each
plugin — the MediaCore vs AE-effect distinction from §2.2.

---

## 4a. Read the samples first

The Premiere SDK's GPUVideoFilter samples settled in minutes several questions
that days of black-box probing against AE got wrong. **Read them before
theorising.** On this box:

```
E:\OFX DEV\AE SDK\Premiere Pro 26.0 C++ SDK Windows\
    Premiere Pro 26.0 C++ SDK\Examples\Projects\GPUVideoFilter\
        SDK_ProcAmp\      simplest complete GPU filter
        Vignette\         AE + Premiere hybrid — closest to what we want
        SDK_CrossDissolve\  transition variant
```

Online mirror of the same docs: https://ppro-plugins.docsforadobe.dev/

Each sample carries `.r` (PiPL flags), `_CPU.cpp` (AE side, `GlobalSetup`),
`_GPU.cpp` (the module), `Win/*.vcxproj` (exact compile/link settings), and a
`readme.txt` with the DXC shader-compile step.

The lesson worth generalising: **a shipping third-party binary tells you what
that plugin does; only a sample tells you what the host requires.** Every
wrong turn recorded in this document came from inferring host behaviour by
sweeping installed `.aex` files.

---

## 5. Tools in this repo

- `tools/pipl_dump.py <file.aex>` — parse a plugin's PiPL straight off disk
  (PE resource walk): OutFlags, OutFlags_2, spec version, match name, entry
  point. Works on any `.aex`, including third-party ones.
- `tools/pipl_sweep.py <dir> [dir…]` — sweep every `.aex` under given roots
  and report which declare `SUPPORTS_GPU_RENDER_F32`, with named flag bits.
  This is what found the specimen population.

Ad-hoc but easily rebuilt: a PE export-table dumper (does a plugin export
`xGPUFilterEntry`?) and a binary suite-name scanner (which host suites does
it acquire?). Both were decisive; see the session history in
`docs/EdgeRepair.md` if they're needed again.

---

## 6. Realistic payoff

For a memory-bandwidth-bound effect at 4K (EdgeRepair's profile — its box
blur was measured *2.3× slower* than the exact Gaussian, which is the
signature of bandwidth-bound, not compute-bound):

- CPU today: ~300 ms/frame at 4K on 8 threads (was ~1150 ms before CPU
  optimisation).
- Expected GPU: **~15–40 ms/frame**, i.e. roughly **8–20×**, mostly from the
  ~10× DRAM bandwidth ratio (≈35–40 GB/s across 8 cores vs ≈350–450 GB/s).
- Watch the transfer floor: a 4K float RGBA frame is 132 MB, so ~20 ms
  round-trip over PCIe 3.0 if the frame isn't already GPU-resident. In a
  Mercury chain with other GPU effects it stays resident and that cost
  disappears.
- Second-order win: moving work off the CPU frees all 8 cores for
  Multi-Frame Rendering, so comp throughput usually improves by more than the
  single-frame numbers suggest.

These are estimates until a real kernel runs; the passthrough milestone
exists to make the first measurement possible.
