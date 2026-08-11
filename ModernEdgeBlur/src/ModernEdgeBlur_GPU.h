/*  ModernEdgeBlur_GPU.h — GPU path: device lifecycle + GPU SmartRender.

    Implemented in ModernEdgeBlur_GPU.cpp on top of the AE GPU device suite
    (PF_GPUDeviceSuite1). Backend policy, and why it is this narrow:

      CUDA    — THE framework AE-on-Windows actually offers effects (measured
                2026-07-23 with EdgeRepair; ../../docs/AE_GPU_Effects.md §1.1),
                and the only backend this suite maintains (§1.2). Host side
                runs through a runtime nvcuda.dll shim (src/gpu/MEB_CudaShim.h
                — no toolkit needed to compile); the kernels ship as
                CUDA_Assets/ModernEdgeBlur_Kernels.ptx, built by nvcc only when
                the CUDA toolkit is installed. PTX absent -> setup declines
                quietly -> CPU fallback.
      DIRECTX — DECLINED. There is no DX12 host here. Modern Edge Blur is an AE
                effect and AE never offers it; a passthrough DX12 host would
                render the effect as a silent no-op in Premiere, which is worse
                than declining. src/gpu/ModernEdgeBlurKernels.hlsl still exists
                and is still compiled every build, but purely so fxc
                type-checks the shared kernel math (§4 item 5).
      METAL   — no wrapper yet. The shared headers carry its shim spellings so
                a Mac build is a toolchain step, not a port.
      OPENCL  — declined.

    Unsupported/unready frameworks decline QUIETLY at PF_Cmd_GPU_DEVICE_SETUP
    (PF_Err_NONE, null gpu_data), and PreRender's GPU_RENDER_POSSIBLE flag is
    only honoured by AE for devices that completed setup — so every decline is
    a clean fall back to the CPU pipeline, which remains the always-correct
    reference.
*/

#pragma once

#include "AE_Effect.h"

struct MEB_Params;

/* PF_Cmd_GPU_DEVICE_SETUP: initialize the per-device GPU state (load the
   precompiled PTX on AE's CUcontext, resolve every kernel) and hand it to AE
   via extra->output->gpu_data. Frameworks this build does not render on, and a
   missing/stale PTX, decline quietly -> AE uses the CPU path. */
PF_Err MEB_GPUDeviceSetup(PF_InData *in_data, PF_OutData *out_data,
                          PF_GPUDeviceSetupExtra *extra);

/* PF_Cmd_GPU_DEVICE_SETDOWN: destroy what Setup created (gpu_data), and hand
   the pipeline scratch back to the suite that owns it. */
PF_Err MEB_GPUDeviceSetdown(PF_InData *in_data, PF_OutData *out_data,
                            PF_GPUDeviceSetdownExtra *extra);

/* True iff this build can render on the given framework — the single authority
   behind PF_RenderOutputFlag_GPU_RENDER_POSSIBLE. Keep in sync with what
   MEB_GPUDeviceSetup actually initializes. */
bool MEB_GPUFrameworkSupported(PF_GPU_Framework what_gpu);

/* PreRender's scratch budget gate: false only when a GPU render of a W x H
   frame with these parameters PROVABLY does not fit the device's scratch
   budget, in which case the caller must leave GPU_RENDER_POSSIBLE unraised and
   let the CPU pipeline take the frame — slower, never wrong. There is no such
   choice later: at SMART_RENDER_GPU the worlds are already device memory, so a
   refused allocation fails the frame outright.

   Returns TRUE whenever the frame cannot be sized (AE has not offered a
   framework yet, no gpu_data, degenerate rect). The flag must never be withheld
   out of ignorance: AE offers what_gpu = NONE until it has seen it at least
   once, so a gate that demanded a known device would deadlock itself shut
   (../../docs/AE_GPU_Effects.md §2.2-§2.3).

   Pass the full output REQUEST rect: the world SmartRender is handed can only
   be smaller, so this sizes the worst case. Same planner the render runs. */
bool MEB_GPUBudgetAllows(PF_GPU_Framework what_gpu, const void *gpu_data,
                         A_long W, A_long H, const MEB_Params &prm);

/* Diagnostic (MEB_GPU_LOG in ModernEdgeBlur_GPU.cpp ->
   %TEMP%\ModernEdgeBlur_gpu.log): records the per-frame PreRender GPU decision
   — what AE offered, whether GPU_RENDER_POSSIBLE was raised, plus the render
   context (bit depth, downsample factor) so AE's CPU/GPU choice can be
   correlated with project settings. The GPU failure mode is SILENT — the
   effect renders correctly on the CPU — so this log is the decisive signal
   (AE_GPU_Effects.md §5.1). No-op when logging is compiled out. */
void MEB_GPULogPreRenderDecision(PF_GPU_Framework what_gpu,
                                 const void *gpu_data, bool raised,
                                 int bitdepth, int dsx_num, int dsx_den);

/* Records a CPU SmartRender alongside it, so a frame AE routed to the CPU
   leaves a trace instead of only a missing GPU line. */
void MEB_GPULogSmartRenderCPU(int srcW, int srcH, int outW, int outH);

/* GPU SmartRender. Called from ModernEdgeBlur.cpp's SmartRender when
   extra->input->what_gpu != NONE, with the already-checked-out GPU worlds and
   the parameters (read BEFORE the CPU/GPU fork, so both paths see identical
   values).

   Runs the full pipeline — Shift Edge, Blur Amount, Blend Radius, composite,
   and the passthrough case — mirroring SmartRender's CPU pipeline pass for
   pass. There is NO CPU fallback after a GPU error: the worlds are device
   memory and CPU code cannot address them. */
PF_Err MEB_SmartRenderGPU(PF_InData *in_data, PF_OutData *out_data,
                          PF_SmartRenderExtra *extra,
                          PF_EffectWorld *src_world, PF_EffectWorld *out_world,
                          const MEB_Params &prm);
