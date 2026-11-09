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

#ifdef _WIN32
#include "gpu/gt_gpu_host.h"
#endif

// Build provenance. Identifies the .aex AE actually LOADED, which neither the
// version quad nor anything derived from the source can do — a build that ran
// while AE held the file lock reports the right version from the wrong binary.
#include "../../common/mm_build_id.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <new>
#include <string>
#include <vector>

// ===========================================================================
// TEMPORARY DIAGNOSTIC LOGGING (GT_DIAG_LOG).
// Appends lines to  %TEMP%\GrainTransfer_diag.log .
// Purpose: answer the in-AE validation checklist items 3/4/5 with hard
// numbers — resolved colour encode, cache HIT/MISS per frame, and the grain
// sigmas actually measured (plus a multi-radius probe showing how much grain
// energy the fixed 2px high-pass discards). Set GT_DIAG_LOG to 0 — or delete
// this block and the three guarded call sites — to strip it entirely. Not for
// release: the multi-radius probe does two extra full-image blurs per cache
// miss.
// ===========================================================================
#define GT_DIAG_LOG 1

#if GT_DIAG_LOG
#ifdef _WIN32
#include <windows.h>
#endif
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

// The temp directory, which is the only thing in this file that has ever cared
// which OS it is running on. Windows has no $TMPDIR; macOS has no
// GetTempPathA, and its TMPDIR is a per-user path under /var/folders that
// already ends in a separator (as does GetTempPathA's result, and as does the
// /tmp/ fallback for the case where the variable is unset).
static std::string GT_LogDir()
{
#ifdef _WIN32
    char dir[MAX_PATH];
    const DWORD dn = GetTempPathA(MAX_PATH, dir);
    if (dn == 0 || dn > MAX_PATH - 24) return std::string();
    return std::string(dir);
#else
    const char *t = std::getenv("TMPDIR");
    return std::string((t && *t) ? t : "/tmp/");
#endif
}

static void GT_Log(const char *fmt, ...)
{
    const std::string dir = GT_LogDir();
    if (dir.empty()) return;
    const std::string path = dir + "GrainTransfer_diag.log";

    // Format the whole line (with newline) into one buffer, then write once —
    // keeps lines intact when AE renders frames on multiple threads.
    char line[1024];
    va_list ap; va_start(ap, fmt);
    int len = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (len < 0) return;
    if (len > (int)sizeof(line) - 2) len = (int)sizeof(line) - 2;
    line[len]     = '\n';
    line[len + 1] = '\0';

    FILE *f = nullptr;
#ifdef _WIN32
    if (fopen_s(&f, path.c_str(), "a") != 0 || !f) return;
#else
    f = std::fopen(path.c_str(), "a");
    if (!f) return;
#endif
    fwrite(line, 1, (size_t)len + 1, f);
    fclose(f);
}

static int GT_DiagDepthBits(PF_PixelFormat fmt)
{
    switch (fmt) {
        case PF_PixelFormat_ARGB128: return 32;
        case PF_PixelFormat_ARGB64:  return 16;
        case PF_PixelFormat_ARGB32:  return 8;
        default:                     return 0;
    }
}
#endif // GT_DIAG_LOG

// Fixed analysis constants — deliberately not user-facing (paring knife).
// TILE candidates: analysis needs at least one tile-sized patch, so small
// grain samples fall back to smaller (still seamless) tiles.
static const int   GT_TILE_CANDIDATES[3] = { 256, 128, 64 };
static const int   GT_TILE_COUNT = 8;      // tiles in the bank

// High-pass split radius. Grain clump size scales with plate resolution, so a
// single fixed radius under-captures coarser grain on high-res plates (2px was
// tuned near 2K; a 4K plate needs ~4px, else >half the grain energy is thrown
// away as "content" — the finer/weaker-than-source result). Scale linearly off
// the long edge from the reference below. Floored at the base (small plates
// keep the original radius) and capped so content structure can't leak in.
// NOTE: interim resolution heuristic — the real fix measures grain correlation
// length from the residual autocorrelation and sets the radius from that.
static const float GT_BLUR_SIGMA_BASE = 2.0f;      // radius at the reference edge
static const float GT_BLUR_REF_EDGE   = 2048.0f;   // long edge the base was tuned at
static const float GT_BLUR_SIGMA_MAX  = 12.0f;     // cap (content-leak guard)

static float GT_BlurSigmaFor(int W, int H)
{
    const int longEdge = W > H ? W : H;
    float s = GT_BLUR_SIGMA_BASE * (float)longEdge / GT_BLUR_REF_EDGE;
    if (s < GT_BLUR_SIGMA_BASE) s = GT_BLUR_SIGMA_BASE;
    if (s > GT_BLUR_SIGMA_MAX)  s = GT_BLUR_SIGMA_MAX;
    return s;
}

// The Grain Source is analyzed at a FIXED frame (comp time 0), never the
// current frame. A moving source is thus snapshotted once and its grain
// character reused for the whole shot — the per-frame stamp still animates the
// grain, so the output moves without re-analyzing. This makes the analyze-once
// cache hit on every frame whether or not the source moves. (If comp time 0
// has no source content the layer just reads as absent there — pick a frame
// that covers t=0, or this becomes a reference-frame control later.)
static const A_long GT_SRC_REF_TIME = 0;

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
    A_long pixelFormat;   // render depth (PF_PixelFormat): AE conforms the
                          // source to project depth, so 8/16/32bpc deliver
                          // clamped/quantized-vs-full-range pixels → a
                          // genuinely different analysis. Must key on it, or a
                          // depth switch reuses the wrong-depth cached result.
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

// The render depth the cache key names. It must be the PROJECT's depth, NOT
// whatever PF_GetPixelFormat reports for the output world, because on the GPU
// path every world is PF_PixelFormat_GPU_BGRA128 regardless of project depth.
// Keying on the world format there would mint a key the CPU path can never
// have populated, so the GPU would miss the cache on every frame, decline
// forever, and the port would look like it silently did nothing.
//
// `bitdepth` is carried on BOTH PF_PreRenderInput and PF_SmartRenderInput, so
// PreRender can build the identical key without an output world — which is
// what makes the cache-gated GPU offer below possible at all. On the CPU path
// this maps 1:1 onto the format the world reports (AE conforms worlds to the
// project depth, and gtc::PixelBytes accepts exactly these three).
static PF_PixelFormat GT_FormatForBitdepth(short bpc)
{
    switch (bpc) {
        case 32: return PF_PixelFormat_ARGB128;
        case 16: return PF_PixelFormat_ARGB64;
        default: return PF_PixelFormat_ARGB32;
    }
}

// The two 64-bit halves of the analysis key. Factored out so the compute
// cache's key and the GPU host's tile-bank identity are derived from ONE
// expression — if they drifted, the device would keep serving a stale bank
// under a key the cache considers current.
// Format version of the analysis PRODUCT. The key ingredients below describe
// the analysis INPUT, so a change to how the input is turned into a tile bank
// is invisible to them -- a cached bank from the previous algorithm would go on
// being served under a key that still looks current. Bump this whenever the
// meaning of GrainSpectrum or the synthesis changes, so every existing entry
// misses once and re-analyzes. No migration: the cost is one slow frame per
// source, and the value is that a stale bank can never outlive its algorithm.
//   1 -> scalar crossCorr phase-blend
//   2 -> per-pair crossPair + common-factor synthesis
//   3 -> response curve binned on BLURRED luma + identifiability guard
#define GT_ANALYSIS_FORMAT 3

static void GT_AnalysisHashes(const GT_AnalysisKey &key, uint64_t &h1, uint64_t &h2)
{
    const uint64_t s = 0x4754414E414C5953ull;   // arbitrary class salt
    const A_long *k = key.srcState;
    h1 = gt::HashSeed((uint64_t)(uint32_t)k[0] << 32 | (uint32_t)k[1],
                      (uint64_t)(uint32_t)k[2] << 32 | (uint32_t)k[3], s);
    h2 = gt::HashSeed(h1, (uint64_t)(uint32_t)key.seed,
                      (uint64_t)(uint32_t)key.encodeMode);
    h2 = gt::HashSeed(h2, (uint64_t)(uint32_t)key.pixelFormat, s);
    h2 = gt::HashSeed(h2, (uint64_t)GT_ANALYSIS_FORMAT, s);
}

// Identity of the analysis PRODUCT, for the device-side tile-bank cache. Zero
// is reserved for "no bank", so a hash that lands there is nudged.
static uint64_t GT_AnalysisIdentity(const GT_AnalysisKey &key)
{
    uint64_t h1 = 0, h2 = 0;
    GT_AnalysisHashes(key, h1, h2);
    return h2 ? h2 : 1ull;
}

extern "C" {

static A_Err GT_CacheGenerateKey(AEGP_CCComputeOptionsRefconP optionsP,
                                 AEGP_CCComputeKeyP out_keyP)
{
    const GT_AnalysisOptions *opt = static_cast<const GT_AnalysisOptions*>(optionsP);
    uint64_t h1 = 0, h2 = 0;
    GT_AnalysisHashes(opt->key, h1, h2);
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

#if GT_DIAG_LOG
    const auto gtDiagT0 = std::chrono::steady_clock::now();   // pure DSP timer
#endif

    const int W = opt->W, H = opt->H;
    const float blurSigma = GT_BlurSigmaFor(W, H);

    // Residuals once; spectrum + response share them.
    std::vector<float> res[3];
    const float *resP[3];
    bool ok = true;
    try {
        for (int c = 0; c < 3; ++c) {
            res[c].resize((size_t)W * H);
            gt::HighPassResidual(opt->planes[c], res[c].data(), W, H, blurSigma);
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

#if GT_DIAG_LOG
    {
        // analyzeMs = pure DSP cost of this analysis (residual blur + spectrum
        // + response + tile synth), NOT the AE source render/unpack that
        // precedes it (timed separately in SmartRender as [timing]). Splitting
        // the two tells us whether the first-frame cost is ours to optimize.
        const double analyzeMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - gtDiagT0).count();

        GT_Log("[analyze] %dx%d depth=%dbpc enc=%d ok=%d blur=%.2f tile=%d patches=%d crossCorr=%.3f analyzeMs=%.0f | "
               "specSigma RGB=%.6f/%.6f/%.6f | flatSigma RGB=%.6f/%.6f/%.6f",
               (int)W, (int)H, GT_DiagDepthBits((PF_PixelFormat)opt->key.pixelFormat),
               (int)opt->key.encodeMode, (int)ok, blurSigma,
               val->spec.n, val->spec.patchesUsed, val->spec.crossCorr, analyzeMs,
               val->spec.sigma[0], val->spec.sigma[1], val->spec.sigma[2],
               val->resp.flatSigma[0], val->resp.flatSigma[1], val->resp.flatSigma[2]);
        // Cross-channel structure, the whole story on one line: what each pair
        // measured, whether the common-factor model could factor it, and how
        // much of the target the channel spectra actually permitted. A
        // `model=scalar` here means the pairs were inconsistent (typically
        // content leaking into the residual on a coloured plate) and synthesis
        // fell back to the scalar phase blend.
        {
            const gt::CrossChannelModel cm = gt::SolveCrossChannel(val->spec);
            if (cm.useCommonFactor)
                GT_Log("[analyze] xchan pairs RG/GB/RB=%.3f/%.3f/%.3f mean=%.3f | "
                       "model=common-factor a=%.3f/%.3f/%.3f overlap=%.3f achieved=%.3f",
                       val->spec.crossPair[0], val->spec.crossPair[1],
                       val->spec.crossPair[2], val->spec.crossCorr,
                       cm.a[0], cm.a[1], cm.a[2], cm.overlap, cm.achieved);
            else
                GT_Log("[analyze] xchan pairs RG/GB/RB=%.3f/%.3f/%.3f mean=%.3f | "
                       "model=scalar (unfactorable)",
                       val->spec.crossPair[0], val->spec.crossPair[1],
                       val->spec.crossPair[2], val->spec.crossCorr);
        }
        GT_Log("[analyze] band[G] L:sigma = %.3f:%.6f %.3f:%.6f %.3f:%.6f "
               "%.3f:%.6f %.3f:%.6f",
               val->resp.lumaMid[0], val->resp.sigma[1][0],
               val->resp.lumaMid[1], val->resp.sigma[1][1],
               val->resp.lumaMid[2], val->resp.sigma[1][2],
               val->resp.lumaMid[3], val->resp.sigma[1][3],
               val->resp.lumaMid[4], val->resp.sigma[1][4]);
    }
#endif

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

    // Build provenance: one line per load into the suite build log. This ships
    // and is deliberately NOT behind GT_DIAG_LOG - on a bug report the first
    // question is which binary ran. See common/mm_build_id.h.
    mmbuild::Announce("GrainTransfer", MAJOR_VERSION, MINOR_VERSION,
                      BUG_VERSION, BUILD_VERSION);

#if GT_DIAG_LOG
    // The same line into the diag log, so that file is self-identifying when
    // read alone and the harness can tie its uncapped [render] lines - the only
    // reliable render count in the suite - to a specific binary.
    {
        char vbuf[32], bline[1200];
        std::snprintf(vbuf, sizeof(vbuf), "%d.%d.%d.%d", MAJOR_VERSION,
                      MINOR_VERSION, BUG_VERSION, BUILD_VERSION);
        GT_Log("%s", mmbuild::FormatLine(bline, sizeof(bline),
                                         "GrainTransfer", vbuf));
    }
#endif

    out_data->my_version = PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION,
                                      STAGE_VERSION, BUILD_VERSION);
    // Must match resources/GrainTransferPiPL.r literals EXACTLY (0x02000444 /
    // 0x2A201400) or AE rejects the plugin at load with a version/outflags
    // mismatch (25::16). Recompute both sides when either changes:
    //   out_flags  = 0x02000000 | 0x400 | 0x40 | 0x4               = 0x02000444
    //   out_flags2 = 0x400 | 0x1000 | 0x00200000 | 0x02000000
    //              | 0x08000000 | 0x20000000                       = 0x2A201400
    // The GPU pair (0x22000000) is Windows-only on BOTH sides — the PiPL
    // branches on AE_OS_WIN — so macOS is 0x08201400 (AE error 25::16 if not).
    out_data->out_flags  = PF_OutFlag_DEEP_COLOR_AWARE |
                           PF_OutFlag_PIX_INDEPENDENT  |
                           PF_OutFlag_USE_OUTPUT_EXTENT |
                           PF_OutFlag_NON_PARAM_VARY;    // grain varies per frame
    out_data->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE          |
                           PF_OutFlag2_SUPPORTS_SMART_RENDER      |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING |
                           PF_OutFlag2_I_MIX_GUID_DEPENDENCIES;    // PreRender calls GuidMixInPtr

#ifdef _WIN32
    // GPU support. BOTH flags are required and they do different jobs:
    // SUPPORTS_GPU_RENDER_F32 is the actual declaration, and
    // SUPPORTS_DIRECTX_RENDERING does NOT force the effect onto DirectX —
    // Adobe's own samples set it alongside CUDA hosts, and §8 hypothesis 1
    // records that adding/removing it changed nothing on the CUDA path.
    // ModernEdgeBlur, the suite's other CUDA-only plugin, declares the same
    // pair. If AE ever did drive DirectX here, GT_CudaDeviceSetup declines
    // (PF_Err_NONE, gpu_data null) and the CPU renders the frame.
    //
    // Declaring it here is necessary but NOT sufficient: GPU_DEVICE_SETUP must
    // RE-AFFIRM out_flags2 as a plain assignment, or AE offers what_gpu = NONE
    // at every PreRender forever (AE_GPU_Effects.md §2.2 — the mistake that
    // cost the suite seven experiments). See gpu/gt_gpu_cuda.cpp.
    out_data->out_flags2 |= PF_OutFlag2_SUPPORTS_GPU_RENDER_F32 |
                            PF_OutFlag2_SUPPORTS_DIRECTX_RENDERING;
#endif

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
// at its full extent AND at a fixed reference frame (GT_SRC_REF_TIME) for
// analysis, resolve the apply space, and stash everything SmartRender needs.
// Sampling the source at a fixed frame is what makes analyze-once hold for a
// moving source. Frame index + resolved encode are mixed into the render GUID
// so AE's frame cache stays honest (grain re-randomizes per frame; Auto can
// change with project settings, not just params).
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
        g = reinterpret_cast<GT_GlobalData*>(*(in_data->global_data));

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

        // Sample the source at the FIXED reference time, not current_time, so
        // the analysis (and its cache key) is identical on every output frame.
        PF_CheckoutResult src_result;
        AEFX_CLR_STRUCT(src_result);
        PF_Err serr = extra->cb->checkout_layer(in_data->effect_ref,
            GT_SOURCE, GT_SOURCE, &req,
            GT_SRC_REF_TIME, in_data->time_step, in_data->time_scale,
            &src_result);
        if (serr == PF_Err_NONE &&
            src_result.result_rect.right > src_result.result_rect.left &&
            src_result.result_rect.bottom > src_result.result_rect.top) {
            prd->hasSource = TRUE;

            // Source param state — the analyze-once cache key. Scoped to the
            // SAME fixed reference time, so every frame hashes identically and
            // hits the cache (a moving source no longer re-analyzes per frame).
            A_Time start; start.value = GT_SRC_REF_TIME;   start.scale = in_data->time_scale;
            A_Time dur;   dur.value = in_data->time_step;   dur.scale = in_data->time_scale;
            try {
                suites.ParamUtilsSuite3()->PF_GetCurrentState(
                    in_data->effect_ref, GT_SOURCE, &start, &dur, &prd->srcState);
            } catch (...) { /* state stays zero — cache degrades, render doesn't */ }
        }
    }

#ifdef _WIN32
    // -----------------------------------------------------------------------
    // GPU render offer (../../docs/AE_GPU_Effects.md §2.3).
    //
    // This is the ONE plugin in the suite whose PreRender withholds the flag on
    // a condition, and the distinction matters: it withholds on a FACT IT KNOWS
    // — the analysis for this exact source state is not in the compute cache —
    // never on ignorance of the device. It does NOT consult what_gpu and does
    // NOT consult gpu_data. Gating on those is a DEADLOCK, not caution: AE
    // offers what_gpu = NONE with gpu_data = null at every PreRender until it
    // has SEEN this flag at least once, so a gate conditioned on a known device
    // never raises it and is therefore never offered a device. MM_Qualify and
    // MM_FreqSep both shipped exactly that bug and neither reached the GPU
    // until 2026-08-04.
    //
    // WHY A CACHE GATE IS NEEDED AT ALL. In SMART_RENDER_GPU every world is
    // device memory, INCLUDING the Grain Source — so the CPU FFT analysis
    // cannot read it, and §2.4 is explicit that there is no falling back
    // mid-render. The CPU path's "no analysis yet -> pass the frame through"
    // branch would therefore become permanent on the GPU: every frame passes
    // through, the analysis is never computed, grain never appears. Gating on a
    // cache HIT inverts that. Miss -> the CPU renders this frame, which pays
    // for the analysis and populates the cache; hit -> every later frame of
    // that source state goes GPU. That is exactly the plugin's own cost model.
    //
    // The probe is KEY-ONLY (AEGP_CheckoutCached, never ComputeIfNeeded): it
    // must not do work here, only ask whether the work is already done.
    {
        bool cached = false;
        if (!err && prd && prd->hasSource) {
            GT_AnalysisOptions opt;
            memset(&opt, 0, sizeof(opt));
            memcpy(opt.key.srcState, &prd->srcState, sizeof(opt.key.srcState));
            opt.key.seed = prd->p.seedL;
            opt.key.encodeMode = prd->encodeMode;
            // The output world does not exist yet, so the depth comes from the
            // render context — the whole reason GT_FormatForBitdepth exists.
            opt.key.pixelFormat =
                (A_long)GT_FormatForBitdepth(extra->input->bitdepth);

            GT_GlobalData *gg = g;
            SPBasicSuite *bs = in_data->pica_basicP;
            AEGP_ComputeCacheSuite1 *ccs = nullptr;
            if (gg && gg->cache_registered && bs &&
                bs->AcquireSuite(kAEGPComputeCacheSuite,
                    kAEGPComputeCacheSuiteVersion1,
                    (const void**)&ccs) == kSPNoError && ccs) {
                AEGP_CCCheckoutReceiptP receipt = nullptr;
                if (ccs->AEGP_CheckoutCached(GT_CACHE_CLASS, &opt, &receipt)
                        == A_Err_NONE && receipt) {
                    cached = true;
                    ccs->AEGP_CheckinComputeReceipt(receipt);
                }
                bs->ReleaseSuite(kAEGPComputeCacheSuite,
                                 kAEGPComputeCacheSuiteVersion1);
            }
        }

        if (cached)
            extra->output->flags |= PF_RenderOutputFlag_GPU_RENDER_POSSIBLE;

        const PF_LRect &rq = extra->input->output_request.rect;
        gtgpu::GT_GPULogPreRenderDecision(
            extra->input->what_gpu, extra->input->gpu_data, cached, cached,
            (int)(rq.right - rq.left), (int)(rq.bottom - rq.top));
    }
#endif

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
            g = reinterpret_cast<GT_GlobalData*>(*(in_data->global_data));

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
#if GT_DIAG_LOG
        bool diagCacheHit = false;
#endif

        // Render downsample. Grain analysis needs FULL-RESOLUTION source: at
        // reduced res AE aliases the grain away, so analyzing downsampled
        // pixels measures content, not grain (huge sigma, tiny tile — the
        // "high intensity + pattern" seen at low preview res). So only COMPUTE
        // at full res; at reduced res reuse the cached full-res analysis if
        // present, else pass through. The reused full-res tile is rescaled by
        // the downsample factor so grain keeps its correct apparent size.
        const double dsx = in_data->downsample_x.den
            ? (double)in_data->downsample_x.num / (double)in_data->downsample_x.den : 1.0;
        const double dsy = in_data->downsample_y.den
            ? (double)in_data->downsample_y.num / (double)in_data->downsample_y.den : 1.0;
        const bool isFullRes = dsx <= 1.0001 && dsy <= 1.0001;

        if (prd && prd->hasSource) {
            GT_AnalysisOptions opt;
            memset(&opt, 0, sizeof(opt));
            memcpy(opt.key.srcState, &prd->srcState, sizeof(opt.key.srcState));
            opt.key.seed = prd->p.seedL;
            opt.key.encodeMode = prd->encodeMode;
            // Re-analyze on a depth switch. Derived from the render context's
            // bitdepth, NOT from `fmt`, so PreRender's key-only probe and this
            // one agree — on the GPU path `fmt` would be GPU_BGRA128 and could
            // never match. On this path the two are the same value.
            opt.key.pixelFormat =
                (A_long)GT_FormatForBitdepth(extra->input->bitdepth);

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
#if GT_DIAG_LOG
                diagCacheHit = (analysis != nullptr);
#endif
            }

            // Miss: render + unpack the source and analyze — but ONLY at full
            // resolution (analyzing downsampled source measures content, not
            // grain). At reduced res with no cached full-res analysis we fall
            // through to passthrough rather than bake a wrong-scale one.
            if (!analysis && isFullRes) {
                PF_EffectWorld *grain_world = nullptr;
#if GT_DIAG_LOG
                const auto tCk0 = std::chrono::steady_clock::now();
#endif
                PF_Err serr = extra->cb->checkout_layer_pixels(
                    in_data->effect_ref, GT_SOURCE, &grain_world);
#if GT_DIAG_LOG
                const double checkoutMs = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - tCk0).count();
#endif
                if (serr == PF_Err_NONE && grain_world &&
                    grain_world->width >= GT_TILE_CANDIDATES[2] &&
                    grain_world->height >= GT_TILE_CANDIDATES[2]) {

#if GT_DIAG_LOG
                    {
                        // Source is unpacked with the OUTPUT format (fmt). If AE
                        // delivers the source at a different depth, that read is
                        // wrong — log the source's own format to catch it.
                        PF_PixelFormat srcFmt = PF_PixelFormat_INVALID;
                        wsP->PF_GetPixelFormat(grain_world, &srcFmt);
                        GT_Log("[srccheck] sourceDepth=%dbpc outputDepth=%dbpc %s",
                               GT_DiagDepthBits(srcFmt), GT_DiagDepthBits(fmt),
                               srcFmt == fmt ? "(match)" : "(MISMATCH!)");
                    }
#endif

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

#if GT_DIAG_LOG
                        {
                            // Split the first-frame cost: AE rendering the
                            // source frame (checkout) vs our unpack. The DSP
                            // analysis is timed separately as [analyze] analyzeMs.
                            const double unpackMs =
                                std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - tCk0).count()
                                - checkoutMs;
                            GT_Log("[timing] %ldx%ld sourceCheckout=%.0fms unpack=%.0fms",
                                   (long)sw, (long)sh, checkoutMs, unpackMs);
                        }
#endif

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

#if GT_DIAG_LOG
        GT_Log("[render] id=%08x frame=%d cache=%s depth=%dbpc ds=%.2f fullRes=%d enc=%d %dx%d canStamp=%d hasSource=%d",
               mmbuild::ShortId(),
               prd ? (int)prd->frame : -1,
               diagCacheHit ? "HIT" : "MISS",
               GT_DiagDepthBits(fmt), dsx, (int)isFullRes,
               prd ? (int)prd->encodeMode : -1,
               (int)W, (int)H, (int)canStamp,
               (prd && prd->hasSource) ? 1 : 0);
#endif

#ifdef _WIN32
        // Same moment, into the GPU log: this frame is being rendered by the
        // CPU. Whether that is correct depends on fullRes and the cache.
        // `analysis`, not diagCacheHit — the latter only exists under
        // GT_DIAG_LOG, and this line must not depend on that switch.
        gtgpu::GT_GPULogSmartRenderCPU((int)W, (int)H, (int)isFullRes,
                                       analysis ? 1 : 0);
#endif

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
            // Tiles are always full-resolution (analysis is full-res-gated).
            // At reduced render res, scale tile sampling by the downsample
            // factor so grain keeps its full-res apparent size (dsx = 1 at
            // full res → no-op).
            rc.sp.invScale = (float)((100.0 / (prd->p.sizeF < GT_SIZE_MIN
                                               ? GT_SIZE_DFLT : prd->p.sizeF)) * dsx);
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

#ifdef _WIN32
// ---------------------------------------------------------------------------
// SMART_RENDER_GPU — a SEPARATE command from SMART_RENDER (§2.4), not a mode
// of it. Every world here is device memory.
//
// This runs ONLY when PreRender raised GPU_RENDER_POSSIBLE, which it does only
// on a cache hit, so the analysis is guaranteed present and the source layer is
// never touched: no checkout, no unpack, no FFT. That is the entire reason the
// gate exists — see PreRender.
//
// The tile bank is handed over as a host pointer plus an identity; the CUDA
// host uploads it once per analysis and reuses it for every later frame.
// ---------------------------------------------------------------------------
static PF_Err
SmartRenderGPU(PF_InData *in_data, PF_OutData *out_data,
               PF_SmartRenderExtra *extra)
{
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;

    PF_EffectWorld *src_world = nullptr;
    PF_EffectWorld *out_world = nullptr;
    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, GT_INPUT, &src_world));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &out_world));
    if (err || !src_world || !out_world) return err ? err : PF_Err_BAD_CALLBACK_PARAM;

    {
        const GT_PreRenderData *prd =
            static_cast<const GT_PreRenderData*>(extra->input->pre_render_data);

        GT_GlobalData *g = nullptr;
        if (in_data->global_data)
            g = static_cast<GT_GlobalData*>(*(in_data->global_data));

        gtgpu::GT_GPUJob job;
        job.W = (int)(src_world->width  < out_world->width  ? src_world->width  : out_world->width);
        job.H = (int)(src_world->height < out_world->height ? src_world->height : out_world->height);

        AEGP_CCCheckoutReceiptP receipt = nullptr;
        AEGP_ComputeCacheSuite1 *ccs = nullptr;
        const GT_AnalysisValue *analysis = nullptr;

        if (prd && prd->hasSource) {
            GT_AnalysisOptions opt;
            memset(&opt, 0, sizeof(opt));
            memcpy(opt.key.srcState, &prd->srcState, sizeof(opt.key.srcState));
            opt.key.seed = prd->p.seedL;
            opt.key.encodeMode = prd->encodeMode;
            opt.key.pixelFormat =
                (A_long)GT_FormatForBitdepth(extra->input->bitdepth);

            SPBasicSuite *bs = in_data->pica_basicP;
            if (g && g->cache_registered && bs &&
                bs->AcquireSuite(kAEGPComputeCacheSuite,
                    kAEGPComputeCacheSuiteVersion1,
                    (const void**)&ccs) == kSPNoError && ccs) {
                // Key-only, exactly as PreRender probed. A miss here would mean
                // the entry was evicted between PreRender and now; the job then
                // stays passthrough rather than attempting an analysis that
                // cannot be done from device memory.
                if (ccs->AEGP_CheckoutCached(GT_CACHE_CLASS, &opt, &receipt)
                        == A_Err_NONE && receipt) {
                    AEGP_CCComputeValueRefconP vp = nullptr;
                    if (ccs->AEGP_GetReceiptComputeValue(receipt, &vp) == A_Err_NONE)
                        analysis = static_cast<const GT_AnalysisValue*>(vp);
                }
            }

            if (analysis && analysis->bank.valid() && analysis->resp.valid) {
                const double dsx = in_data->downsample_x.den
                    ? (double)in_data->downsample_x.num / (double)in_data->downsample_x.den : 1.0;

                gt::StampParams sp = gt::MakeFrameStamp(
                    (uint64_t)(uint32_t)prd->p.seedL, prd->frame,
                    analysis->bank.count, analysis->bank.n);
                sp.amount = (float)(prd->p.amountF / 100.0);
                sp.trim   = (float)(prd->p.responseF / 100.0);
                sp.invScale = (float)((100.0 / (prd->p.sizeF < GT_SIZE_MIN
                                                ? GT_SIZE_DFLT : prd->p.sizeF)) * dsx);
                sp.encode = (gt::Encode)prd->encodeMode;
                sp.previewGrainOnly = prd->p.previewB ? true : false;

                job.bankKey    = GT_AnalysisIdentity(opt.key);
                job.bankData   = analysis->bank.tiles.data();
                job.bankFloats = analysis->bank.tiles.size();
                job.tileN      = analysis->bank.n;

                job.tileIndex = sp.tileIndex;
                job.offX = sp.offX;   job.offY = sp.offY;
                job.flipX = sp.flipX ? 1 : 0;
                job.flipY = sp.flipY ? 1 : 0;
                // Same predicate gt::StampRow uses, so the two paths pick the
                // same sampling branch for the same Size.
                job.exact = std::fabs(sp.invScale - 1.f) < 1e-6f ? 1 : 0;
                job.encode = (int)sp.encode;
                job.preview = sp.previewGrainOnly ? 1 : 0;
                job.invScale = sp.invScale;
                job.amount = sp.amount;
                job.trim = sp.trim;
                job.previewBase = GT_PreviewBase((int)sp.encode);
                gt::PackResponse(analysis->resp, job.resp);

                job.passthrough = false;
            }
        }

        ERR(gtgpu::GT_CudaRender(in_data, out_data, extra->input->gpu_data,
                                 extra->input->device_index, job,
                                 src_world, out_world));

        // Check in only AFTER the render — the bank pointer above is borrowed
        // from the receipt and the upload reads it.
        if (receipt && ccs)
            ccs->AEGP_CheckinComputeReceipt(receipt);
        if (ccs && in_data->pica_basicP)
            in_data->pica_basicP->ReleaseSuite(kAEGPComputeCacheSuite,
                                              kAEGPComputeCacheSuiteVersion1);
    }

    (void)err2;
    return err;
}
#endif /* _WIN32 */

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
#ifdef _WIN32
            // SMART_RENDER_GPU is a SEPARATE command, not a flag on
            // SMART_RENDER — handling only the latter is a silent CPU render
            // (AE_GPU_Effects.md §2.4).
            case PF_Cmd_GPU_DEVICE_SETUP:
                err = gtgpu::GT_CudaDeviceSetup(in_data, out_data, (PF_GPUDeviceSetupExtra*)extra); break;
            case PF_Cmd_GPU_DEVICE_SETDOWN:
                err = gtgpu::GT_CudaDeviceSetdown(in_data, out_data, (PF_GPUDeviceSetdownExtra*)extra); break;
            case PF_Cmd_SMART_RENDER_GPU:
                err = SmartRenderGPU(in_data, out_data, (PF_SmartRenderExtra*)extra); break;
#endif
        }
    } catch (PF_Err &e) { err = e; }
    return err;
}
