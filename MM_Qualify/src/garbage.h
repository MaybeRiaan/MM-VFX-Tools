/*  garbage.h — garbage matte: AE-free sampling over a premult ARGB plane.

    The render path checks out the Garbage Matte layer param, unpacks its
    world into a premultiplied ARGB float plane, and builds a QK_GarbageField
    describing that plane plus the Fit-to-Layer mapping (the resolution-
    invariant layer-space mapping proven in MM FreqSep's matte driver — v1
    ships Fit only). Per matte pixel the pipeline multiplies the raw
    qualifier matte by QK_SampleGarbage() BEFORE any cleanup (plan §4).

    AE-free (plain float buffers) so the mapping + bilinear sampling are
    unit-testable without the SDK, and so the GPU parity test can diff the
    qk_garbage.kernel transcription against exactly this code. The channel
    extraction and UV mapping themselves live in qk_core.h
    (QK_GarbageChannel / QK_GarbageUV) — shared verbatim with the kernel.

    Coordinates: (ox, oy) are OUTPUT-buffer pixel coords. Margin pixels of
    the inflated matte plane arrive as negative / oversized (ox, oy); the
    Fit mapping is linear so they extrapolate correctly, and reads land
    clamped to the garbage buffer edge (a Fit-stretched garbage covers the
    frame; beyond it the edge value is the least surprising answer).
*/

#pragma once

namespace qk {

struct QK_GarbageField {
	const float *data     = nullptr;   // W*H*4 premult ARGB (mm::UnpackRow layout)
	int          W        = 0;
	int          H        = 0;
	int          mOriginX = 0;         // garbage world top-left, garbage-layer coords
	int          mOriginY = 0;
	int          mRefW    = 0;         // garbage LAYER frame (full-res)
	int          mRefH    = 0;
	int          outOriginX = 0;       // output world top-left (downsampled coords)
	int          outOriginY = 0;
	int          srcRefW  = 0;         // source LAYER frame (full-res)
	int          srcRefH  = 0;
	float        dsx      = 1.f;       // downsample num/den (<= 1)
	float        dsy      = 1.f;
	int          channel  = 1;         // QK_GCHAN_* (1-based, popup value)
	bool         active   = false;     // false -> SampleGarbage returns 1.0
};

// Bilinear channel sample for output pixel (ox, oy); 1.0 when inactive.
float SampleGarbage(const QK_GarbageField &f, int ox, int oy);

} // namespace qk
