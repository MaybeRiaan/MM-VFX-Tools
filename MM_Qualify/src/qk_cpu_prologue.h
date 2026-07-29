/*  qk_cpu_prologue.h — C++ spellings of the qk_core.h vocabulary.

    Include this, then qk_core.h, from any C++ translation unit (render path
    or tests). The HLSL counterpart is src/gpu/kernels/qk_prologue_hlsl.h;
    both must define the SAME names — qk_core.h is compiled by both languages
    and may only use this vocabulary. See qk_core.h's header comment.
*/

#pragma once

#include <cmath>

#define QK_FUNC     static inline
#define QK_FLOOR(x) std::floor(x)
#define QK_POW(x,y) std::pow((x), (y))
#define QK_LOG2(x)  std::log2(x)
#define QK_LOG10(x) std::log10(x)
#define QK_MIN(a,b) ((a) < (b) ? (a) : (b))
#define QK_MAX(a,b) ((a) > (b) ? (a) : (b))
#define QK_ABS(x)   std::fabs(x)
#define QK_SAT(x)   (QK_MIN(QK_MAX((x), 0.0f), 1.0f))
#define QK_OUTF(n)  float &n
