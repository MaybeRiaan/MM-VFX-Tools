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
follows it fully (`MM_Qualify/src/gpu/`, all four prologues as of 2026-08-04 —
HLSL, CUDA, host-C++; Metal still to write); EdgeRepair reaches the same
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
> by After Effects**, which offers CUDA on Windows/NVIDIA. That did not
> invalidate anything in this document; the kernels were bit-exact and the
> layout sound. It meant the DX12 *host* is a Premiere-host path, and that AE
> coverage needed a CUDA backend behind the same kernels. **Both plugins now
> have one** (2026-08-04) — and §6's decisions were backend-independent enough
> to carry over unchanged, which is the clearest evidence the layout was right.

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

This is the same discipline `MM_FreqSep/src/fs_common.h` already states
for CPU code ("AE-facing but algorithm-free; pure image algorithms go in
AE-free headers"), extended one axis: **AE-free → also platform-free.**

---

## 1. Layout

```
MM_FreqSep/src/gpu/          (MM_Qualify/src/gpu/ mirrors this, qk_* prefix)
  kernels/
    fs_prologue_hlsl.h      HLSL spellings of the shared vocabulary
    fs_prologue_cuda.h      CUDA spellings of the SAME vocabulary
    fs_prologue_host.h      host-C++ spellings — layout mirror + type-check
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
  cuda/
    fs_kernels.cu           one shim, ALL kernel files; nvcc emits one PTX
                            carrying every entry point, deployed as
                            CUDA_Assets/ next to the .aex and loaded with
                            cuModuleLoadDataEx (EdgeRepair's arrangement).
                            CMake SKIPS this step when the toolkit is absent:
                            no PTX ships, device setup declines CUDA, AE
                            renders on the CPU — the plugin still builds.
    fs_kernel_args.h        the same kernel files through fs_prologue_host.h:
                            the launch-argument structs, compiled FROM the
                            kernels so the host cannot drift from them (§2.1),
                            and — because a body compiled here is ordinary,
                            runnable C++ — what the AE-free tests EXECUTE
                            instead of a transcription (§5)
  mm_gpu_host.h             AE-facing, platform-free, header-only: the band
                            tile planner + FS_GPUJob description. ALL tiling
                            arithmetic lives here so tests can prove it with
                            no GPU (see §5); platform hosts only execute it.
                            Also owns the framework→backend dispatch helpers.
  mm_gpu_cuda.cpp           CUDA host: PTX module, launcher, cached scratch
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

| Vocabulary        | HLSL                              | CUDA                               | MSL (planned)                          |
|-------------------|-----------------------------------|------------------------------------|----------------------------------------|
| `KERNEL_BEGIN(n)` | *(nothing)*                       | `namespace fsk_n { struct Args {`  | same as CUDA                            |
| `KERNEL_ENTRY(n)` | `[numthreads(8,8,1)] void n(...)` | `__device__ void run(int,int) const` | `void run(...)` on the arg struct     |
| `KERNEL_END(n)`   | *(nothing)*                       | `}; }` + `__global__` trampoline   | `}; }` + `kernel void` trampoline       |
| `GID_X` / `GID_Y` | `tid.x` / `tid.y` (SV_DispatchThreadID) | `run()`'s two int params     | `gid.x` / `gid.y` (thread_position_in_grid) |
| `BUF_IN(n, i)`    | `RWStructuredBuffer<float>` @ `u(i)`   | `const float *n;` member — i'th declared | `device const float*` @ `buffer(i)` |
| `BUF_OUT(n, i)`   | `RWStructuredBuffer<float>` @ `u(4+i)` | `float *n;` member               | `device float*` @ `buffer(4+i)`         |
| `PARAM_BLOCK_BEGIN` / `PARAM_BLOCK_END` | `cbuffer` @ `b0` (root constants) `{` … `};` | *(nothing)* — the params become plain members of the same struct | `constant ...&` argument |
| `KF_EXP(x)`       | `exp(x)`                          | `::expf(x)`                        | `metal::exp(x)`                         |

> **Why the param block is a BEGIN/END pair and not a single `PARAM_BLOCK`.**
> An earlier revision of this table named one token. It cannot be one: on HLSL
> the block has to *bracket* its declarations (`cbuffer Name : register(b0) {`
> … `};`), and a single macro cannot open a scope that the kernel's own field
> list then closes. Both plugins have always spelled it `PARAM_BLOCK_BEGIN` /
> `PARAM_BLOCK_END`; the table was the thing that was wrong. Fixed 2026-08-04.

**The register/index map IS the binding contract:** inputs occupy slots 0–3,
outputs 4–7, on every platform. Order, not spelling, must match.

### 2.1 Why entry points are BRACKETED — the rule that makes §0 achievable

HLSL binds buffers by **register at file scope**: a `RWStructuredBuffer` sits
outside every entry point and all of them see it. **CUDA and Metal do not have
that.** In both, a kernel's buffers and constants are its *function parameters*
(`__global__ void k(Args)`, `device float* b [[buffer(0)]]`), and neither can
reference a file-scope buffer declaration at all.

So a kernel written only against the file-scope form is expressible **in HLSL
and nowhere else** — and §0's promise, that a kernel validated on DirectX is
already validated on Metal modulo host plumbing, is not reachable. This is not a
CUDA quirk; HLSL is the outlier.

`KERNEL_BEGIN(name)` / `KERNEL_END(name)` bracket each entry point so its
`BUF_*` and `PARAM_BLOCK` declarations — unchanged, in their original order —
become the **members of one per-entry argument struct**, and the body becomes a
method on it. The body then reads `gSrc` and `gW` as inherited scope, exactly as
it read the HLSL globals; binding order is preserved because declaration order
is preserved. The HLSL prologue expands both brackets to **nothing**, so the
preprocessed HLSL is what it always was.

Two consequences worth stating outright:

- **Every entry point must be self-contained.** Under HLSL, cbuffer members land
  in one global scope, so an entry could silently borrow a sibling's
  `PARAM_BLOCK` — `fs_guided_squares` did. Nothing leaks between argument
  structs, so each entry now declares its own. Prefix param names per entry
  (`gSqW`, not a second `gW`) and the HLSL side stays legal too.
- **The host builds the same structs.** `cuda/fs_kernel_args.h` includes the
  same `.kernel` files through `fs_prologue_host.h`, so the launch arguments are
  compiled *from* the kernel declarations rather than hand-mirrored beside them.
  There is one declaration, compiled twice; there is nothing left to keep in
  sync. That header also type-checks every kernel body as ordinary C++ on every
  build, with no GPU and no toolkit — the C++ counterpart of the always-compiled
  HLSL entry in `AE_GPU_Effects.md` §4.5.

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
Matches `fs::UnpackRow`. Index as `(y * W + x) * 4 + c`, c: 0=A 1=R 2=G 3=B.

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

> **`double` is affordable only while the sum is O(1) per pixel.** Consumer
> GeForce cards run fp64 at **1/64** of fp32 rate (0.34 vs 21.7 TFLOPS on the
> RTX 3070 Ti test box); CUDA carries the identical penalty, so this is not a
> DX-only concern. `fs_guided.kernel` deliberately trades the CPU's O(1) sliding
> window for an O(r) direct sum to parallelise, justified in its header against
> `GuidedRadius(sigma=8) ≈ 9`. But the radius follows the separation sigma —
> `GuidedRadius(σ) = 0.5(√(1+6σ²)−1)`, so σ=90 gives **r ≈ 110**, a 221-tap
> window. That is ~110× the CPU's work at ~3× the throughput: the GPU box pass
> lands *slower than the CPU it replaces*, and more so the larger the radius.
> The accumulator is not the bug — the O(r) shape is. See the MM FreqSep plan's
> Phase 5 for the chunked sliding-window fix, which restores O(1) and keeps
> `double` cheap on every backend.
>
> **This is the GUIDED method only. It is not a property of Separation Radius.**
> Measured in AE 2026-08-12 at σ=90 with `guided=0` (Gaussian, `sepR=270`, a
> 541-tap separable pass at 3840×2636): the GPU ran *orders of magnitude faster*
> than the CPU, not slower. A separable Gaussian is O(r) per axis but trivially
> parallel and bandwidth-bound; the guided box pass is what degrades. Do not cite
> §3.7 when sweeping radius on the Gaussian path — the in-AE checklist did, and
> it set the expectation exactly backwards.

**3.6 — `fma` contraction is allowed to differ; tolerance absorbs it.**
Do not chase bit-exactness across platforms. Target max-abs ≤ 1e-5 against the
CPU reference (see §5), which comfortably covers contraction differences while
still catching real algorithmic error.

**3.6.1 — The 1e-5 target names a measurement point: kernel output, or an
image-bearing result. It does not survive a steep nonlinearity.**
A gate, a threshold, a step — anything whose job is to turn small input
differences into large output ones — multiplies contraction-level error by its
local slope. Measure *behind* one and you are grading the user's parameter
choices, not the code: the same kernels pass or fail depending on how soft
someone set a gate.

Measured on MM_Qualify, in AE, same footage and same build (2026-08-11):

| measured at | frame content | max-abs |
|---|---|---|
| image-bearing output (Selection Overlay) | 25.4% intermediate | **2.384e-07** — 4 ULP at 0.5, the fma signature |
| matte, very soft gates | 80.7% intermediate | **3.027e-09** |
| matte, moderate gates, denoise **off** | 37.1% intermediate | 6.855e-06 |
| matte, moderate gates, denoise on | 37.1% intermediate | **1.657e-05** — over target |
| matte, hard gates | 99.8% clipped | 0.000e+00 |

Same kernels throughout. The spread is gate slope, not math: the denoise-off
control already sits at 6.855e-06 with no Gaussian running, and the error does
not scale with tap count. In deliverable terms the 1.657e-05 worst case is
1/237th of an 8-bit code value — below the quantisation of any real output.

So: **gate a backend on an image-bearing mode.** Treat divergence measured on a
matte or other decision surface as informational, and if you must put a number
on it, state the gate softness alongside — the figure is meaningless without it.

**And check what you measured before trusting it.** A frame that is clipped,
binary, or empty matches bit-for-bit on any backend, because clipping destroys
exactly the last-bit differences the test looks for. Three separate `0.000e+00`
results during the 2026-08-11 session were hollow for this reason. Report the
fraction of pixels carrying intermediate values next to any parity figure; a
`0.000e+00` on a 99.8%-clipped frame is not evidence.

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

**Was parked 2026-08-02, unparked 2026-08-04.** Step 5.2 could not be run in
After Effects for either DX12 plugin: AE offers `what_gpu = CUDA` on the test
box and both hosts accepted DIRECTX only, so they declined the device and AE
rendered them on the CPU — silently and correctly. Both now have a CUDA host
behind the same kernels (`MM_FreqSep/src/gpu/mm_gpu_cuda.cpp`,
`MM_Qualify/src/gpu/qk_gpu_cuda.cpp`), so 5.2 is reachable and is the
outstanding step for each. 5.1 was unaffected throughout and remains the
regression gate.

**MM_Qualify additionally closed 5.1's own gap.** Its harness proved *tiling*
but not kernel bodies — both sides called the same `qk_core.h`, so the pixel
math was never independently exercised. Adding `qk_prologue_host.h` (§1) made
the bodies compile as ordinary C++ and therefore RUNNABLE, so
`MM_Qualify/tests/kernel_test.cpp` now runs every entry point against the CPU
reference at exactly `0.0`, and `pipeline_test.cpp`'s tiled side runs those
same real kernels through the real planner rather than a hand-written stand-in.
That is the arrangement to copy: a transcription can rot silently against the
kernel it mirrors (`MM_FreqSep/tests/gpu_parity_test.cpp` still transcribes, and
its own header notes the prologue is what lets that be deleted).

## 6. DX12 host decisions (Premiere's path — see §5.2)

Kept in full. These decisions were sound engineering against the wrong *host*,
and most of them are backend-independent: the budget gate, the tile planner,
the scratch-plane scheduling and the MFR-safety argument **did** carry over to
the CUDA hosts unchanged, which is now demonstrated rather than predicted. Read
this section as "how the host side was reasoned about".

What a CUDA host changes, and nothing else did:

- **No UAV barriers.** Launches on one CUDA stream execute in issue order with
  an implicit dependency; D3D12 gives no such guarantee for UAV writes, which
  is why the DX hosts barrier after every dispatch.
- **Scratch is cached per device, not per render** (EdgeRepair's arrangement),
  so a steady-state render allocates nothing. That makes it shared between
  concurrent MFR renders, so a mutex covers the whole ENQUEUE sequence — and
  because the stream is FIFO, serializing enqueue serializes execution.
- **Kernels ship as PTX in `CUDA_Assets/` next to the `.aex`**, not embedded,
  because `nvcc` produces it and the driver JITs it. Resolve every entry point
  at device setup and decline the whole backend if one is missing — a stale
  PTX is the classic half-finished deploy, and there is no CPU fallback once a
  render is underway.
- **The doubles device gate is MM_FreqSep's alone.** It exists for
  `fs_guided.kernel`'s box sums (§3.7). MM_Qualify has no box sums and gates on
  nothing; do not copy it reflexively.

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
