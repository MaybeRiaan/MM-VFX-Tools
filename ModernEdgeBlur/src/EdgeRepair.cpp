/*  EdgeRepair.cpp — Stage 1: params + inflated checkout + tiling debug mode.

    Edge Repair extends trusted interior color across contaminated matte
    edges (keying halos, motion-blur rims) along a single per-frame Euclidean
    distance field: core matte → EDT → pull–push fill → distance-weighted
    blend with frequency-separated Detail (texture preserve) → optional alpha
    choke. Pure stream operator: RGBA in, RGBA out, RGB modified, alpha
    untouched unless the user explicitly chokes it. Five locked params
    (Erode/Extend/Detail/Alpha Choke/View Mode, disk IDs 101-105); premult is
    internal-only, no knob.

    THIS STAGE: the five params are registered and the inflated PreRender
    checkout is implemented, but no image algorithm runs yet — every View
    Mode renders passthrough. The one thing that DOES run is the tiling debug
    visualization (compile-time switch ER_TILING_DEBUG), whose whole job is to
    prove the inflation is correct: it makes any missing inflated border show
    up as a hard seam at tile boundaries. Stages 2-4 replace the passthrough
    body with the real pipeline.

    THE #1 CORRECTNESS PROPERTY — input-rect inflation. Unlike ModernEdgeBlur
    (which deliberately does NO inflation), every pixel of Edge Repair's
    output depends on input pixels up to `maxRadius` away:
        maxRadius = ceil( max(Erode,0) + max(Extend,0) + |AlphaChoke|
                          + ER_TEXTURE_BLUR_RADIUS )
    (Erode/Choke clamped at 0 on the outward side — only their outward reach
    matters for how far we must READ; the pull–push fill runs unbounded but is
    only USED out to Extend, so Extend is what governs the read radius.)
    PreRender expands the checkout request rect by maxRadius on all sides
    (AE clamps to available layer data); SmartRender then addresses the larger
    source world through the origin offset AE reports. Get this wrong and
    seams appear that differ between RAM preview and final render — the single
    hardest bug in the project, which is why Stage 1 gates on it explicitly.

    MFR-safe: no mutable global/static state anywhere in this plugin. The
    inflation amount and rects computed in PreRender are stashed in
    pre_render_data (heap, owned by AE, freed in our handler) and read back in
    SmartRender — never in a global.
*/

#define NOMINMAX
#include "EdgeRepair.h"
#include "common/MM_Common.h"
#include "common/MM_DistanceField.h"
#include "common/MM_CoreMatte.h"

#include <cmath>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <vector>
#include <new>

// Fixed texture-frequency-split blur radius (px). The Detail high-pass in
// Stage 4 subtracts a small Gaussian of this radius from the plate; it
// contributes to the inflation radius because that Gaussian reads neighbors.
// Exposed nowhere — a fixed sensible value per the brief.
static const float ER_TEXTURE_BLUR_RADIUS = 8.0f;

// Tiling debug visualization. 1 = every View Mode renders the Stage 1 seam-
// test pattern (see ER_TileDebugRow); 0 = the real pipeline/view modes run.
// Inflation was signed off at the Stage 1 gate, so this is OFF now; flip to 1
// only to re-run the seam test.
#define ER_TILING_DEBUG 0

// ---------------------------------------------------------------------------
// Inflation radius from the current params. Only OUTWARD reach matters for
// how far output pixels must read into the input: Extend governs the used
// band, Erode/Choke contribute their magnitudes (either sign moves the edge,
// and the render region must cover the moved edge either way), plus the
// texture blur's own kernel reach. Returned in pixels, ceil'd, >= 0.
// ---------------------------------------------------------------------------
static A_long ER_InflateRadius(const ER_Params &p)
{
    const double reach =
        std::max(0.0, std::abs((double)p.erodeF)) +
        std::max(0.0, (double)p.extendF) +
        std::abs((double)p.chokeF) +
        (double)ER_TEXTURE_BLUR_RADIUS;
    A_long r = (A_long)std::ceil(reach);
    return r < 0 ? 0 : r;
}

// Stashed by PreRender, read by SmartRender. Just the inflation amount — the
// source-world origin offset is recovered from the checked-out world itself
// in SmartRender, so nothing here needs to survive that isn't a plain scalar.
typedef struct {
    A_long inflate;   // px added on every side in PreRender
} ER_PreRenderData;

// AE calls this to free the block we handed it via
// extra->output->pre_render_data (see PreRender). Signature is fixed by
// PF_DeletePreRenderDataFunc.
extern "C" void
ER_DeletePreRenderData(void *pre_render_dataPV)
{
    if (pre_render_dataPV) free(pre_render_dataPV);
}

// ---------------------------------------------------------------------------
// Param read. current_time/time_step/time_scale come straight from in_data.
// ---------------------------------------------------------------------------
static PF_Err
ReadParams(PF_InData *in_data, ER_Params *p)
{
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    PF_ParamDef def;

    #define ER_GET_FLOAT(IDX, FLD) \
        AEFX_CLR_STRUCT(def); \
        ERR(PF_CHECKOUT_PARAM(in_data, IDX, in_data->current_time, \
            in_data->time_step, in_data->time_scale, &def)); \
        p->FLD = def.u.fs_d.value; \
        ERR2(PF_CHECKIN_PARAM(in_data, &def));

    ER_GET_FLOAT(ER_ERODE,  erodeF)
    ER_GET_FLOAT(ER_EXTEND, extendF)
    ER_GET_FLOAT(ER_DETAIL, detailF)
    ER_GET_FLOAT(ER_CHOKE,  chokeF)
    #undef ER_GET_FLOAT

    AEFX_CLR_STRUCT(def);
    ERR(PF_CHECKOUT_PARAM(in_data, ER_VIEW, in_data->current_time,
        in_data->time_step, in_data->time_scale, &def));
    p->viewL = def.u.pd.value;   // popup: 1-based index
    ERR2(PF_CHECKIN_PARAM(in_data, &def));

    return err;
}

// ---------------------------------------------------------------------------
// Tiling debug pass — a true circular dilation of the input alpha by radius R.
// For each output pixel it takes the MAX input alpha over the disc of radius R
// centered on that pixel, i.e. "is there any matte within R of me?" Because
// dilation by a disc is rotationally symmetric, a filled circle grown by R
// stays a perfect circle — so the ONLY way the halo can show a straight-edged
// discontinuity is a real render-tile seam (input data missing outside a
// tile because PreRender under-inflated), never the sampler's own
// neighborhood shape.
//
// [Why not the cheap 4-tap axis sampler used earlier: sampling only (x±R,y)
// and (x,y±R) traces a DIAMOND, not a disc, so it drew faceted/octagonal
// halos that were easy to mistake for seams. A correct-inflation frame looked
// buggy. The disc removes that ambiguity — see the Stage 1 gate notes.]
//
// Cost: rather than the full O(R^2) disc (~785K taps/pixel at R=500), we scan
// each row offset dy in [-R,R] and sample only the disc's horizontal EXTENT
// endpoints at that row, (sx ± floor(sqrt(R^2 - dy^2)), sy+dy). ~2R taps/pixel
// (~1000 at R=500) — fine for a debug view. This is EXACT for a convex opaque
// matte (the endpoints are the extreme reach on each row, so if the disc
// overlaps the shape it overlaps an endpoint) — which is exactly the circle
// probe the gate relies on. For a non-convex matte with an opaque sliver
// sitting strictly between an endpoint and center, this can under-report on
// that row; harmless for the gate (use the circle as the shape oracle), and
// the whole debug view is removed in Stage 2 regardless.
//
// srcOX/srcOY map an OUTPUT pixel to its coordinate in the (larger, inflated)
// source buffer: output pixel (ox,oy) lives at source (ox+srcOX, oy+srcOY).
// Sampling is clamped to the source buffer bounds, so at the true layer edge
// it degrades gracefully instead of reading garbage.
// ---------------------------------------------------------------------------
struct ER_TileDebugRefcon {
    const float    *src;        // inflated source, srcW*srcH*4 ARGB float
    A_long          srcW, srcH;
    A_long          srcOX, srcOY;   // output→source origin offset (>= 0)
    A_long          radius;         // R: disc radius == maxRadius
    PF_EffectWorld *out_world;
    PF_PixelFormat  fmt;
    A_long          outW;
};

extern "C" {

static PF_Err
ER_TileDebugRow(void *refconPV, A_long /*thread_indexL*/, A_long oy, A_long /*iterationsL*/)
{
    const ER_TileDebugRefcon *rc = static_cast<const ER_TileDebugRefcon*>(refconPV);
    const A_long W  = rc->outW;
    const A_long R  = rc->radius;
    const A_long R2 = R * R;
    const A_long sy = oy + rc->srcOY;

    thread_local std::vector<float> rowv;
    if ((A_long)rowv.size() < W * 4) rowv.resize((size_t)W * 4);
    float *dst = rowv.data();

    for (A_long ox = 0; ox < W; ++ox, dst += 4) {
        const A_long sx = ox + rc->srcOX;

        float m = rc->src[((size_t)sy * rc->srcW + sx) * 4 + 0]; // center tap
        for (A_long dy = -R; dy <= R && m < 1.f; ++dy) {
            const A_long syy = mm::ClampL(sy + dy, 0, rc->srcH - 1);
            const A_long dx  = (A_long)std::floor(std::sqrt((double)(R2 - dy * dy)));
            const float *row = rc->src + (size_t)syy * rc->srcW * 4;

            const A_long sxL = mm::ClampL(sx - dx, 0, rc->srcW - 1);
            const float  aL  = row[(size_t)sxL * 4 + 0];
            if (aL > m) m = aL;

            const A_long sxR = mm::ClampL(sx + dx, 0, rc->srcW - 1);
            const float  aR  = row[(size_t)sxR * 4 + 0];
            if (aR > m) m = aR;
        }

        // Greyscale, fully opaque so the pattern is visible over any comp bg.
        dst[0] = 1.f; dst[1] = m; dst[2] = m; dst[3] = m;
    }

    void *outRow = reinterpret_cast<char*>(rc->out_world->data) + (size_t)oy * rc->out_world->rowbytes;
    mm::PackRow(rowv.data(), outRow, W, rc->fmt);
    return PF_Err_NONE;
}

} // extern "C"

// ---------------------------------------------------------------------------
// Passthrough row: copy the (offset) source region straight to output via
// unpack→repack so it works regardless of the source origin offset. Used by
// every non-debug View Mode this stage.
// ---------------------------------------------------------------------------
struct ER_PassRefcon {
    const float    *src;        // inflated source ARGB float
    A_long          srcW;
    A_long          srcOX, srcOY;
    PF_EffectWorld *out_world;
    PF_PixelFormat  fmt;
    A_long          outW;
};

extern "C" {

static PF_Err
ER_PassRow(void *refconPV, A_long /*thread_indexL*/, A_long oy, A_long /*iterationsL*/)
{
    const ER_PassRefcon *rc = static_cast<const ER_PassRefcon*>(refconPV);
    const float *srcRow = rc->src + ((size_t)(oy + rc->srcOY) * rc->srcW + rc->srcOX) * 4;
    void *outRow = reinterpret_cast<char*>(rc->out_world->data) + (size_t)oy * rc->out_world->rowbytes;
    mm::PackRow(srcRow, outRow, rc->outW, rc->fmt);
    return PF_Err_NONE;
}

} // extern "C"

// Unpack the whole (inflated) source world into a contiguous ARGB float
// buffer. One threaded pass; the rest of the pipeline works off this buffer.
struct ER_UnpackRefcon {
    const PF_EffectWorld *world;
    PF_PixelFormat        fmt;
    float                *dst;    // srcW*srcH*4
    A_long                srcW;
};

extern "C" {

static PF_Err
ER_UnpackRow(void *refconPV, A_long /*thread_indexL*/, A_long y, A_long /*iterationsL*/)
{
    const ER_UnpackRefcon *rc = static_cast<const ER_UnpackRefcon*>(refconPV);
    const void *srcRow = reinterpret_cast<const char*>(rc->world->data) + (size_t)y * rc->world->rowbytes;
    mm::UnpackRow(srcRow, rc->dst + (size_t)y * rc->srcW * 4, rc->srcW, rc->fmt);
    return PF_Err_NONE;
}

} // extern "C"

// ---------------------------------------------------------------------------
// View-mode visualizations (Stage 2). Both read a full-inflated-source plane
// (core matte, or distance field) and write the OUTPUT region of it, mapped
// through the srcOX/srcOY origin offset. Computing on the inflated source and
// cropping to output is what keeps the field correct right up to the output
// edge — distances there depend on core pixels in the inflated border.
// ---------------------------------------------------------------------------

// Core Matte: white where core, black elsewhere. Opaque greyscale.
struct ER_CoreViewRefcon {
    const uint8_t  *core;       // srcW*srcH 0/1
    A_long          srcW;
    A_long          srcOX, srcOY;
    PF_EffectWorld *out_world;
    PF_PixelFormat  fmt;
    A_long          outW;
};

// Distance Field: normalized d/maxDisplay → greyscale (0 at core, brightening
// outward, clamped to white past maxDisplay). Opaque.
struct ER_DistViewRefcon {
    const float    *dist;       // srcW*srcH
    A_long          srcW;
    A_long          srcOX, srcOY;
    float           invMaxDisplay;   // 1 / normalization distance
    PF_EffectWorld *out_world;
    PF_PixelFormat  fmt;
    A_long          outW;
};

extern "C" {

static PF_Err
ER_CoreViewRow(void *refconPV, A_long /*thread_indexL*/, A_long oy, A_long /*iterationsL*/)
{
    const ER_CoreViewRefcon *rc = static_cast<const ER_CoreViewRefcon*>(refconPV);
    const A_long W = rc->outW;
    const uint8_t *srow = rc->core + (size_t)(oy + rc->srcOY) * rc->srcW + rc->srcOX;

    thread_local std::vector<float> rowv;
    if ((A_long)rowv.size() < W * 4) rowv.resize((size_t)W * 4);
    float *dst = rowv.data();
    for (A_long ox = 0; ox < W; ++ox, dst += 4) {
        const float v = srow[ox] ? 1.f : 0.f;
        dst[0] = 1.f; dst[1] = v; dst[2] = v; dst[3] = v;
    }

    void *outRow = reinterpret_cast<char*>(rc->out_world->data) + (size_t)oy * rc->out_world->rowbytes;
    mm::PackRow(rowv.data(), outRow, W, rc->fmt);
    return PF_Err_NONE;
}

static PF_Err
ER_DistViewRow(void *refconPV, A_long /*thread_indexL*/, A_long oy, A_long /*iterationsL*/)
{
    const ER_DistViewRefcon *rc = static_cast<const ER_DistViewRefcon*>(refconPV);
    const A_long W = rc->outW;
    const float *srow = rc->dist + (size_t)(oy + rc->srcOY) * rc->srcW + rc->srcOX;

    thread_local std::vector<float> rowv;
    if ((A_long)rowv.size() < W * 4) rowv.resize((size_t)W * 4);
    float *dst = rowv.data();
    for (A_long ox = 0; ox < W; ++ox, dst += 4) {
        const float v = mm::ClampF(srow[ox] * rc->invMaxDisplay, 0.f, 1.f);
        dst[0] = 1.f; dst[1] = v; dst[2] = v; dst[3] = v;
    }

    void *outRow = reinterpret_cast<char*>(rc->out_world->data) + (size_t)oy * rc->out_world->rowbytes;
    mm::PackRow(rowv.data(), outRow, W, rc->fmt);
    return PF_Err_NONE;
}

} // extern "C"

// ---------------------------------------------------------------------------
// Plugin entry points
// ---------------------------------------------------------------------------

static PF_Err
About(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *[], PF_LayerDef *)
{
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    suites.ANSICallbacksSuite1()->sprintf(out_data->return_msg,
        "%s v%d.%d\rEdge extension and decontamination along a Euclidean distance field.",
        STR(StrID_Name), MAJOR_VERSION, MINOR_VERSION);
    return PF_Err_NONE;
}

static PF_Err
GlobalSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *[], PF_LayerDef *)
{
    out_data->my_version = PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION,
                                      STAGE_VERSION, BUILD_VERSION);
    // Must match resources/EdgeRepairPiPL.r literals exactly (0x02000440 /
    // 0x08001400) or AE rejects with a version/outflags mismatch.
    out_data->out_flags  = PF_OutFlag_DEEP_COLOR_AWARE |
                           PF_OutFlag_PIX_INDEPENDENT  |
                           PF_OutFlag_USE_OUTPUT_EXTENT;
    out_data->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE        |
                           PF_OutFlag2_SUPPORTS_SMART_RENDER     |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    return PF_Err_NONE;
}

static PF_Err
ParamsSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *[], PF_LayerDef *)
{
    PF_Err      err = PF_Err_NONE;
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Erode",
        ER_ERODE_MIN, ER_ERODE_MAX, ER_ERODE_MIN, ER_ERODE_MAX,
        ER_ERODE_DFLT, PF_Precision_HUNDREDTHS, 0, 0, ER_ERODE_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Extend",
        ER_EXTEND_MIN, ER_EXTEND_MAX, ER_EXTEND_MIN, ER_EXTEND_MAX,
        ER_EXTEND_DFLT, PF_Precision_HUNDREDTHS, 0, 0, ER_EXTEND_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Detail",
        ER_DETAIL_MIN, ER_DETAIL_MAX, ER_DETAIL_MIN, ER_DETAIL_MAX,
        ER_DETAIL_DFLT, PF_Precision_HUNDREDTHS, 0, 0, ER_DETAIL_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Alpha Choke",
        ER_CHOKE_MIN, ER_CHOKE_MAX, ER_CHOKE_MIN, ER_CHOKE_MAX,
        ER_CHOKE_DFLT, PF_Precision_HUNDREDTHS, 0, 0, ER_CHOKE_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("View Mode",
        ER_VIEW_NUM - 1,        // number of items
        ER_VIEW_DFLT,           // default (1-based)
        ER_VIEW_ITEMS,
        ER_VIEW_DISK_ID);

    out_data->num_params = ER_NUM_PARAMS;
    return err;
}

// ---------------------------------------------------------------------------
// PreRender — inflate the checkout request by maxRadius on all sides. AE
// clamps the request to whatever layer data actually exists, so at the true
// layer boundary we simply get less than we asked for (handled by clamped
// addressing in SmartRender). The output result rect is intersected back to
// the originally-requested rect: our alpha extent never grows.
// ---------------------------------------------------------------------------
static PF_Err
PreRender(PF_InData *in_data, PF_OutData *out_data, PF_PreRenderExtra *extra)
{
    PF_Err err = PF_Err_NONE;

    ER_Params p; AEFX_CLR_STRUCT(p);
    ERR(ReadParams(in_data, &p));
    const A_long inflate = err ? 0 : ER_InflateRadius(p);

    // Stash the inflation amount for SmartRender. AE owns this pointer once
    // assigned and frees it via our PF_Cmd_SMART_PRE_RENDER cleanup contract.
    ER_PreRenderData *prd = static_cast<ER_PreRenderData*>(
        malloc(sizeof(ER_PreRenderData)));
    if (prd) {
        prd->inflate = inflate;
        extra->output->pre_render_data = prd;
        extra->output->delete_pre_render_data_func = ER_DeletePreRenderData;
    }

    PF_RenderRequest req = extra->input->output_request;

    // Expand the requested rect on all sides. AE intersects this against the
    // real layer bounds internally during checkout.
    if (inflate > 0) {
        req.rect.left   -= inflate;
        req.rect.top    -= inflate;
        req.rect.right  += inflate;
        req.rect.bottom += inflate;
    }
    // Internal-only premult handling: we ask AE for premultiplied pixels
    // (its native delivery) and hand them back premultiplied. No preserve
    // override — that is a user-facing footage-interpretation knob we do not
    // expose.

    PF_CheckoutResult in_result;
    ERR(extra->cb->checkout_layer(in_data->effect_ref,
        ER_INPUT, ER_INPUT, &req,
        in_data->current_time, in_data->time_step, in_data->time_scale,
        &in_result));

    UnionLRect(&in_result.result_rect,     &extra->output->result_rect);
    UnionLRect(&in_result.max_result_rect, &extra->output->max_result_rect);

    // Clamp the result rect back to the ORIGINAL request — output alpha
    // extent must not grow from the inflated read.
    const PF_LRect &out_req = extra->input->output_request.rect;
    if (extra->output->result_rect.left   < out_req.left)   extra->output->result_rect.left   = out_req.left;
    if (extra->output->result_rect.top    < out_req.top)    extra->output->result_rect.top    = out_req.top;
    if (extra->output->result_rect.right  > out_req.right)  extra->output->result_rect.right  = out_req.right;
    if (extra->output->result_rect.bottom > out_req.bottom) extra->output->result_rect.bottom = out_req.bottom;

    return err;
}

// ---------------------------------------------------------------------------
// SmartRender — Stage 1: unpack the inflated source, then render either the
// tiling debug pattern (ER_TILING_DEBUG) or passthrough. The real pipeline
// arrives in Stages 2-4.
// ---------------------------------------------------------------------------
static PF_Err
SmartRender(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra)
{
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;

    PF_WorldSuite2 *wsP = nullptr;
    ERR(AEFX_AcquireSuite(in_data, out_data, kPFWorldSuite, kPFWorldSuiteVersion2,
        "", reinterpret_cast<void**>(&wsP)));

    PF_EffectWorld *src_world = nullptr;
    PF_EffectWorld *out_world = nullptr;
    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, ER_INPUT, &src_world));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &out_world));

    if (err || !wsP || !src_world || !out_world) goto cleanup;

    {
        PF_PixelFormat fmt = PF_PixelFormat_INVALID;
        ERR(wsP->PF_GetPixelFormat(out_world, &fmt));
        if (err) goto cleanup;
        if (mm::PixelBytes(fmt) == 0) { err = PF_Err_BAD_CALLBACK_PARAM; goto cleanup; }

        ER_Params p; AEFX_CLR_STRUCT(p);
        ERR(ReadParams(in_data, &p));
        if (err) goto cleanup;

        const ER_PreRenderData *prd =
            static_cast<const ER_PreRenderData*>(extra->input->pre_render_data);
        const A_long inflate = prd ? prd->inflate : 0;

        const A_long srcW = src_world->width;
        const A_long srcH = src_world->height;
        const A_long outW = out_world->width;
        const A_long outH = out_world->height;

        // Output→source origin offset. AE gives the inflated source its own
        // extent; the output's top-left sits `inflate` pixels in from the
        // source's top-left, BUT only as far as the layer edge allowed the
        // inflation to actually happen. Recover the true offset from how much
        // bigger the source came back on each leading edge, capped at the
        // requested inflate and floored at 0. Using the size difference keeps
        // us correct when AE clamped the inflated request at the layer
        // boundary (source smaller than out+2*inflate on that side).
        A_long srcOX = inflate, srcOY = inflate;
        if (srcOX > (srcW - outW)) srcOX = srcW - outW;   // clamped at left/right layer edge
        if (srcOY > (srcH - outH)) srcOY = srcH - outH;
        if (srcOX < 0) srcOX = 0;
        if (srcOY < 0) srcOY = 0;

        // Unpack the whole inflated source into one ARGB float buffer.
        std::vector<float> srcBuf;
        try {
            srcBuf.resize((size_t)srcW * srcH * 4);
        } catch (const std::bad_alloc &) {
            err = PF_Err_OUT_OF_MEMORY;
            goto cleanup;
        }
        {
            ER_UnpackRefcon urc{ src_world, fmt, srcBuf.data(), srcW };
            mm::ForEachRow(in_data, srcH, &urc, ER_UnpackRow);
        }

#if ER_TILING_DEBUG
        // Stage 1 seam test (disabled by default now that Stage 2's real view
        // modes exist). Flip ER_TILING_DEBUG to 1 to re-run it — it overrides
        // all view modes.
        {
            ER_TileDebugRefcon rc;
            rc.src = srcBuf.data(); rc.srcW = srcW; rc.srcH = srcH;
            rc.srcOX = srcOX; rc.srcOY = srcOY;
            rc.radius = inflate;
            rc.out_world = out_world; rc.fmt = fmt; rc.outW = outW;
            mm::ForEachRow(in_data, outH, &rc, ER_TileDebugRow);
        }
#else
        // -----------------------------------------------------------------
        // Stage 2 pipeline (partial): core matte + the single driving
        // distance field, computed over the FULL inflated source so the
        // field is correct right to the output edge, then cropped to output
        // by the view callbacks. Result and Extended Colour are still
        // passthrough (their pipeline arrives in Stages 3-4); Core Matte and
        // Distance Field are real.
        // -----------------------------------------------------------------
        const size_t srcN = (size_t)srcW * srcH;

        // Extract the straight-alpha plane from the inflated source. (AE
        // hands premultiplied pixels; alpha itself is identical either way,
        // so no unpremult is needed just to threshold it.)
        std::vector<float>   alphaPlane;
        std::vector<uint8_t> core;
        std::vector<float>   dist;
        bool haveField = false;
        try {
            alphaPlane.resize(srcN);
            core.resize(srcN);
            dist.resize(srcN);
        } catch (const std::bad_alloc &) {
            err = PF_Err_OUT_OF_MEMORY;
            goto cleanup;
        }
        for (size_t i = 0; i < srcN; ++i) alphaPlane[i] = srcBuf[i * 4 + 0];

        // Core matte: threshold at 0.5, signed Erode. Then the driving field.
        mm::BuildCoreMatte(alphaPlane.data(), core.data(), (int)srcW, (int)srcH,
                           0.5f, (float)p.erodeF);
        haveField = mm::CoreDistanceField(core.data(), dist.data(), (int)srcW, (int)srcH);

        switch (p.viewL) {
            case ER_VIEW_CORE_MATTE: {
                ER_CoreViewRefcon rc;
                rc.core = core.data(); rc.srcW = srcW;
                rc.srcOX = srcOX; rc.srcOY = srcOY;
                rc.out_world = out_world; rc.fmt = fmt; rc.outW = outW;
                mm::ForEachRow(in_data, outH, &rc, ER_CoreViewRow);
                break;
            }
            case ER_VIEW_DISTANCE_FIELD: {
                if (!haveField) {
                    // Empty core → field is "infinitely far" everywhere;
                    // show black rather than garbage.
                    for (size_t i = 0; i < srcN; ++i) dist[i] = 1e20f;
                }
                // Normalize so the visualization spans 0..Extend (the band the
                // pipeline actually uses); fall back to a sane constant if
                // Extend is ~0 so the view isn't all-white.
                const float norm = (float)p.extendF >= 1.f ? (float)p.extendF : 64.f;
                ER_DistViewRefcon rc;
                rc.dist = dist.data(); rc.srcW = srcW;
                rc.srcOX = srcOX; rc.srcOY = srcOY;
                rc.invMaxDisplay = 1.f / norm;
                rc.out_world = out_world; rc.fmt = fmt; rc.outW = outW;
                mm::ForEachRow(in_data, outH, &rc, ER_DistViewRow);
                break;
            }
            case ER_VIEW_RESULT:
            case ER_VIEW_EXTENDED_COLOUR:
            default: {
                // Passthrough until Stages 3-4 wire up fill + composite.
                ER_PassRefcon rc;
                rc.src = srcBuf.data(); rc.srcW = srcW;
                rc.srcOX = srcOX; rc.srcOY = srcOY;
                rc.out_world = out_world; rc.fmt = fmt; rc.outW = outW;
                mm::ForEachRow(in_data, outH, &rc, ER_PassRow);
                break;
            }
        }
#endif
    }

cleanup:
    if (wsP) ERR2(AEFX_ReleaseSuite(in_data, out_data, kPFWorldSuite, kPFWorldSuiteVersion2, ""));
    return err;
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------
extern "C" DllExport PF_Err
PluginDataEntryFunction2(PF_PluginDataPtr inPtr, PF_PluginDataCB2 inPluginDataCallBackPtr,
    SPBasicSuite *inSPBasicSuitePtr, const char *inHostName, const char *inHostVersion)
{
    PF_Err result = PF_Err_NONE;
    result = PF_REGISTER_EFFECT_EXT2(inPtr, inPluginDataCallBackPtr,
        "Edge Repair", "MM Edge Repair", "MM",
        AE_RESERVED_INFO, "EffectMain", "");
    return result;
}

PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
    PF_ParamDef *params[], PF_LayerDef *output, void *extra)
{
    PF_Err err = PF_Err_NONE;
    try {
        switch (cmd) {
            case PF_Cmd_ABOUT:            err = About(in_data, out_data, params, output);              break;
            case PF_Cmd_GLOBAL_SETUP:     err = GlobalSetup(in_data, out_data, params, output);        break;
            case PF_Cmd_PARAMS_SETUP:     err = ParamsSetup(in_data, out_data, params, output);        break;
            case PF_Cmd_SMART_PRE_RENDER: err = PreRender(in_data, out_data, (PF_PreRenderExtra*)extra); break;
            case PF_Cmd_SMART_RENDER:     err = SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra); break;
        }
    } catch (PF_Err &e) { err = e; }
    return err;
}
