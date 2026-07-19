/*  qk_core.h — MM Qualify shared per-pixel math (C++ AND HLSL).

    THE single source of truth for every per-pixel decision the qualifier
    makes: qualify-in transforms, RGB->H/S conversion, the three gates
    (including seam-free hue wraparound), matte cleanup (clean black/white,
    clip levels), in/out ratio, the temporal IIR step, garbage channel
    extraction, and output-mode compose.

    This file is compiled by BOTH sides:
      - CPU render + tests:  #include "qk_cpu_prologue.h" then this file
      - GPU kernels (HLSL):  #include "qk_prologue_hlsl.h" then this file
    which is what makes GPU/CPU parity a property of the build rather than a
    hand-kept promise (extends docs/gpu-kernel-rules.md §0 from "platform-free
    kernels" to "one shared body"). Rules for code in this file:

      - Only the QK_* prologue vocabulary for math (QK_FLOOR, QK_POW, ...).
      - float / int scalars and plain structs. No pointers, no references,
        no templates, no includes, no namespaces, no 'inline' keyword
        (QK_FUNC carries the linkage per language).
      - Out-parameters via QK_OUTF(name) (HLSL 'out float', C++ 'float &').
      - No clamping of image intermediates; mattes are [0,1] by construction
        and QK_SAT is used only where that construction requires it.

    Numeric conventions:
      - Hue in degrees. Angle params are unbounded; all hue math wraps.
      - Gate values (sat/luma) unbounded above (HDR-safe); bounds params may
        legitimately exceed 1.0.
      - ACEScct constants match GrainMatch's GT_Colour.h (the suite copy).
*/

#ifndef QK_CORE_H
#define QK_CORE_H

/* ---- popup value spaces (0-based, kernel space; params.h popups are
        1-based and the host subtracts 1) ---------------------------------- */

/* Working Space (kernel-space, 0-based). Plain-language: "what is my footage,
   for the purpose of building the matte?" — the image is NEVER touched, only
   the qualification COPY. As-Is keys the pixels directly (correct for sRGB /
   Rec.709 / display AND any already-log-encoded footage — all perceptually
   spread already). Linear->Log lifts scene-linear (ACEScg / linear) into a
   log space first, so darks aren't crushed together and the gates behave the
   way they do in Resolve's ACES timeline. */
#define QK_QIN_ASIS     0
#define QK_QIN_LINLOG   1

/* Saturation model. The UI ships HSL only (matches Resolve's qualifier — no
   HSV toggle); QK_RGBToHS still implements both, and QK_MODEL_HSV stays
   defined for tests and any future use. */
#define QK_MODEL_HSV    0
#define QK_MODEL_HSL    1

#define QK_OUT_KEYED     0   /* Original + Alpha (keyed image)  */
#define QK_OUT_MATTE     1
#define QK_OUT_MATTE_INV 2
#define QK_OUT_HILITE    3   /* flat-grey highlight preview     */
#define QK_OUT_RAW       4   /* qualifier matte, pre-cleanup    */

/* garbage channel, 1-based to match the popup directly */
#define QK_GCHAN_ALPHA     1
#define QK_GCHAN_ALPHA_INV 2
#define QK_GCHAN_LUMA      3
#define QK_GCHAN_LUMA_INV  4

#define QK_EPS 1e-8f

/* ---- small helpers ----------------------------------------------------- */

QK_FUNC int QK_ClampI(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

QK_FUNC float QK_Wrap360(float x)
{
    return x - 360.0f * QK_FLOOR(x * (1.0f / 360.0f));
}

/* Premultiplied -> straight color. Zero-coverage pixels read as black (their
   matte value is irrelevant downstream: every image-bearing output mode
   multiplies by alpha again). */
QK_FUNC void QK_Straight(float a, float pr, float pg, float pb,
                         QK_OUTF(r), QK_OUTF(g), QK_OUTF(b))
{
    if (a > QK_EPS) {
        float ia = 1.0f / a;
        r = pr * ia; g = pg * ia; b = pb * ia;
    } else {
        r = 0.0f; g = 0.0f; b = 0.0f;
    }
}

/* ---- Qualify In: transforms the QUALIFICATION PATH ONLY (plan §1) ------- */

QK_FUNC float QK_QualifyIn1(float x, int mode)
{
    if (mode == QK_QIN_LINLOG) {
        /* scene-linear (ACEScg / linear) -> ACEScct log encoding. Constants
           match GrainMatch GT_Colour.h. The linear toe segment extends below
           0 gracefully (no NaN on negatives). This is the exact curve
           Resolve's ACES timeline qualifies in, which is why it "just works"
           for ACEScg footage: log-spread luma, sane saturation distribution,
           uniform gate softness. */
        return (x <= 0.0078125f)
            ? (10.5402377416545f * x + 0.0729055341958355f)
            : ((QK_LOG2(x) + 9.72f) / 17.52f);
    }
    return x;   /* QK_QIN_ASIS */
}

/* ---- RGB -> hue/saturation --------------------------------------------- */

/* Hue in degrees [0,360); saturation per model. Negative channels are
   clamped to 0 first (linear-light sources can dip below zero; hue/sat are
   undefined there). HSV sat stays well-defined for HDR (>1) values; HSL sat
   degrades to 0 outside its [0,1] lightness domain — HSV is the default. */
QK_FUNC void QK_RGBToHS(float r, float g, float b, int model,
                        QK_OUTF(hOut), QK_OUTF(sOut))
{
    r = QK_MAX(r, 0.0f); g = QK_MAX(g, 0.0f); b = QK_MAX(b, 0.0f);
    float mx = QK_MAX(r, QK_MAX(g, b));
    float mn = QK_MIN(r, QK_MIN(g, b));
    float c  = mx - mn;

    float h = 0.0f;
    if (c > QK_EPS) {
        if (mx == r)      h = (g - b) / c;
        else if (mx == g) h = (b - r) / c + 2.0f;
        else              h = (r - g) / c + 4.0f;
        h *= 60.0f;
        if (h < 0.0f) h += 360.0f;
    }

    float s = 0.0f;
    if (model == QK_MODEL_HSL) {
        float d = 1.0f - QK_ABS(mx + mn - 1.0f);   /* 1 - |2L - 1| */
        if (d > QK_EPS) s = c / d;
    } else {
        if (mx > QK_EPS) s = c / mx;
    }
    hOut = h; sOut = s;
}

/* ---- gates -------------------------------------------------------------- */

/* Circular hue gate. In/Out are absolute degrees (any real value; wrapped);
   the selected arc runs FORWARD from In to Out, so In > Out crosses 0° and
   reds behave naturally. Width wrap(Out-In) == 0 (In == Out, or 360 apart —
   the 0/360 default) means the FULL circle. Softness extends linearly
   (trapezoid, Lumetri/Resolve-style) outside each edge. */
QK_FUNC float QK_HueGate(float h, float hIn, float hOut,
                         float inSoft, float outSoft)
{
    float w = QK_Wrap360(hOut - hIn);
    if (w <= 0.0f) return 1.0f;
    float d = QK_Wrap360(h - hIn);
    if (d <= w) return 1.0f;

    float dOut = d - w;            /* past the Out edge, going forward  */
    float dIn  = 360.0f - d;       /* before the In edge, going back    */
    float f = 0.0f;
    if (outSoft > 0.0f && dOut < outSoft) f = 1.0f - dOut / outSoft;
    if (inSoft > 0.0f && dIn < inSoft) {
        float fi = 1.0f - dIn / inSoft;
        if (fi > f) f = fi;        /* overlapping falloffs: take the max */
    }
    return f;
}

/* Linear gate for saturation / luma. Absolute bounds + per-side linear
   softness. Bounds are swapped if reversed (robust under animation). */
QK_FUNC float QK_LinGate(float v, float lo, float hi,
                         float inSoft, float outSoft)
{
    if (lo > hi) { float t = lo; lo = hi; hi = t; }
    if (v >= lo && v <= hi) return 1.0f;
    if (v < lo) {
        if (inSoft <= 0.0f) return 0.0f;
        float d = lo - v;
        return (d < inSoft) ? (1.0f - d / inSoft) : 0.0f;
    }
    if (outSoft <= 0.0f) return 0.0f;
    float d = v - hi;
    return (d < outSoft) ? (1.0f - d / outSoft) : 0.0f;
}

/* ---- the qualifier ------------------------------------------------------ */

/* All gate parameters for one evaluation, resolved to kernel space by the
   host (enables as 0/1 floats, popups 0-based, luma weights as 3 floats).
   Kept as all-float so the GPU cbuffer maps 1:1 onto root constants. */
struct QK_Gates {
    float hueEn, hueIn, hueOut, hueInSoft, hueOutSoft;
    float satEn, satIn, satOut, satInSoft, satOutSoft;
    float lumEn, lumIn, lumOut, lumInSoft, lumOutSoft;
    float lumR, lumG, lumB;     /* luma weights (popup resolved by host) */
    float model;                /* QK_MODEL_*   */
    float qin;                  /* QK_QIN_*     */
};
#define QK_GATES_FLOATS 20

/* Field-list macros so GPU kernels can declare the 20 gate constants inside
   a PARAM_BLOCK and rebuild the struct without hand-copying. The order IS
   the binding contract with the host's QK_PackGates — both derive from the
   QK_Gates member order above; change all three together or not at all. */
#define QK_GATES_DECL \
    float gHueEn; float gHueIn; float gHueOut; float gHueInSoft; float gHueOutSoft; \
    float gSatEn; float gSatIn; float gSatOut; float gSatInSoft; float gSatOutSoft; \
    float gLumEn; float gLumIn; float gLumOut; float gLumInSoft; float gLumOutSoft; \
    float gLumR; float gLumG; float gLumB; float gModel; float gQin;
#define QK_GATES_LOAD(gp) \
    gp.hueEn = gHueEn; gp.hueIn = gHueIn; gp.hueOut = gHueOut; \
    gp.hueInSoft = gHueInSoft; gp.hueOutSoft = gHueOutSoft; \
    gp.satEn = gSatEn; gp.satIn = gSatIn; gp.satOut = gSatOut; \
    gp.satInSoft = gSatInSoft; gp.satOutSoft = gSatOutSoft; \
    gp.lumEn = gLumEn; gp.lumIn = gLumIn; gp.lumOut = gLumOut; \
    gp.lumInSoft = gLumInSoft; gp.lumOutSoft = gLumOutSoft; \
    gp.lumR = gLumR; gp.lumG = gLumG; gp.lumB = gLumB; \
    gp.model = gModel; gp.qin = gQin;

/* Raw qualifier matte for one STRAIGHT-color pixel: transform the
   qualification copy, evaluate the enabled gates, AND them (product — the
   soft-set intersection). No gates enabled -> 1.0 everywhere. */
QK_FUNC float QK_RawMatte(float r, float g, float b, QK_Gates gp)
{
    int qin = (int)gp.qin;
    float tr = QK_QualifyIn1(r, qin);
    float tg = QK_QualifyIn1(g, qin);
    float tb = QK_QualifyIn1(b, qin);

    float m = 1.0f;
    if (gp.hueEn > 0.5f || gp.satEn > 0.5f) {
        float h, s;
        QK_RGBToHS(tr, tg, tb, (int)gp.model, h, s);
        if (gp.hueEn > 0.5f)
            m *= QK_HueGate(h, gp.hueIn, gp.hueOut, gp.hueInSoft, gp.hueOutSoft);
        if (gp.satEn > 0.5f)
            m *= QK_LinGate(s, gp.satIn, gp.satOut, gp.satInSoft, gp.satOutSoft);
    }
    if (gp.lumEn > 0.5f) {
        float y = gp.lumR * tr + gp.lumG * tg + gp.lumB * tb;
        m *= QK_LinGate(y, gp.lumIn, gp.lumOut, gp.lumInSoft, gp.lumOutSoft);
    }
    return m;
}

/* ---- matte finesse: per-pixel stages ------------------------------------ */

/* Clean Black / Clean White + Black/White Clip. All args 0..1 (percent/100).
   Identity at cleanB=cleanW=clipB=0, clipW=1.

   These reshape the matte's soft ramp (so they act on a FEATHERED key, not a
   binary one). Deliberately STRONG — a little slider goes a long way, like
   Resolve's equivalents:

   Clean Black raises the black point AND applies an expanding gamma, so haze
   in the "off" region collapses to solid black fast. At full it drives the
   lower ~85% of the range to 0. Clean White is the mirror on the high end.

   Black/White Clip are a hard levels remap of [clipB, clipW] -> [0,1] with a
   generous gain: even a small clip visibly crushes the toe / shoulder. */
QK_FUNC float QK_CleanClip(float m, float cleanB, float cleanW,
                           float clipB, float clipW)
{
    /* Clean Black: hard black-point push (same threshold+gain shape as In/Out
       Ratio, which reads as strong). The threshold rises with cleanB — at
       full it sits near 0.9, so everything below ~0.9 is driven to solid
       black through a steep transition. */
    if (cleanB > 0.0f) {
        float thresh = 0.9f * cleanB;              /* 0 .. 0.9 */
        float gain   = 1.0f + 12.0f * cleanB;      /* 1 .. 13 */
        m = QK_SAT((m - thresh) * gain);
    }
    /* Clean White: mirror — hard white-point push. Threshold falls with
       cleanW; everything above the threshold is driven to solid white through
       a steep transition. out = 1 - SAT((thresh - m) * gain). */
    if (cleanW > 0.0f) {
        float thresh = 1.0f - 0.9f * cleanW;       /* 1 .. 0.1 */
        float gain   = 1.0f + 12.0f * cleanW;
        m = 1.0f - QK_SAT((thresh - m) * gain);
    }
    /* Black/White Clip: hard levels remap [clipB, clipW] -> [0,1]. clipB is
       the black cut point (default 0), clipW the white cut point (default 1).
       As they close in, the ramp between them steepens toward a threshold. */
    if (clipW - clipB > 1e-6f) m = (m - clipB) / (clipW - clipB);
    else                       m = (m <= clipB) ? 0.0f : 1.0f;
    return QK_SAT(m);
}

/* In/Out Ratio: bias the (blurred) matte edge outward (grow, ratio > 0) or
   inward (shrink, ratio < 0) by remapping the soft ramp — this only moves an
   edge that HAS a ramp, so pair it with Blur Radius. ratio in [-1, 1],
   identity at 0. Deliberately strong: the remap threshold sweeps almost the
   whole [0,1] range across the slider, so a modest value visibly chokes or
   spreads the edge.

   Grow: pull the 50% crossing DOWN toward 0 (more pixels reach full), up to
   near 0 at ratio 1. Shrink: push it UP toward 1. Implemented as a threshold
   + steep gain around it. */
QK_FUNC float QK_InOutRatio(float m, float ratio)
{
    if (ratio > -1e-6f && ratio < 1e-6f) return QK_SAT(m);

    /* Threshold the ramp should re-center on. Grow (ratio>0) moves it toward
       0.05; shrink (ratio<0) toward 0.95. Gain steepens with |ratio|. */
    float thresh = 0.5f - 0.45f * ratio;       /* 0.95 .. 0.05 */
    float gain   = 1.0f + 8.0f * QK_ABS(ratio); /* 1 .. 9 */
    float m2 = (m - thresh) * gain + 0.5f;
    return QK_SAT(m2);
}

/* ---- temporal stabilize (MM IIR module, shared design w/ BlemishRepair) - */

/* One asymmetric IIR step: s' = s + k*(m - s), k chosen per direction.
   kAttack governs the matte GROWING (new selection appearing), kRelease the
   matte SHRINKING (selection letting go). k == 1 follows instantly. */
QK_FUNC float QK_IIRStep(float s, float m, float kAttack, float kRelease)
{
    float k = (m > s) ? kAttack : kRelease;
    return s + (m - s) * k;
}

/* Smoothing percent (0..100) -> IIR k. 0% = k 1.0 (no smoothing); 100% =
   k 0.15, which the 6-frame warm-start window still converges through. */
QK_FUNC float QK_SmoothK(float pct)
{
    float p = QK_SAT(pct * 0.01f);
    return 1.0f - 0.85f * p;
}

/* ---- garbage matte ------------------------------------------------------ */

/* Channel extraction from an interpolated premultiplied ARGB sample. Luma is
   coverage-gated (luma of PREMULT rgb — alpha is already baked in), matching
   MM FreqSep's matte-driver semantics. Clamped: a garbage weight above 1
   must never push the qualifier matte above 1. */
QK_FUNC float QK_GarbageChannel(float a, float r, float g, float b, int channel)
{
    float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    float v;
    if (channel == QK_GCHAN_ALPHA_INV)     v = 1.0f - a;
    else if (channel == QK_GCHAN_LUMA)     v = luma;
    else if (channel == QK_GCHAN_LUMA_INV) v = a - luma;
    else                                   v = a;
    return QK_SAT(v);
}

/* Map an OUTPUT-buffer pixel (ox,oy) onto the garbage BUFFER (u,v), all in
   pixel centers. Fit-to-Layer in full-res layer space: output buffer coord ->
   source-layer frame normalized -> garbage-layer frame -> garbage buffer.
   Mirrors MM FreqSep's resolution-invariant Fit mapping. */
QK_FUNC void QK_GarbageUV(float ox, float oy,
                          float outOriginX, float outOriginY,
                          float srcRefW, float srcRefH,
                          float mRefW, float mRefH,
                          float mOriginX, float mOriginY,
                          float dsx, float dsy,
                          QK_OUTF(u), QK_OUTF(v))
{
    float fx = (ox + 0.5f + outOriginX) / QK_MAX(dsx, 1e-6f);
    float fy = (oy + 0.5f + outOriginY) / QK_MAX(dsy, 1e-6f);
    float nx = (srcRefW > 0.0f) ? (fx / srcRefW) : 0.0f;
    float ny = (srcRefH > 0.0f) ? (fy / srcRefH) : 0.0f;
    u = nx * mRefW * dsx - mOriginX - 0.5f;
    v = ny * mRefH * dsy - mOriginY - 0.5f;
}

/* ---- output ------------------------------------------------------------- */

#define QK_HILITE_GREY 0.5f

/* Compose one output pixel from the finished matte m, the raw (pre-cleanup)
   matte rawM, and the PREMULTIPLIED source pixel. Every image-bearing mode
   is expressed against premultiplied color directly (straight*m*a == pm*m),
   so no unpremultiply — and no zero-alpha special case — is ever needed. */
QK_FUNC void QK_Compose(int mode, float m, float rawM,
                        float a, float pr, float pg, float pb,
                        QK_OUTF(oa), QK_OUTF(orr), QK_OUTF(og), QK_OUTF(ob))
{
    if (mode == QK_OUT_MATTE) {
        oa = 1.0f; orr = m; og = m; ob = m;
    } else if (mode == QK_OUT_MATTE_INV) {
        float im = 1.0f - m;
        oa = 1.0f; orr = im; og = im; ob = im;
    } else if (mode == QK_OUT_HILITE) {
        float w  = m * a;
        float gy = QK_HILITE_GREY * (1.0f - w);
        oa = 1.0f;
        orr = gy + m * pr; og = gy + m * pg; ob = gy + m * pb;
    } else if (mode == QK_OUT_RAW) {
        oa = 1.0f; orr = rawM; og = rawM; ob = rawM;
    } else {   /* QK_OUT_KEYED */
        oa = m * a; orr = m * pr; og = m * pg; ob = m * pb;
    }
}

#endif /* QK_CORE_H */
