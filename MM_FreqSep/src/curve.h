/*  curve.h — Working Curve interface (stubbed this session).

    Plan §0: the FS core is space-agnostic; on scene-linear pipelines the
    curve popup internally encodes → processes → decodes (sRGB or ACEScct)
    so high-band amplitude stays perceptually uniform instead of scaling
    with brightness. The curve applies to PROCESSING ONLY — never to the
    matte, and the output is decoded back, so Result with Texture=100/
    Smooth=0 stays an identity regardless of curve.

    smart_render.cpp marks the hook points: encode the unpacked STRAIGHT
    color before the split (the blur must see encoded values for the bands
    to be perceptually uniform), decode after recombine, before the final
    mix. STUB: identity for every popup value, so the hooks stay dormant
    this session.
*/

#pragma once

#include "AE_Effect.h"

namespace fs {

// curveL is FS_CURVE_* (1-based popup value). Applied per RGB channel to
// STRAIGHT (unpremultiplied) color. Identity until the curve ships.
inline float CurveEncode(float v, A_long /*curveL*/) { return v; }
inline float CurveDecode(float v, A_long /*curveL*/) { return v; }

} // namespace fs
