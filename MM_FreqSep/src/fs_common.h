/*  fs_common.h — MM FreqSep's AE-facing helpers. Private to this plugin.

    Was src/common/MM_Common.h until 2026-08-02, in the `fs::` namespace since
    the same change. MM_Qualify and EdgeRepair have their own equivalents
    (qk_common.h, ER_Common.h). MM_Qualify's was byte-identical to this file at
    the time of the rename — which was exactly the hazard: two files that look
    shared, are not, and would drift silently. The rename makes ownership
    visible in an include line and makes the compiler reject code moved
    between plugins instead of binding it to a diverged twin.

    "common" here means common across THIS plugin's translation units. The
    suite deliberately has no shared plugin code yet; if that changes it will
    be a designed step with a real home, not an inference from a filename.

    Contents are AE-facing (PF types) but algorithm-free: row pack/unpack
    between AE worlds and float ARGB, the iterate_generic row-fanout wrapper,
    and small clamps. Pure image algorithms (distance transforms, fills, …)
    must NOT live here — they go in AE-free headers so they stay unit-testable
    without the SDK.
*/

#pragma once

#include "AEConfig.h"

#ifdef AE_OS_WIN
    #include <Windows.h>
#endif

#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectPixelFormat.h"   // PF_PixelFormat_* for the 8/16/32bpc dispatch

namespace fs {

inline A_long ClampL(A_long v, A_long lo, A_long hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

inline float ClampF(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Bytes per pixel for the three AE packed formats (ARGB32=4, ARGB64=8,
// ARGB128=16). Unknown formats return 0 — callers must treat that as an
// unsupported-world error, not render through it.
size_t PixelBytes(PF_PixelFormat fmt);

// Unpack one row of an AE world into float ARGB, all channels scaled to
// [0,1]. dst must hold W*4 floats. Layout matches PF channel order:
// dst[0]=A, dst[1]=R, dst[2]=G, dst[3]=B per pixel.
void UnpackRow(const void *srcRow, float *dst, A_long W, PF_PixelFormat fmt);

// Pack float ARGB back into an AE world row. 8/16bpc are clamped to [0,1]
// and rounded; float passes through unclamped (32bpc is legitimately HDR).
void PackRow(const float *src, void *dstRow, A_long W, PF_PixelFormat fmt);

// Run a per-row callback threaded across AE's workers via iterate_generic
// when available, serially otherwise. Callbacks must only make disjoint
// writes keyed off the row index — the serial loop is then an exact
// fallback. Signature matches PF iterate_generic:
//   PF_Err cb(void *refcon, A_long thread_index, A_long i, A_long total)
void ForEachRow(PF_InData *in_data, A_long rows, void *refcon,
                PF_Err (*cb)(void *, A_long, A_long, A_long));

} // namespace fs
