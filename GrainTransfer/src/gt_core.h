/*  gt_core.h — the per-pixel stamp math, compiled by every backend.

    PLATFORM-FREE (../../docs/gpu-kernel-rules.md §0). This file is the ONE
    body of Grain Transfer's per-frame math. The CPU render path compiles it
    through gt_cpu_prologue.h, nvcc through gpu/kernels/gt_prologue_cuda.h,
    dxc through gt_prologue_hlsl.h, and the AE-free tests through
    gt_prologue_host.h. It may use ONLY the GT_* vocabulary those prologues
    define — no <cmath>, no std::, no enum class, no references, no
    templates, no dynamic allocation.

    WHY IT EXISTS. Everything here previously lived in core/GT_Colour.h,
    core/GT_Response.cpp and core/GT_Stamp.cpp, and those files still own the
    CPU-facing API — they now forward to this header instead of holding a
    second copy of the arithmetic. A GPU port that transcribes the math into a
    kernel gets two bodies that drift silently; this one is compiled twice, so
    "the kernel matches the CPU" is not a claim to be re-tested after every
    edit, it is the same source text.

    THREE THINGS THAT DID NOT SURVIVE THE TRIP FROM GT_Colour.h, and how they
    are spelled here instead:
      - std::log2 / std::exp2      -> GT_LOG2 / GT_EXP2 (HLSL has no std::).
      - enum class Encode          -> a plain int, GT_ENC_DISPLAY /
                                      GT_ENC_LOGLINEAR. HLSL has no enums;
                                      the param block carries an int.
      - ResponseCurve's float[3][5] -> a flat 23-float block. HLSL cbuffers pad
                                      every array element to 16 bytes, so an
                                      array member would not lay out the same
                                      on both backends. GT_RESP_DECL declares
                                      23 CONSECUTIVE scalar floats and
                                      GT_RESP_LOAD gathers them, which is
                                      layout-identical everywhere.

    Numerical rule (rules §3): expression shape is load-bearing. Every formula
    below preserves the exact association and evaluation order of the CPU code
    it replaced, because the AE-free harness diffs the two compiles at EXACTLY
    0.0. Do not "simplify" `a + b * (c * d)` into `a + b * c * d`.
*/

#ifndef GT_CORE_H
#define GT_CORE_H

/* ---- apply space (core/GT_Colour.h) ------------------------------------- */

#define GT_ENC_DISPLAY   0
#define GT_ENC_LOGLINEAR 1

/* ACEScct constants (S-2016-001). The toe segment is linear, so values <= 0
   (wide-gamut negatives) pass through without NaNs and the transform stays
   exactly invertible. No clamping anywhere: 32bpc values above 1.0 must
   survive the round trip. */
GT_FUNC float GT_LinToLog(float x)
{
    return (x <= 0.0078125f)
        ? (10.5402377416545f * x + 0.0729055341958355f)
        : ((GT_LOG2(x) + 9.72f) / 17.52f);
}

GT_FUNC float GT_LogToLin(float y)
{
    return (y <= 0.155251141552511f)
        ? ((y - 0.0729055341958355f) / 10.5402377416545f)
        : GT_EXP2(y * 17.52f - 9.72f);
}

GT_FUNC float GT_EncodeChannel(float v, int m)
{
    return m == GT_ENC_LOGLINEAR ? GT_LinToLog(v) : v;
}

GT_FUNC float GT_DecodeChannel(float v, int m)
{
    return m == GT_ENC_LOGLINEAR ? GT_LogToLin(v) : v;
}

/* Rec.709 luma of APPLY-SPACE (already encoded) RGB — the x-axis of the
   response curve. Measurement and evaluation share this definition, so the
   exact weights are uncritical. */
GT_FUNC float GT_Luma(float r, float g, float b)
{
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

/* Mid-grey in apply space for the preview background: 0.5 display, or
   log(0.18) for the linear path (~ scene mid-grey in ACEScct). */
GT_FUNC float GT_PreviewBase(int enc)
{
    return enc == GT_ENC_LOGLINEAR ? GT_LinToLog(0.18f) : 0.5f;
}

/* ---- response curve (core/GT_Response.h) --------------------------------
   The 23 floats are 5 band mids, then 15 sigmas in CHANNEL-MAJOR order
   (c * 5 + b, matching the CPU's sigma[c][b]), then 3 flat sigmas. The host's
   GT_PackResponse writes them in exactly this order; GT_RESP_DECL declares
   them in exactly this order. That correspondence is the binding contract and
   it is asserted, not assumed, in gpu/cuda/gt_kernel_args.h.               */

#define GT_RESP_BANDS  5
#define GT_RESP_FLOATS 23

struct GT_Resp {
    float mid[GT_RESP_BANDS];
    float sig[3 * GT_RESP_BANDS];
    float flat[3];
};

#define GT_RESP_DECL \
    float gRMid0; float gRMid1; float gRMid2; float gRMid3; float gRMid4; \
    float gRSig0; float gRSig1; float gRSig2; float gRSig3; float gRSig4; \
    float gRSig5; float gRSig6; float gRSig7; float gRSig8; float gRSig9; \
    float gRSig10; float gRSig11; float gRSig12; float gRSig13; float gRSig14; \
    float gRFlat0; float gRFlat1; float gRFlat2;

#define GT_RESP_LOAD(r) \
    r.mid[0] = gRMid0; r.mid[1] = gRMid1; r.mid[2] = gRMid2; \
    r.mid[3] = gRMid3; r.mid[4] = gRMid4; \
    r.sig[0] = gRSig0; r.sig[1] = gRSig1; r.sig[2] = gRSig2; \
    r.sig[3] = gRSig3; r.sig[4] = gRSig4; r.sig[5] = gRSig5; \
    r.sig[6] = gRSig6; r.sig[7] = gRSig7; r.sig[8] = gRSig8; \
    r.sig[9] = gRSig9; r.sig[10] = gRSig10; r.sig[11] = gRSig11; \
    r.sig[12] = gRSig12; r.sig[13] = gRSig13; r.sig[14] = gRSig14; \
    r.flat[0] = gRFlat0; r.flat[1] = gRFlat1; r.flat[2] = gRFlat2;

/* sigma for `channel` at apply-space luminance L. Piecewise-linear between
   band centers, clamped at the ends, then lerped toward flatSigma by
   (1 - trim). Byte-for-byte the CPU's gt::EvalSigma. */
GT_FUNC float GT_EvalSigma(GT_Resp c, int channel, float L, float trim)
{
    int   base = channel * GT_RESP_BANDS;
    float s;
    if (L <= c.mid[0]) {
        s = c.sig[base + 0];
    } else if (L >= c.mid[GT_RESP_BANDS - 1]) {
        s = c.sig[base + (GT_RESP_BANDS - 1)];
    } else {
        int b = 0;
        while (b < GT_RESP_BANDS - 2 && L > c.mid[b + 1]) ++b;
        float t = (L - c.mid[b]) / (c.mid[b + 1] - c.mid[b]);
        s = c.sig[base + b] + t * (c.sig[base + b + 1] - c.sig[base + b]);
    }
    return c.flat[channel] + trim * (s - c.flat[channel]);
}

/* ---- tile sampling (core/GT_Stamp.cpp) ----------------------------------
   The RPN tiles are TOROIDAL by construction — that is the whole reason they
   tile seamlessly — so wrap is the correct edge rule here. rules §3.2's
   "out-of-range taps are SKIPPED, never clamped" governs CONVOLUTION
   COVERAGE and does not apply to a tile lookup. Index math only; no sampler
   on either backend, so bilinear is spelled out and identical.

   The bank is count x 3 x n x n floats, [tile][channel][y][x] — the flat
   form of gt::TileBank::tile(t, c).                                       */

GT_FUNC int GT_WrapI(int v, int n)
{
    v = v % n;
    return v < 0 ? v + n : v;
}

GT_FUNC float GT_WrapF(float t, float n)
{
    float f = t - GT_FLOOR(t / n) * n;
    if (f >= n) f = 0.0f;
    return f;
}

GT_FUNC float GT_Bilerp(float t00, float t01, float t10, float t11,
                        float ax, float ay)
{
    float top = t00 + ax * (t01 - t00);
    float bot = t10 + ax * (t11 - t10);
    return top + ay * (bot - top);
}

/* Tile addressing for image pixel (x, y): the four tap offsets WITHIN one
   channel plane, the two interpolation weights, and the flat offsets of the
   tile's three channel planes.

   This returns INDICES rather than samples because HLSL's
   RWStructuredBuffer<float> is not a pointer and cannot be passed to a
   function or offset arithmetically — buffer access belongs in a .kernel
   body, never in shared core (the same split qk_core.h keeps). What is shared
   is the addressing, which is the part with the wrap, flip, scale and
   toroidal-neighbour subtleties in it; the fetch each caller then performs is
   three subscripts and is covered by the parity test.

   On the `exact` path the four taps collapse to one and the weights are zero,
   so a caller may either branch on `exact` or feed all four to GT_Bilerp —
   both yield the identical float. */
GT_FUNC void GT_GrainAddr(int n, int tileIndex, int x, int y,
                          int offX, int offY, int flipX, int flipY,
                          float invScale, int exact,
                          GT_OUTI(base), GT_OUTI(plane),
                          GT_OUTI(i00), GT_OUTI(i01), GT_OUTI(i10), GT_OUTI(i11),
                          GT_OUTF(ax), GT_OUTF(ay))
{
    plane = n * n;
    base  = (tileIndex * 3) * plane;

    if (exact != 0) {
        int ty = flipY ? (offY - y) : (offY + y);
        int iy = GT_WrapI(ty, n);
        int tx = flipX ? (offX - x) : (offX + x);
        int ix = GT_WrapI(tx, n);
        i00 = iy * n + ix;
        i01 = i00; i10 = i00; i11 = i00;
        ax = 0.0f; ay = 0.0f;
    } else {
        float tyf = flipY ? ((float)offY - (float)y * invScale)
                          : ((float)offY + (float)y * invScale);
        float fy  = GT_WrapF(tyf, (float)n);
        float txf = flipX ? ((float)offX - (float)x * invScale)
                          : ((float)offX + (float)x * invScale);
        float fx  = GT_WrapF(txf, (float)n);

        int x0  = (int)fx, y0i = (int)fy;
        int x1  = (x0  + 1 == n) ? 0 : x0  + 1;
        int y1i = (y0i + 1 == n) ? 0 : y0i + 1;
        ax = fx - (float)x0; ay = fy - (float)y0i;
        i00 = y0i * n + x0;
        i01 = y0i * n + x1;
        i10 = y1i * n + x0;
        i11 = y1i * n + x1;
    }
}

/* ---- the per-pixel stamp (core/GT_Stamp.cpp) ----------------------------
   One premultiplied ARGB pixel in, one out. Outputs default to the inputs, so
   a pixel this function declines to touch is a passthrough — on the CPU that
   was a `continue` over an in-place buffer, and on the GPU dst is a different
   allocation from src, so the copy has to be explicit.

   ALPHA CONTRACT: alpha <= 1e-6 passes through untouched, alpha is never
   modified, and everything else round-trips unpremultiply -> encode -> add ->
   decode -> repremultiply. previewGrainOnly instead renders mid-grey plus the
   modulated grain, OPAQUE — the trust view of exactly the field being added.
*/
GT_FUNC void GT_StampPixel(float a, float r, float g, float b,
                           float gr, float gg, float gb,
                           GT_Resp resp, int enc, float amount, float trim,
                           int previewGrainOnly, float previewBase,
                           GT_OUTF(oa), GT_OUTF(orr), GT_OUTF(og), GT_OUTF(ob))
{
    oa = a; orr = r; og = g; ob = b;

    if (previewGrainOnly != 0) {
        /* Luminance modulation from the target where it exists, so the
           preview shows the field actually being added. */
        float L = previewBase;
        if (a > 1e-6f) {
            float inv = 1.0f / a;
            L = GT_Luma(GT_EncodeChannel(r * inv, enc),
                        GT_EncodeChannel(g * inv, enc),
                        GT_EncodeChannel(b * inv, enc));
        }
        oa = 1.0f;
        orr = GT_DecodeChannel(previewBase + gr * (GT_EvalSigma(resp, 0, L, trim) * amount), enc);
        og  = GT_DecodeChannel(previewBase + gg * (GT_EvalSigma(resp, 1, L, trim) * amount), enc);
        ob  = GT_DecodeChannel(previewBase + gb * (GT_EvalSigma(resp, 2, L, trim) * amount), enc);
        return;
    }

    if (a <= 1e-6f) return;   /* nothing to grain */

    float inv = 1.0f / a;
    float er = GT_EncodeChannel(r * inv, enc);
    float eg = GT_EncodeChannel(g * inv, enc);
    float eb = GT_EncodeChannel(b * inv, enc);
    float L  = GT_Luma(er, eg, eb);

    float sr = GT_EvalSigma(resp, 0, L, trim) * amount;
    float sg = GT_EvalSigma(resp, 1, L, trim) * amount;
    float sb = GT_EvalSigma(resp, 2, L, trim) * amount;

    orr = GT_DecodeChannel(er + gr * sr, enc) * a;
    og  = GT_DecodeChannel(eg + gg * sg, enc) * a;
    ob  = GT_DecodeChannel(eb + gb * sb, enc) * a;
}

#endif /* GT_CORE_H */
