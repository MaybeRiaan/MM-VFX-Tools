/*  fs_core.h — AE-free frequency-separation per-pixel math.

    The band split/recombine that smart_render.cpp's compose loop runs, factored
    out so it can be unit-tested without the AE SDK (tests/identity_test.cpp
    drives exactly this code). Operates on STRAIGHT (unpremultiplied) color;
    the caller handles premultiply/renormalize and packing. No clamping — all
    intermediates stay unbounded (only the final 8/16bpc pack clamps).

    Keep this header pure: no PF types, no globals. Anything that needs the SDK
    stays in smart_render.cpp.
*/

#pragma once

namespace fs {

// Recombine one straight-color channel:
//   hi   = src - lo
//   proc = lo + texGain * hi
//   out  = src + (proc - src) * m          (== lerp(src, proc, m))
// Written as src + (proc-src)*m so that texGain==1 && m==1 collapses to
// proc==src and out==src in exact float arithmetic — the identity property.
inline float RecombineChannel(float src, float lo, float texGain, float m)
{
	const float hi   = src - lo;
	const float proc = lo + texGain * hi;
	return src + (proc - src) * m;
}

// High-band inspection value: (texGain * hi) + 0.5 (PS-style mid-grey pivot).
// texGain matches what the Result recombine applies, so the High Band view
// shows the detail band AS IT WILL BE USED — Texture 0 flattens the view to
// grey, Texture 200% exaggerates it — consistent with how Low Band Smooth
// already previews in the Low Band view. Unbounded.
inline float HighBandDisplay(float src, float lo, float texGain)
{
	return texGain * (src - lo) + 0.5f;
}

} // namespace fs
