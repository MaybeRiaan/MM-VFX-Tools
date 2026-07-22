/*  MM_Common.h — shared AE-facing helpers for the MM plugin suite.

    Compiled into each plugin target that lists MM_COMMON_SOURCES in its
    add_meb_plugin() call (same pattern as SDK_SOURCES — no separate lib
    target). ModernEdgeBlur predates this file and keeps its own private
    copies of these helpers; it can migrate onto this header later, but is
    deliberately NOT being touched now.

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

#include "MM_Parallel.h"            // ParallelBackend — glued to iterate_generic here

namespace mm {

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

// Build a ParallelBackend (see MM_Parallel.h) that fans mm::ParallelFor's
// range out across AE's workers via in_data->utils->iterate_generic, so the
// pure algorithm headers (EDT, Gaussian, pull–push) parallelize inside AE
// while staying serial (and deterministic) in the headless tests. Falls back
// to a serial loop when iterate_generic is unavailable. The returned backend
// captures in_data by value-of-pointer; it must not outlive the render call
// (SmartRender installs it via mm::ScopedParallelBackend for the render scope
// only).
ParallelBackend MakeIterateGenericBackend(PF_InData *in_data);

} // namespace mm
