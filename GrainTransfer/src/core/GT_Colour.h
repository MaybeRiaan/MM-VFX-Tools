/*  GT_Colour.h — apply-space encoding for Grain Transfer.

    Film grain is ~constant in density (log); it only becomes exposure-
    dependent when viewed in linear. So BOTH extraction and application run
    in an apply space that is log-density-like:

      Display      — incoming pixels are display-gamma encoded (Rec.709/sRGB
                     working space, or CM bypassed). Gamma is already a rough
                     perceptual curve; apply as-is (identity encode).
      LogFromLinear — incoming pixels are scene-linear (ACEScg / linearized
                     working space). Encode linear → ACEScct-style log,
                     apply, decode back. The ACEScct toe segment is linear,
                     so values ≤ 0 (wide-gamut negatives) pass through
                     without NaNs and the transform stays exactly invertible.

    Float-native throughout — no clamping anywhere in this header; 32bpc
    values above 1.0 must survive the round trip (log2 handles them).
*/

#pragma once

#include <cmath>

namespace gt {

enum class Encode {
    Display,        // identity — already perceptual-ish
    LogFromLinear   // ACEScct lin↔log
};

// ACEScct constants (S-2016-001).
inline float LinToLog(float x)
{
    return (x <= 0.0078125f)
        ? (10.5402377416545f * x + 0.0729055341958355f)
        : ((std::log2(x) + 9.72f) / 17.52f);
}

inline float LogToLin(float y)
{
    return (y <= 0.155251141552511f)
        ? ((y - 0.0729055341958355f) / 10.5402377416545f)
        : std::exp2(y * 17.52f - 9.72f);
}

inline float EncodeChannel(float v, Encode m)
{
    return m == Encode::LogFromLinear ? LinToLog(v) : v;
}

inline float DecodeChannel(float v, Encode m)
{
    return m == Encode::LogFromLinear ? LogToLin(v) : v;
}

// Rec.709 luma of APPLY-SPACE (already encoded) RGB — the x-axis of the
// luminance→grain-amplitude response curve. Both measurement and evaluation
// use this same definition, so the exact weights are uncritical.
inline float Luma(float r, float g, float b)
{
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

} // namespace gt
