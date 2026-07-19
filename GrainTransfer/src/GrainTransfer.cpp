/*  GrainTransfer.cpp — Grain Transfer (match name "MM Grain Transfer").

    A fast grain-transfer primitive: takes the grain character of a source
    plate and lays ghost-free, seam-free grain onto the target layer. The
    cost model inverts AE Match Grain's: analyze the source ONCE (average
    power spectrum of its high-frequency residual + luminance→sigma response
    curve), precompute a bank of phase-randomized (RPN) seamless tiles ONCE,
    and keep the per-frame render trivial (stamp + modulate + add). The DSP
    core lives in src/core (AE-free, gate-tested headlessly); this file is
    only the AE plumbing.

    ANALYZE-ONCE under MFR: the analysis result is held in AE's Compute
    Cache (AEGP_ComputeCacheSuite1), keyed on the source layer param's
    PF_GetCurrentState hash + Seed + resolved apply-space. Render threads
    share one computed value; a still/plate source (the intended use)
    analyzes exactly once, and revisited frames always hit the cache. The
    source is sampled at the effect's current time — use a still or a
    locked-off plate as Grain Source; a moving source re-analyzes on first
    visit to each frame.

    COLOUR MANAGEMENT (first-class, per the brief): grain math runs in a
    log/density-like apply space — both extraction and application. Auto
    mode queries the project working space (AEGP_ColorSettingsSuite6): OCIO
    mode → parse the OCIO working colorspace name; ICC mode → approximate
    gamma of the working-space profile (≈1 ⇒ scene-linear). Every query
    failure falls back to Display, which is also the correct answer for the
    CM-bypassed/Preserve-RGB workflow; the Working Space popup overrides
    Auto for exactly that case. Float-native everywhere — no clamps between
    unpack and pack; 32bpc HDR values survive.

    MFR-safe: no mutable global/static state. Global data holds only the
    AEGP plugin id + registration flags (written once at GLOBAL_SETUP).
    Per-render state travels in pre_render_data (AE-owned, freed by our
    handler) and on the SmartRender stack.
*/

#define NOMINMAX
#include "GrainTransfer.h"
#include "common/GT_Common.h"
#include "core/GT_Colour.h"
#include "core/GT_Grain.h"
#include "core/GT_Random.h"
#include "core/GT_Response.h"
#include "core/GT_Stamp.h"

#include "AE_ComputeCacheSuite.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <new>
#include <string>
#include <vector>

// Fixed analysis constants — deliberately not user-facing (paring knife).
// TILE candidates: analysis needs at least one tile-sized patch, so small
// grain samples fall back to smaller (still seamless) tiles.
static const int   GT_TILE_CANDIDATES[3] = { 256, 128, 64 };
static const int   GT_TILE_COUNT = 8;      // tiles in the bank
static const float GT_BLUR_SIGMA = 2.0f;   // high-pass split radius (px)

// ---------------------------------------------------------------------------
// Global data: AEGP plugin id (for AEGP suite calls from this effect) and
// compute-cache class registration. Written once at GLOBAL_SETUP, read-only
// afterwards.
// ---------------------------------------------------------------------------
typedef struct {
    AEGP_PluginID aegp_id;
    A_Boolean     has_aegp;
    A_Boolean     cache_registered;
} GT_GlobalData;

static const char *GT_CACHE_CLASS = "mm.graintransfer.analysis.v1";

// ---------------------------------------------------------------------------
// Compute-cache plumbing. Options = key ingredients + (borrowed) input
// planes; value = the full analysis product. The planes pointers are only
// valid during the SmartRender call that provides them — the cache key never
// depends on them, and compute() is only invoked from that same call.
// ---------------------------------------------------------------------------
typedef struct {
    A_long srcState[4];   // PF_State of the Grain Source layer param
    A_long seed;
    A_long encodeMode;    // resolved gt::Encode
} GT_AnalysisKey;

typedef struct {
    GT_AnalysisKey key;
    const float   *planes[3];   // apply-space source planes (borrowed)
    A_long         W, H;
} GT_AnalysisOptions;

struct GT_AnalysisValue {
    gt::GrainSpectrum spec;
    gt::ResponseCurve resp;
    gt::TileBank      bank;
};

extern "C" {

static A_Err GT_CacheGenerateKey(AEGP_CCComputeOptionsRefconP optionsP,
                                 AEGP_CCComputeKeyP out_keyP)
{
    const GT_AnalysisOptions *opt = static_cast<const GT_AnalysisOptions*>(optionsP);
    uint64_t s = 0x4754414E414C5953ull;   // arbitrary class salt
    uint64_t h1 = 0, h2 = 0;
    const A_long *k = opt->key.srcState;
    h1 = gt::HashSeed((uint64_t)(uint32_t)k[0] << 32 | (uint32_t)k[1],
                      (uint64_t)(uint32_t)k[2] << 32 | (uint32_t)k[3], s);
    h2 = gt::HashSeed(h1, (uint64_t)(uint32_t)opt->key.seed,
                      (uint64_t)(uint32_t)opt->key.encodeMode);
    out_keyP->bytes[0] = (A_long)(uint32_t)(h1 >> 32);
    out_keyP->bytes[1] = (A_long)(uint32_t)h1;
    out_keyP->bytes[2] = (A_long)(uint32_t)(h2 >> 32);
    out_keyP->bytes[3] = (A_long)(uint32_t)h2;
    return A_Err_NONE;
}

static A_Err GT_CacheCompute(AEGP_CCComputeOptionsRefconP optionsP,
                             AEGP_CCComputeValueRefconP *out_valuePP)
{
    const GT_AnalysisOptions *opt = static_cast<const GT_AnalysisOptions*>(optionsP);
    GT_AnalysisValue *val = new (std::nothrow) GT_AnalysisValue;
    if (!val) return A_Err_ALLOC;

    const int W = opt->W, H = opt->H;

    // Residuals once; spectrum + response share them.
    std::vector<float> res[3];
    const float *resP[3];
    bool ok = true;
    try {
        for (int c = 0; c < 3; ++c) {
            res[c].resize((size_t)W * H);
            gt::HighPassResidual(opt->planes[c], res[c].data(), W, H, GT_BLUR_SIGMA);
            resP[c] = res[c].data();
        }
    } catch (const std::bad_alloc &) {
        delete val;
        return A_Err_ALLOC;
    }

    ok = false;
    for (int cand : GT_TILE_CANDIDATES) {
        if (gt::MeasureGrainSpectrumRes(resP, W, H, cand, val->spec)) { ok = true; break; }
    }
    if (ok) {
        gt::MeasureResponse(opt->planes, resP, W, H, val->resp);
        gt::SynthesizeTileBank(val->spec, GT_TILE_COUNT,
                               (uint64_t)(uint32_t)opt->key.seed, val->bank);
        ok = val->resp.valid && val->bank.valid();
    }

    // An unanalyzable source (too small / degenerate) still caches — as an
    // explicitly invalid value — so we don't retry the analysis every frame.
    if (!ok) { val->spec = gt::GrainSpectrum(); val->bank = gt::TileBank(); }

    *out_valuePP = val;
    return A_Err_NONE;
}

static size_t GT_CacheApproxSize(AEGP_CCComputeValueRefconP valueP)
{
    const GT_AnalysisValue *val = static_cast<const GT_AnalysisValue*>(valueP);
    size_t sz = sizeof(GT_AnalysisValue);
    sz += val->bank.tiles.size() * sizeof(float);
    for (int c = 0; c < 3; ++c) sz += val->spec.power[c].size() * sizeof(float);
    return sz;
}

static void GT_CacheDeleteValue(AEGP_CCComputeValueRefconP valueP)
{
    delete static_cast<GT_AnalysisValue*>(valueP);
}

} // extern "C"

static const AEGP_ComputeCacheCallbacks GT_CACHE_CALLBACKS = {
    GT_CacheGenerateKey,
    GT_CacheCompute,
    GT_CacheApproxSize,
    GT_CacheDeleteValue
};

// ---------------------------------------------------------------------------
// Param read.
// ---------------------------------------------------------------------------
static PF_Err
ReadParams(PF_InData *in_data, GT_Params *p)
{
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    PF_ParamDef def;

    #define GT_GET_FLOAT(IDX, FLD) \
        AEFX_CLR_STRUCT(def); \
        ERR(PF_CHECKOUT_PARAM(in_data, IDX, in_data->current_time, \
            in_data->time_step, in_data->time_scale, &def)); \
        p->FLD = def.u.fs_d.value; \
        ERR2(PF_CHECKIN_PARAM(in_data, &def));

    GT_GET_FLOAT(GT_AMOUNT,   amountF)
    GT_GET_FLOAT(GT_SIZE,     sizeF)
    GT_GET_FLOAT(GT_RESPONSE, responseF)
    #undef GT_GET_FLOAT

    AEFX_CLR_STRUCT(def);
    ERR(PF_CHECKOUT_PARAM(in_data, GT_SEED, in_data->current_time,
        in_data->time_step, in_data->time_scale, &def));
    p->seedL = def.u.sd.value;
    ERR2(PF_CHECKIN_PARAM(in_data, &def));

    AEFX_CLR_STRUCT(def);
    ERR(PF_CHECKOUT_PARAM(in_data, GT_WORKSPACE, in_data->current_time,
        in_data->time_step, in_data->time_scale, &def));
    p->workspaceL = def.u.pd.value;
    ERR2(PF_CHECKIN_PARAM(in_data, &def));

    AEFX_CLR_STRUCT(def);
    ERR(PF_CHECKOUT_PARAM(in_data, GT_PREVIEW, in_data->current_time,
        in_data->time_step, in_data->time_scale, &def));
    p->previewB = (PF_Boolean)def.u.bd.value;
    ERR2(PF_CHECKIN_PARAM(in_data, &def));

    return err;
}

// ---------------------------------------------------------------------------
// Working-space auto-query. Returns the resolved apply-space encode.
//
// Order of trust: OCIO working colorspace name (OCIO-managed projects) →
// ICC working profile approximate gamma (classic-managed projects) →
// Display on any failure, which is also the correct behaviour for the
// CM-off / Preserve-RGB workflow (no reliable signal exists — exactly why
// the manual override is in the param set).
//
// Name parsing (OCIO names are config-defined free text): log-like names
// (cct/cc/log) are already perceptual — treat as Display (no re-encode);
// otherwise linear-like names (lin/aces/cg/scene) get the log encode.
// ---------------------------------------------------------------------------
static gt::Encode
QueryAutoEncode(PF_InData *in_data, const GT_GlobalData *g)
{
    gt::Encode enc = gt::Encode::Display;
    if (!g || !g->has_aegp || !in_data->pica_basicP) return enc;

    SPBasicSuite *bs = in_data->pica_basicP;
    AEGP_ColorSettingsSuite6 *css = nullptr;
    if (bs->AcquireSuite(kAEGPColorSettingsSuite, kAEGPColorSettingsSuiteVersion6,
                         (const void**)&css) != kSPNoError || !css)
        return enc;

    try {
        AEGP_SuiteHandler suites(in_data->pica_basicP);

        A_Boolean isOCIO = FALSE;
        if (css->AEGP_IsOCIOColorManagementUsed(g->aegp_id, &isOCIO) == A_Err_NONE
            && isOCIO) {
            AEGP_MemHandle nameH = nullptr;
            if (css->AEGPD_GetOCIOWorkingColorSpace(g->aegp_id, &nameH) == A_Err_NONE
                && nameH) {
                void *ptr = nullptr;
                std::string name;
                if (suites.MemorySuite1()->AEGP_LockMemHandle(nameH, &ptr) == A_Err_NONE
                    && ptr) {
                    name = static_cast<const char*>(ptr);
                    suites.MemorySuite1()->AEGP_UnlockMemHandle(nameH);
                }
                suites.MemorySuite1()->AEGP_FreeMemHandle(nameH);

                for (char &ch : name) ch = (char)std::tolower((unsigned char)ch);
                const bool isLog = name.find("cct") != std::string::npos ||
                                   name.find("log") != std::string::npos ||
                                   name.find("acescc") != std::string::npos;
                const bool isLin = name.find("lin") != std::string::npos ||
                                   name.find("aces") != std::string::npos ||
                                   name.find("cg") != std::string::npos ||
                                   name.find("scene") != std::string::npos;
                if (!isLog && isLin) enc = gt::Encode::LogFromLinear;
            }
        } else {
            // Classic ICC management: working-space profile gamma.
            AEGP_LayerH layerH = nullptr;
            AEGP_CompH  compH = nullptr;
            if (suites.PFInterfaceSuite1()->AEGP_GetEffectLayer(
                    in_data->effect_ref, &layerH) == A_Err_NONE && layerH)
                suites.LayerSuite9()->AEGP_GetLayerParentComp(layerH, &compH);

            AEGP_ColorProfileP profP = nullptr;
            if (compH &&
                css->AEGP_GetNewWorkingSpaceColorProfile(g->aegp_id, compH, &profP)
                    == A_Err_NONE && profP) {
                A_FpShort gamma = 0.f;
                if (css->AEGP_GetColorProfileApproximateGamma(profP, &gamma)
                        == A_Err_NONE && gamma > 0.7f && gamma < 1.3f)
                    enc = gt::Encode::LogFromLinear;
                css->AEGP_DisposeColorProfile(profP);
            }
        }
    } catch (...) {
        enc = gt::Encode::Display;   // any suite hiccup → safe default
    }

    bs->ReleaseSuite(kAEGPColorSettingsSuite, kAEGPColorSettingsSuiteVersion6);
    return enc;
}

// ---------------------------------------------------------------------------
// Pre-render data: everything SmartRender needs that PreRender resolved.
// ---------------------------------------------------------------------------
typedef struct {
    GT_Params  p;
    A_long     frame;        // current_time / time_step
    A_long     encodeMode;   // resolved gt::Encode
    PF_State   srcState;     // Grain Source param state (cache key)
    A_Boolean  hasSource;
} GT_PreRenderData;

extern "C" void
GT_DeletePreRenderData(void *pre_render_dataPV)
{
    if (pre_render_dataPV) free(pre_render_dataPV);
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------
static PF_Err
About(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *[], PF_LayerDef *)
{
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    suites.ANSICallbacksSuite1()->sprintf(out_data->return_msg,
        "%s v%d.%d\r%s",
        STR(StrID_Name), MAJOR_VERSION, MINOR_VERSION, STR(StrID_Description));
    return PF_Err_NONE;
}

static PF_Err
GlobalSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *[], PF_LayerDef *)
{
    PF_Err err = PF_Err_NONE;

    out_data->my_version = PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION,
                                      STAGE_VERSION, BUILD_VERSION);
    // Must match resources/GrainTransferPiPL.r literals exactly (0x02000444 /
    // 0x08001400) or AE rejects with a version/outflags mismatch.
    out_data->out_flags  = PF_OutFlag_DEEP_COLOR_AWARE |
                           PF_OutFlag_PIX_INDEPENDENT  |
                           PF_OutFlag_USE_OUTPUT_EXTENT |
                           PF_OutFlag_NON_PARAM_VARY;    // grain varies per frame
    out_data->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE          |
                           PF_OutFlag2_SUPPORTS_SMART_RENDER      |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING;

    AEGP_SuiteHandler suites(in_data->pica_basicP);
    PF_Handle globH = suites.HandleSuite1()->host_new_handle(sizeof(GT_GlobalData));
    if (!globH) return PF_Err_OUT_OF_MEMORY;

    GT_GlobalData *g = static_cast<GT_GlobalData*>(
        suites.HandleSuite1()->host_lock_handle(globH));
    if (g) {
        g->aegp_id = 0;
        g->has_aegp = FALSE;
        g->cache_registered = FALSE;

        // AEGP plugin id — lets this effect call AEGP suites (colour
        // settings). Failure is non-fatal: Auto falls back to Display.
        try {
            if (suites.UtilitySuite6()->AEGP_RegisterWithAEGP(
                    nullptr, STR(StrID_Name), &g->aegp_id) == A_Err_NONE)
                g->has_aegp = TRUE;
        } catch (...) { g->has_aegp = FALSE; }

        // Compute-cache class for analyze-once. Failure is non-fatal:
        // SmartRender falls back to computing inline per frame.
        {
            SPBasicSuite *bs = in_data->pica_basicP;
            AEGP_ComputeCacheSuite1 *ccs = nullptr;
            if (bs && bs->AcquireSuite(kAEGPComputeCacheSuite,
                    kAEGPComputeCacheSuiteVersion1,
                    (const void**)&ccs) == kSPNoError && ccs) {
                if (ccs->AEGP_ClassRegister(GT_CACHE_CLASS, &GT_CACHE_CALLBACKS)
                        == A_Err_NONE)
                    g->cache_registered = TRUE;
                bs->ReleaseSuite(kAEGPComputeCacheSuite, kAEGPComputeCacheSuiteVersion1);
            }
        }

        suites.HandleSuite1()->host_unlock_handle(globH);
    }
    out_data->global_data = globH;
    return err;
}

static PF_Err
GlobalSetdown(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *[], PF_LayerDef *)
{
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    if (in_data->global_data) {
        GT_GlobalData *g = static_cast<GT_GlobalData*>(
            suites.HandleSuite1()->host_lock_handle(in_data->global_data));
        if (g && g->cache_registered) {
            SPBasicSuite *bs = in_data->pica_basicP;
            AEGP_ComputeCacheSuite1 *ccs = nullptr;
            if (bs && bs->AcquireSuite(kAEGPComputeCacheSuite,
                    kAEGPComputeCacheSuiteVersion1,
                    (const void**)&ccs) == kSPNoError && ccs) {
                ccs->AEGP_ClassUnregister(GT_CACHE_CLASS);
                bs->ReleaseSuite(kAEGPComputeCacheSuite, kAEGPComputeCacheSuiteVersion1);
            }
        }
        if (g) suites.HandleSuite1()->host_unlock_handle(in_data->global_data);
        suites.HandleSuite1()->host_dispose_handle(in_data->global_data);
    }
    return PF_Err_NONE;
}

static PF_Err
ParamsSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *[], PF_LayerDef *)
{
    PF_Err      err = PF_Err_NONE;
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER("Grain Source", PF_LayerDefault_NONE, GT_SOURCE_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Amount",
        GT_AMOUNT_MIN, GT_AMOUNT_MAX, GT_AMOUNT_MIN, GT_AMOUNT_MAX,
        GT_AMOUNT_DFLT, PF_Precision_HUNDREDTHS, 0, 0, GT_AMOUNT_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Size",
        GT_SIZE_MIN, GT_SIZE_MAX, GT_SIZE_MIN, GT_SIZE_MAX,
        GT_SIZE_DFLT, PF_Precision_HUNDREDTHS, 0, 0, GT_SIZE_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Response",
        GT_RESPONSE_MIN, GT_RESPONSE_MAX, GT_RESPONSE_MIN, GT_RESPONSE_MAX,
        GT_RESPONSE_DFLT, PF_Precision_HUNDREDTHS, 0, 0, GT_RESPONSE_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Seed",
        GT_SEED_MIN, GT_SEED_MAX, GT_SEED_MIN, GT_SEED_MAX,
        GT_SEED_DFLT, GT_SEED_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Working Space",
        GT_WS_NUM - 1,
        GT_WS_DFLT,
        GT_WS_ITEMS,
        GT_WORKSPACE_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Preview Grain Only", FALSE, 0, GT_PREVIEW_DISK_ID);

    out_data->num_params = GT_NUM_PARAMS;
    return err;
}

// ---------------------------------------------------------------------------
// PreRender. Checkout the target with the unmodified request (this effect
// reads only the pixel it writes — no inflation), checkout the Grain Source
// at its full extent for analysis, resolve the apply space, and stash
// everything SmartRender needs. Frame index + resolved encode are mixed
// into the render GUID so AE's frame cache stays honest (grain re-random-
// izes per frame; Auto can change with project settings, not just params).
// ---------------------------------------------------------------------------
static PF_Err
PreRender(PF_InData *in_data, PF_OutData *out_data, PF_PreRenderExtra *extra)
{
    PF_Err err = PF_Err_NONE;

    GT_Params p; AEFX_CLR_STRUCT(p);
    ERR(ReadParams(in_data, &p));

    GT_GlobalData *g = nullptr;
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    if (in_data->global_data)
        g = static_cast<GT_GlobalData*>(*(in_data->global_data));

    // Resolve the apply space once per render.
    gt::Encode enc = gt::Encode::Display;
    switch (p.workspaceL) {
        case GT_WS_AUTO:         enc = QueryAutoEncode(in_data, g); break;
        case GT_WS_DISPLAY:      enc = gt::Encode::Display;        break;
        case GT_WS_SCENE_LINEAR:
        case GT_WS_ACES:         enc = gt::Encode::LogFromLinear;  break;
        default:                 break;
    }

    const A_long frame = in_data->time_step != 0
        ? in_data->current_time / in_data->time_step : 0;

    GT_PreRenderData *prd = static_cast<GT_PreRenderData*>(
        malloc(sizeof(GT_PreRenderData)));
    if (prd) {
        memset(prd, 0, sizeof(*prd));
        prd->p = p;
        prd->frame = frame;
        prd->encodeMode = (A_long)enc;
        extra->output->pre_render_data = prd;
        extra->output->delete_pre_render_data_func = GT_DeletePreRenderData;
    }

    // Mix per-frame + environment-derived state into the render GUID.
    {
        A_long mix[2] = { frame, (A_long)enc };
        extra->cb->GuidMixInPtr(in_data->effect_ref, sizeof(mix), mix);
    }

    // Target layer: unmodified request.
    PF_CheckoutResult in_result;
    ERR(extra->cb->checkout_layer(in_data->effect_ref,
        GT_INPUT, GT_INPUT, &extra->input->output_request,
        in_data->current_time, in_data->time_step, in_data->time_scale,
        &in_result));

    UnionLRect(&in_result.result_rect,     &extra->output->result_rect);
    UnionLRect(&in_result.max_result_rect, &extra->output->max_result_rect);

    // Grain Source: full extent (AE clamps the request to the layer's real
    // bounds). A disconnected layer param errors or returns empty — both
    // mean "no source", never a render failure.
    if (!err && prd) {
        PF_RenderRequest req = extra->input->output_request;
        req.rect.left = -1000000; req.rect.top = -1000000;
        req.rect.right = 1000000; req.rect.bottom = 1000000;

        PF_CheckoutResult src_result;
        AEFX_CLR_STRUCT(src_result);
        PF_Err serr = extra->cb->checkout_layer(in_data->effect_ref,
            GT_SOURCE, GT_SOURCE, &req,
            in_data->current_time, in_data->time_step, in_data->time_scale,
            &src_result);
        if (serr == PF_Err_NONE &&
            src_result.result_rect.right > src_result.result_rect.left &&
            src_result.result_rect.bottom > src_result.result_rect.top) {
            prd->hasSource = TRUE;

            // Source param state — the analyze-once cache key. Scoped to
            // the current checkout time: still sources hash identically on
            // every frame.
            A_Time start; start.value = in_data->current_time; start.scale = in_data->time_scale;
            A_Time dur;   dur.value = in_data->time_step;      dur.scale = in_data->time_scale;
            try {
                suites.ParamUtilsSuite3()->PF_GetCurrentState(
                    in_data->effect_ref, GT_SOURCE, &start, &dur, &prd->srcState);
            } catch (...) { /* state stays zero — cache degrades, render doesn't */ }
        }
    }

    return err;
}

// ---------------------------------------------------------------------------
// Row workers.
// ---------------------------------------------------------------------------

// Byte-identical passthrough (also the no-source path).
struct GT_CopyRefcon {
    const PF_EffectWorld *src;
    PF_EffectWorld       *out;
    size_t                rowBytes;   // W * PixelBytes(fmt)
};

// Unpack + apply-space-encode the source into three planes.
struct GT_SrcPlanesRefcon {
    const PF_EffectWorld *src;
    PF_PixelFormat        fmt;
    float                *planes[3];
    A_long                W;
    gt::Encode            enc;
};

// Unpack target row → stamp → pack to output row.
struct GT_StampRefcon {
    const PF_EffectWorld    *src;
    PF_EffectWorld          *out;
    PF_PixelFormat           fmt;
    A_long                   W;
    const gt::TileBank      *bank;
    const gt::ResponseCurve *resp;
    gt::StampParams          sp;
};

extern "C" {

static PF_Err
GT_CopyRow(void *refconPV, A_long, A_long y, A_long)
{
    const GT_CopyRefcon *rc = static_cast<const GT_CopyRefcon*>(refconPV);
    const char *s = reinterpret_cast<const char*>(rc->src->data) + (size_t)y * rc->src->rowbytes;
    char *d = reinterpret_cast<char*>(rc->out->data) + (size_t)y * rc->out->rowbytes;
    memcpy(d, s, rc->rowBytes);
    return PF_Err_NONE;
}

static PF_Err
GT_SrcPlanesRow(void *refconPV, A_long, A_long y, A_long)
{
    const GT_SrcPlanesRefcon *rc = static_cast<const GT_SrcPlanesRefcon*>(refconPV);
    const A_long W = rc->W;

    thread_local std::vector<float> rowv;
    if ((A_long)rowv.size() < W * 4) rowv.resize((size_t)W * 4);

    const void *srcRow = reinterpret_cast<const char*>(rc->src->data) + (size_t)y * rc->src->rowbytes;
    gtc::UnpackRow(srcRow, rowv.data(), W, rc->fmt);

    const float *px = rowv.data();
    const size_t base = (size_t)y * W;
    for (A_long x = 0; x < W; ++x, px += 4)
        for (int c = 0; c < 3; ++c)
            rc->planes[c][base + x] = gt::EncodeChannel(px[1 + c], rc->enc);
    return PF_Err_NONE;
}

static PF_Err
GT_StampRow(void *refconPV, A_long, A_long y, A_long)
{
    const GT_StampRefcon *rc = static_cast<const GT_StampRefcon*>(refconPV);
    const A_long W = rc->W;

    thread_local std::vector<float> rowv;
    if ((A_long)rowv.size() < W * 4) rowv.resize((size_t)W * 4);

    const void *srcRow = reinterpret_cast<const char*>(rc->src->data) + (size_t)y * rc->src->rowbytes;
    gtc::UnpackRow(srcRow, rowv.data(), W, rc->fmt);

    gt::StampRow(rowv.data(), (int)W, (int)y, *rc->bank, *rc->resp, rc->sp);

    void *outRow = reinterpret_cast<char*>(rc->out->data) + (size_t)y * rc->out->rowbytes;
    gtc::PackRow(rowv.data(), outRow, W, rc->fmt);
    return PF_Err_NONE;
}

} // extern "C"

// ---------------------------------------------------------------------------
// SmartRender.
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
    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, GT_INPUT, &src_world));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &out_world));

    if (err || !wsP || !src_world || !out_world) goto cleanup;

    {
        PF_PixelFormat fmt = PF_PixelFormat_INVALID;
        ERR(wsP->PF_GetPixelFormat(out_world, &fmt));
        if (err) goto cleanup;
        if (gtc::PixelBytes(fmt) == 0) { err = PF_Err_BAD_CALLBACK_PARAM; goto cleanup; }

        const GT_PreRenderData *prd =
            static_cast<const GT_PreRenderData*>(extra->input->pre_render_data);

        const A_long W = src_world->width  < out_world->width  ? src_world->width  : out_world->width;
        const A_long H = src_world->height < out_world->height ? src_world->height : out_world->height;

        GT_GlobalData *g = nullptr;
        if (in_data->global_data)
            g = static_cast<GT_GlobalData*>(*(in_data->global_data));

        // ------------------------------------------------------------------
        // Analysis product. THE COST MODEL LIVES HERE: probe the cache by
        // key first — a hit means the source frame is never rendered, never
        // unpacked, and the per-frame cost is just the stamp below. Only a
        // miss pays for checkout + unpack + analyze, once per source state.
        // Inline compute is the fallback when the cache suite is missing.
        // ------------------------------------------------------------------
        const GT_AnalysisValue *analysis = nullptr;
        GT_AnalysisValue *inlineVal = nullptr;
        AEGP_CCCheckoutReceiptP receipt = nullptr;
        AEGP_ComputeCacheSuite1 *ccs = nullptr;

        if (prd && prd->hasSource) {
            GT_AnalysisOptions opt;
            memset(&opt, 0, sizeof(opt));
            memcpy(opt.key.srcState, &prd->srcState, sizeof(opt.key.srcState));
            opt.key.seed = prd->p.seedL;
            opt.key.encodeMode = prd->encodeMode;

            SPBasicSuite *bs = in_data->pica_basicP;
            const bool haveCache = g && g->cache_registered && bs &&
                bs->AcquireSuite(kAEGPComputeCacheSuite,
                    kAEGPComputeCacheSuiteVersion1,
                    (const void**)&ccs) == kSPNoError && ccs;

            // Fast path: cached analysis, key-only lookup.
            if (haveCache &&
                ccs->AEGP_CheckoutCached(GT_CACHE_CLASS, &opt, &receipt) == A_Err_NONE
                && receipt) {
                AEGP_CCComputeValueRefconP vp = nullptr;
                if (ccs->AEGP_GetReceiptComputeValue(receipt, &vp) == A_Err_NONE)
                    analysis = static_cast<const GT_AnalysisValue*>(vp);
            }

            // Miss: render + unpack the source and analyze (into the cache
            // when available, inline otherwise).
            if (!analysis) {
                PF_EffectWorld *grain_world = nullptr;
                PF_Err serr = extra->cb->checkout_layer_pixels(
                    in_data->effect_ref, GT_SOURCE, &grain_world);
                if (serr == PF_Err_NONE && grain_world &&
                    grain_world->width >= GT_TILE_CANDIDATES[2] &&
                    grain_world->height >= GT_TILE_CANDIDATES[2]) {

                    const A_long sw = grain_world->width, sh = grain_world->height;
                    std::vector<float> planes[3];
                    bool planesOK = true;
                    try {
                        for (int c = 0; c < 3; ++c) {
                            planes[c].resize((size_t)sw * sh);
                            opt.planes[c] = planes[c].data();
                        }
                    } catch (const std::bad_alloc &) { planesOK = false; }

                    if (planesOK) {
                        GT_SrcPlanesRefcon rc;
                        rc.src = grain_world; rc.fmt = fmt; rc.W = sw;
                        rc.enc = (gt::Encode)prd->encodeMode;
                        for (int c = 0; c < 3; ++c) rc.planes[c] = planes[c].data();
                        gtc::ForEachRow(in_data, sh, &rc, GT_SrcPlanesRow);

                        opt.W = sw; opt.H = sh;

                        if (haveCache &&
                            ccs->AEGP_ComputeIfNeededAndCheckout(
                                GT_CACHE_CLASS, &opt, true, &receipt) == A_Err_NONE
                            && receipt) {
                            AEGP_CCComputeValueRefconP vp = nullptr;
                            if (ccs->AEGP_GetReceiptComputeValue(receipt, &vp)
                                    == A_Err_NONE)
                                analysis = static_cast<const GT_AnalysisValue*>(vp);
                        }
                        if (!analysis) {
                            AEGP_CCComputeValueRefconP vp = nullptr;
                            if (GT_CacheCompute(&opt, &vp) == A_Err_NONE) {
                                inlineVal = static_cast<GT_AnalysisValue*>(vp);
                                analysis = inlineVal;
                            }
                        }
                    }
                }
                ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, GT_SOURCE));
            }
        }

        // ------------------------------------------------------------------
        // Render: stamp when we have a usable analysis, else passthrough.
        // ------------------------------------------------------------------
        const bool canStamp = analysis && analysis->bank.valid() &&
                              analysis->resp.valid && prd;

        if (canStamp) {
            GT_StampRefcon rc;
            rc.src = src_world; rc.out = out_world; rc.fmt = fmt; rc.W = W;
            rc.bank = &analysis->bank;
            rc.resp = &analysis->resp;
            rc.sp = gt::MakeFrameStamp((uint64_t)(uint32_t)prd->p.seedL,
                                       prd->frame,
                                       analysis->bank.count,
                                       analysis->bank.n);
            rc.sp.amount = (float)(prd->p.amountF / 100.0);
            rc.sp.trim = (float)(prd->p.responseF / 100.0);
            rc.sp.invScale = (float)(100.0 / (prd->p.sizeF < GT_SIZE_MIN
                                              ? GT_SIZE_DFLT : prd->p.sizeF));
            rc.sp.encode = (gt::Encode)prd->encodeMode;
            rc.sp.previewGrainOnly = prd->p.previewB ? true : false;
            gtc::ForEachRow(in_data, H, &rc, GT_StampRow);
        } else {
            GT_CopyRefcon rc;
            rc.src = src_world; rc.out = out_world;
            rc.rowBytes = (size_t)W * gtc::PixelBytes(fmt);
            gtc::ForEachRow(in_data, H, &rc, GT_CopyRow);
        }

        // Receipt check-in AFTER the bank/curve were last read.
        if (receipt && ccs)
            ccs->AEGP_CheckinComputeReceipt(receipt);
        if (ccs && in_data->pica_basicP)
            in_data->pica_basicP->ReleaseSuite(kAEGPComputeCacheSuite,
                                              kAEGPComputeCacheSuiteVersion1);
        delete inlineVal;
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
        "Grain Transfer", "MM Grain Transfer", "MM",
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
            case PF_Cmd_GLOBAL_SETDOWN:   err = GlobalSetdown(in_data, out_data, params, output);      break;
            case PF_Cmd_PARAMS_SETUP:     err = ParamsSetup(in_data, out_data, params, output);        break;
            case PF_Cmd_SMART_PRE_RENDER: err = PreRender(in_data, out_data, (PF_PreRenderExtra*)extra); break;
            case PF_Cmd_SMART_RENDER:     err = SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra); break;
        }
    } catch (PF_Err &e) { err = e; }
    return err;
}
