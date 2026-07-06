/*  EdgeRepair.cpp — Stage 0 skeleton.

    Edge Repair extends trusted interior color across contaminated matte
    edges (keying halos, motion-blur rims) along a single per-frame Euclidean
    distance field: core matte → EDT → pull–push fill → distance-weighted
    blend with frequency-separated texture preserve → optional alpha choke.
    Pure stream operator: RGBA in, RGBA out, RGB modified, alpha untouched
    unless the user explicitly chokes it. See the build brief in the repo
    docs for the locked seven-parameter model.

    THIS STAGE: pure passthrough. No params, no processing — SmartFX
    pre-render/render plumbing only, output byte-identical to input at
    8/16/32bpc. Each later stage layers one subsystem onto this skeleton.

    Key divergence from ModernEdgeBlur (the suite's reference plugin), from
    Stage 1 onward: ModernEdgeBlur deliberately does NO input-rect inflation;
    this plugin MUST inflate its checkout by the full influence radius
    (Erode + Extend + |Alpha Choke| + texture blur) or tile seams appear that
    differ between RAM preview and final render. Stage 0 checks out the
    request rect unmodified — inflation lands with the params in Stage 1.

    MFR-safe: no mutable global/static state anywhere in this plugin.
*/

#define NOMINMAX
#include "EdgeRepair.h"
#include "common/MM_Common.h"

#include <cstring>

// ---------------------------------------------------------------------------
// Passthrough row copy — raw bytes, so output is bit-identical to input at
// every depth (no unpack/repack rounding anywhere in the path).
// ---------------------------------------------------------------------------

struct ER_CopyRefcon {
    const PF_EffectWorld *src;
    PF_EffectWorld       *dst;
    size_t                bytesPerRow;   // width * PixelBytes(fmt), NOT rowbytes
};

extern "C" {

static PF_Err
ER_CopyRow(void *refconPV, A_long /*thread_indexL*/, A_long y, A_long /*iterationsL*/)
{
    const ER_CopyRefcon *rc = static_cast<const ER_CopyRefcon*>(refconPV);
    const void *srcRow = reinterpret_cast<const char*>(rc->src->data) + (size_t)y * rc->src->rowbytes;
    void       *dstRow = reinterpret_cast<char*>(rc->dst->data)       + (size_t)y * rc->dst->rowbytes;
    std::memcpy(dstRow, srcRow, rc->bytesPerRow);
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
    // 0x08001400) or AE rejects with a silent/misleading mismatch error.
    // PIX_INDEPENDENT + USE_OUTPUT_EXTENT follow ModernEdgeBlur's precedent;
    // the rest are the brief's required set.
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
    // Stage 0: input layer only. The locked seven params (disk IDs 101-107)
    // are registered in Stage 1.
    out_data->num_params = ER_NUM_PARAMS;
    return PF_Err_NONE;
}

// ---------------------------------------------------------------------------
// PreRender — Stage 0: check out exactly the requested rect, unmodified.
// preserve_rgb_of_zero_alpha is left as delivered so passthrough returns
// held RGB untouched (byte-identity gate). Stage 1 replaces this with the
// inflated checkout.
// ---------------------------------------------------------------------------
static PF_Err
PreRender(PF_InData *in_data, PF_OutData *out_data, PF_PreRenderExtra *extra)
{
    PF_Err err = PF_Err_NONE;

    PF_RenderRequest req = extra->input->output_request;

    PF_CheckoutResult in_result;
    ERR(extra->cb->checkout_layer(in_data->effect_ref,
        ER_INPUT, ER_INPUT, &req,
        in_data->current_time, in_data->time_step, in_data->time_scale,
        &in_result));

    UnionLRect(&in_result.result_rect,     &extra->output->result_rect);
    UnionLRect(&in_result.max_result_rect, &extra->output->max_result_rect);

    // Clamp the result rect to what was actually requested — the output's
    // alpha extent never grows (same discipline as ModernEdgeBlur).
    const PF_LRect &out_req = extra->input->output_request.rect;
    if (extra->output->result_rect.left   < out_req.left)   extra->output->result_rect.left   = out_req.left;
    if (extra->output->result_rect.top    < out_req.top)    extra->output->result_rect.top    = out_req.top;
    if (extra->output->result_rect.right  > out_req.right)  extra->output->result_rect.right  = out_req.right;
    if (extra->output->result_rect.bottom > out_req.bottom) extra->output->result_rect.bottom = out_req.bottom;

    return err;
}

// ---------------------------------------------------------------------------
// SmartRender — Stage 0: raw row copy, input → output.
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

        const size_t bpp = mm::PixelBytes(fmt);
        if (bpp == 0) { err = PF_Err_BAD_CALLBACK_PARAM; goto cleanup; }

        // Stage 0 checkout is uninflated, so src and output rects coincide;
        // min() is pure defensiveness, not a tiling strategy.
        const A_long W = src_world->width  < out_world->width  ? src_world->width  : out_world->width;
        const A_long H = src_world->height < out_world->height ? src_world->height : out_world->height;

        ER_CopyRefcon rc{ src_world, out_world, (size_t)W * bpp };
        mm::ForEachRow(in_data, H, &rc, ER_CopyRow);
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
