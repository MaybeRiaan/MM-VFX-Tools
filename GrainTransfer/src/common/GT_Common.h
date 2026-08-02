/*  GT_Common.h — AE-facing row helpers for Grain Transfer.

    Row pack/unpack between AE worlds and float ARGB with float32 internal
    math, plus the iterate_generic row-fanout wrapper. Written independently
    rather than cross-referenced, back when this plugin lived in its own repo.

    That turned out to be the pattern the whole suite settled on: as of
    2026-08-02 every plugin owns its equivalent under its own name and
    namespace — EdgeRepair/src/core/ER_Common.h (er::), MM_FreqSep's
    fs_common.h (fs::), MM_Qualify's qk_common.h (qk::) — so a fix in one
    never silently implies a fix in the others. This file was the precedent.

    PackRow leaves float32 unclamped so HDR values survive the round trip;
    8/16bpc are clamped and rounded at the boundary only.
*/

#pragma once

#include "AEConfig.h"

#ifdef AE_OS_WIN
    #include <Windows.h>
#endif

#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectPixelFormat.h"

namespace gtc {

// Bytes per pixel for the three AE packed formats. Unknown formats return 0
// — callers must treat that as an unsupported-world error.
size_t PixelBytes(PF_PixelFormat fmt);

// Unpack one row of an AE world into float ARGB, channels scaled to [0,1]
// for 8/16bpc, native float for 32bpc. dst holds W*4 floats, order A,R,G,B.
void UnpackRow(const void *srcRow, float *dst, A_long W, PF_PixelFormat fmt);

// Pack float ARGB back into an AE world row. 8/16bpc clamp to [0,1] and
// round; float passes through unclamped.
void PackRow(const float *src, void *dstRow, A_long W, PF_PixelFormat fmt);

// Run a per-row callback threaded across AE's workers via iterate_generic
// when available, serially otherwise. Callbacks must only make disjoint
// writes keyed off the row index.
void ForEachRow(PF_InData *in_data, A_long rows, void *refcon,
                PF_Err (*cb)(void *, A_long, A_long, A_long));

} // namespace gtc
