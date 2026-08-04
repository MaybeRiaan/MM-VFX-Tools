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

    THE ARITHMETIC MOVED. The curves themselves now live in ../gt_core.h so
    the GPU kernels compile the same source text (they cannot use std:: or
    enum class). This header keeps the C++-facing API — the strongly typed
    Encode, and the names the DSP core and tests already call — and forwards.
    There is one body; this is its C++ spelling, not a second copy.
*/

#pragma once

#include "../gt_cpu_prologue.h"
#include "../gt_core.h"

namespace gt {

enum class Encode {
    Display,        // identity — already perceptual-ish
    LogFromLinear   // ACEScct lin↔log
};

// The kernels carry the mode as a plain int in their param block, so the
// enumerators must keep the values gt_core.h's GT_ENC_* macros use. If either
// side is ever reordered, this stops the build instead of quietly graining the
// wrong space on one backend.
static_assert((int)Encode::Display == GT_ENC_DISPLAY,
              "Encode::Display must match GT_ENC_DISPLAY");
static_assert((int)Encode::LogFromLinear == GT_ENC_LOGLINEAR,
              "Encode::LogFromLinear must match GT_ENC_LOGLINEAR");

// ACEScct constants (S-2016-001).
inline float LinToLog(float x) { return GT_LinToLog(x); }
inline float LogToLin(float y) { return GT_LogToLin(y); }

inline float EncodeChannel(float v, Encode m) { return GT_EncodeChannel(v, (int)m); }
inline float DecodeChannel(float v, Encode m) { return GT_DecodeChannel(v, (int)m); }

// Rec.709 luma of APPLY-SPACE (already encoded) RGB — the x-axis of the
// luminance→grain-amplitude response curve. Both measurement and evaluation
// use this same definition, so the exact weights are uncritical.
inline float Luma(float r, float g, float b) { return GT_Luma(r, g, b); }

} // namespace gt
