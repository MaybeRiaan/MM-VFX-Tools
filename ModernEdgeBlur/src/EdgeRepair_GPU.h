/*  EdgeRepair_GPU.h — Stage 6 GPU path: device lifecycle + GPU SmartRender.

    Implemented in EdgeRepair_GPU.cpp on top of the AE GPU device suite
    (PF_GPUDeviceSuite1). Backend status (Stage 6, 2026-07-23 —
    docs/EdgeRepair.md):
      CUDA    — THE framework AE-on-Windows actually offers effects (measured
                via the ER_GPU_LOG diagnostics; SUPPORTS_DIRECTX_RENDERING
                did not make AE probe DX12 — that flag serves Premiere
                hosts). Host side runs through a runtime nvcuda.dll shim
                (src/gpu/ER_CudaShim.h — no toolkit needed to compile);
                kernel ships as CUDA_Assets/EdgeRepair_Kernels.ptx, built by
                nvcc only when the CUDA toolkit is installed. PTX absent →
                setup declines quietly → CPU fallback.
      DIRECTX — implemented and kept working (Premiere hosts / any Windows
                GPU). Kernels precompiled to DirectX_Assets/*.cso/.rs via
                Adobe's DX12 helper (sdk/Util/DirectXUtils).
      METAL   — source maintained; Mac builds only.
      OPENCL  — declined; AE falls back to CPU.
    Unsupported/unready frameworks decline QUIETLY at PF_Cmd_GPU_DEVICE_SETUP
    (PF_Err_NONE, null gpu_data) and PreRender never raises
    GPU_RENDER_POSSIBLE for them = clean CPU fallback, the CPU pipeline being
    the always-correct reference.
*/

#pragma once

#include "AE_Effect.h"

/* PF_Cmd_GPU_DEVICE_SETUP: initialize the per-device GPU state (DX12: wrap
   AE's ID3D12Device/queue in a DXContext, load the precompiled kernels) and
   hand it to AE via extra->output->gpu_data. Errors for frameworks we do not
   (yet) build → AE uses the CPU path. */
PF_Err ER_GPUDeviceSetup(PF_InData *in_data, PF_OutData *out_data,
                         PF_GPUDeviceSetupExtra *extra);

/* PF_Cmd_GPU_DEVICE_SETDOWN: destroy what Setup created (gpu_data). */
PF_Err ER_GPUDeviceSetdown(PF_InData *in_data, PF_OutData *out_data,
                           PF_GPUDeviceSetdownExtra *extra);

/* Declare the GPU pixel formats this effect accepts (PF_PixelFormatSuite /
   "PF Pixel Format Suite"). PREMIERE ONLY — call from GLOBAL_SETUP guarded by
   `in_data->appl_id == 'PrMr'`, exactly as Adobe's SDK_ProcAmp and Vignette
   samples do. In AE the suite does not exist (kSPSuiteNotFoundError), and that
   is expected, not a defect.

   An earlier revision claimed this was the reason AE registers our
   GPUVideoFilter module but never instantiates it. That was WRONG; the
   instantiation question is still open. See docs/AE_GPU_Effects.md §2.3.
   No-op when the suite is unavailable — CPU path unaffected. */
void ER_DeclareGPUPixelFormats(PF_InData *in_data, PF_OutData *out_data);

/* True iff this build can render on the given framework — the single
   authority PreRender uses to raise PF_RenderOutputFlag_GPU_RENDER_POSSIBLE.
   Keep in sync with what ER_GPUDeviceSetup actually initializes. */
bool ER_GPUFrameworkSupported(PF_GPU_Framework what_gpu);

/* Diagnostic (ER_GPU_LOG in EdgeRepair_GPU.cpp, %TEMP%\EdgeRepair_gpu.log):
   records the per-frame PreRender GPU decision — what AE offered, whether
   GPU_RENDER_POSSIBLE was raised, plus the render context (bitdepth AE is
   driving the effect at, downsample factor) to correlate AE's CPU/GPU choice
   with project settings. No-op when logging is compiled out. */
void ER_GPULogPreRenderDecision(PF_GPU_Framework what_gpu,
                                const void *gpu_data, bool raised,
                                int bitdepth, int dsx_num, int dsx_den);

/* Diagnostic: one-line tagged event in the same log (first 8 per tag site).
   Used to observe rare host commands (e.g. whether AE ever sends
   PF_Cmd_QUERY_DYNAMIC_FLAGS). No-op when logging is compiled out. */
void ER_GPULogEvent(const char *tag);

/* printf-style variant of the above, for events whose error codes matter
   (suite acquisition, per-format rejection). Same first-N budget. */
#ifdef __GNUC__
__attribute__((format(printf, 1, 2)))
#endif
void ER_GPULogEventF(const char *fmt, ...);

/* GPU SmartRender. Called from EdgeRepair.cpp's SmartRender when
   extra->input->what_gpu != NONE, with the already-checked-out GPU worlds, the
   parameters, and the output→source origin offset (srcOX/srcOY, derived from
   the PreRender rects — same values the CPU path uses; both stay single-sourced
   in EdgeRepair.cpp and are read BEFORE the CPU/GPU fork).

   CUDA runs the full pipeline (core matte -> JFA+2 distance -> pull–push fill
   -> grain -> composite, plus all four View Modes). DIRECTX is still the
   passthrough milestone — see ER_RenderDX12's note in the .cpp. There is no CPU
   fallback after a GPU error: the worlds are device memory and CPU code cannot
   address them. */
struct ER_Params;
PF_Err ER_SmartRenderGPU(PF_InData *in_data, PF_OutData *out_data,
                         PF_SmartRenderExtra *extra,
                         PF_EffectWorld *src_world, PF_EffectWorld *out_world,
                         const ER_Params &prm, A_long srcOX, A_long srcOY);
