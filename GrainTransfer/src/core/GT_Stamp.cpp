/*  GT_Stamp.cpp — see GT_Stamp.h.  */

#include "GT_Stamp.h"
#include "GT_Random.h"

#include <cmath>

namespace gt {

StampParams MakeFrameStamp(uint64_t seed, int32_t frame, int tileCount, int tileN)
{
    StampParams sp;
    Rng rng(HashSeed(seed, (uint64_t)(uint32_t)frame, 0x7374616Dull));
    sp.tileIndex = (int)rng.nextBelow((uint32_t)(tileCount > 0 ? tileCount : 1));
    sp.offX = (int)rng.nextBelow((uint32_t)(tileN > 0 ? tileN : 1));
    sp.offY = (int)rng.nextBelow((uint32_t)(tileN > 0 ? tileN : 1));
    sp.flipX = rng.nextBool();
    sp.flipY = rng.nextBool();
    return sp;
}

void StampRow(float *rowArgb, int W, int y,
              const TileBank &bank, const ResponseCurve &resp,
              const StampParams &sp)
{
    if (!bank.valid() || !resp.valid) return;

    // Every line of arithmetic below this point is in ../gt_core.h, which the
    // GPU kernels compile too (gpu/kernels/gt_stamp.kernel). This function is
    // the CPU's loop and memory layout around that shared body — deliberately
    // NOT a second implementation of the stamp. tests/kernel_test.cpp diffs
    // the two compiles at exactly 0.0.
    const int    n     = bank.n;
    const float *tiles = bank.tiles.data();
    const int    exact = std::fabs(sp.invScale - 1.f) < 1e-6f ? 1 : 0;
    const int    enc   = (int)sp.encode;
    const int    prev  = sp.previewGrainOnly ? 1 : 0;
    const int    flipX = sp.flipX ? 1 : 0;
    const int    flipY = sp.flipY ? 1 : 0;

    const float   previewBase = GT_PreviewBase(enc);
    const GT_Resp rp          = CoreResp(resp);

    float *px = rowArgb;
    for (int x = 0; x < W; ++x, px += 4) {
        // Grain sample per channel (unit sigma). Addressing is shared; the
        // three fetches are ours, because a GPU structured buffer cannot be
        // handed to a function (see GT_GrainAddr's comment).
        int   base, plane, i00, i01, i10, i11;
        float ax, ay;
        GT_GrainAddr(n, sp.tileIndex, x, y, sp.offX, sp.offY,
                     flipX, flipY, sp.invScale, exact,
                     base, plane, i00, i01, i10, i11, ax, ay);

        float g[3];
        if (exact) {
            g[0] = tiles[base + 0 * plane + i00];
            g[1] = tiles[base + 1 * plane + i00];
            g[2] = tiles[base + 2 * plane + i00];
        } else {
            for (int c = 0; c < 3; ++c) {
                const int p = base + c * plane;
                g[c] = GT_Bilerp(tiles[p + i00], tiles[p + i01],
                                 tiles[p + i10], tiles[p + i11], ax, ay);
            }
        }

        float oa, orr, og, ob;
        GT_StampPixel(px[0], px[1], px[2], px[3], g[0], g[1], g[2],
                      rp, enc, sp.amount, sp.trim, prev, previewBase,
                      oa, orr, og, ob);
        px[0] = oa; px[1] = orr; px[2] = og; px[3] = ob;
    }
}

void StampRows(float *argb, int W, int /*H*/, int y0, int y1,
               const TileBank &bank, const ResponseCurve &resp,
               const StampParams &sp)
{
    for (int y = y0; y < y1; ++y)
        StampRow(argb + (size_t)y * W * 4, W, y, bank, resp, sp);
}

} // namespace gt
