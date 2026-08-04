# Starter brief — give Grain Transfer a CUDA GPU host

*Paste this as the opening prompt for the port. Written 2026-08-04; verify
every "state of play" claim against the tree before acting.*

---

Give Grain Transfer a CUDA GPU host, so After Effects actually GPU-renders it.
It is the last plugin in the suite with no GPU path at all.

Read these first, they are the contracts:

    docs/AE_GPU_Effects.md      — the HOST contract. §1.1/§1.2 (why CUDA is the
                                  only backend that matters for AE), §2.1 (the
                                  flags + PiPL pair), §2.2 (the re-affirm),
                                  §2.3 (the PreRender flag), §2.6 (PiPL must
                                  equal the runtime flags).
    docs/gpu-kernel-rules.md    — the KERNEL contract. §0, §2 (the shared
                                  vocabulary), §2.1 (entry-point bracketing),
                                  §3 (numerical rules), §5.
    docs/gpu-validation-checklist.md — how the finished thing gets validated,
                                  and the four-item audit it must pass.

## STATE OF PLAY (verified against the tree 2026-08-04)

Grain Transfer is **not** where MM_Qualify and MM_FreqSep were. Those had a
complete DX12 pipeline aimed at the wrong backend. This has **nothing**:

    no src/gpu/            no .kernel files       no prologues
    no GPU_DEVICE_SETUP    no SMART_RENDER_GPU    no GPU flags anywhere
    CMakeLists has no dxc and no nvcc step

**The one hard prerequisite is already met: it is SmartFX.**
`GrainTransfer.cpp` handles `PF_Cmd_SMART_PRE_RENDER` / `PF_Cmd_SMART_RENDER`
and declares `SUPPORTS_SMART_RENDER`. The AE GPU path is a SmartFX path, so
there is no render-architecture conversion to do — this is additive.

Current flags (`GlobalSetup`, mirrored in `resources/GrainTransferPiPL.r` as
`0x02000444` / `0x08201400`):

    out_flags  = DEEP_COLOR_AWARE | PIX_INDEPENDENT | USE_OUTPUT_EXTENT
               | NON_PARAM_VARY
    out_flags2 = FLOAT_COLOR_AWARE | SUPPORTS_SMART_RENDER
               | SUPPORTS_THREADED_RENDERING | I_MIX_GUID_DEPENDENCIES

`NON_PARAM_VARY` and `I_MIX_GUID_DEPENDENCIES` are load-bearing here and are
not present in the other three — preserve both. `USE_OUTPUT_EXTENT` stays:
AE_GPU_Effects.md §2.1 advises against it but §8 hypothesis 5 records that it
was tested and is irrelevant to the GPU gate, and ModernEdgeBlur keeps it for
the same reason. **Both PiPL literals must change with GlobalSetup or AE
rejects the plugin at load (25::16).** Bump `BUILD_VERSION` (currently 13) in
`src/GrainTransfer.h` and the PiPL together.

### The shape of this plugin is the OPPOSITE of the other three — read this before planning

    ANALYSE (once per source state, CPU, cached)   PER FRAME (cheap, parallel)
    ─────────────────────────────────────────────  ──────────────────────────
    high-pass residual                             sample one precomputed tile
    patch selection (median/MAD statistics)        (wrap, or bilinear-wrap)
    FFT per patch, average |F|²  ── kissfft        unpremultiply
    measure σ(L) response curve (percentiles)      encode to apply space
    bake 8 RPN tiles (FFT + inverse FFT)           luma → EvalSigma → × Amount
                                                   add, decode, repremultiply

**Only the right-hand column goes on the GPU.** It is
`src/core/GT_Stamp.cpp:StampRow` and it is pure per-pixel work with no image
neighbourhood reads whatsoever.

Two consequences, and they make this the **simplest port in the suite**:

1. **One kernel.** `gt_stamp`. Possibly a second trivial `gt_copy` for the
   passthrough branch. That is the whole kernel set.
2. **No tiling, no margins, no band planner.** Every other GPU plugin here
   needed `*_PlanTiles` because a blur reads neighbours. This reads only the
   tile bank, which is a small fixed resource. **There is no counterpart to
   `QK_PlanTiles` and you should not invent one.** If you find yourself writing
   band margins, stop — you have misread the algorithm.

**The analysis stays on the CPU. Do not port the FFT.** It runs once per source
state behind `AEGP_ComputeCacheSuite1`, it is kissfft (CPU-only), and it is
gated to full resolution. Moving it would be a rewrite with no payoff.

## THE ONE HARD PROBLEM — settle it before writing a line of code

In `SMART_RENDER_GPU` **every world is device memory**, including the Grain
Source layer. The CPU analysis path cannot read it. AE_GPU_Effects.md §2.4 is
explicit that there is no falling back mid-render.

The CPU path today handles "no analysis yet" by passing the frame through
ungrained (`canStamp == false`). On the GPU path that same branch becomes a
**permanent trap**: every frame passes through, the analysis is never computed,
and grain never appears at all.

**Recommended shape — raise `GPU_RENDER_POSSIBLE` only when the analysis is
already CACHED.** Probe the cache key-only in PreRender
(`AEGP_CheckoutCached`, no compute). Miss → CPU renders the frame, which pays
for the analysis and populates the cache. Hit → every subsequent frame of that
source state goes GPU. This matches the plugin's own documented cost model
exactly ("a hit means the source frame is never rendered, never unpacked, and
the per-frame cost is just the stamp").

**This is the ONE legitimate case in the suite where PreRender withholds the
flag on a condition** — and note precisely why it is legitimate: it withholds
on a fact it *knows* (the analysis is not cached), never on ignorance of the
device. The rule from AE_GPU_Effects.md §2.3 is unchanged and still binding:
never withhold because `what_gpu` is NONE or `gpu_data` is null. Getting that
distinction wrong is what cost MM_Qualify and MM_FreqSep their first ports.

**Wrinkle to resolve honestly, not paper over:** the cache key includes
`pixelFormat` (`opt.key.pixelFormat = (A_long)fmt`), and PreRender does not
have the output world's format — SmartRender reads it from `out_world`. Decide
up front: derive the format in PreRender, drop it from the key, or probe both
candidates. Write down which and why. **If you cannot resolve it cleanly, say
so and stop** rather than shipping a gate that silently never fires — that is
the exact failure mode this suite has now hit twice.

> **RESOLVED 2026-08-04 — derive it, from `bitdepth`.** `PF_PreRenderInput`
> carries `short bitdepth` ("bitdepth the effect is being driven in (in bpc)",
> `AE_Effect.h:2495`), and `PF_SmartRenderInput` carries the same field, so
> both paths can build the identical key with no output world. The mapping is
> 1:1 and total: `gtc::PixelBytes` accepts exactly `ARGB32/64/128`, which are
> exactly 8/16/32 bpc. `GT_FormatForBitdepth` in `GrainTransfer.cpp` is that
> mapping, and BOTH SmartRender and PreRender now use it.
>
> The wrinkle turned out to be **worse than stated, and in a way that only
> shows on the GPU**: keying on `PF_GetPixelFormat(out_world)` is not merely
> unavailable in PreRender, it is *wrong* in `SMART_RENDER_GPU`, where every
> world reports `PF_PixelFormat_GPU_BGRA128` whatever the project depth. A GPU
> path built on that would mint a key the CPU path can never have populated,
> miss on every frame, and decline forever — a silent permanent-CPU render
> that looks exactly like the port having done nothing. Deriving from
> `bitdepth` is what makes the two paths agree; it is not a stylistic choice.

## THE WORK, in order

1. **`src/gpu/kernels/` + the prologue trio.** `gt_prologue_hlsl.h`,
   `gt_prologue_cuda.h`, `gt_prologue_host.h` defining the same vocabulary
   (rules §2), plus `gt_stamp.kernel` bracketed with `KERNEL_BEGIN`/
   `KERNEL_END` from the start — do not repeat MM_Qualify's file-scope retrofit.

   The per-pixel math already exists and is *nearly* kernel-ready, which is the
   nice surprise of this port: `src/core/GT_Colour.h` (`LinToLog`, `LogToLin`,
   `EncodeChannel`, `DecodeChannel`, `Luma`) and `GT_Response.h:EvalSigma`.
   Follow MM_Qualify's `qk_core.h` + `qk_cpu_prologue.h` arrangement so ONE
   body serves CPU and GPU. Three things in there do not survive HLSL and must
   move into the vocabulary:
     - `std::log2` / `std::exp2` → wrap as `GT_LOG2` / `GT_EXP2`.
     - `enum class Encode` → a plain `int` in the param block. HLSL has no
       enum class.
     - `ResponseCurve` carries `float sigma[3][5]`. Flatten it to a packed
       float block with a `GT_PackResponse` + `GT_RESP_DECL`/`GT_RESP_LOAD`
       macro trio, exactly as `QK_PackGates` / `QK_GATES_DECL` do. 5 band mids
       + 15 sigmas + 3 flat sigmas = **23 floats**.

2. **Extend the AE-free tests so the kernel body is covered.** This is the
   regression gate; nothing in AE is worth trying until it is green.
   `tests/test_core.cpp` (596 lines) and `test_realfootage.cpp` are excellent
   but they are DSP gates — they know nothing about a GPU path. Add a test that
   compiles `gt_stamp.kernel` through `gt_prologue_host.h` and **runs it**
   against the real `gt::StampRow`, at **exactly 0.0**. Copy MM_Qualify's
   `tests/kernel_test.cpp` + `src/gpu/cuda/qk_kernel_args.h` arrangement: the
   test executes the shipped kernel source, not a transcription that can rot.
   Cover both branches (normal stamp and `previewGrainOnly`), both encodes
   (Display and LogFromLinear, including HDR and negative values), exact vs
   bilinear-wrap sampling, `flipX/flipY`, and `alpha == 0` passthrough.

3. **`src/gpu/gt_cuda_shim.h`** (runtime `nvcuda.dll`, no toolkit needed to
   COMPILE) and **`src/gpu/cuda/gt_kernels.cu`** — one shim including the
   kernel file, nvcc emits one PTX, deployed as `CUDA_Assets/` next to the
   `.aex`. **CMake must skip the nvcc step gracefully when the toolkit is
   absent**, and that skip must be *tested*, not asserted — MM_Qualify's
   CMakeLists takes `-DQK_NVCC_EXE=NOTFOUND` precisely so the no-toolkit path
   can be exercised on a machine that has the toolkit. Do the same.

4. **`src/gpu/gt_gpu_cuda.cpp`**: device setup/setdown, the launcher on AE's
   own stream, and **the device-side tile bank**. The bank is
   `count × 3 × n × n` floats — with `GT_TILE_COUNT = 8` and n from
   `GT_TILE_CANDIDATES = {256,128,64}`, that is at most **6 MB**. Upload it
   **once per analysis**, not per frame; cache it on the per-device `gpu_data`
   keyed on analysis identity, and re-upload only when the key changes. The
   response block (23 floats) and the `StampParams` scalars are per-frame and
   ride in the kernel argument struct.

5. **Flags + PreRender + dispatch.** Add `SUPPORTS_GPU_RENDER_F32` and
   `SUPPORTS_DIRECTX_RENDERING` to `GlobalSetup`, update BOTH PiPL literals to
   match, wire `PF_Cmd_GPU_DEVICE_SETUP` / `_SETDOWN` / `PF_Cmd_SMART_RENDER_GPU`,
   and add the cache-gated `GPU_RENDER_POSSIBLE` from the section above.

6. **In AE:** check `%TEMP%` first — the plugin already writes
   `GrainTransfer_diag.log` (`GT_DIAG_LOG`), so either add GPU lines there or
   open `GrainTransfer_gpu.log` alongside it; pick one and say which. Confirm
   `what_gpu=3` at PreRender and a `SMART_RENDER GPU` line, then A/B against
   Mercury → Software Only, max-abs ≤ 1e-5. Full protocol:
   `docs/gpu-validation-checklist.md`.

## TRAPS, in rough order of how much they have cost this suite

- **`GPU_DEVICE_SETUP` must RE-AFFIRM `out_flags2 = SUPPORTS_GPU_RENDER_F32`,
  as a plain assignment.** Missing it cost EdgeRepair seven failed experiments
  and perfectly imitates "AE doesn't support third-party GPU effects".
- **Do NOT gate the PreRender flag on `what_gpu != NONE` or on `gpu_data`.** AE
  offers NONE until it has seen the flag, so that gate deadlocks itself shut.
  Both MM_Qualify and MM_FreqSep shipped this bug and neither reached the GPU
  until 2026-08-04. The cache gate above is a *different* thing and is fine.
- **PiPL and GlobalSetup must agree exactly**, or AE refuses to load with a
  misleading "global outflags mismatch" (25::16). This plugin has four flags
  the others do not — recompute both literals, do not copy MM_Qualify's.
- **Wrap addressing is the CORRECT edge rule here, unlike everywhere else in
  the suite.** The RPN tiles are toroidal by construction — that is the whole
  reason they tile seamlessly. `gpu-kernel-rules.md` §3.2 says out-of-range
  taps are SKIPPED, never clamped; that rule is about *convolution coverage*
  and does not apply to tile lookup. Do not "fix" the wrap to a clamp, and do
  not reach for a sampler — index math only, same as everywhere else.
- **The frame RNG (`MakeFrameStamp`) stays on the CPU.** It produces five
  scalars per frame from `(seed, frame)`. A GPU-side re-derivation would break
  determinism against the CPU path and there is nothing to gain.
- **Alpha semantics.** `alpha <= 1e-6` passes through untouched; alpha is never
  modified; everything else round-trips unpremultiply → encode → add → decode →
  repremultiply. Get one of those backwards and the parity test catches it —
  make sure the test actually contains zero-alpha and low-alpha pixels.
- **No shader doubles are needed here** (no box sums, no reductions), so
  MM_FreqSep's `DoublePrecisionFloatShaderOps` device gate does NOT carry over.
  Neither does `ACCUM_T`.
- **Worlds are BGRA on the GPU**, ARGB on the CPU (`gtc::UnpackRow`). The
  swizzle is index reversal `(3 - c)` and belongs in a shared kernel, never in
  a host — see `qk_tile.kernel`. Note this plugin's stamp is in-place on
  ARGB today; on the GPU decide whether to swizzle in the stamp kernel or keep
  separate load/store kernels, and write down why.

## REFERENCES, in order of usefulness

    MM_Qualify (2026-08-04) — the closest and most recent worked example:
      prologue trio, bracketed kernels, kernel_test.cpp running the REAL
      bodies at 0.0, the PreRender budget gate, the CUDA host with cached
      device scratch, and the CMake nvcc skip with its NOTFOUND test hook.
      Large parts are near-mechanical.
    ModernEdgeBlur (ccdddfa, 758e66b) — the budget-gate shape and grow-only
      cached device scratch.
    EdgeRepair — the suite's original CUDA reference (shim, CUDA_Assets
      layout, the -ccbin/nvcc-vs-VS2026 recipe in its CMakeLists).

## CONSTRAINTS

- **Do not touch the CPU stamp path or the DSP core** (`GT_Grain`, `GT_FFT`,
  `GT_Response`, `GT_Stamp`) except to bring the shared per-pixel math into the
  prologue vocabulary — and that refactor must leave the CPU output
  bit-identical, proven by the existing `tests/build_tests.ps1` gates.
- **Do not port the FFT/analysis to the GPU.**
- There is no DX12 host here and none is wanted — CUDA only, per
  AE_GPU_Effects.md §1.2. Keep one HLSL entry compiling anyway for the §4
  item 5 type-check, exactly as ModernEdgeBlur does.
- Build with `.\build.ps1 -Project GrainTransfer`; tests with
  `GrainTransfer\tests\build_tests.ps1`. **Report what you actually ran.**
