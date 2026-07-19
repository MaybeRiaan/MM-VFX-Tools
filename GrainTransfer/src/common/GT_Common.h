/*  GT_Common.h — AE-facing row helpers for Grain Transfer.

    Same semantics as the suite's proven MM_Common (ModernEdgeBlur repo):
    row pack/unpack between AE worlds and float ARGB with float32 internal
    math, and the iterate_generic row-fanout wrapper. Re-implemented here
    (not cross-referenced) because this plugin lives in its own repo — the
    suite's shared code is never touched from this session.

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
