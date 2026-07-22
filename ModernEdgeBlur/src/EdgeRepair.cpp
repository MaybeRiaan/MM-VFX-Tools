/*  EdgeRepair.cpp — Stage 4: full Result composite.

    Edge Repair extends trusted interior color across contaminated matte
    edges (keying halos, motion-blur rims) along a single per-frame Euclidean
    distance field: core matte → EDT → pull–push fill → distance-weighted
    blend with frequency-separated Detail (texture preserve) → optional alpha
    choke. Pure stream operator: RGBA in, RGBA out, RGB modified, alpha
    untouched unless the user explicitly chokes it. Five locked params
    (Erode/Extend/Detail/Alpha Choke/View Mode, disk IDs 101-105); premult is
    internal-only, no knob.

    THIS STAGE: all four view modes are real. Result runs the composite —
    in the band 0 < d <= Extend the plate's low frequencies are replaced by
    the pull–push extension weighted by distance, the plate's high
    frequencies (texture/grain, from an alpha-weighted Gaussian split) ride
    on top scaled by Detail, and Alpha Choke remaps alpha along a field
    anchored at the ALPHA'S OWN edge (per the 2026-07-09 decision — extra
    EDT, Erode-independent, profile-preserving). All per-pixel formulas live
    in src/common/MM_Composite.h — that header is the CPU/GPU lockstep
    contract.

    ALPHA CONVENTION (fixed at the Stage 4 gate — the "dark edge" bug): AE
    effect worlds are STRAIGHT (unmatted) alpha, in AND out — the same
    lesson ModernEdgeBlur's black-halo bug taught (premult→blur→unpremult).
    The brief's original "AE hands premultiplied pixels" claim was wrong.
    So: px RGB is straight colour; wherever the maths must be coverage-
    weighted (pull–push level-0 init, the Detail low-pass) the premultiply
    happens INTERNALLY and is divided back out; the output RGB is never
    scaled by alpha. Remaining: Stage 5 = CPU perf, Stage 6 = GPU.

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
#include "common/MM_PullPush.h"
#include "common/MM_Gaussian.h"
#include "common/MM_Grain.h"
#include "common/MM_Composite.h"

#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <new>

// ---------------------------------------------------------------------------
// Stage 5 perf instrumentation. Set ER_PERF_LOG to 1 to append one line per
// Result-mode frame to %TEMP%\EdgeRepair_perf.log with wall-clock timings for
// each hot pass and the whole SmartRender. OFF for shipping builds (zero cost
// — the timer helpers compile to nothing). This is how we pull real AE timing
// numbers at the Stage 5 gate: build with it on, RAM-preview a 4K comp, read
// the log. Logging itself is off the hot path (once per frame, after render).
// ---------------------------------------------------------------------------
#define ER_PERF_LOG 0

#if ER_PERF_LOG
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
namespace {
using ER_Clock = std::chrono::high_resolution_clock;
inline double ER_ms(ER_Clock::time_point a, ER_Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
// Append a line to the perf log. Serialized across threads by the CRT's file
// locking; called once per frame so contention is irrelevant.
inline void ER_PerfLog(const char *fmt, ...) {
    char path[1024];
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = ".";
    snprintf(path, sizeof(path), "%s\\EdgeRepair_perf.log", tmp);
    FILE *f = fopen(path, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}
} // namespace
#define ER_PERF_T(var)   ER_Clock::time_point var = ER_Clock::now()
#define ER_PERF_DT(a,b)  ER_ms(a,b)
#else
#define ER_PERF_T(var)   ((void)0)
#define ER_PERF_DT(a,b)  (0.0)
#endif

// Fixed texture-frequency-split blur radius (px). The Detail high-pass
// subtracts a Gaussian of this radius from the plate; it contributes to the
// inflation radius because that Gaussian reads neighbors. Exposed nowhere —
// a fixed value inside the brief's "~4-8px" range.
//
// This radius IS the cutoff between "texture Detail preserves" and
// "contamination the repair removes": at Detail 100 the composite reduces to
// out = orig + w*(ext - low), a purely LOW-frequency correction, so any
// feature narrower than this radius (grain, but also a narrow light-wrap
// rim) rides through untouched. Stage 4 gate round 3 (2026-07-09): at 8.0
// a 2-4px wrap rim survived Detail 100 and read worse than source; dropped
// to the in-spec bottom 4.0 so rim-scale features fall into the repaired
// low band while ~1px grain still survives. If wrap rims still persist at
// Detail 100, going below 4 needs a spec conversation (frequency alone
// cannot separate wrap rims from same-scale motion-blur micro-detail).
static const float ER_TEXTURE_BLUR_RADIUS = 4.0f;

// Inner-band ease width as a FRACTION of Extend (2026-07-22). Over the
// innermost `ER_INNER_SOFT_FRAC * Extend` px out from the core, the blend
// weight ramps 0 -> 1 instead of snapping to full replacement at the core
// boundary — so the recomposited band eases back into the REAL core colour
// rather than leaving a hard line where band meets core (visible on wide light
// wraps). Resolution-relative by construction (scales with Extend). Reshapes
// the weight only; sample points are unchanged. ~15% is subtle: enough to kill
// the line without visibly eating into the repair. Exposed nowhere (no param).
static const float ER_INNER_SOFT_FRAC = 0.15f;

// Tiling debug visualization. 1 = every View Mode renders the Stage 1 seam-
// test pattern (see ER_TileDebugRow); 0 = the real pipeline/view modes run.
// Inflation was signed off at the Stage 1 gate, so this is OFF now; flip to 1
// only to re-run the seam test.
#define ER_TILING_DEBUG 0

// ---------------------------------------------------------------------------
// Per-render scratch buffers, reused ACROSS frames without any mutable global
// or static state (Stage 5: "kill per-frame allocation" while staying MFR-
// reentrant). Held in a thread_local: AE's render worker threads persist, so
// each thread's buffers survive frame to frame and grow-only .resize() stops
// reallocating after the first frame at a given size — yet every SmartRender
// invocation on a given thread has its OWN buffers, so concurrent renders on
// different threads (MFR) never share state. This is the same discipline the
// row callbacks already use with their thread_local `rowv`, generalized to the
// whole pipeline's working set.
//
// resize() on an already-large-enough vector is a no-op (no allocation) but
// leaves stale contents; buffers whose correctness depends on zero-init
// (`ext`, `chokeField`) are cleared explicitly at their use site — see the
// EnsureZeroed helper. Buffers fully overwritten before read (srcBuf,
// alphaPlane, core, dist, low, plane, tmp, seeds) need no clear.
struct ER_Scratch {
    std::vector<float>   srcBuf;      // srcW*srcH*4 ARGB
    std::vector<float>   alphaPlane;  // srcN
    std::vector<uint8_t> core;        // srcN
    std::vector<float>   dist;        // srcN
    std::vector<float>   ext;         // srcN*3  (zero-init semantics)
    std::vector<float>   low;         // srcN*3
    std::vector<float>   gplane;      // srcN     Gaussian working plane
    std::vector<float>   gtmp;        // srcN     Gaussian H→V temp
    std::vector<float>   chokeField;  // srcN     (zero-init semantics)
    std::vector<uint8_t> seeds;       // srcN
};

// One scratch set per render worker thread. Never shared across threads, so no
// synchronization and no MFR hazard; freed when the thread exits.
static thread_local ER_Scratch g_erScratch;

// Grow `v` to at least n and zero the first n elements (for buffers whose
// algorithm relies on a black/zero starting state). Reuses capacity; only the
// grow beyond capacity allocates.
template <typename T>
static void EnsureZeroed(std::vector<T> &v, size_t n)
{
    if (v.size() < n) v.resize(n);
    std::fill_n(v.begin(), n, T{});
}

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

// Stashed by PreRender, read by SmartRender. The two checkout rects are the
// ONLY reliable way to map output pixels to source pixels: inferring the
// offset from buffer-size differences (the original approach) silently
// assumed AE clamps the inflated request symmetrically — when it clamps one
// side only (layer extending past the comp on one side, tiled requests) the
// inferred offset was wrong by up to the full inflation radius and the whole
// image shifted. Found at the Stage 4 gate as a visible layer shift on
// effect toggle.
typedef struct {
    A_long   inflate;   // px added on every side in PreRender
    PF_LRect inRect;    // in_result.result_rect — the source world's rect
    PF_LRect outRect;   // output result rect (post-clamp) — the out world's rect
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

// Extended Colour (Stage 3): the pull–push fill result — straight
// (unpremultiplied) RGB, shown fully opaque. Deliberately UNLIMITED: this
// view exists to inspect raw fill quality everywhere, so the Extend clamp
// (which only governs how far the result is USED in the Stage 4 composite)
// does not apply here.
struct ER_ExtViewRefcon {
    const float    *ext;        // srcW*srcH*3 straight RGB
    A_long          srcW;
    A_long          srcOX, srcOY;
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

static PF_Err
ER_ExtViewRow(void *refconPV, A_long /*thread_indexL*/, A_long oy, A_long /*iterationsL*/)
{
    const ER_ExtViewRefcon *rc = static_cast<const ER_ExtViewRefcon*>(refconPV);
    const A_long W = rc->outW;
    const float *srow = rc->ext + ((size_t)(oy + rc->srcOY) * rc->srcW + rc->srcOX) * 3;

    thread_local std::vector<float> rowv;
    if ((A_long)rowv.size() < W * 4) rowv.resize((size_t)W * 4);
    float *dst = rowv.data();
    for (A_long ox = 0; ox < W; ++ox, dst += 4, srow += 3) {
        dst[0] = 1.f; dst[1] = srow[0]; dst[2] = srow[1]; dst[3] = srow[2];
    }

    void *outRow = reinterpret_cast<char*>(rc->out_world->data) + (size_t)oy * rc->out_world->rowbytes;
    mm::PackRow(rowv.data(), outRow, W, rc->fmt);
    return PF_Err_NONE;
}

} // extern "C"

// ---------------------------------------------------------------------------
// Result (Stage 4) — the real composite. Per-pixel: choke the alpha along
// the alpha-edge field, then inside the band 0 < d <= Extend replace the
// plate's low frequencies with the extension and lay the plate's own high
// frequencies back on top scaled by Detail. Straight-space maths from
// MM_Composite.h; STRAIGHT alpha in and out. Null pointers switch features off:
//   dist/ext/low == null → band inactive (Extend 0, or empty core)
//   chokeField   == null → choke inactive (choke 0, or no seeds)
// The w<=0, newA==a fast path copies the source premult VERBATIM — that is
// what makes "untouched outside the band" and "Choke 0 → bit-identical
// alpha" hold bitwise, not just approximately.
// ---------------------------------------------------------------------------
struct ER_ResultRefcon {
    const float    *src;        // inflated STRAIGHT-alpha ARGB
    const float    *dist;       // core distance field (null = band off)
    const float    *ext;        // straight extended colour (null = band off)
    const float    *chokeField; // alpha-edge EDT (null = choke off)
    float           extendF;
    float           innerSoft;  // inner-ease width (px) for BlendWeight
    float           detail01;   // Detail param / 100
    float           grainAmp;   // synthetic-grain amplitude (from core estimate)
    float           chokeF;     // signed px; nonzero only when chokeField set
    A_long          srcW;
    A_long          srcOX, srcOY;
    PF_EffectWorld *out_world;
    PF_PixelFormat  fmt;
    A_long          outW;
};

extern "C" {

static PF_Err
ER_ResultRow(void *refconPV, A_long /*thread_indexL*/, A_long oy, A_long /*iterationsL*/)
{
    const ER_ResultRefcon *rc = static_cast<const ER_ResultRefcon*>(refconPV);
    const A_long W = rc->outW;
    const size_t base = (size_t)(oy + rc->srcOY) * rc->srcW + rc->srcOX;

    thread_local std::vector<float> rowv;
    if ((A_long)rowv.size() < W * 4) rowv.resize((size_t)W * 4);
    float *dst = rowv.data();

    for (A_long ox = 0; ox < W; ++ox, dst += 4) {
        const size_t i  = base + (size_t)ox;
        const float *px = rc->src + i * 4;
        const float  a  = px[0];

        // Alpha first: choke along the alpha-edge field.
        float newA = a;
        if (rc->chokeField) {
            newA = rc->chokeF > 0.f
                 ? mm::ChokeInAlpha(a, rc->chokeField[i], rc->chokeF)
                 : mm::GrowAlpha(a, rc->chokeField[i], -rc->chokeF);
        }

        // Band weight — only strictly OUTSIDE the core (d > 0); the core is
        // trusted and never recomposited.
        float w = 0.f;
        if (rc->dist) {
            const float d = rc->dist[i];
            if (d > 0.f) w = mm::BlendWeight(d, rc->extendF, rc->innerSoft);
        }

        // STRAIGHT alpha throughout (AE's effect-world convention — see the
        // file header): px[1..3] ARE the straight colour, and the output RGB
        // is never scaled by alpha. Premultiplying here was the Stage 4 gate
        // "dark edge along the alpha boundary" bug — every semi-transparent
        // pixel came out darkened by exactly its alpha.
        if (w <= 0.f) {
            if (newA == a) {
                // Bitwise passthrough.
                dst[0] = px[0]; dst[1] = px[1]; dst[2] = px[2]; dst[3] = px[3];
            } else {
                // Choke changed alpha; colour is alpha-independent, so it
                // stays — except a grown zero-alpha pixel, whose own RGB is
                // undefined garbage: the extension is its only real colour.
                dst[0] = newA;
                if (a <= 1e-6f && rc->ext) {
                    dst[1] = rc->ext[i * 3 + 0];
                    dst[2] = rc->ext[i * 3 + 1];
                    dst[3] = rc->ext[i * 3 + 2];
                } else {
                    dst[1] = px[1]; dst[2] = px[2]; dst[3] = px[3];
                }
            }
        } else {
            // In-band composite (MM_Composite.h contract), straight space:
            // base = lerp(orig, ext, w); + w*detail*grain. Grain is a zero-mean
            // luma sample keyed to the inflated-source pixel coord (sx,sy) so it
            // is deterministic and stable across frames for a stationary render.
            // At a~0 the plate RGB is garbage, so use ext as `orig` there.
            const float *E = rc->ext + i * 3;
            float orig[3];
            if (a > 1e-6f) {
                orig[0] = px[1]; orig[1] = px[2]; orig[2] = px[3];
            } else {
                orig[0] = E[0]; orig[1] = E[1]; orig[2] = E[2];
            }
            const int sx = (int)(ox + rc->srcOX);
            const int sy = (int)(oy + rc->srcOY);
            const float grain = mm::GrainAt(sx, sy) * rc->grainAmp;
            float outS[3];
            mm::ComposeBandRGB(orig, E, grain, w, rc->detail01, outS);
            dst[0] = newA;
            dst[1] = outS[0];
            dst[2] = outS[1];
            dst[3] = outS[2];
        }
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

    // Stash the inflation amount for SmartRender (rects filled in below,
    // once the checkout reports what AE will actually return). AE owns this
    // pointer once assigned and frees it via our cleanup contract.
    ER_PreRenderData *prd = static_cast<ER_PreRenderData*>(
        malloc(sizeof(ER_PreRenderData)));
    if (prd) {
        memset(prd, 0, sizeof(*prd));
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
    // No footage-interpretation override: AE effect worlds are STRAIGHT
    // alpha in and out (see the file header — coverage weighting happens
    // internally where the maths needs it, never at the AE boundary).

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

    // Record the actual rects: the source world SmartRender checks out
    // corresponds to in_result.result_rect, the output world to our clamped
    // result_rect. Their origin difference IS the output→source offset —
    // no size-based inference (see ER_PreRenderData note).
    if (prd && !err) {
        prd->inRect  = in_result.result_rect;
        prd->outRect = extra->output->result_rect;
    }

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
    ER_PERF_T(tRender0);

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

        // Install AE's iterate_generic as the parallel backend for every
        // mm::ParallelFor inside the algorithm cores (EDT, Gaussian, pull–push)
        // for the duration of this render. Serial in the headless tests where
        // no backend is set; AE's own scheduler owns the threads here (MFR-safe
        // — no std::thread oversubscription against AE's per-frame concurrency).
        // Cleared automatically when this scope exits.
        mm::ScopedParallelBackend parallelGuard(
            mm::MakeIterateGenericBackend(in_data));

        const ER_PreRenderData *prd =
            static_cast<const ER_PreRenderData*>(extra->input->pre_render_data);
        const A_long inflate = prd ? prd->inflate : 0;

        const A_long srcW = src_world->width;
        const A_long srcH = src_world->height;
        const A_long outW = out_world->width;
        const A_long outH = out_world->height;

        // Output→source origin offset, from the ACTUAL checkout rects stashed
        // in PreRender: output pixel (0,0) sits at source pixel
        // (outRect.left - inRect.left, outRect.top - inRect.top). The old
        // size-difference inference silently assumed symmetric clamping and
        // shifted the image when AE clamped the inflated request on one side
        // only (Stage 4 gate finding). Defensive clamps keep addressing in
        // bounds if the delivered worlds ever disagree with the rects.
        A_long srcOX = 0, srcOY = 0;
        if (prd) {
            srcOX = prd->outRect.left - prd->inRect.left;
            srcOY = prd->outRect.top  - prd->inRect.top;
        }
        if (srcOX > (srcW - outW)) srcOX = srcW - outW;
        if (srcOY > (srcH - outH)) srcOY = srcH - outH;
        if (srcOX < 0) srcOX = 0;
        if (srcOY < 0) srcOY = 0;

        // Unpack the whole inflated source into one ARGB float buffer. Scratch
        // is reused across frames (see ER_Scratch) — fully overwritten by the
        // unpack below, so no clear needed.
        std::vector<float> &srcBuf = g_erScratch.srcBuf;
        try {
            if (srcBuf.size() < (size_t)srcW * srcH * 4)
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
        // Pipeline: core matte + the single driving distance field, computed
        // over the FULL inflated source so the field is correct right to the
        // output edge, then cropped to output by the view callbacks. All
        // four view modes are real as of Stage 4.
        // -----------------------------------------------------------------
        const size_t srcN = (size_t)srcW * srcH;

        // Result-mode early-out: Extend <= 0 and Choke == 0 means the effect
        // does nothing at all — skip every field/fill computation and pass
        // through bitwise (also the first of the Stage 5 perf early-outs).
        const bool chokeActive = (p.chokeF >= 0.01 || p.chokeF <= -0.01);
        const bool resultView  = (p.viewL != ER_VIEW_CORE_MATTE &&
                                  p.viewL != ER_VIEW_DISTANCE_FIELD &&
                                  p.viewL != ER_VIEW_EXTENDED_COLOUR);
        if (resultView && p.extendF <= 0.0 && !chokeActive) {
            ER_PassRefcon rc;
            rc.src = srcBuf.data(); rc.srcW = srcW;
            rc.srcOX = srcOX; rc.srcOY = srcOY;
            rc.out_world = out_world; rc.fmt = fmt; rc.outW = outW;
            mm::ForEachRow(in_data, outH, &rc, ER_PassRow);
            goto cleanup;
        }

        // Extract the alpha plane from the inflated source (straight-alpha
        // worlds — the alpha channel is the coverage, used directly). Scratch
        // reused across frames; all three are fully overwritten before read.
        std::vector<float>   &alphaPlane = g_erScratch.alphaPlane;
        std::vector<uint8_t> &core       = g_erScratch.core;
        std::vector<float>   &dist       = g_erScratch.dist;
        bool haveField = false;
        try {
            if (alphaPlane.size() < srcN) alphaPlane.resize(srcN);
            if (core.size()       < srcN) core.resize(srcN);
            if (dist.size()       < srcN) dist.resize(srcN);
        } catch (const std::bad_alloc &) {
            err = PF_Err_OUT_OF_MEMORY;
            goto cleanup;
        }
        for (size_t i = 0; i < srcN; ++i) alphaPlane[i] = srcBuf[i * 4 + 0];

        // Core matte: threshold at 0.5, signed Erode. Then the driving field.
        ER_PERF_T(tcf0);
        mm::BuildCoreMatte(alphaPlane.data(), core.data(), (int)srcW, (int)srcH,
                           0.5f, (float)p.erodeF);
        haveField = mm::CoreDistanceField(core.data(), dist.data(), (int)srcW, (int)srcH);
        ER_PERF_T(tcf1);
#if ER_PERF_LOG
        ER_PerfLog("[EdgeRepair] %ldx%ld view=%ld  core+field %.1f ms\n",
                   (long)srcW, (long)srcH, (long)p.viewL, ER_PERF_DT(tcf0, tcf1));
#endif

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
            case ER_VIEW_EXTENDED_COLOUR: {
                // Stage 3: the unlimited pull–push fill of trusted core
                // colour over the full inflated source, cropped to output by
                // the view callback. srcBuf is AE-native premultiplied, which
                // is exactly what PullPushFill wants; the fill unpremultiplies
                // internally, so `ext` is straight RGB.
                std::vector<float> &ext = g_erScratch.ext;
                try {
                    EnsureZeroed(ext, srcN * 3);   // black where fill leaves gaps
                } catch (const std::bad_alloc &) {
                    err = PF_Err_OUT_OF_MEMORY;
                    goto cleanup;
                }
                // Empty core → no trusted colour exists anywhere; leave the
                // zeroed black rather than showing garbage (same guard
                // philosophy as the Distance Field view).
                mm::PullPushFill(srcBuf.data(), core.data(), ext.data(),
                                 (int)srcW, (int)srcH);

                ER_ExtViewRefcon rc;
                rc.ext = ext.data(); rc.srcW = srcW;
                rc.srcOX = srcOX; rc.srcOY = srcOY;
                rc.out_world = out_world; rc.fmt = fmt; rc.outW = outW;
                mm::ForEachRow(in_data, outH, &rc, ER_ExtViewRow);
                break;
            }
            case ER_VIEW_RESULT:
            default: {
                // ---------------------------------------------------------
                // Stage 4 — the real composite. Build the per-feature inputs
                // (extension, low-pass, choke field), then one threaded
                // per-row pass does the straight-space maths from
                // MM_Composite.h and re-premultiplies.
                // ---------------------------------------------------------
                bool doBand = haveField && p.extendF > 0.0;

                // Pull–push extension + low-pass — the two colour sources of
                // the band composite. Both fully overwritten before read
                // (PullPushFill writes every pixel; the Gaussian loop writes
                // every channel), so reused scratch needs no clear.
                std::vector<float> &ext = g_erScratch.ext;
                if (doBand) {
                    try {
                        if (ext.size() < srcN * 3) ext.resize(srcN * 3);
                    } catch (const std::bad_alloc &) {
                        err = PF_Err_OUT_OF_MEMORY;
                        goto cleanup;
                    }
                    ER_PERF_T(tpp0);
                    if (!mm::PullPushFill(srcBuf.data(), core.data(),
                                          ext.data(), (int)srcW, (int)srcH)) {
                        doBand = false;   // no trusted colour anywhere
                    }
                    ER_PERF_T(tpp1);
#if ER_PERF_LOG
                    ER_PerfLog("  pull-push %.1f ms\n", ER_PERF_DT(tpp0, tpp1));
#endif
                }
                // Synthetic-grain amplitude, estimated from the TRUSTED CORE
                // (2026-07-22 — replaces the old frequency-separation low-pass;
                // see MM_Composite.h / MM_Grain.h for why). We want the plate's
                // local high-frequency luma energy where it is CLEAN — inside
                // the core, away from the contaminated edge, so no edge step
                // pollutes the estimate. Cheap, blur-free proxy: mean absolute
                // horizontal+vertical luma neighbour difference over core
                // pixels, converted to a grain amplitude. This is what lets the
                // added grain sit in the plate instead of reading as pasted-on.
                float grainAmp = 0.f;
                if (doBand) {
                    ER_PERF_T(tg0);
                    double acc = 0.0; size_t cnt = 0;
                    const int Wi = (int)srcW, Hi = (int)srcH;
                    // Luma of a straight-RGB pixel at flat index j.
                    auto lumAt = [&](size_t j) -> double {
                        const float *q = srcBuf.data() + j * 4;
                        return 0.299 * q[1] + 0.587 * q[2] + 0.114 * q[3];
                    };
                    for (int y = 0; y < Hi; ++y) {
                        for (int x = 0; x < Wi - 1; ++x) {
                            const size_t j = (size_t)y * Wi + x;
                            if (!core[j] || !core[j + 1]) continue;   // both in core
                            acc += std::fabs(lumAt(j) - lumAt(j + 1));
                            ++cnt;
                        }
                    }
                    for (int y = 0; y < Hi - 1; ++y) {
                        for (int x = 0; x < Wi; ++x) {
                            const size_t j = (size_t)y * Wi + x;
                            if (!core[j] || !core[j + Wi]) continue;
                            acc += std::fabs(lumAt(j) - lumAt(j + (size_t)Wi));
                            ++cnt;
                        }
                    }
                    // Mean |neighbour diff| ~ grain std * ~1.13 for white noise;
                    // GrainAt() spans [-1,1] (std ~0.577), so scale the mean by
                    // 1/(1.13*0.577) ≈ 1.53 to make grain*amp match the plate's
                    // measured neighbour spread. Approximate is fine — the look
                    // is judged in AE, and Detail scales it anyway.
                    const double meanDiff = cnt ? acc / (double)cnt : 0.0;
                    grainAmp = (float)(meanDiff * 1.53);
                    ER_PERF_T(tg1);
#if ER_PERF_LOG
                    ER_PerfLog("  grain-amp %.1f ms  (amp=%.4f, %zu core px)\n",
                               ER_PERF_DT(tg0, tg1), grainAmp, cnt);
#endif
                }

                // Alpha Choke field — anchored at the ALPHA'S OWN edge (see
                // MM_Composite.h; 2026-07-09 decision): choke-in measures to
                // the nearest zero-alpha pixel, grow to the nearest solid
                // (a > 0.5) pixel. Only computed when the knob is touched —
                // choke == 0 never reaches this path, which is what makes
                // the bit-identical-alpha gate hold.
                std::vector<float> &chokeField = g_erScratch.chokeField;
                bool haveChoke = false;
                if (chokeActive) {
                    try {
                        std::vector<uint8_t> &seeds = g_erScratch.seeds;
                        if (seeds.size()      < srcN) seeds.resize(srcN);
                        if (chokeField.size() < srcN) chokeField.resize(srcN);
                        const bool in = p.chokeF > 0.0;
                        bool any = false;
                        for (size_t i = 0; i < srcN; ++i) {
                            const float a = srcBuf[i * 4 + 0];
                            const uint8_t s = in ? (a <= 0.001f ? 1 : 0)
                                                 : (a >  0.5f   ? 1 : 0);
                            seeds[i] = s;
                            if (s) any = true;
                        }
                        // No seeds → nothing to measure from (fully-opaque
                        // frame for choke-in; no solid region for grow):
                        // choke is a well-defined no-op.
                        if (any) {
                            mm::EuclideanDistanceTransform(seeds.data(),
                                chokeField.data(), (int)srcW, (int)srcH);
                            haveChoke = true;
                        }
                    } catch (const std::bad_alloc &) {
                        err = PF_Err_OUT_OF_MEMORY;
                        goto cleanup;
                    }
                }

                ER_ResultRefcon rc;
                rc.src        = srcBuf.data();
                rc.dist       = doBand ? dist.data() : nullptr;
                rc.ext        = doBand ? ext.data()  : nullptr;
                rc.chokeField = haveChoke ? chokeField.data() : nullptr;
                rc.extendF    = (float)p.extendF;
                rc.innerSoft  = (float)(p.extendF * ER_INNER_SOFT_FRAC);
                rc.detail01   = (float)(p.detailF / 100.0);
                rc.grainAmp   = doBand ? grainAmp : 0.f;
                rc.chokeF     = haveChoke ? (float)p.chokeF : 0.f;
                rc.srcW = srcW; rc.srcOX = srcOX; rc.srcOY = srcOY;
                rc.out_world = out_world; rc.fmt = fmt; rc.outW = outW;
                ER_PERF_T(tcomp0);
                mm::ForEachRow(in_data, outH, &rc, ER_ResultRow);
                ER_PERF_T(tcomp1);
#if ER_PERF_LOG
                ER_PerfLog("  composite %.1f ms\n", ER_PERF_DT(tcomp0, tcomp1));
#endif
                break;
            }
        }
#endif
    }

cleanup:
    if (wsP) ERR2(AEFX_ReleaseSuite(in_data, out_data, kPFWorldSuite, kPFWorldSuiteVersion2, ""));
#if ER_PERF_LOG
    {
        ER_PERF_T(tRender1);
        ER_PerfLog("  TOTAL SmartRender %.1f ms\n\n", ER_PERF_DT(tRender0, tRender1));
    }
#endif
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
