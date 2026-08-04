/*  gt_prologue_hlsl.h — HLSL spellings of the shared kernel vocabulary.

    Included BEFORE a .kernel body (and before gt_core.h, which the kernels
    include and which uses ONLY the GT_* math subset defined here). The C++
    counterpart of that subset is src/gt_cpu_prologue.h; the CUDA one is
    gt_prologue_cuda.h and the host-test one gt_prologue_host.h. All four
    define the SAME names with the SAME binding ORDER, so a body never knows
    which one it got. See ../../../docs/gpu-kernel-rules.md §2.

    Grain Transfer ships a CUDA host only (AE_GPU_Effects.md §1.2 — CUDA is
    the backend AE actually drives on Windows). This prologue exists anyway so
    one HLSL entry stays in the build and dxc type-checks the whole
    translation unit on every build: AE_GPU_Effects.md §4 item 5, the same
    reason ModernEdgeBlur keeps its own.

    Nothing in this file is algorithm. Nothing in a .kernel file is syntax.
*/

#ifndef GT_PROLOGUE_HLSL_H
#define GT_PROLOGUE_HLSL_H

#define GT_PASTE2(a, b) a##b
#define GT_PASTE(a, b)  GT_PASTE2(a, b)

/* ---- threadgroup geometry (prologue-only decision) ---------------------- */
#define GT_TG_X 8
#define GT_TG_Y 8

#define KERNEL_ENTRY(name)                     \
    [numthreads(GT_TG_X, GT_TG_Y, 1)]          \
    void name(uint3 gt_tid : SV_DispatchThreadID)

/* ---- entry-point brackets ------------------------------------------------
   Both expand to NOTHING here: HLSL binds buffers by REGISTER at file scope
   and needs no help. They exist for CUDA and Metal, which bind buffers as
   kernel ARGUMENTS and cannot reference a file-scope buffer declaration at
   all — see gpu-kernel-rules §2.1. Consequence: every entry point must be
   SELF-CONTAINED, because nothing leaks between argument structs. */
#define KERNEL_BEGIN(name)
#define KERNEL_END(name)

#define GID_X (int)gt_tid.x
#define GID_Y (int)gt_tid.y

/* ---- buffers ------------------------------------------------------------
   AE hands us raw linear device buffers, NOT textures — no samplers, no
   bilinear, no wrap addressing in hardware. Deliberate: the tile lookup does
   its own index math so both backends wrap identically. EVERYTHING binds as a
   UAV (one binding class = zero resource-state transitions on resources whose
   state we do not own). REGISTER MAP (the cross-platform binding contract):
     BUF_IN (name, slot)  -> u(slot)      inputs  occupy u0..u3
     BUF_OUT(name, slot)  -> u(4 + slot)  outputs occupy u4..u7            */
#define BUF_IN(name, slot)  RWStructuredBuffer<float> name : register(GT_PASTE(u, slot))

#define GT_OUT_REG_0 u4
#define GT_OUT_REG_1 u5
#define GT_OUT_REG_2 u6
#define GT_OUT_REG_3 u7
#define BUF_OUT(name, slot) RWStructuredBuffer<float> name : register(GT_PASTE(GT_OUT_REG_, slot))

/* Param blocks map to root constants at b0. Each expansion mints a unique
   cbuffer NAME via __COUNTER__ — all share b0, legal because each entry point
   uses exactly one and dxc strips the rest. */
#define PARAM_BLOCK_BEGIN cbuffer GT_PASTE(GT_Params_, __COUNTER__) : register(b0) {
#define PARAM_BLOCK_END   };

/* ---- math vocabulary (must mirror src/gt_cpu_prologue.h) ---------------- */
#define GT_FUNC     static
#define GT_FLOOR(x) floor(x)
#define GT_FABS(x)  abs(x)
#define GT_LOG2(x)  log2(x)
#define GT_EXP2(x)  exp2(x)
#define GT_MIN(a,b) min(a,b)
#define GT_MAX(a,b) max(a,b)
#define GT_OUTF(n)  out float n
#define GT_OUTI(n)  out int n

/* Standard bounds guard — DX12 has no partial threadgroups; the explicit
   guard also keeps bodies correct under Metal's dispatchThreads:. */
#define GT_GUARD(W, H) if (GID_X >= (W) || GID_Y >= (H)) return;

#endif /* GT_PROLOGUE_HLSL_H */
