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

// Wrap an integer into [0,n). n is a power of two in practice but this does
// not rely on it.
static inline int WrapI(int v, int n)
{
    v %= n;
    return v < 0 ? v + n : v;
}

void StampRows(float *argb, int W, int H, int y0, int y1,
               const TileBank &bank, const ResponseCurve &resp,
               const StampParams &sp)
{
    if (!bank.valid() || !resp.valid) return;

    const int n = bank.n;
    const float *tiles[3] = {
        bank.tile(sp.tileIndex, 0),
        bank.tile(sp.tileIndex, 1),
        bank.tile(sp.tileIndex, 2),
    };

    const bool exact = std::fabs(sp.invScale - 1.f) < 1e-6f;
    const Encode enc = sp.encode;
    const float amount = sp.amount;
    const float trim = sp.trim;

    // Mid-grey in apply space for the preview background: 0.5 display, or
    // log(0.18) for the linear path (≈ scene mid-grey in ACEScct).
    const float previewBase =
        enc == Encode::LogFromLinear ? LinToLog(0.18f) : 0.5f;

    for (int y = y0; y < y1; ++y) {
        float *px = argb + (size_t)y * W * 4;

        // Tile row coordinate for this output row.
        float fy = 0.f;
        int iy = 0;
        if (exact) {
            const int ty = sp.flipY ? (sp.offY - y) : (sp.offY + y);
            iy = WrapI(ty, n);
        } else {
            const float ty = sp.flipY ? ((float)sp.offY - (float)y * sp.invScale)
                                      : ((float)sp.offY + (float)y * sp.invScale);
            fy = ty - std::floor(ty / (float)n) * (float)n;   // wrap to [0,n)
            if (fy >= (float)n) fy = 0.f;
        }

        for (int x = 0; x < W; ++x, px += 4) {
            // Grain sample per channel (unit sigma).
            float g[3];
            if (exact) {
                const int tx = sp.flipX ? (sp.offX - x) : (sp.offX + x);
                const int ix = WrapI(tx, n);
                const size_t idx = (size_t)iy * n + ix;
                g[0] = tiles[0][idx];
                g[1] = tiles[1][idx];
                g[2] = tiles[2][idx];
            } else {
                const float tx = sp.flipX
                    ? ((float)sp.offX - (float)x * sp.invScale)
                    : ((float)sp.offX + (float)x * sp.invScale);
                float fx = tx - std::floor(tx / (float)n) * (float)n;
                if (fx >= (float)n) fx = 0.f;

                const int x0 = (int)fx, y0i = (int)fy;
                const int x1 = x0 + 1 == n ? 0 : x0 + 1;
                const int y1i = y0i + 1 == n ? 0 : y0i + 1;
                const float ax = fx - (float)x0, ay = fy - (float)y0i;
                const size_t i00 = (size_t)y0i * n + x0;
                const size_t i01 = (size_t)y0i * n + x1;
                const size_t i10 = (size_t)y1i * n + x0;
                const size_t i11 = (size_t)y1i * n + x1;
                for (int c = 0; c < 3; ++c) {
                    const float *t = tiles[c];
                    const float top = t[i00] + ax * (t[i01] - t[i00]);
                    const float bot = t[i10] + ax * (t[i11] - t[i10]);
                    g[c] = top + ay * (bot - top);
                }
            }

            const float a = px[0];

            if (sp.previewGrainOnly) {
                // Luminance modulation from the target where it exists so the
                // preview shows the field actually being added.
                float L = previewBase;
                if (a > 1e-6f) {
                    const float inv = 1.f / a;
                    L = Luma(EncodeChannel(px[1] * inv, enc),
                             EncodeChannel(px[2] * inv, enc),
                             EncodeChannel(px[3] * inv, enc));
                }
                px[0] = 1.f;
                for (int c = 0; c < 3; ++c) {
                    const float s = EvalSigma(resp, c, L, trim) * amount;
                    px[1 + c] = DecodeChannel(previewBase + g[c] * s, enc);
                }
                continue;
            }

            if (a <= 1e-6f) continue;   // nothing to grain

            const float inv = 1.f / a;
            const float er = EncodeChannel(px[1] * inv, enc);
            const float eg = EncodeChannel(px[2] * inv, enc);
            const float eb = EncodeChannel(px[3] * inv, enc);
            const float L = Luma(er, eg, eb);

            const float sr = EvalSigma(resp, 0, L, trim) * amount;
            const float sg = EvalSigma(resp, 1, L, trim) * amount;
            const float sb = EvalSigma(resp, 2, L, trim) * amount;

            px[1] = DecodeChannel(er + g[0] * sr, enc) * a;
            px[2] = DecodeChannel(eg + g[1] * sg, enc) * a;
            px[3] = DecodeChannel(eb + g[2] * sb, enc) * a;
        }
    }
}

} // namespace gt
