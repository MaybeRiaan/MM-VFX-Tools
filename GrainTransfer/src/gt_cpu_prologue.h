/*  gt_cpu_prologue.h — C++ spellings of the gt_core.h vocabulary.

    Include this, then gt_core.h, from any C++ translation unit (render path
    or tests). The GPU counterparts are src/gpu/kernels/gt_prologue_hlsl.h,
    gt_prologue_cuda.h and gt_prologue_host.h; all four must define the SAME
    names, because gt_core.h is compiled by every one of them and may use
    nothing else. See gt_core.h's header comment.
*/

#pragma once

#include <cmath>

#define GT_FUNC     static inline
#define GT_FLOOR(x) std::floor(x)
#define GT_FABS(x)  std::fabs(x)
#define GT_LOG2(x)  std::log2(x)
#define GT_EXP2(x)  std::exp2(x)
#define GT_MIN(a,b) ((a) < (b) ? (a) : (b))
#define GT_MAX(a,b) ((a) > (b) ? (a) : (b))
#define GT_OUTF(n)  float &n
#define GT_OUTI(n)  int &n
