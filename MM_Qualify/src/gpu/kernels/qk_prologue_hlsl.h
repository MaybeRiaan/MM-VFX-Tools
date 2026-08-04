/*  qk_prologue_hlsl.h — HLSL spellings of the shared kernel vocabulary.

    Included BEFORE a .kernel body (and before qk_core.h, which the kernels
    include and which uses ONLY the QK_* math subset defined here). The C++
    counterpart of the math subset is src/qk_cpu_prologue.h; a future Metal
    prologue must define the SAME names with MSL spellings and the SAME
    binding ORDER. See ../docs/gpu-kernel-rules.md (suite-level) — this file
    follows its §2 vocabulary, minus the double accumulator (no box sums in
    this plugin, hence no shader-doubles device requirement).

    Nothing in this file is algorithm. Nothing in a .kernel file is syntax.
*/

#ifndef QK_PROLOGUE_HLSL_H
#define QK_PROLOGUE_HLSL_H

#define QK_PASTE2(a, b) a##b
#define QK_PASTE(a, b)  QK_PASTE2(a, b)

/* ---- threadgroup geometry (prologue-only decision) ---------------------- */
#define QK_TG_X 8
#define QK_TG_Y 8

#define KERNEL_ENTRY(name)                     \
    [numthreads(QK_TG_X, QK_TG_Y, 1)]          \
    void name(uint3 qk_tid : SV_DispatchThreadID)

/* ---- entry-point brackets ------------------------------------------------
   KERNEL_BEGIN precedes an entry's BUF_* and PARAM_BLOCK declarations,
   KERNEL_END follows its body. Both expand to NOTHING here, so the
   preprocessed HLSL is byte-for-byte what it was before the brackets
   existed — HLSL binds buffers by REGISTER at file scope and needs no help.

   They exist for CUDA and Metal, which bind buffers as kernel ARGUMENTS and
   cannot reference a file-scope buffer declaration at all. Those prologues
   expand this pair into a per-entry argument struct whose members are
   exactly the BUF_* and PARAM_BLOCK declarations, read by the body under the
   same bare names. Without them the vocabulary is expressible in HLSL and
   nowhere else, and gpu-kernel-rules §0's promise is unreachable — see its
   §2.1. Consequence: every entry point must be SELF-CONTAINED. Nothing
   leaks between argument structs, so an entry may not borrow a sibling's
   PARAM_BLOCK from HLSL's shared global scope; param names are prefixed
   per entry so the HLSL side stays legal too.                             */
#define KERNEL_BEGIN(name)
#define KERNEL_END(name)

#define GID_X (int)qk_tid.x
#define GID_Y (int)qk_tid.y

/* ---- buffers ------------------------------------------------------------
   AE hands us raw linear device buffers, NOT textures — index math only, so
   out-of-range taps can be SKIPPED exactly as blur.cpp does. EVERYTHING
   binds as a UAV (one binding class = zero resource-state transitions on
   resources whose state we don't own; see FreqSep's prologue for the full
   rationale). REGISTER MAP (the cross-platform binding contract):
     BUF_IN (name, slot)  -> u(slot)      inputs  occupy u0..u3
     BUF_OUT(name, slot)  -> u(4 + slot)  outputs occupy u4..u7            */
#define BUF_IN(name, slot)  RWStructuredBuffer<float> name : register(QK_PASTE(u, slot))

#define QK_OUT_REG_0 u4
#define QK_OUT_REG_1 u5
#define QK_OUT_REG_2 u6
#define QK_OUT_REG_3 u7
#define BUF_OUT(name, slot) RWStructuredBuffer<float> name : register(QK_PASTE(QK_OUT_REG_, slot))

/* Param blocks map to root constants at b0. A kernel FILE may contain
   several PARAM_BLOCKs; each expansion mints a unique cbuffer NAME via
   __COUNTER__ — all share b0, legal because each entry point uses exactly
   one and dxc strips the rest. */
#define PARAM_BLOCK_BEGIN cbuffer QK_PASTE(QK_Params_, __COUNTER__) : register(b0) {
#define PARAM_BLOCK_END   };

/* ---- math vocabulary (must mirror qk_cpu_prologue.h) -------------------- */
#define QK_FUNC     static
#define QK_FLOOR(x) floor(x)
#define QK_POW(x,y) pow((x), (y))
#define QK_LOG2(x)  log2(x)
#define QK_LOG10(x) log10(x)
#define QK_MIN(a,b) min(a,b)
#define QK_MAX(a,b) max(a,b)
#define QK_ABS(x)   abs(x)
#define QK_SAT(x)   saturate(x)
#define QK_OUTF(n)  out float n

/* Standard bounds guard — DX12 has no partial threadgroups; the explicit
   guard also keeps bodies correct under Metal's dispatchThreads:. */
#define QK_GUARD(W, H) if (GID_X >= (W) || GID_Y >= (H)) return;

/* Premultiplied ARGB indexing, channel order A,R,G,B (qk::UnpackRow). */
#define QK_IDX(x, y, W) (((y) * (W) + (x)) * 4)

#endif /* QK_PROLOGUE_HLSL_H */
