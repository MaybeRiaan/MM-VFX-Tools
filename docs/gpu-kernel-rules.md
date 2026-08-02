# MM Suite — GPU Kernel Rules (DX12 now, Metal later)

**Status:** authored during MM FreqSep Phase 5. The DirectX 12 host is
implemented (device setup, embedded-DXIL PSOs, banded tiling, matte, fence
sync) and the **complete pipeline — kernels AND band tiling — is validated
bit-exact against the CPU reference in the AE-free harness**
(`MM_FreqSep/tests/gpu_parity_test.cpp`, 0.000e+00 on every case). First in-AE
render validation (View popup per band, GPU vs Software-Only renderer) is the
remaining hands-on step — **currently blocked, see §5.2**; the protocol is
`MM_FreqSep/docs/phase6-validation.md`. **Metal is not yet
written.** These rules exist so that writing it later is a mechanical port,
not a re-derivation.

This document is the **suite reference** for kernel structure. MM_Qualify
already follows it (`MM_Qualify/src/gpu/`); EdgeRepair reaches the same
discipline through a single shared header instead of a prologue split — see
`AE_GPU_Effects.md` §4.6, which reconciles the two and recommends **this**
layout for new kernel work. (Explicitly *not* MasterFilm; see the Phase 5 note
in `MM_FreqSep/docs/mm-freqsep-plan.md` for why it is not applicable.)

> **This is the KERNEL contract — how to write shader bodies that survive a
> platform port.** Its companion,
> [`AE_GPU_Effects.md`](AE_GPU_Effects.md), is the HOST contract — how to get
> After Effects to dispatch to your GPU code in the first place, and what it
> hands you when it does. **Read that one first if your GPU path has never
> rendered a frame in AE**, because none of the rules here matter until the
> host gate is open — and see its §1.1 before choosing a backend: the DX12
> hosts described in §6 below are, on the current test box, **never exercised
> by After Effects**, which offers CUDA on Windows/NVIDIA. That does not
> invalidate anything in this document; the kernels are bit-exact and the
> layout is sound. It means the DX12 *host* is currently a Premiere-host path,
> and AE coverage needs a CUDA backend behind the same kernels.

Paths in this document are relative to the **suite root** unless they name a
project folder explicitly; where a rule cites `blur.cpp` or `guided.cpp`
without a prefix, it means MM_FreqSep's copy (`MM_FreqSep/src/`).

---

## 0. The one rule

> **Kernel bodies contain no platform syntax and no `#ifdef`.**

All platform difference lives in a *prologue* header that is included before the
kernel body. If a kernel needs a platform conditional, the prologue abstraction
is wrong — fix the prologue, not the kernel.

Why this is load-bearing: it means a kernel validated on DirectX is *already*
validated on Metal modulo host plumbing. Hand-translating two copies would leave
Metal unverified in both plumbing **and** math, and you would be debugging both
at once on a machine you did not have while writing it.

This is the same discipline `MM_FreqSep/src/common/MM_Common.h` already states
for CPU code ("AE-facing but algorithm-free; pure image algorithms go in
AE-free headers"), extended one axis: **AE-free → also platform-free.**

---

## 1. Layout

```
MM_FreqSep/src/gpu/          (MM_Qualify/src/gpu/ mirrors this, qk_* prefix)
  kernels/
    fs_prologue_hlsl.h      HLSL spellings of the shared vocabulary
    fs_prologue_metal.h     MSL spellings of the SAME vocabulary   [TODO: Metal]
    fs_gauss.kernel         separable Gaussian     — math only
    fs_guided.kernel        guided filter          — math only
    fs_compose.kernel       band recombine + view  — math only
    fs_matte.kernel         matte fit/bilinear/channel — math only
    fs_tile.kernel          world↔tile move + swizzle + fill — data movement
  dx/
    fs_*_cs.hlsl            2-line shims: prologue + kernel, one TU per file;
                            dxc compiles each entry point (-E) at BUILD time
                            into DXIL embedded as C headers (no runtime
                            compiler, no loose .cso next to the .aex)
  mm_gpu_host.h             AE-facing, platform-free, header-only: the band
                            tile planner + FS_GPUJob description. ALL tiling
                            arithmetic lives here so tests can prove it with
                            no GPU (see §5); platform hosts only execute it.
  mm_gpu_dx.cpp             DX12 host: root sig, PSOs, dispatch, fence
  mm_gpu_metal.mm           Metal host: pipeline state, encoder, dispatch [TODO]
```

`.kernel` is a deliberate non-extension: these files are not standalone
translation units and must never be compiled directly. They are `#include`d
by a platform prologue.

Worlds arrive as `PF_PixelFormat_GPU_BGRA128` (float BGRA, pitched rows) on
**both** DX and Metal — which is why the BGRA↔ARGB swizzle lives in a shared
kernel (`fs_tile.kernel`), not in a platform host. BGRA reversed IS ARGB, so
the swizzle is index reversal `(3 - c)`.

---

## 2. The shared vocabulary

Every kernel is written against these names only. The prologue defines them.

| Vocabulary        | HLSL                              | MSL (planned)                          |
|-------------------|-----------------------------------|----------------------------------------|
| `KERNEL_ENTRY(n)` | `[numthreads(8,8,1)] void n(...)` | `kernel void n(...)`                    |
| `GID_X` / `GID_Y` | `tid.x` / `tid.y` (SV_DispatchThreadID) | `gid.x` / `gid.y` (thread_position_in_grid) |
| `BUF_IN(n, i)`    | `RWStructuredBuffer<float>` @ `u(i)`   | `device const float*` @ `buffer(i)`     |
| `BUF_OUT(n, i)`   | `RWStructuredBuffer<float>` @ `u(4+i)` | `device float*` @ `buffer(4+i)`         |
| `PARAM_BLOCK`     | `cbuffer` @ `b0` (root constants) | `constant ...&` argument                |
| `KF_EXP(x)`       | `exp(x)`                          | `metal::exp(x)`                         |

**The register/index map IS the binding contract:** inputs occupy slots 0–3,
outputs 4–7, on both platforms. Order, not spelling, must match.

**Why inputs are `RW` on HLSL:** everything binds as a UAV, always. AE owns
the state of its world/scratch resources, and D3D12 buffers implicitly
promote COMMON→(first use) per submission — one binding class means zero
resource-state transitions on resources whose state we cannot know. Declaring
an input `RW` and never writing it costs nothing measurable on buffer loads.
Metal has no resource-state machine, so its prologue keeps `const`.

**A kernel FILE may hold several `PARAM_BLOCK`s** (fs_guided.kernel has
three). The HLSL prologue mints a unique cbuffer name per expansion
(`__COUNTER__`); all share register `b0`, which is legal because each entry
point uses exactly one and dxc strips the rest. The DX host feeds them as
root 32-bit constants — no CBV upload, no descriptor heap anywhere.

**Threadgroup size is 8×8 and is fixed in the prologue, not the kernel.** Metal
may want a different size (Apple GPUs commonly prefer 32-wide execution); that
is a prologue edit, and it must not require touching kernel bodies.

---

## 3. Numerical rules (these are correctness, not style)

**3.1 — Kernel weights are built on the CPU in `double`, uploaded as `float`.**
`blur.cpp:BuildKernel` accumulates the normalization sum in `double` before
casting. A GPU-side `float` rebuild diverges in the last bits and turns a
tolerance check into a coin flip. Upload the exact same weights the CPU used.

**3.2 — Out-of-range taps are SKIPPED, never clamped.**
`blur.cpp:71-72` narrows the tap range at buffer edges; the kernel weight sum
legitimately falls below 1.0 there. That reduced sum **is** the premultiplied
coverage weight that the compose pass renormalizes against. Clamp-sampling the
edge (the reflexive GPU choice, and what a texture sampler would do for free)
silently breaks the anti-fringing invariant. Index math only — no samplers.

**3.3 — No clamping of intermediates, anywhere.**
The high band is legitimately unbounded and may exceed [0,1] or go negative.
Only the final 8/16bpc pack clamps, and that happens on the CPU side.

**3.4 — Premultiplied ARGB throughout, channel order A,R,G,B.**
Matches `mm::UnpackRow`. Index as `(y * W + x) * 4 + c`, c: 0=A 1=R 2=G 3=B.

**3.5 — Blurred-alpha renormalization stays in compose, not in blur.**
The blur delivers premultiplied planes including a blurred alpha; compose
divides by it. Do not normalize inside the blur kernel.

**3.7 — Box sums accumulate in `ACCUM_T`, never raw `float`.**
`guided.cpp` accumulates its sliding window in `double` (guided.cpp:48-51). The
GPU kernel replaces that serial window with a parallel per-pixel direct sum —
a deliberate divergence, see the header of `fs_guided.kernel` — and that sum is
bit-exact against the CPU *only* in double. Measured on this codebase:

| accumulator      | max-abs vs `guided.cpp` |
|------------------|-------------------------|
| `double`         | **0.000e+00** (bit-exact, r up to 24) |
| `float`          | 2.441e-04 — 24× the 1e-5 tolerance     |

Route all box accumulation through `ACCUM_T` / `ACC_ADD` / `ACC_GET` so the
Metal prologue can substitute a compensated (Kahan) float sum, since Apple GPUs
do not support `double` in shaders. A `control:` test in
`MM_FreqSep/tests/gpu_parity_test.cpp` asserts the float variant still drifts — if that
test ever starts passing trivially, this hazard has stopped being covered.

**3.6 — `fma` contraction is allowed to differ; tolerance absorbs it.**
Do not chase bit-exactness across platforms. Target max-abs ≤ 1e-5 against the
CPU reference (see §5), which comfortably covers contraction differences while
still catching real algorithmic error.

---

## 4. Metal-specific items requiring on-device verification

Flagged now so they are not assumed working later. **None of these are math** —
they are all plumbing, which is the point of the structure.

- **Threadgroup sizing** — 8×8 is an HLSL-tuned guess; verify against
  `maxTotalThreadsPerThreadgroup` and prefer multiples of the SIMD width.
- **Buffer binding semantics** — MSL binds by `[[buffer(n)]]` index; HLSL binds
  by register/descriptor table. The prologue must make the *order* identical.
- **Non-uniform threadgroup dispatch** — Metal supports partial threadgroups
  via `dispatchThreads:`; DX12 requires manual bounds guards. Keep the explicit
  `if (x >= W || y >= H) return;` guard in kernels so both are safe.
- **Command buffer / fence lifetime** — AE hands us an `MTLCommandQueue`; the
  completion-wait model differs from DX12 fences.
- **`AcquireExclusiveDeviceAccess`** — per the suite header, not required for
  full-GPU entry points but required otherwise. Confirm on Metal.

---

## 5. Validation

Two layers, in order of authority:

**5.1 — AE-free, automated (`MM_FreqSep/tests/run_tests.ps1`).** Every `.kernel` body is
transcribed into C and diffed against the real CPU implementation, AND the
complete banded pipeline — load/swizzle → separation → smooth → matte →
compose → store, driven by the REAL `FS_PlanTiles` — is diffed against the
untiled CPU reference, including multi-band plans, margins clipped at world
edges, HDR (superwhite) values, and all View modes. All of it holds at
**0.000e+00**. Consequence: band margins, clipping, compose offsets, the
BGRA swizzle and the matte mapping are *proven* before AE ever runs — any
in-AE mismatch is D3D12 plumbing (bindings, barriers, sync) by elimination.

**5.2 — In-AE, hands-on.** Render the same frame with Mercury GPU (DirectX)
and with Software Only and diff per band via the View popup — Low Band and
High Band isolate the blur/guided kernels away from compose, which is what
makes a mismatch diagnosable rather than just visible. Full protocol:
`MM_FreqSep/docs/phase6-validation.md`. Target: **max-abs ≤ 1e-5** per channel
(§3.6 — fma/contraction differences on real silicon land well inside this).

Known gap, accepted deliberately: 5.2 is a *development-time* check, not a
regression gate — GPU rendering needs a live AE-hosted device. 5.1 is the
regression gate, and it covers everything except the D3D12 API layer itself.

**Parked, as of 2026-08-02:** step 5.2 cannot be run in After Effects for
either DX12 plugin. AE offers `what_gpu = CUDA` on the test box and these hosts
accept DIRECTX only, so they decline the device and AE renders them on the CPU
— silently and correctly. Per the decision in `AE_GPU_Effects.md` §1.2, CUDA is
the backend this suite maintains and these two plugins are re-examined after
unification. 5.1 is unaffected and remains the regression gate; what is missing
is real-silicon confirmation, and it will come via a CUDA host.

## 6. DX12 host decisions (parked — see §5.2)

Kept in full. These decisions were sound engineering against the wrong target,
and most of them are backend-independent: the budget gate, the tile planner,
the scratch-plane scheduling and the MFR-safety argument all carry over to a
CUDA host unchanged. Read this section as "how the host side was reasoned
about", not as "what to build next".

- **Shaders compile at BUILD time** (dxc from the Windows SDK, one `-E` per
  entry point) into DXIL embedded as C headers. No runtime compiler DLL, no
  `DirectX_Assets/` folder, a single self-contained `.aex`.
- **Root signature only** — 24 root constants at `b0` + eight root UAV
  descriptors (`u0..u7`). No descriptor heaps, no CBV ring buffer. Buffers
  bind by GPU virtual address, which also lets several logical planes live
  at offsets inside ONE `AllocateDeviceMemory` allocation.
- **No resource-state transitions** except our own weights buffer's
  COPY_DEST→UNORDERED_ACCESS after upload. Everything else relies on buffer
  COMMON-promotion/decay (§2). Hazards are ordered with global UAV barriers
  after every dispatch — the pipeline is a dependency chain anyway.
- **Gaussian weights upload verbatim** from `fs::BuildGaussianKernel`
  (double-accumulated, §3.1) through an upload-heap staging buffer we create
  ourselves (the suite's AllocateHostMemory contract is ambiguous on DX).
- **Guided runs in 3 scratch planes, not 5, by SCHEDULING** — the A/B stage
  writes into planes whose prior contents are dead (ping-pong), because a
  D3D12 dispatch cannot bind one resource as SRV and UAV at once. Same
  memory outcome as the original "write in place" plan, no aliasing hazard
  for Metal to inherit.
- **Doubles are a device gate:** GPU_DEVICE_SETUP refuses (silently → CPU
  render) on devices without `DoublePrecisionFloatShaderOps` (§3.7).
- **Budget gate in PreRender:** `PF_RenderOutputFlag_GPU_RENDER_POSSIBLE` is
  set only when the worst-case tile plan fits the scratch budget (¼ of
  dedicated VRAM, clamped [256MB, 1GB]) — the same planner the render runs,
  so gate and render cannot disagree. Infeasible frames render on the CPU:
  slower, never wrong.
- **Per-render objects only** (allocator/list/fence/scratch on the render
  stack) — MFR-safe with zero shared mutable state; AE's queue is
  free-threaded by API contract.
