/*  ModernEdgeBlur.cpp  —  v1.2

    Pipeline (per frame):
      1. [Shift Edge]  Stubbed — morphological grow/shrink on alpha. No-op when = 0 (default).
      2. [Blur Amount] Gaussian blur full premultiplied RGBA (GaussBlurRGBA, threaded via
                       iterate_generic, sigma = radius * 0.25) → blurred base (A). No AE
                       world/native convolve involved — see Phase 2 comment in SmartRender.
      3. [Blend Radius] Erode sharp alpha inward by radius (octagon erosion — 4-direction
                        approximation of a circular min-filter, threaded via iterate_generic),
                        then Gaussian-blur to feather (also threaded via iterate_generic) →
                        weight w = clamp(eroded+blurred, 0, 1). w controls how much sharp
                        (top) layer shows vs blurred (base) layer — eroding w reveals more
                        of the blurred base at the edge.
      4. [Composite]   out_rgb   = lerp(A_rgb, sharp_rgb, w)
                       out_alpha = A_alpha  (blurred alpha = soft feathered edge, unchanged by Blend Radius)

    Passthrough: Blur=0 and Blend=0 → input copied directly to output, held RGB preserved.
    Premult: preserve_rgb_of_zero_alpha=FALSE on checkout, PLUS an explicit zero-RGB-
             where-alpha-is-0 pass on sharp_buf right after unpacking. The checkout flag
             alone isn't enough — it only lets AE discard hidden RGB it manages itself,
             it can't retroactively clean pixels an upstream effect already baked in (e.g.
             a precomp with an internal keyer that only touches alpha, leaving un-keyed
             footage colour sitting underneath). Enforcing it ourselves means honest
             premult holds regardless of what the source actually did.
             Gaussian sees zero colour outside matte → no dark halo.
    No input inflation — src_world is exactly output size.
*/

#define NOMINMAX
#include "ModernEdgeBlur.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <chrono>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static inline A_long clampL(A_long v, A_long lo, A_long hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float clampF(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Unpack one row of an AE world into float ARGB (all channels [0,1]).
static void UnpackRow(const void *srcRow, float *dst, A_long W, PF_PixelFormat fmt)
{
    switch (fmt) {
        case PF_PixelFormat_ARGB128: {
            const PF_PixelFloat *p = static_cast<const PF_PixelFloat*>(srcRow);
            for (A_long x = 0; x < W; ++x, ++p, dst += 4) {
                dst[0] = p->alpha; dst[1] = p->red; dst[2] = p->green; dst[3] = p->blue;
            }
            break;
        }
        case PF_PixelFormat_ARGB64: {
            const PF_Pixel16 *p = static_cast<const PF_Pixel16*>(srcRow);
            const float s = 1.f / (float)PF_MAX_CHAN16;
            for (A_long x = 0; x < W; ++x, ++p, dst += 4) {
                dst[0] = p->alpha*s; dst[1] = p->red*s; dst[2] = p->green*s; dst[3] = p->blue*s;
            }
            break;
        }
        default: {
            const PF_Pixel8 *p = static_cast<const PF_Pixel8*>(srcRow);
            const float s = 1.f / (float)PF_MAX_CHAN8;
            for (A_long x = 0; x < W; ++x, ++p, dst += 4) {
                dst[0] = p->alpha*s; dst[1] = p->red*s; dst[2] = p->green*s; dst[3] = p->blue*s;
            }
            break;
        }
    }
}

// Unpack an entire AE world to a heap float[H*W*4] ARGB buffer.
static float* UnpackWorld(const PF_EffectWorld *world, PF_PixelFormat fmt)
{
    const A_long W = world->width, H = world->height;
    float *buf = new (std::nothrow) float[(size_t)H * W * 4];
    if (!buf) return nullptr;
    for (A_long y = 0; y < H; ++y) {
        const void *srcRow = reinterpret_cast<const char*>(world->data) + y * world->rowbytes;
        UnpackRow(srcRow, buf + (size_t)y * W * 4, W, fmt);
    }
    return buf;
}

// Pack float ARGB back into an AE world row.
static void PackRow(const float *src, void *dstRow, A_long W, PF_PixelFormat fmt)
{
    switch (fmt) {
        case PF_PixelFormat_ARGB128: {
            PF_PixelFloat *p = static_cast<PF_PixelFloat*>(dstRow);
            for (A_long x = 0; x < W; ++x, ++p, src += 4) {
                p->alpha = src[0]; p->red = src[1]; p->green = src[2]; p->blue = src[3];
            }
            break;
        }
        case PF_PixelFormat_ARGB64: {
            PF_Pixel16 *p = static_cast<PF_Pixel16*>(dstRow);
            const float M = (float)PF_MAX_CHAN16;
            for (A_long x = 0; x < W; ++x, ++p, src += 4) {
                p->alpha = (A_u_short)(clampF(src[0],0,1)*M + .5f);
                p->red   = (A_u_short)(clampF(src[1],0,1)*M + .5f);
                p->green = (A_u_short)(clampF(src[2],0,1)*M + .5f);
                p->blue  = (A_u_short)(clampF(src[3],0,1)*M + .5f);
            }
            break;
        }
        default: {
            PF_Pixel8 *p = static_cast<PF_Pixel8*>(dstRow);
            const float M = (float)PF_MAX_CHAN8;
            for (A_long x = 0; x < W; ++x, ++p, src += 4) {
                p->alpha = (A_u_char)(clampF(src[0],0,1)*M + .5f);
                p->red   = (A_u_char)(clampF(src[1],0,1)*M + .5f);
                p->green = (A_u_char)(clampF(src[2],0,1)*M + .5f);
                p->blue  = (A_u_char)(clampF(src[3],0,1)*M + .5f);
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Morphological min/max filter on a single float plane (circular kernel,
// brute-force). dilate=true grows the plane (max filter); dilate=false
// shrinks it (min filter / erosion). Shared by ShiftEdgeAlpha (Phase 1) and
// the Blend Radius alpha erosion (Phase 3).
// ---------------------------------------------------------------------------
static void MorphPlane(const float *in, float *out, A_long W, A_long H, float radius, bool dilate)
{
    const float r = std::abs(radius);
    if (r < 0.5f) {
        std::memcpy(out, in, sizeof(float) * (size_t)W * H);
        return;
    }

    const int   ri = (int)std::ceil(r);
    const float r2 = r * r;

    for (A_long y = 0; y < H; ++y) {
        for (A_long x = 0; x < W; ++x) {
            float best = dilate ? 0.f : 1.f;
            for (int dy = -ri; dy <= ri; ++dy) {
                for (int dx = -ri; dx <= ri; ++dx) {
                    if ((float)(dx*dx + dy*dy) > r2) continue;
                    const A_long sx = clampL(x + dx, 0, W - 1);
                    const A_long sy = clampL(y + dy, 0, H - 1);
                    const float  a  = in[(size_t)sy * W + sx];
                    if (dilate) { if (a > best) best = a; }
                    else        { if (a < best) best = a; }
                }
            }
            out[(size_t)y * W + x] = best;
        }
    }
}

// ---------------------------------------------------------------------------
// Phase 1: Morphological shift on alpha.
// radius > 0 → dilate (grow alpha); radius < 0 → erode (shrink alpha)
// Operates in-place on a float[H*W*4] ARGB buffer (channel 0 = alpha).
// ---------------------------------------------------------------------------
static void ShiftEdgeAlpha(float *buf, A_long W, A_long H, float radius)
{
    if (std::abs(radius) < 0.5f) return;

    const bool dilate = (radius > 0.f);

    std::vector<float> alpha_in((size_t)W * H);
    for (A_long i = 0; i < W * H; ++i) alpha_in[i] = buf[i * 4 + 0];

    std::vector<float> alpha_out((size_t)W * H);
    MorphPlane(alpha_in.data(), alpha_out.data(), W, H, radius, dilate);

    // Write shifted alpha back; RGB unchanged
    for (A_long i = 0; i < W * H; ++i) buf[i * 4 + 0] = alpha_out[i];
}

// ---------------------------------------------------------------------------
// Octagonal erosion — approximates the circular min-filter used by Blend
// Radius (Phase 3) at a fraction of the cost, without dropping to a square.
//
// Erosion composes exactly under Minkowski sum: eroding by shape A then by
// shape B gives the identical result to eroding once by A⊕B. So instead of
// one O(radius^2) 2D circular scan, we erode along 4 individual directions —
// horizontal, vertical, and both diagonals — each a cheap O(halfLen) 1D scan.
// Their combined shape is an octagon. Sizing the axis half-length as
// radius*(sqrt(2)-1) and the diagonal half-length as radius*(1-1/sqrt(2))
// makes the octagon reach exactly `radius` pixels along both the axis and
// 45-degree directions — a standard "near-circle" construction. Visually
// very close to a true disc, especially since the result is immediately
// Gaussian-feathered afterward (see MakeBlendKernel / GaussBlurPlane).
//
// Each directional pass reads from one buffer and writes to a different one,
// so every output pixel is independent of every other — safe to split across
// AE's worker threads via iterate_generic with no shared scratch state.
// ---------------------------------------------------------------------------

struct MEB_DirMinRefcon {
    const float *in;
    float       *out;
    A_long       W, H;
    int          dx, dy, halfLen;
};

extern "C" {

static PF_Err
MEB_DirMinRow(void *refconPV, A_long /*thread_indexL*/, A_long y, A_long /*iterationsL*/)
{
    const MEB_DirMinRefcon *rc = static_cast<const MEB_DirMinRefcon*>(refconPV);
    const A_long W       = rc->W;
    const int    dx      = rc->dx;
    const int    dy      = rc->dy;
    const int    halfLen = rc->halfLen;
    float       *outRow  = rc->out + (size_t)y * W;

    for (A_long x = 0; x < W; ++x) {
        float m = 1.f;
        for (int t = -halfLen; t <= halfLen; ++t) {
            const A_long sx = clampL(x + t * dx, 0, W - 1);
            const A_long sy = clampL(y + t * dy, 0, rc->H - 1);
            const float  a  = rc->in[(size_t)sy * W + sx];
            if (a < m) m = a;
        }
        outRow[x] = m;
    }
    return PF_Err_NONE;
}

} // extern "C"

// 1D min-filter of half-width halfLen along direction (dx,dy), clamped to
// image edges. Threaded per output row via iterate_generic when available.
static void DirectionalMinFilter(const float *in, float *out, A_long W, A_long H,
                                  int dx, int dy, int halfLen, PF_InData *in_data)
{
    if (halfLen <= 0) {
        std::memcpy(out, in, sizeof(float) * (size_t)W * H);
        return;
    }

    if (in_data && in_data->utils && in_data->utils->iterate_generic) {
        MEB_DirMinRefcon rc{ in, out, W, H, dx, dy, halfLen };
        if ((*in_data->utils->iterate_generic)(H, &rc, MEB_DirMinRow) == PF_Err_NONE) return;
    }

    // Serial fallback.
    for (A_long y = 0; y < H; ++y) {
        for (A_long x = 0; x < W; ++x) {
            float m = 1.f;
            for (int t = -halfLen; t <= halfLen; ++t) {
                const A_long sx = clampL(x + t * dx, 0, W - 1);
                const A_long sy = clampL(y + t * dy, 0, H - 1);
                const float  a  = in[(size_t)sy * W + sx];
                if (a < m) m = a;
            }
            out[(size_t)y * W + x] = m;
        }
    }
}

// Approximate circular erosion of a float alpha plane via 4-direction octagon
// decomposition. radius < 0.5 is a no-op (copies input to output).
static void OctagonErode(const float *in, float *out, A_long W, A_long H,
                          float radius, PF_InData *in_data)
{
    if (radius < 0.5f) {
        std::memcpy(out, in, sizeof(float) * (size_t)W * H);
        return;
    }

    // Clamp axis half-length to >=1 so any radius past the 0.5 threshold
    // erodes by at least one pixel (avoids a dead zone at very low radius,
    // where sqrt(2)-1 * radius would otherwise round down to 0).
    const int a = std::max(1, (int)std::lround(radius * 0.41421356f)); // sqrt(2)-1
    const int c =              (int)std::lround(radius * 0.29289322f); // 1-1/sqrt(2)

    std::vector<float> tmp((size_t)W * H);

    DirectionalMinFilter(in,         tmp.data(), W, H,  1,  0, a, in_data); // horizontal
    DirectionalMinFilter(tmp.data(), out,        W, H,  0,  1, a, in_data); // vertical
    DirectionalMinFilter(out,        tmp.data(), W, H,  1,  1, c, in_data); // diagonal ↘
    DirectionalMinFilter(tmp.data(), out,        W, H,  1, -1, c, in_data); // diagonal ↗
}

// ---------------------------------------------------------------------------
// Phase 2: Gaussian blur — separable, via AE's PF_Effects_World suite.
// We use AE's built-in FX_GAUSSIAN_BLUR through iterate callbacks to stay
// 100% correct, then fall back to our own separable kernel if unavailable.
// Strategy: blur each channel plane independently using the same kernel.
// ---------------------------------------------------------------------------

// Build a 1-D Gaussian kernel (normalised).
// sigma = radius/4, support = ceil(radius)+1 — gentle blur feel.
static std::vector<float> MakeBlurKernel(float radius)
{
    if (radius < 0.5f) return {};
    const float sigma = radius * 0.25f;
    const int   half  = (int)std::ceil(radius) + 1;
    const int   size  = 2 * half + 1;
    std::vector<float> k(size);
    float sum = 0.f;
    for (int i = 0; i < size; ++i) {
        float x = (float)(i - half);
        k[i] = std::exp(-(x * x) / (2.f * sigma * sigma));
        sum += k[i];
    }
    for (auto &v : k) v /= sum;
    return k;
}

// sigma = radius, support = ceil(radius*3) — wide, aggressive edge erosion.
static std::vector<float> MakeBlendKernel(float radius)
{
    if (radius < 0.5f) return {};
    const float sigma = radius;
    const int   half  = (int)std::ceil(radius * 3.f);
    const int   size  = 2 * half + 1;
    std::vector<float> k(size);
    float sum = 0.f;
    for (int i = 0; i < size; ++i) {
        float x = (float)(i - half);
        k[i] = std::exp(-(x * x) / (2.f * sigma * sigma));
        sum += k[i];
    }
    for (auto &v : k) v /= sum;
    return k;
}

// ---------------------------------------------------------------------------
// iterate_generic infrastructure for the separable Gaussian (GaussBlurPlane).
//
// This blur runs in-place: the horizontal pass must read a whole row before
// overwriting it, and the vertical pass must read a whole column before
// overwriting it, so each callback needs its own scratch buffer distinct from
// every other concurrently-running callback. Two designs were tried before
// this one:
//   1. A pool with one scratch slot per worker thread, sized by probing AE's
//      worker count (PF_Iterations_ONCE_PER_PROCESSOR) and indexed by
//      thread_indexL. Nothing guarantees the probe count matches the
//      concurrency AE actually uses for the real H/V passes — if the real
//      pass used a higher thread_indexL than the probe predicted, that
//      indexing wrote past the end of the allocated buffer. That heap
//      overflow is consistent with the scrambled/torn edge artifacts seen in
//      testing.
//   2. A fresh heap allocation inside every single row/column callback (safe,
//      no shared state) — but for a large frame that's thousands of
//      concurrent allocations, and most allocators serialize on an internal
//      lock under concurrent load, so this was measurably slow.
// Current design: thread_local storage. Each OS thread gets its own scratch
// buffer, grown once on first use and reused for every row/column that
// thread ever processes after that — safe regardless of AE's actual thread
// count (no probing, no bound to violate), and no repeated allocation.
// ---------------------------------------------------------------------------

struct MEB_BlurRefcon {
    float       *buf;
    A_long       W, H;
    const float *kdata;
    int          half;
};

extern "C" {

static PF_Err
MEB_HBlurRow(void *refconPV, A_long /*thread_indexL*/, A_long i, A_long /*iterationsL*/)
{
    const MEB_BlurRefcon *rc = static_cast<const MEB_BlurRefcon*>(refconPV);
    const A_long W    = rc->W;
    const int    half = rc->half;
    float       *row  = rc->buf + (size_t)i * W;

    // thread_local: one persistent buffer per OS thread, grown once and
    // reused across every row this thread ever processes (here and across
    // later calls/frames) — avoids a fresh heap allocation per row, which
    // under concurrent threads serializes on the allocator and was slow.
    thread_local std::vector<float> tmp;
    if ((A_long)tmp.size() < W) tmp.resize((size_t)W);

    for (A_long x = 0; x < W; ++x) {
        float acc = 0.f;
        for (int t = -half; t <= half; ++t) {
            acc += row[clampL(x + t, 0, W - 1)] * rc->kdata[t + half];
        }
        tmp[x] = acc;
    }
    for (A_long x = 0; x < W; ++x) row[x] = tmp[x];
    return PF_Err_NONE;
}

static PF_Err
MEB_VBlurCol(void *refconPV, A_long /*thread_indexL*/, A_long i, A_long /*iterationsL*/)
{
    const MEB_BlurRefcon *rc = static_cast<const MEB_BlurRefcon*>(refconPV);
    const A_long W    = rc->W;
    const A_long H    = rc->H;
    const int    half = rc->half;

    thread_local std::vector<float> tmp;
    if ((A_long)tmp.size() < H) tmp.resize((size_t)H);

    for (A_long y = 0; y < H; ++y) tmp[y] = rc->buf[(size_t)y * W + i];
    for (A_long y = 0; y < H; ++y) {
        float acc = 0.f;
        for (int t = -half; t <= half; ++t) {
            acc += tmp[clampL(y + t, 0, H - 1)] * rc->kdata[t + half];
        }
        rc->buf[(size_t)y * W + i] = acc;
    }
    return PF_Err_NONE;
}

} // extern "C"

// Separable Gaussian blur of a single float plane [H*W], in-place. Threaded
// via iterate_generic when available (see scaffold above); falls back to a
// serial pass otherwise, or if either threaded pass reports an error.
static void GaussBlurPlane(float *buf, A_long W, A_long H, const std::vector<float> &k, PF_InData *in_data)
{
    if (k.empty() || W <= 0 || H <= 0) return;
    const int half = (int)(k.size() / 2);

    if (in_data && in_data->utils && in_data->utils->iterate_generic) {
        MEB_BlurRefcon rc;
        rc.buf   = buf;
        rc.W     = W;
        rc.H     = H;
        rc.kdata = k.data();
        rc.half  = half;

        PF_Err herr = (*in_data->utils->iterate_generic)(H, &rc, MEB_HBlurRow);
        if (herr == PF_Err_NONE) {
            PF_Err verr = (*in_data->utils->iterate_generic)(W, &rc, MEB_VBlurCol);
            if (verr == PF_Err_NONE) return;
        }
        // On failure of either pass, fall through to serial — buf may be
        // partially blurred; the serial path overwrites it cleanly.
    }

    // Serial fallback.
    std::vector<float> tmp((size_t)std::max(W, H));

    // Horizontal pass
    for (A_long y = 0; y < H; ++y) {
        float *row = buf + (size_t)y * W;
        for (A_long x = 0; x < W; ++x) {
            float acc = 0.f;
            for (int t = -half; t <= half; ++t) {
                acc += row[clampL(x + t, 0, W - 1)] * k[t + half];
            }
            tmp[x] = acc;
        }
        for (A_long x = 0; x < W; ++x) row[x] = tmp[x];
    }

    // Vertical pass
    for (A_long x = 0; x < W; ++x) {
        for (A_long y = 0; y < H; ++y) tmp[y] = buf[(size_t)y * W + x];
        for (A_long y = 0; y < H; ++y) {
            float acc = 0.f;
            for (int t = -half; t <= half; ++t) {
                acc += tmp[clampL(y + t, 0, H - 1)] * k[t + half];
            }
            buf[(size_t)y * W + x] = acc;
        }
    }
}

// ---------------------------------------------------------------------------
// TEMPORARY DEBUG LOGGING — kept intentionally for now (useful during v1.3
// testing); revisit before an actual release build. Provides the per-phase
// timing breakdown used throughout Priority 3's performance work.
//
// Log lines are buffered per-thread in memory (DebugLog) and flushed to disk
// in one write per SmartRender call (DebugLogFlush, called at the cleanup:
// label) rather than opening/writing/closing the file on every single call —
// that was ~8-10 file opens per frame, and repeated file I/O can itself be
// slow under antivirus/real-time-scanning software that hooks file opens,
// which would pollute the very timings we're trying to measure.
//
// Log location: next to ModernEdgeBlur.aex itself — same physical location
// regardless of which Windows user account is running AE (unlike %TEMP%,
// which resolves to a different per-user folder on every machine, making it
// awkward to hand this build to someone else's PC). Falls back to %TEMP% if
// that folder isn't writable.
// ---------------------------------------------------------------------------
using MEB_Clock = std::chrono::steady_clock;

static thread_local std::string g_logBuffer;

// Folder the plugin .aex itself lives in, or empty if it can't be determined.
static std::string GetPluginDir()
{
#ifdef AE_OS_WIN
    HMODULE hMod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCSTR>(&GetPluginDir), &hMod) && hMod) {
        char path[MAX_PATH] = {0};
        if (GetModuleFileNameA(hMod, path, MAX_PATH) > 0) {
            std::string p(path);
            size_t pos = p.find_last_of("\\/");
            if (pos != std::string::npos) return p.substr(0, pos + 1);
        }
    }
#endif
    return std::string();
}

static void DebugLog(const char *msg)
{
    g_logBuffer += msg;
    g_logBuffer += '\n';
}

// Writes this thread's accumulated buffer out in one shot, then clears it.
// Call once per SmartRender invocation regardless of exit path.
static void DebugLogFlush()
{
    if (g_logBuffer.empty()) return;

    static const std::string pluginDir   = GetPluginDir();
    static const std::string primaryPath = pluginDir.empty() ? std::string() : (pluginDir + "ModernEdgeBlur_debug.log");
    static const std::string fallbackPath = [] {
        const char *tempDir = std::getenv("TEMP");
        return (tempDir ? std::string(tempDir) : std::string(".")) + "\\ModernEdgeBlur_debug.log";
    }();

    FILE *f = nullptr;
    if (!primaryPath.empty()) fopen_s(&f, primaryPath.c_str(), "a");
    if (!f) fopen_s(&f, fallbackPath.c_str(), "a");
    if (f) {
        fwrite(g_logBuffer.data(), 1, g_logBuffer.size(), f);
        fclose(f);
    }
    g_logBuffer.clear();
}

// Logs "TIMING: <phase>  <ms> ms" using the same debug log buffer.
static void DebugLogTiming(const char *phase, MEB_Clock::time_point t0, MEB_Clock::time_point t1)
{
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    char buf[128];
    snprintf(buf, sizeof(buf), "TIMING: %-12s %9.2f ms", phase, ms);
    DebugLog(buf);
}

// ---------------------------------------------------------------------------
// Separable Gaussian blur of an INTERLEAVED 4-channel float buffer [H*W*4],
// in-place — blurs alpha/red/green/blue together in one pass instead of 4
// sequential single-channel GaussBlurPlane calls. A Gaussian blur is
// per-channel-independent (linear, same kernel per channel), so the result is
// numerically identical to 4 separate passes either way — this is purely a
// perf restructuring, not a behaviour change. Doing it as 1 pass instead of 4
// avoids 4x the iterate_generic dispatch/join overhead and the extract-into-
// a-plane / write-back copies a per-channel loop needs (each an extra
// full-image pass). Used only by SmartRender's Phase 2 (Blur Amount) below;
// GaussBlurPlane (single-channel) is untouched and still used as-is by Blend
// Radius's weight blur — separate code path, not shared with this one.
// ---------------------------------------------------------------------------

struct MEB_BlurRGBARefcon {
    float       *buf;   // interleaved ARGB, W*H*4 floats
    A_long       W, H;
    const float *kdata;
    int          half;
};

extern "C" {

static PF_Err
MEB_HBlurRowRGBA(void *refconPV, A_long /*thread_indexL*/, A_long i, A_long /*iterationsL*/)
{
    const MEB_BlurRGBARefcon *rc = static_cast<const MEB_BlurRGBARefcon*>(refconPV);
    const A_long W    = rc->W;
    const int    half = rc->half;
    float       *row  = rc->buf + (size_t)i * W * 4;

    thread_local std::vector<float> tmp;
    if ((A_long)tmp.size() < W * 4) tmp.resize((size_t)W * 4);

    for (A_long x = 0; x < W; ++x) {
        float acc0 = 0.f, acc1 = 0.f, acc2 = 0.f, acc3 = 0.f;
        for (int t = -half; t <= half; ++t) {
            const float *p  = row + (size_t)clampL(x + t, 0, W - 1) * 4;
            const float  kw = rc->kdata[t + half];
            acc0 += p[0] * kw; acc1 += p[1] * kw;
            acc2 += p[2] * kw; acc3 += p[3] * kw;
        }
        float *o = tmp.data() + (size_t)x * 4;
        o[0] = acc0; o[1] = acc1; o[2] = acc2; o[3] = acc3;
    }
    std::memcpy(row, tmp.data(), sizeof(float) * (size_t)W * 4);
    return PF_Err_NONE;
}

static PF_Err
MEB_VBlurColRGBA(void *refconPV, A_long /*thread_indexL*/, A_long i, A_long /*iterationsL*/)
{
    const MEB_BlurRGBARefcon *rc = static_cast<const MEB_BlurRGBARefcon*>(refconPV);
    const A_long W    = rc->W;
    const A_long H    = rc->H;
    const int    half = rc->half;

    thread_local std::vector<float> tmp;
    if ((A_long)tmp.size() < H * 4) tmp.resize((size_t)H * 4);

    for (A_long y = 0; y < H; ++y) {
        const float *p = rc->buf + ((size_t)y * W + i) * 4;
        float       *o = tmp.data() + (size_t)y * 4;
        o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3];
    }
    for (A_long y = 0; y < H; ++y) {
        float acc0 = 0.f, acc1 = 0.f, acc2 = 0.f, acc3 = 0.f;
        for (int t = -half; t <= half; ++t) {
            const float *p  = tmp.data() + (size_t)clampL(y + t, 0, H - 1) * 4;
            const float  kw = rc->kdata[t + half];
            acc0 += p[0] * kw; acc1 += p[1] * kw;
            acc2 += p[2] * kw; acc3 += p[3] * kw;
        }
        float *dst = rc->buf + ((size_t)y * W + i) * 4;
        dst[0] = acc0; dst[1] = acc1; dst[2] = acc2; dst[3] = acc3;
    }
    return PF_Err_NONE;
}

} // extern "C"

static void GaussBlurRGBA(float *buf, A_long W, A_long H, const std::vector<float> &k, PF_InData *in_data)
{
    if (k.empty() || W <= 0 || H <= 0) return;
    const int half = (int)(k.size() / 2);

    if (in_data && in_data->utils && in_data->utils->iterate_generic) {
        MEB_BlurRGBARefcon rc;
        rc.buf = buf; rc.W = W; rc.H = H; rc.kdata = k.data(); rc.half = half;

        PF_Err herr = (*in_data->utils->iterate_generic)(H, &rc, MEB_HBlurRowRGBA);
        if (herr == PF_Err_NONE) {
            PF_Err verr = (*in_data->utils->iterate_generic)(W, &rc, MEB_VBlurColRGBA);
            if (verr == PF_Err_NONE) return;
        }
        // On failure of either pass, fall through to serial below.
    }

    // Serial fallback.
    std::vector<float> tmp((size_t)std::max(W, H) * 4);

    for (A_long y = 0; y < H; ++y) {
        float *row = buf + (size_t)y * W * 4;
        for (A_long x = 0; x < W; ++x) {
            float acc0 = 0.f, acc1 = 0.f, acc2 = 0.f, acc3 = 0.f;
            for (int t = -half; t <= half; ++t) {
                const float *p  = row + (size_t)clampL(x + t, 0, W - 1) * 4;
                const float  kw = k[t + half];
                acc0 += p[0] * kw; acc1 += p[1] * kw;
                acc2 += p[2] * kw; acc3 += p[3] * kw;
            }
            float *o = tmp.data() + (size_t)x * 4;
            o[0] = acc0; o[1] = acc1; o[2] = acc2; o[3] = acc3;
        }
        std::memcpy(row, tmp.data(), sizeof(float) * (size_t)W * 4);
    }

    for (A_long x = 0; x < W; ++x) {
        for (A_long y = 0; y < H; ++y) {
            const float *p = buf + ((size_t)y * W + x) * 4;
            float       *o = tmp.data() + (size_t)y * 4;
            o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3];
        }
        for (A_long y = 0; y < H; ++y) {
            float acc0 = 0.f, acc1 = 0.f, acc2 = 0.f, acc3 = 0.f;
            for (int t = -half; t <= half; ++t) {
                const float *p  = tmp.data() + (size_t)clampL(y + t, 0, H - 1) * 4;
                const float  kw = k[t + half];
                acc0 += p[0] * kw; acc1 += p[1] * kw;
                acc2 += p[2] * kw; acc3 += p[3] * kw;
            }
            float *dst = buf + ((size_t)y * W + x) * 4;
            dst[0] = acc0; dst[1] = acc1; dst[2] = acc2; dst[3] = acc3;
        }
    }
}

// ---------------------------------------------------------------------------
// Plugin entry points
// ---------------------------------------------------------------------------

static PF_Err
About(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *[], PF_LayerDef *)
{
    AEGP_SuiteHandler suites(in_data->pica_basicP);
    suites.ANSICallbacksSuite1()->sprintf(out_data->return_msg,
        "%s v%d.%d\rGaussian edge blur with sharp interior.",
        STR(StrID_Name), MAJOR_VERSION, MINOR_VERSION);
    return PF_Err_NONE;
}

static PF_Err
GlobalSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *[], PF_LayerDef *)
{
    out_data->my_version = PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION,
                                      STAGE_VERSION, BUILD_VERSION);
    out_data->out_flags  = PF_OutFlag_DEEP_COLOR_AWARE |
                           PF_OutFlag_PIX_INDEPENDENT  |
                           PF_OutFlag_USE_OUTPUT_EXTENT;
    out_data->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE       |
                           PF_OutFlag2_SUPPORTS_SMART_RENDER    |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
    return PF_Err_NONE;
}

static PF_Err
ParamsSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *[], PF_LayerDef *)
{
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Shift Edge",
        MEB_SHIFT_EDGE_MIN, MEB_SHIFT_EDGE_MAX,
        MEB_SHIFT_EDGE_MIN, MEB_SHIFT_EDGE_MAX,
        MEB_SHIFT_EDGE_DFLT, PF_Precision_HUNDREDTHS, 0, 0,
        MEB_SHIFT_EDGE_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Blur Amount",
        MEB_BLUR_AMOUNT_MIN, MEB_BLUR_AMOUNT_MAX,
        MEB_BLUR_AMOUNT_MIN, MEB_BLUR_AMOUNT_MAX,
        MEB_BLUR_AMOUNT_DFLT, PF_Precision_HUNDREDTHS, 0, 0,
        MEB_BLUR_AMOUNT_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Blend Radius",
        MEB_BLEND_RADIUS_MIN, MEB_BLEND_RADIUS_MAX,
        MEB_BLEND_RADIUS_MIN, MEB_BLEND_RADIUS_MAX,
        MEB_BLEND_RADIUS_DFLT, PF_Precision_HUNDREDTHS, 0, 0,
        MEB_BLEND_RADIUS_DISK_ID);

    out_data->num_params = MEB_NUM_PARAMS;
    return PF_Err_NONE;
}

static PF_Err
ReadParams(PF_InData *in_data, MEB_Params *p)
{
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    PF_ParamDef def;

    #define GET(IDX, FLD) \
        AEFX_CLR_STRUCT(def); \
        ERR(PF_CHECKOUT_PARAM(in_data, IDX, in_data->current_time, \
            in_data->time_step, in_data->time_scale, &def)); \
        p->FLD = def.u.fs_d.value; \
        ERR2(PF_CHECKIN_PARAM(in_data, &def));

    GET(MEB_SHIFT_EDGE,   shift_edgeF)
    GET(MEB_BLUR_AMOUNT,  blur_amountF)
    GET(MEB_BLEND_RADIUS, blend_radiusF)
    #undef GET

    return err;
}

// ---------------------------------------------------------------------------
// PreRender
// ---------------------------------------------------------------------------
static PF_Err
PreRender(PF_InData *in_data, PF_OutData *out_data, PF_PreRenderExtra *extra)
{
    PF_Err err = PF_Err_NONE;

    PF_RenderRequest req = extra->input->output_request;
    req.preserve_rgb_of_zero_alpha = FALSE;

    PF_CheckoutResult in_result;
    ERR(extra->cb->checkout_layer(in_data->effect_ref,
        MEB_INPUT, MEB_INPUT, &req,
        in_data->current_time, in_data->time_step, in_data->time_scale,
        &in_result));

    UnionLRect(&in_result.result_rect,     &extra->output->result_rect);
    UnionLRect(&in_result.max_result_rect, &extra->output->max_result_rect);

    const PF_LRect &out_req = extra->input->output_request.rect;
    extra->output->result_rect.left   = std::max(extra->output->result_rect.left,   out_req.left);
    extra->output->result_rect.top    = std::max(extra->output->result_rect.top,    out_req.top);
    extra->output->result_rect.right  = std::min(extra->output->result_rect.right,  out_req.right);
    extra->output->result_rect.bottom = std::min(extra->output->result_rect.bottom, out_req.bottom);

    return err;
}

// ---------------------------------------------------------------------------
// SmartRender
// ---------------------------------------------------------------------------
static PF_Err
SmartRender(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra)
{
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;

    AEGP_SuiteHandler suites(in_data->pica_basicP);
    PF_WorldSuite2 *wsP = nullptr;
    ERR(AEFX_AcquireSuite(in_data, out_data, kPFWorldSuite, kPFWorldSuiteVersion2,
        "", reinterpret_cast<void**>(&wsP)));

    PF_EffectWorld *src_world   = nullptr;
    PF_EffectWorld *out_world   = nullptr;
    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, MEB_INPUT, &src_world));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &out_world));

    if (err || !wsP || !src_world || !out_world) goto cleanup;

    {
        PF_PixelFormat fmt = PF_PixelFormat_INVALID;
        ERR(wsP->PF_GetPixelFormat(out_world, &fmt));
        if (err) goto cleanup;

        const A_long outW = out_world->width;
        const A_long outH = out_world->height;

        MEB_Params p; AEFX_CLR_STRUCT(p);
        ERR(ReadParams(in_data, &p));
        if (err) goto cleanup;

        // No inflation — src_world is exactly output size.
        // Unpack directly; no crop needed.
        const size_t outN = (size_t)outW * outH;
        const auto t_total0 = MEB_Clock::now();
        auto t0 = t_total0;
        float *sharp_buf = UnpackWorld(src_world, fmt);
        if (!sharp_buf) { err = PF_Err_OUT_OF_MEMORY; goto cleanup; }
        DebugLogTiming("Unpack", t0, MEB_Clock::now());

        // TEMPORARY DIAGNOSTIC — dump raw (alpha, R, G, B) across the first
        // soft-edge transition found on the middle row, straight from
        // UnpackWorld before Shift Edge/zeroing touch anything. Settles
        // whether the source is straight alpha (RGB stays near-constant as
        // alpha falls toward 0) or already premultiplied (RGB falls off
        // roughly proportionally with alpha) — remove once answered.
        // g_edgeY/g_edgeX are reused further down (Phase 4) to dump w,
        // Blur Amount's alpha, and the final composited values at the SAME
        // tracked pixel, to check whether out_rgb and out_alpha stay
        // consistent with each other through Phase 3/4.
        A_long g_edgeY = -1, g_edgeX = -1;
        {
            const A_long midY = outH / 2;
            const float *row = sharp_buf + (size_t)midY * outW * 4;
            A_long edgeX = -1;
            for (A_long x = 1; x < outW; ++x) {
                const float a0 = row[(x - 1) * 4 + 0];
                const float a1 = row[x * 4 + 0];
                if ((a0 <= 0.01f && a1 > 0.01f) || (a0 >= 0.99f && a1 < 0.99f) ||
                    (a1 > 0.01f && a1 < 0.99f)) {
                    edgeX = x;
                    break;
                }
            }
            if (edgeX >= 0) {
                g_edgeY = midY;
                g_edgeX = edgeX;
                char buf[256];
                snprintf(buf, sizeof(buf), "EDGE DUMP: row=%ld, edge found at x=%ld", (long)midY, (long)edgeX);
                DebugLog(buf);
                const A_long lo = std::max((A_long)0, edgeX - 10);
                const A_long hi = std::min(outW - 1, edgeX + 10);
                for (A_long x = lo; x <= hi; ++x) {
                    const float *p = row + (size_t)x * 4;
                    snprintf(buf, sizeof(buf), "  x=%ld  a=%.4f  r=%.4f  g=%.4f  b=%.4f", (long)x, p[0], p[1], p[2], p[3]);
                    DebugLog(buf);
                }
            } else {
                DebugLog("EDGE DUMP: no soft/transition edge found on middle row (subject may not cross it)");
            }
        }

        // Phase 1: Shift Edge — stubbed, only runs when non-zero (default=0).
        // Must run BEFORE the zero-RGB pass below: it changes the alpha shape
        // (grow/shrink), and the zeroing needs to key off the FINAL alpha, not
        // the pre-shift one — otherwise a grow would reveal newly-alpha>0
        // pixels whose RGB we'd already zeroed (black instead of real colour),
        // and a shrink would leave a leftover-colour ring we never got to zero.
        t0 = MEB_Clock::now();
        if (std::abs(p.shift_edgeF) >= 0.5f) {
            ShiftEdgeAlpha(sharp_buf, outW, outH, (float)p.shift_edgeF);
        }
        DebugLogTiming("ShiftEdge", t0, MEB_Clock::now());

        // Force honest premult ourselves: zero RGB wherever alpha is 0.
        // Requesting preserve_rgb_of_zero_alpha=FALSE on checkout asks AE to
        // discard hidden RGB behind zero-alpha pixels, but that's only a
        // courtesy for data AE itself manages (e.g. native masks). It can't
        // retroactively clean pixels an UPSTREAM effect already baked in —
        // e.g. a precomp with an internal color keyer that only ever touches
        // alpha and leaves the original un-keyed footage colour sitting
        // underneath.
        t0 = MEB_Clock::now();
        for (size_t i = 0; i < outN; ++i) {
            if (sharp_buf[i * 4 + 0] <= 0.f) {
                sharp_buf[i * 4 + 1] = 0.f;
                sharp_buf[i * 4 + 2] = 0.f;
                sharp_buf[i * 4 + 3] = 0.f;
            }
        }
        DebugLogTiming("ZeroRGB", t0, MEB_Clock::now());

        // Capture the (now honestly zeroed) premultiplied RGBA — this is what
        // Phase 2 blurs. See the Phase 2 comment: we blur this directly, we do
        // NOT treat it as straight/un-multiplied data.
        float *blurred_buf = new (std::nothrow) float[outN * 4];
        if (!blurred_buf) { delete[] sharp_buf; err = PF_Err_OUT_OF_MEMORY; goto cleanup; }
        std::memcpy(blurred_buf, sharp_buf, outN * 4 * sizeof(float));

        // -----------------------------------------------------------------
        // Passthrough: if no processing is requested, copy sharp to output
        // and exit. Preserves held RGB exactly as AE delivered it.
        // -----------------------------------------------------------------
        const bool do_blur  = (p.blur_amountF  >= 0.5f);
        const bool do_blend = (p.blend_radiusF >= 0.5f);

        if (!do_blur && !do_blend) {
            for (A_long y = 0; y < outH; ++y) {
                void *outRow = reinterpret_cast<char*>(out_world->data) + y * out_world->rowbytes;
                PackRow(sharp_buf + (size_t)y * outW * 4, outRow, outW, fmt);
            }
            delete[] sharp_buf;
            delete[] blurred_buf;
            goto cleanup;
        }

        // -----------------------------------------------------------------
        // Phase 2: Blurred base.
        // blurred_buf holds the zeroed data (0 outside the matte, true colour
        // inside — see the zero-RGB pass above). Blur it with GaussBlurRGBA,
        // then DIVIDE the blurred RGB by the blurred alpha — NOT multiply.
        // This is the actual bug in every earlier attempt: zeroing RGB where
        // alpha=0 forces the blur kernel to average real edge colour together
        // with hard black, which darkens the estimate near any edge — the
        // exact same pollution problem the white background caused before
        // zeroing, just black instead of white. For a locally-constant true
        // colour C, blur(C·alpha)/blur(alpha) recovers C exactly regardless
        // of how much of the kernel's support falls outside the matte, since
        // the division cancels the coverage-dependent attenuation instead of
        // compounding it. This makes blurred_buf hold the BLURRED LAYER'S TRUE
        // COLOUR (not premultiplied) after this step — Phase 4 premultiplies
        // once, at the very end, after blending with the sharp layer's colour.
        // Division is guarded near alpha=0 (unstable/meaningless there; RGB
        // is left as whatever near-zero value it already has, which won't
        // matter since it's multiplied by ~0 alpha at composite time anyway).
        //
        // No AE world needed here — AE's native convolve utility measured
        // 10-20x slower than our own threaded blur (see NEXT_STEPS.md), so we
        // no longer route through it at all. Blur the plain float buffer
        // directly via GaussBlurRGBA, no PF_NewWorld/PackRow/UnpackWorld/
        // DisposeWorld round trip needed.
        // -----------------------------------------------------------------
        t0 = MEB_Clock::now();
        if (do_blur) {
            const auto k = MakeBlurKernel((float)p.blur_amountF);

            auto ts = MEB_Clock::now();
            GaussBlurRGBA(blurred_buf, outW, outH, k, in_data);
            DebugLogTiming("  GaussBlurRGBA", ts, MEB_Clock::now());

            ts = MEB_Clock::now();
            // Clamp the divisor rather than branching on it — a hard
            // if (a > epsilon) cutoff makes the recovered colour jump
            // discontinuously right at the threshold (divided-and-correct on
            // one side, left-raw-and-much-darker on the other), and that
            // internal jump survives the final premultiply as a visible thin
            // dark ring wherever the blurred alpha happens to cross epsilon.
            // Clamping instead keeps inv_a continuous everywhere: it matches
            // the true division above epsilon and smoothly saturates instead
            // of jumping below it, so there's no seam to show up as fringing.
            const float kAlphaNormEpsilon = 0.02f;
            for (size_t i = 0; i < outN; ++i) {
                const float a      = blurred_buf[i * 4 + 0];
                const float inv_a  = 1.0f / std::max(a, kAlphaNormEpsilon);
                blurred_buf[i * 4 + 1] *= inv_a;
                blurred_buf[i * 4 + 2] *= inv_a;
                blurred_buf[i * 4 + 3] *= inv_a;
            }
            DebugLogTiming("  Unpremultiply", ts, MEB_Clock::now());
        } else {
            std::memcpy(blurred_buf, sharp_buf, outN * 4 * sizeof(float));
        }
        DebugLogTiming("BlurAmount", t0, MEB_Clock::now());

        // -----------------------------------------------------------------
        // Phase 3: Blend Radius — erode the sharp (top) layer's reveal mask.
        // 1. Erode the sharp alpha inward by blend_radius (octagon erosion —
        //    4-direction approximation of a circular min-filter, much cheaper
        //    than a true 2D circular scan; see OctagonErode) — this shrinks
        //    the region where the sharp layer is shown at full strength.
        // 2. Gaussian-blur the eroded mask to feather the new, smaller edge.
        // w is purely the sharp-vs-blurred blend weight: at w=1 the sharp
        // layer shows fully, at w=0 only the blurred layer shows through.
        // Increasing Blend Radius erodes w inward, revealing more of the
        // blurred layer at the edge; at extreme radius w→0 everywhere and
        // the sharp layer is fully eroded away. Output alpha is untouched —
        // it still comes from Blur Amount's blurred alpha (Phase 4).
        // -----------------------------------------------------------------
        t0 = MEB_Clock::now();
        std::vector<float> sharp_alpha(outN);
        for (size_t i = 0; i < outN; ++i) sharp_alpha[i] = sharp_buf[i * 4 + 0];

        float *weight = new (std::nothrow) float[outN];
        if (!weight) {
            delete[] sharp_buf;
            delete[] blurred_buf;
            err = PF_Err_OUT_OF_MEMORY;
            goto cleanup;
        }

        OctagonErode(sharp_alpha.data(), weight, outW, outH, (float)p.blend_radiusF, in_data);

        const auto blend_k = MakeBlendKernel((float)p.blend_radiusF);
        if (!blend_k.empty()) GaussBlurPlane(weight, outW, outH, blend_k, in_data);

        for (size_t i = 0; i < outN; ++i) {
            weight[i] = clampF(weight[i], 0.f, 1.f);
        }
        DebugLogTiming("BlendRadius", t0, MEB_Clock::now());

        // -----------------------------------------------------------------
        // Phase 4: Composite and pack output.
        // sharp_buf's RGB is true colour (held, zero only at alpha=0 — see
        // the zero-RGB pass). blurred_buf's RGB is now ALSO true colour as of
        // Phase 2's unpremultiply-by-division step, not premultiplied. Blend
        // the two TRUE colours by w, then premultiply ONCE at the very end by
        // out_alpha — not before. Multiplying earlier (per-layer, before this
        // blend) is exactly what caused the black-halo bug: it darkens each
        // layer's colour individually wherever ITS OWN alpha is low, before
        // the blend even happens, rather than letting the blend produce a
        // clean true-colour estimate first and only fading it via alpha once,
        // at the end.
        // out_rgb   = lerp(blurred_true_rgb, sharp_true_rgb, w) × out_alpha
        // out_alpha = blurred_alpha  (unchanged — still Blur Amount's alpha)
        // -----------------------------------------------------------------
        t0 = MEB_Clock::now();
        std::vector<float> trackedOutRow;
        {
            std::vector<float> out_row((size_t)outW * 4);

            for (A_long y = 0; y < outH; ++y) {
                const float *sh  = sharp_buf   + (size_t)y * outW * 4;
                const float *bl  = blurred_buf + (size_t)y * outW * 4;
                const float *wt  = weight      + (size_t)y * outW;
                float       *dst = out_row.data();

                for (A_long x = 0; x < outW; ++x) {
                    const float w      = wt[x];
                    const float inv    = 1.f - w;
                    const float outA   = bl[0];
                    dst[0] = outA;
                    dst[1] = (sh[1] * w + bl[1] * inv) * outA;
                    dst[2] = (sh[2] * w + bl[2] * inv) * outA;
                    dst[3] = (sh[3] * w + bl[3] * inv) * outA;
                    sh += 4; bl += 4; dst += 4;
                }

                if (y == g_edgeY) trackedOutRow = out_row;

                void *outRow = reinterpret_cast<char*>(out_world->data) + y * out_world->rowbytes;
                PackRow(out_row.data(), outRow, outW, fmt);
            }
        }
        DebugLogTiming("Composite", t0, MEB_Clock::now());

        // TEMPORARY DIAGNOSTIC — dump w, Blur Amount's alpha, sharp/blurred
        // RGB, and the final composited RGBA across the same tracked edge
        // pixel range as the EDGE DUMP above. Checks whether out_rgb and
        // out_alpha stay consistent (out_alpha comes only from Blur Amount's
        // blur, while the RGB blend weight w comes from Blend Radius's
        // separate erosion+feather — if the two disagree near the edge,
        // out_rgb can show meaningful colour while out_alpha has already
        // dropped near zero, which is itself an invalid-premult condition
        // independent of anything Phase 2 does). Remove once answered.
        if (g_edgeX >= 0 && !trackedOutRow.empty()) {
            char buf[320];
            snprintf(buf, sizeof(buf), "PHASE4 DUMP: row=%ld", (long)g_edgeY);
            DebugLog(buf);
            const A_long lo = std::max((A_long)0, g_edgeX - 10);
            const A_long hi = std::min(outW - 1, g_edgeX + 10);
            for (A_long x = lo; x <= hi; ++x) {
                const float *sh = sharp_buf   + ((size_t)g_edgeY * outW + x) * 4;
                const float *bl = blurred_buf + ((size_t)g_edgeY * outW + x) * 4;
                const float  w  = weight[(size_t)g_edgeY * outW + x];
                const float *o  = trackedOutRow.data() + (size_t)x * 4;
                snprintf(buf, sizeof(buf),
                    "  x=%ld  w=%.4f  sh(a=%.3f r=%.3f)  bl(a=%.3f r=%.3f)  out(a=%.4f r=%.4f g=%.4f b=%.4f)",
                    (long)x, w, sh[0], sh[1], bl[0], bl[1], o[0], o[1], o[2], o[3]);
                DebugLog(buf);
            }
        }

        DebugLogTiming("TOTAL", t_total0, MEB_Clock::now());
        DebugLog("---");

        delete[] weight;
        delete[] blurred_buf;
        delete[] sharp_buf;
    }

cleanup:
    DebugLogFlush();
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
        "Modern Edge Blur", "MM Modern Edge Blur", "MM",
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
