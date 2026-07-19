/*  GT_Stamp.h — the cheap per-frame path of Grain Transfer.

    Everything expensive happened once (spectrum measure, tile bank bake).
    Per frame this just stamps: for each pixel, sample one precomputed tile
    (wrap addressing — the tile is toroidal by construction, so any offset,
    flip, or scale tiles seamlessly), scale by the response curve at the
    target's local luminance and by Amount, add in apply space.

    Per-frame variation (grain must never freeze — static grain reads as
    dirt on the lens): tile choice, offset, and flips are re-randomized per
    frame from (seed, frame). Near-zero cost.

    Row-based API with read-only shared state and disjoint row writes —
    safe under AE's iterate_generic and MFR, and trivially benchmarkable
    headlessly. AE-free.
*/

#pragma once

#include <cstdint>

#include "GT_Colour.h"
#include "GT_Grain.h"
#include "GT_Response.h"

namespace gt {

struct StampParams {
    int    tileIndex = 0;
    int    offX = 0, offY = 0;   // integer tile-space offsets
    bool   flipX = false, flipY = false;
    float  invScale = 1.f;       // tile-space step per output pixel = 100/Size%
    float  amount = 1.f;         // Amount/100
    float  trim = 1.f;           // Response/100
    Encode encode = Encode::Display;
    bool   previewGrainOnly = false;
};

// Deterministic per-frame randomization from (user seed, frame number).
StampParams MakeFrameStamp(uint64_t seed, int32_t frame, int tileCount, int tileN);

// Stamp ONE row (image row y, W pixels of ARGB float at rowArgb). This is
// the unit AE's iterate_generic fans out over rows — read-only shared state,
// each call writes only its own row.
void StampRow(float *rowArgb, int W, int y,
              const TileBank &bank, const ResponseCurve &resp,
              const StampParams &sp);

// Stamp rows [y0,y1) of an ARGB float image (AE order A,R,G,B per pixel),
// in place. Per pixel with alpha > 0: unpremultiply → encode to apply space
// → add tile grain × EvalSigma(local luma) × amount → decode → repremultiply.
// Alpha is never modified; alpha == 0 pixels pass through.
// previewGrainOnly instead renders mid-grey + the modulated grain, opaque —
// the trust/debug view of exactly the field being added.
// Sampling: exact integer wrap at invScale == 1, bilinear-wrap otherwise.
void StampRows(float *argb, int W, int H, int y0, int y1,
               const TileBank &bank, const ResponseCurve &resp,
               const StampParams &sp);

} // namespace gt
