# MM Suite — GPU Kernel Rules (DX12 now, Metal later)

**Status:** authored during MM FreqSep Phase 5. DirectX 12 is implemented and
validated against the CPU reference. **Metal is not yet written.** These rules
exist so that writing it later is a mechanical port, not a re-derivation.

This document is the **suite reference** — ModernEdgeBlur and EdgeRepair adopt
the same structure when they go cross-platform. (Explicitly *not* MasterFilm;
see the Phase 5 note in `mm-freqsep-plan.md` for why it is not applicable.)

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

This is the same discipline `src/common/MM_Common.h` already states for CPU code
("AE-facing but algorithm-free; pure image algorithms go in AE-free headers"),
extended one axis: **AE-free → also platform-free.**

---

## 1. Layout

```
src/gpu/
  kernels/
    fs_prologue_hlsl.h      HLSL spellings of the shared vocabulary
    fs_prologue_metal.h     MSL spellings of the SAME vocabulary   [TODO: Metal]
    fs_gauss.kernel         separable Gaussian    — math only
    fs_guided.kernel        guided filter         — math only
    fs_compose.kernel       band recombine + view — math only
  mm_gpu_host.h/.cpp        AE-facing, platform-free: device acquire, buffers
  mm_gpu_dx.cpp             DX12 host: PSO, root sig, descriptor heap, dispatch
  mm_gpu_metal.mm           Metal host: pipeline state, encoder, dispatch [TODO]
```

`.kernel` is a deliberate non-extension: these files are not standalone
translation units and must never be compiled directly. They are `#include`d
by a platform prologue.

---

## 2. The shared vocabulary

Every kernel is written against these names only. The prologue defines them.

| Vocabulary        | HLSL                              | MSL (planned)                          |
|-------------------|-----------------------------------|----------------------------------------|
| `KERNEL_ENTRY(n)` | `[numthreads(8,8,1)] void n(...)` | `kernel void n(...)`                    |
| `GID_X` / `GID_Y` | `tid.x` / `tid.y` (SV_DispatchThreadID) | `gid.x` / `gid.y` (thread_position_in_grid) |
| `BUF_IN(i)`       | `StructuredBuffer<float>`         | `device const float*`                   |
| `BUF_OUT(i)`      | `RWStructuredBuffer<float>`       | `device float*`                         |
| `PARAM_BLOCK`     | `cbuffer`                         | `constant ...&` argument                |
| `KF_EXP(x)`       | `exp(x)`                          | `metal::exp(x)`                         |

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
`tests/gpu_parity_test.cpp` asserts the float variant still drifts — if that
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

GPU output is validated **against the CPU path**, which is the reference by
definition (see `mm-freqsep-plan.md` §0). Method: render the same frame both
ways and diff per band via the View popup — Low Band and High Band isolate the
blur/guided kernels away from compose, which is what makes a mismatch
diagnosable rather than just visible.

Target: **max-abs ≤ 1e-5** per channel.

Known gap, accepted deliberately: this is a *development-time* check, not a
regression gate. The `tests/` suite is AE-free by design and GPU rendering needs
a live AE-hosted device, so GPU correctness is not CI-protected the way the CPU
core is. Revisit if the GPU path starts changing often.
