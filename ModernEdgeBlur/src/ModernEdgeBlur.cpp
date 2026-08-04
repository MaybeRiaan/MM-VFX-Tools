/*  ModernEdgeBlur.cpp  —  v1.2

    Pipeline (per frame):
      1. [Shift Edge]  Stubbed — morphological grow/shrink on alpha. No-op when = 0 (default).
      2. [Blur Amount] Premultiply (rgb *= alpha, every pixel) a copy of sharp_buf, Gaussian
                       blur it whole (GaussBlurRGBA, threaded via iterate_generic, sigma =
                       radius * 0.5), then UNPREMULTIPLY (rgb /= max(blurred alpha, 1e-4))
                       → blurred base (A) in straight colour. Premult space for the blur
                       itself, straight colour out — matching AE's straight-alpha effect
                       pipeline on both sides. No AE world/native convolve involved — see
                       Phase 2 comment in SmartRender.
      3. [Blend Radius] Erode sharp alpha inward by radius (octagon erosion — 4-direction
                        approximation of a circular min-filter, threaded via iterate_generic),
                        then Gaussian-blur to feather (also threaded via iterate_generic) →
                        weight w = clamp(eroded+blurred, 0, 1). w controls how much sharp
                        (top) layer shows vs blurred (base) layer — eroding w reveals more
                        of the blurred base at the edge.
      4. [Composite]   out_rgb   = (sharp_rgb·w·sharp_a + A_rgb·(1-w)·A_a) / (w·sharp_a + (1-w)·A_a)
                       — alpha-weighted blend in straight colour, NO alpha multiply on the
                       result; AE applies alpha itself at composite time. Reduces to a plain
                       lerp in the interior; drops the sharp term where sharp coverage is 0.
                       out_alpha = A_alpha  (blurred alpha = soft feathered edge, unchanged by Blend Radius)

    GPU: the same pipeline, pass for pass, in src/ModernEdgeBlur_GPU.cpp (CUDA —
         the framework AE-on-Windows actually offers effects). AE picks; when it
         picks the CPU, this file runs unchanged. Every formula the two paths
         share now lives in core/MEB_CoreMath.h (CPU) and src/gpu/MEB_KernelMath.h
         + MEB_KernelPasses.h (GPU), and tests/test_gpuparity.cpp diffs them with
         `==`. A formula changes in BOTH in the same commit, or not at all.

    Passthrough: Blur=0 and Blend=0 → input copied directly to output, held RGB preserved.
    Premult: preserve_rgb_of_zero_alpha=FALSE on checkout, PLUS an explicit zero-RGB-
             where-alpha-is-0 pass on sharp_buf right after unpacking. The checkout flag
             alone isn't enough — it only lets AE discard hidden RGB it manages itself,
             it can't retroactively clean pixels an upstream effect already baked in (e.g.
             a precomp with an internal keyer that only touches alpha, leaving un-keyed
             footage colour sitting underneath). Enforcing it ourselves means honest
             premult holds regardless of what the source actually did. sharp_buf itself
             stays straight/true-colour (zeroed cliff only) for the sharp-layer contribution;
             a SEPARATE copy goes through premultiply → blur → unpremultiply for Blur Amount
             (see Phase 2 comment). Both halves matter: blurring premultiplied means a
             partial-alpha edge pixel contributes only its coverage-weighted share of colour
             to the kernel sum (no black contamination), and unpremultiplying after means
             the output is straight colour, which is what AE expects back — handing back
             premultiplied colour makes AE apply alpha a second time (colour×alpha²), a
             dark halo on every source including the transparency grid.
    No input inflation — src_world is exactly output size.
*/

#define NOMINMAX
#include "ModernEdgeBlur.h"
#include "ModernEdgeBlur_GPU.h"   // device lifecycle + GPU SmartRender

// Every per-pixel formula this file evaluates now lives here, AE-free, so the
// GPU path can be diffed against the REAL CPU code with no AE SDK in the loop
// (tests/test_gpuparity.cpp). Nothing about the pipeline changed: the callbacks
// below keep their thread_local scratch, their column blocking and MEB_Pool
// exactly as before — only the innermost arithmetic became a call.
#include "core/MEB_CoreMath.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <chrono>
#include <new>

// MEB_CoreMath.h spells A_long as its own MEB_Long so it can stay free of the
// AE SDK. If a future SDK ever widened one of them, every index in the shared
// math would silently truncate.
static_assert(sizeof(A_long) == sizeof(MEB_Long),
              "MEB_Long must match A_long — see src/core/MEB_CoreMath.h");

// ---------------------------------------------------------------------------
// Per-render-thread buffer pool. SmartRender previously allocated ~640MB of
// fresh heap per 4.6K frame (sharp + blurred + weight + alpha + morph
// scratch); the OS commits and zeroes new pages on first touch, a cost that
// smeared invisibly across every phase's timing. thread_local + grow-once
// resize means each of AE's render threads pays that once and then reuses
// the same warm pages every frame (same pattern as the blur callbacks'
// scratch). Trade-off: the pool is retained per render thread (~0.6GB each
// at 4.6K float) for as long as AE keeps the thread alive — acceptable for
// MFR's typical 2-4 concurrent renders, revisit if AE spawns more.
// ---------------------------------------------------------------------------
struct MEB_Pool {
    std::vector<float> sharp;    // W*H*4, straight colour (zeroed at a<=0)
    std::vector<float> blurred;  // W*H*4
    std::vector<float> weight;   // W*H
    std::vector<float> alpha;    // W*H
};
static thread_local MEB_Pool g_pool;

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

// MorphPlane — the circular min/max filter behind Shift Edge — now lives in
// core/MEB_CoreMath.h, alongside every other formula the GPU has to match.

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
    MEB_DirMinRowInline(rc->in, rc->out + (size_t)y * rc->W,
                        rc->W, rc->H, y, rc->dx, rc->dy, rc->halfLen);
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

    // Serial fallback — same row body as the threaded callback above.
    for (A_long y = 0; y < H; ++y)
        MEB_DirMinRowInline(in, out + (size_t)y * W, W, H, y, dx, dy, halfLen);
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

    // Half-lengths come from the shared helpers (core/MEB_CoreMath.h) so the
    // GPU pass sequence cannot disagree with this one about the octagon's size.
    const int a = MEB_OctagonAxisHalf(radius);   // sqrt(2)-1, clamped to >=1
    const int c = MEB_OctagonDiagHalf(radius);   // 1-1/sqrt(2), may be 0

    // Grow-once per render thread, reused every frame (see MEB_Pool comment).
    thread_local std::vector<float> tmp;
    if (tmp.size() < (size_t)W * H) tmp.resize((size_t)W * H);

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

// MakeBlurKernel (sigma = radius/2, support ceil(3 sigma)) and MakeBlendKernel
// (sigma = radius, support ceil(3*radius)) now live in core/MEB_CoreMath.h.
// The GPU path calls those same builders on the host and uploads the resulting
// float array verbatim, so both paths convolve with identical bits — see
// gpu-kernel-rules.md §3.1 for why a GPU-side rebuild is not acceptable.

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

    MEB_BlurRowPlane(row, tmp.data(), W, rc->kdata, half);
    for (A_long x = 0; x < W; ++x) row[x] = tmp[x];
    return PF_Err_NONE;
}

// Vertical pass runs on BLOCKS of columns, not single columns. One column at
// a time means every kernel tap strides a full row pitch (~18KB at 4.6K) —
// each tap a fresh cache line and TLB entry, the same cache-hostile access
// pattern that made AE's native convolve 10-20x slower than it should be
// (see NEXT_STEPS.md). Copying a block of adjacent columns into a compact
// row-major scratch first makes the convolution's working set a sliding
// window of kernel_size × block floats (a few KB — stays in L1), and the
// inner per-column loop contiguous, which auto-vectorizes.
static const int MEB_PLANE_VBLOCK = 64; // columns per vertical-pass callback

static PF_Err
MEB_VBlurCols(void *refconPV, A_long /*thread_indexL*/, A_long i, A_long /*iterationsL*/)
{
    const MEB_BlurRefcon *rc = static_cast<const MEB_BlurRefcon*>(refconPV);
    const A_long W    = rc->W;
    const A_long H    = rc->H;
    const int    half = rc->half;
    const A_long c0   = i * MEB_PLANE_VBLOCK;
    const A_long nc   = std::min((A_long)MEB_PLANE_VBLOCK, W - c0);

    thread_local std::vector<float> tmp;
    if ((A_long)tmp.size() < H * MEB_PLANE_VBLOCK) tmp.resize((size_t)H * MEB_PLANE_VBLOCK);

    // Gather the column block into compact row-major layout [y][c].
    for (A_long y = 0; y < H; ++y)
        std::memcpy(tmp.data() + (size_t)y * nc, rc->buf + (size_t)y * W + c0, sizeof(float) * nc);

    float acc[MEB_PLANE_VBLOCK];
    for (A_long y = 0; y < H; ++y) {
        MEB_BlurColsPlane(tmp.data(), y, H, nc, rc->kdata, half, acc);
        std::memcpy(rc->buf + (size_t)y * W + c0, acc, sizeof(float) * nc);
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
            const A_long nBlocks = (W + MEB_PLANE_VBLOCK - 1) / MEB_PLANE_VBLOCK;
            PF_Err verr = (*in_data->utils->iterate_generic)(nBlocks, &rc, MEB_VBlurCols);
            if (verr == PF_Err_NONE) return;
        }
        // On failure of either pass, fall through to serial — buf may be
        // partially blurred; the serial path overwrites it cleanly.
    }

    // Serial fallback — same row/column bodies as the threaded callbacks, with
    // the vertical pass degenerating to a single-column block (nc = 1).
    std::vector<float> tmp((size_t)std::max(W, H));

    // Horizontal pass
    for (A_long y = 0; y < H; ++y) {
        float *row = buf + (size_t)y * W;
        MEB_BlurRowPlane(row, tmp.data(), W, k.data(), half);
        for (A_long x = 0; x < W; ++x) row[x] = tmp[x];
    }

    // Vertical pass
    for (A_long x = 0; x < W; ++x) {
        for (A_long y = 0; y < H; ++y) tmp[y] = buf[(size_t)y * W + x];
        for (A_long y = 0; y < H; ++y) {
            float acc;
            MEB_BlurColsPlane(tmp.data(), y, H, 1, k.data(), half, &acc);
            buf[(size_t)y * W + x] = acc;
        }
    }
}

// ---------------------------------------------------------------------------
// DEBUG LOGGING — compiled out by default (MEB_DEBUG_LOG 0) for distribution
// builds. Flip to 1 to re-enable the per-phase timing breakdown and the
// EDGE/PHASE4 pixel dumps for the next performance pass — all call sites
// stay in place; the bodies and the dump blocks become no-ops when disabled.
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
#define MEB_DEBUG_LOG 0   // 1 = per-frame timing log + pixel dumps, 0 = silent

using MEB_Clock = std::chrono::steady_clock;

#if MEB_DEBUG_LOG
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
#else
// Logging disabled — inline no-ops, call sites unchanged.
static inline void DebugLog(const char *) {}
static inline void DebugLogFlush() {}
static inline void DebugLogTiming(const char *, MEB_Clock::time_point, MEB_Clock::time_point) {}
#endif // MEB_DEBUG_LOG

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
    bool         unpremult; // divide RGB by blurred alpha as the V pass writes
                            // out — folds Phase 2's unpremultiply into the
                            // blur's last pass instead of a separate
                            // full-buffer walk afterwards
};

// See MEB_VBlurCols for why the vertical pass works on column BLOCKS.
// 16 RGBA pixels = 64 floats per block row — contiguous inner loops that
// auto-vectorize, sliding-window working set that stays in L1.
static const int MEB_RGBA_VBLOCK = 16;

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

    MEB_BlurRowRGBA(row, tmp.data(), W, rc->kdata, half);
    std::memcpy(row, tmp.data(), sizeof(float) * (size_t)W * 4);
    return PF_Err_NONE;
}

static PF_Err
MEB_VBlurColsRGBA(void *refconPV, A_long /*thread_indexL*/, A_long i, A_long /*iterationsL*/)
{
    const MEB_BlurRGBARefcon *rc = static_cast<const MEB_BlurRGBARefcon*>(refconPV);
    const A_long W    = rc->W;
    const A_long H    = rc->H;
    const int    half = rc->half;
    const A_long c0   = i * MEB_RGBA_VBLOCK;             // first pixel column
    const A_long nc   = std::min((A_long)MEB_RGBA_VBLOCK, W - c0);
    const A_long nf   = nc * 4;                          // floats per block row

    thread_local std::vector<float> tmp;
    if ((A_long)tmp.size() < H * MEB_RGBA_VBLOCK * 4) tmp.resize((size_t)H * MEB_RGBA_VBLOCK * 4);

    // Adjacent pixel columns are contiguous in the interleaved buffer, so a
    // block row gathers with a single memcpy.
    for (A_long y = 0; y < H; ++y)
        std::memcpy(tmp.data() + (size_t)y * nf, rc->buf + ((size_t)y * W + c0) * 4, sizeof(float) * nf);

    float acc[MEB_RGBA_VBLOCK * 4];
    for (A_long y = 0; y < H; ++y) {
        MEB_BlurColsRGBA(tmp.data(), y, H, nf, rc->kdata, half, rc->unpremult, acc);
        std::memcpy(rc->buf + ((size_t)y * W + c0) * 4, acc, sizeof(float) * nf);
    }
    return PF_Err_NONE;
}

} // extern "C"

static void GaussBlurRGBA(float *buf, A_long W, A_long H, const std::vector<float> &k, PF_InData *in_data,
                          bool unpremult_after)
{
    if (k.empty() || W <= 0 || H <= 0) return;
    const int half = (int)(k.size() / 2);

    if (in_data && in_data->utils && in_data->utils->iterate_generic) {
        MEB_BlurRGBARefcon rc;
        rc.buf = buf; rc.W = W; rc.H = H; rc.kdata = k.data(); rc.half = half;
        rc.unpremult = unpremult_after;

        PF_Err herr = (*in_data->utils->iterate_generic)(H, &rc, MEB_HBlurRowRGBA);
        if (herr == PF_Err_NONE) {
            const A_long nBlocks = (W + MEB_RGBA_VBLOCK - 1) / MEB_RGBA_VBLOCK;
            PF_Err verr = (*in_data->utils->iterate_generic)(nBlocks, &rc, MEB_VBlurColsRGBA);
            if (verr == PF_Err_NONE) return;
        }
        // On failure of either pass, fall through to serial below.
    }

    // Serial fallback — same row/column bodies as the threaded callbacks, with
    // the vertical pass degenerating to a single-pixel-wide block (nf = 4).
    std::vector<float> tmp((size_t)std::max(W, H) * 4);

    for (A_long y = 0; y < H; ++y) {
        float *row = buf + (size_t)y * W * 4;
        MEB_BlurRowRGBA(row, tmp.data(), W, k.data(), half);
        std::memcpy(row, tmp.data(), sizeof(float) * (size_t)W * 4);
    }

    for (A_long x = 0; x < W; ++x) {
        for (A_long y = 0; y < H; ++y) {
            const float *p = buf + ((size_t)y * W + x) * 4;
            float       *o = tmp.data() + (size_t)y * 4;
            o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3];
        }
        for (A_long y = 0; y < H; ++y) {
            float acc[4];
            MEB_BlurColsRGBA(tmp.data(), y, H, 4, k.data(), half, unpremult_after, acc);
            float *dst = buf + ((size_t)y * W + x) * 4;
            dst[0] = acc[0]; dst[1] = acc[1]; dst[2] = acc[2]; dst[3] = acc[3];
        }
    }
}

// ---------------------------------------------------------------------------
// Fused per-row pipeline passes, threaded via iterate_generic.
//
// The pipeline is memory-bandwidth-bound: a 4.6K float frame is ~233MB, and
// the previous structure walked it serially many separate times (unpack,
// zero-RGB, copy-to-blurred, premultiply, unpremultiply, alpha extract,
// weight clamp, composite, pack) — ~245ms/frame of single-threaded memory
// passes before any actual filtering. These callbacks fuse the work at each
// end of the pipeline into one cache-hot walk per row, run across AE's
// worker threads:
//
//   INGEST  (one pass): unpack AE world row → sharp (straight colour), zero
//           RGB at the a<=0 cliff, and derive in the same pass whatever this
//           frame needs: the premultiplied (or plain) copy for Blur Amount
//           and the alpha plane for Blend Radius's erosion.
//   DERIVE  (only when Shift Edge ran): the zero+derive half of ingest by
//           itself — Shift Edge changes the alpha shape in-place between
//           unpack and derivation, so the derived buffers must be rebuilt
//           from the post-shift alpha (see the Shift Edge ordering note in
//           NEXT_STEPS.md Priority 2).
//   COMPOSITE (one pass): clamp w, alpha-weighted blend, pack straight into
//           the output world row.
//   PACK    (passthrough only): pack sharp rows straight to output.
//
// Unpremultiply is not here — it's folded into the tail of the blur's own
// vertical pass (MEB_VBlurColsRGBA), which touches every pixel anyway.
// ---------------------------------------------------------------------------

// MEB_DeriveRowInline — zero RGB at the a<=0 cliff and derive the per-frame
// buffers — now lives in core/MEB_CoreMath.h.

struct MEB_IngestRefcon {
    const PF_EffectWorld *world;
    PF_PixelFormat        fmt;
    float                *sharp;    // W*H*4
    float                *blurred;  // W*H*4 or null
    float                *alpha;    // W*H   or null
    A_long                W;
    bool                  do_derive;       // false when Shift Edge must run first
    bool                  premult_blurred;
};

struct MEB_DeriveRefcon {
    float  *sharp;
    float  *blurred;  // or null
    float  *alpha;    // or null
    A_long  W;
    bool    premult_blurred;
};

struct MEB_CompositeRefcon {
    const float    *sharp;
    const float    *blurred;
    const float    *weight;   // unclamped; clamped inline here
    PF_EffectWorld *out_world;
    PF_PixelFormat  fmt;
    A_long          W;
    A_long          trackRow; // -1, or the row to capture for the PHASE4 dump
    float          *trackBuf; // W*4 capture destination (only trackRow writes)
};

struct MEB_PackRefcon {
    const float    *src;      // W*H*4 straight-colour buffer
    PF_EffectWorld *out_world;
    PF_PixelFormat  fmt;
    A_long          W;
};

extern "C" {

static PF_Err
MEB_IngestRow(void *refconPV, A_long /*thread_indexL*/, A_long y, A_long /*iterationsL*/)
{
    const MEB_IngestRefcon *rc = static_cast<const MEB_IngestRefcon*>(refconPV);
    const A_long W = rc->W;
    const void *srcRow = reinterpret_cast<const char*>(rc->world->data) + (size_t)y * rc->world->rowbytes;
    float *sh = rc->sharp + (size_t)y * W * 4;
    UnpackRow(srcRow, sh, W, rc->fmt);
    if (rc->do_derive) {
        MEB_DeriveRowInline(sh,
                            rc->blurred ? rc->blurred + (size_t)y * W * 4 : nullptr,
                            rc->alpha   ? rc->alpha   + (size_t)y * W     : nullptr,
                            W, rc->premult_blurred);
    }
    return PF_Err_NONE;
}

static PF_Err
MEB_DeriveRow(void *refconPV, A_long /*thread_indexL*/, A_long y, A_long /*iterationsL*/)
{
    const MEB_DeriveRefcon *rc = static_cast<const MEB_DeriveRefcon*>(refconPV);
    const A_long W = rc->W;
    MEB_DeriveRowInline(rc->sharp + (size_t)y * W * 4,
                        rc->blurred ? rc->blurred + (size_t)y * W * 4 : nullptr,
                        rc->alpha   ? rc->alpha   + (size_t)y * W     : nullptr,
                        W, rc->premult_blurred);
    return PF_Err_NONE;
}

static PF_Err
MEB_CompositeRow(void *refconPV, A_long /*thread_indexL*/, A_long y, A_long /*iterationsL*/)
{
    const MEB_CompositeRefcon *rc = static_cast<const MEB_CompositeRefcon*>(refconPV);
    const A_long W = rc->W;
    const float *sh = rc->sharp   + (size_t)y * W * 4;
    const float *bl = rc->blurred + (size_t)y * W * 4;
    const float *wt = rc->weight  + (size_t)y * W;

    thread_local std::vector<float> rowv;
    if ((A_long)rowv.size() < W * 4) rowv.resize((size_t)W * 4);
    float *dst = rowv.data();

    for (A_long x = 0; x < W; ++x) {
        MEB_CompositePixel(sh, bl, wt[x], dst);
        sh += 4; bl += 4; dst += 4;
    }

    if (y == rc->trackRow && rc->trackBuf)
        std::memcpy(rc->trackBuf, rowv.data(), sizeof(float) * (size_t)W * 4);

    void *outRow = reinterpret_cast<char*>(rc->out_world->data) + (size_t)y * rc->out_world->rowbytes;
    PackRow(rowv.data(), outRow, W, rc->fmt);
    return PF_Err_NONE;
}

static PF_Err
MEB_PackRow_cb(void *refconPV, A_long /*thread_indexL*/, A_long y, A_long /*iterationsL*/)
{
    const MEB_PackRefcon *rc = static_cast<const MEB_PackRefcon*>(refconPV);
    void *outRow = reinterpret_cast<char*>(rc->out_world->data) + (size_t)y * rc->out_world->rowbytes;
    PackRow(rc->src + (size_t)y * rc->W * 4, outRow, rc->W, rc->fmt);
    return PF_Err_NONE;
}

} // extern "C"

// Run a per-row callback threaded when iterate_generic is available, serially
// otherwise. The callbacks only require disjoint row writes, so a plain
// serial loop over the same function is an exact fallback.
static void MEB_ForEachRow(PF_InData *in_data, A_long rows, void *refcon,
                           PF_Err (*cb)(void*, A_long, A_long, A_long))
{
    if (in_data && in_data->utils && in_data->utils->iterate_generic) {
        if ((*in_data->utils->iterate_generic)(rows, refcon, cb) == PF_Err_NONE) return;
    }
    for (A_long y = 0; y < rows; ++y) cb(refcon, 0, y, rows);
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

    // These must match resources/ModernEdgeBlurPiPL.r literally, or AE rejects
    // the plugin at load with "global outflags mismatch" (25::16).
    //
    // USE_OUTPUT_EXTENT is KEPT. AE_GPU_Effects.md §2.1 says not to set it,
    // because Adobe's AE GPU sample declares 0x02000400 — but §8 hypothesis 5
    // records that it was actually tested and is irrelevant to the GPU gate
    // ("wrong both ways"). It IS part of how this effect's render region
    // behaves today, so it stays; the GPU path is additive here, not a
    // rewrite.
    out_data->out_flags  = PF_OutFlag_DEEP_COLOR_AWARE |
                           PF_OutFlag_PIX_INDEPENDENT  |
                           PF_OutFlag_USE_OUTPUT_EXTENT;
    out_data->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE       |
                           PF_OutFlag2_SUPPORTS_SMART_RENDER    |
                           PF_OutFlag2_SUPPORTS_THREADED_RENDERING;

    // GPU declaration, exactly as Adobe's AE GPU sample does it: announce F32 +
    // DIRECTX for every host EXCEPT Premiere, which wants pixel formats instead
    // and which this plugin does not target (its flags stay as they were).
    //
    // SUPPORTS_DIRECTX_RENDERING does NOT force the effect onto DirectX — Adobe
    // sets it unconditionally in this same branch and still receives
    // what_gpu = CUDA. If AE ever did offer DIRECTX, MEB_GPUDeviceSetup declines
    // it quietly and the CPU path renders the frame.
    //
    // Two more pieces are required and both are easy to miss: PreRender must
    // raise GPU_RENDER_POSSIBLE per frame, and GPU_DEVICE_SETUP must RE-AFFIRM
    // SUPPORTS_GPU_RENDER_F32 (see MEB_GPUDeviceSetup). Without the second, AE
    // offers what_gpu = NONE at every PreRender, forever, with no error — the
    // silent failure AE_GPU_Effects.md §2.2 was written about.
    //
    // Do NOT add SUPPORTS_QUERY_DYNAMIC_FLAGS: it is not part of the GPU recipe
    // and mishandling PF_Cmd_QUERY_DYNAMIC_FLAGS raises an unsuppressable AE
    // error 25::228 on EVERY apply.
    if (in_data->appl_id != 'PrMr') {
        out_data->out_flags2 |= PF_OutFlag2_SUPPORTS_GPU_RENDER_F32 |
                                PF_OutFlag2_SUPPORTS_DIRECTX_RENDERING;
    }

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

    // Allow the GPU render for this frame. Without this per-frame flag AE never
    // takes the GPU path, even with SUPPORTS_GPU_RENDER_F32 declared. It
    // declares POSSIBILITY, not a demand — AE still decides per frame, and
    // expect what_gpu = NONE for thumbnails and heavily downsampled draft
    // renders even when the GPU path is working perfectly.
    //
    // Raised unconditionally EXCEPT for one case the GPU path cannot survive:
    // a frame whose device scratch will not fit (MEB_GPUBudgetAllows — 641 MB
    // at 4.6K, more at 8K, all-or-nothing). Declining here costs a CPU render;
    // discovering it at SMART_RENDER_GPU costs the frame, because by then the
    // worlds are device memory and there is nothing to fall back to.
    //
    // The gate NEVER fires on a frame it could not size — no framework offered
    // yet, no gpu_data, params unreadable. Withholding the flag out of
    // ignorance is the deadlock EdgeRepair burned seven experiments on: AE
    // offers what_gpu = NONE until it has seen the flag, so gating the flag on
    // `what_gpu != NONE` means it is never raised and never offered a device.
    {
        MEB_Params gp; AEFX_CLR_STRUCT(gp);
        // A param-read failure must not fail PreRender (it never read params
        // before this gate existed) — it just leaves the frame ungated.
        const bool haveParams = (ReadParams(in_data, &gp) == PF_Err_NONE);

        const PF_LRect &gpu_req = extra->input->output_request.rect;
        const bool gpuPossible = !err &&
            (!haveParams ||
             MEB_GPUBudgetAllows(extra->input->what_gpu, extra->input->gpu_data,
                                 gpu_req.right - gpu_req.left,
                                 gpu_req.bottom - gpu_req.top, gp));

        if (gpuPossible)
            extra->output->flags |= PF_RenderOutputFlag_GPU_RENDER_POSSIBLE;
        MEB_GPULogPreRenderDecision(extra->input->what_gpu,
                                    extra->input->gpu_data, gpuPossible,
                                    (int)extra->input->bitdepth,
                                    (int)in_data->downsample_x.num,
                                    (int)in_data->downsample_x.den);
    }

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
        // Params are read BEFORE the CPU/GPU fork: both paths need the same
        // values, and PF_CHECKOUT_PARAM is a host call that must not behave
        // differently against GPU worlds.
        MEB_Params p; AEFX_CLR_STRUCT(p);
        ERR(ReadParams(in_data, &p));
        if (err) goto cleanup;

        // -----------------------------------------------------------------
        // GPU render. AE hands GPU effect worlds (BGRA128 float, on the
        // device) when PreRender raised GPU_RENDER_POSSIBLE and it chose the
        // GPU for this frame; everything below this branch is the CPU
        // pipeline and never sees a GPU world. Do NOT fall back to the CPU
        // path on a GPU error — CPU code cannot address device memory.
        // -----------------------------------------------------------------
        if (extra->input->what_gpu != PF_GPU_Framework_NONE) {
            err = MEB_SmartRenderGPU(in_data, out_data, extra,
                                     src_world, out_world, p);
            goto cleanup;
        }

        PF_PixelFormat fmt = PF_PixelFormat_INVALID;
        ERR(wsP->PF_GetPixelFormat(out_world, &fmt));
        if (err) goto cleanup;

        const A_long outW = out_world->width;
        const A_long outH = out_world->height;

        // No inflation — src_world is exactly output size.
        const size_t outN = (size_t)outW * outH;
        const bool do_blur     = (p.blur_amountF  >= 0.5f);
        const bool do_blend    = (p.blend_radiusF >= 0.5f);
        const bool do_shift    = (std::abs((float)p.shift_edgeF) >= 0.5f);
        const bool passthrough = !do_blur && !do_blend;

        // Acquire pooled buffers — grow-once per render thread, reused every
        // frame (see MEB_Pool). Passthrough frames only need sharp.
        float *sharp_buf = nullptr, *blurred_buf = nullptr,
              *weight = nullptr, *sharp_alpha = nullptr;
        try {
            g_pool.sharp.resize(outN * 4);
            sharp_buf = g_pool.sharp.data();
            if (!passthrough) {
                g_pool.blurred.resize(outN * 4);
                g_pool.weight.resize(outN);
                g_pool.alpha.resize(outN);
                blurred_buf = g_pool.blurred.data();
                weight      = g_pool.weight.data();
                sharp_alpha = g_pool.alpha.data();
            }
        } catch (const std::bad_alloc &) {
            err = PF_Err_OUT_OF_MEMORY;
            goto cleanup;
        }

        const auto t_total0 = MEB_Clock::now();
        auto t0 = t_total0;

        // INGEST — one threaded pass: unpack each AE world row into sharp
        // (straight colour), zero RGB at the a<=0 cliff, and write the
        // derived buffers (premultiplied blur input, alpha plane) while the
        // row is hot in cache. When Shift Edge is active the derivation must
        // wait until after the alpha shape has been shifted (see Phase 1),
        // so ingest only unpacks and Phase 1 runs the derive pass itself.
        {
            MEB_IngestRefcon irc;
            irc.world = src_world; irc.fmt = fmt; irc.W = outW;
            irc.sharp = sharp_buf;
            irc.do_derive = !do_shift;
            irc.blurred = do_shift ? nullptr : blurred_buf; // null on passthrough anyway
            irc.alpha   = do_shift ? nullptr : sharp_alpha;
            irc.premult_blurred = do_blur;
            MEB_ForEachRow(in_data, outH, &irc, MEB_IngestRow);
        }
        DebugLogTiming("Ingest", t0, MEB_Clock::now());

        // DIAGNOSTIC (compiled out unless MEB_DEBUG_LOG) — dump (alpha, R,
        // G, B) across the first soft-edge transition found on the middle
        // row. NOTE: since the ingest fusion this reads POST-zero data (RGB
        // already zeroed at the a<=0 cliff), not the raw AE delivery — the
        // original straight-vs-premultiplied question this answered is
        // settled (straight). g_edgeY/g_edgeX are reused further down
        // (Phase 4) to dump w, Blur Amount's alpha, and the final composited
        // values at the SAME tracked pixel, to check whether out_rgb and
        // out_alpha stay consistent with each other through Phase 3/4.
        A_long g_edgeY = -1, g_edgeX = -1;
#if MEB_DEBUG_LOG
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
#endif // MEB_DEBUG_LOG

        // Phase 1: Shift Edge — stubbed, only runs when non-zero (default=0).
        // Runs BEFORE the zero-RGB/derive work: it changes the alpha shape
        // (grow/shrink), and the zeroing needs to key off the FINAL alpha, not
        // the pre-shift one — otherwise a grow would reveal newly-alpha>0
        // pixels whose RGB we'd already zeroed (black instead of real colour),
        // and a shrink would leave a leftover-colour ring we never got to zero.
        // That's why ingest skipped derivation on shift frames: the zero pass
        // and the derived buffers are rebuilt here, from post-shift alpha.
        // (The zero-RGB pass itself exists because requesting
        // preserve_rgb_of_zero_alpha=FALSE on checkout only lets AE discard
        // hidden RGB it manages itself — it can't retroactively clean pixels
        // an upstream effect already baked in, e.g. a precomp with an
        // internal keyer that only touches alpha.)
        t0 = MEB_Clock::now();
        if (do_shift) {
            ShiftEdgeAlpha(sharp_buf, outW, outH, (float)p.shift_edgeF);
            MEB_DeriveRefcon drc{ sharp_buf, blurred_buf, sharp_alpha, outW, do_blur };
            MEB_ForEachRow(in_data, outH, &drc, MEB_DeriveRow);
        }
        DebugLogTiming("ShiftEdge", t0, MEB_Clock::now());

        // -----------------------------------------------------------------
        // Passthrough: if no processing is requested, pack sharp to output
        // (threaded) and exit. Preserves held RGB except at the zeroed
        // a<=0 cliff, same as before.
        // -----------------------------------------------------------------
        if (passthrough) {
            MEB_PackRefcon prc{ sharp_buf, out_world, fmt, outW };
            MEB_ForEachRow(in_data, outH, &prc, MEB_PackRow_cb);
            DebugLogTiming("TOTAL", t_total0, MEB_Clock::now());
            DebugLog("---");
            goto cleanup;
        }

        // -----------------------------------------------------------------
        // Phase 2: Blurred base — premultiply → blur → UNPREMULTIPLY.
        // AE's effect pipeline is STRAIGHT (unmatted) alpha on both sides:
        // the EDGE DUMP proved the input side (a=0.78 and a=1.0 pixels carry
        // identical RGB — straight colour, not scaled by coverage), and the
        // output side is symmetric — AE multiplies our RGB by our alpha
        // itself at composite time. So the blur must run in premultiplied
        // space (otherwise a partial-alpha edge pixel contributes its FULL
        // colour to the kernel sum instead of its coverage-weighted share —
        // that contamination was one half of the black-halo bug), but the
        // RESULT handed back must be straight colour (outputting
        // premultiplied colour makes AE multiply by alpha a second time —
        // colour×alpha², a dark fringe on every source including the
        // transparency grid, and a visibly "shrunken" blur. That was the
        // other half).
        //
        // The unpremultiply divide is SAFE here, unlike the earlier "divide
        // by alpha" attempt on straight input: blur(colour×alpha)/blur(alpha)
        // is a convex weighted average of true colours inside the kernel
        // support (weights = alpha×kernel), mathematically bounded by those
        // colours — no amplification possible. The divisor clamp only
        // matters where blurred alpha ≈ 0, where RGB is ≈ 0 too; 1e-4
        // matches the shipped BuildCleanSource implementation of this same
        // technique (commit 84e713f).
        //
        // No AE world needed here — AE's native convolve utility measured
        // 10-20x slower than our own threaded blur (see NEXT_STEPS.md), so we
        // no longer route through it at all. Blur the plain float buffer
        // directly via GaussBlurRGBA, no PF_NewWorld/PackRow/UnpackWorld/
        // DisposeWorld round trip needed.
        // -----------------------------------------------------------------
        // Premultiply already happened at ingest (blurred_buf was written as
        // a*rgb when do_blur); unpremultiply is folded into the blur's own
        // vertical pass (unpremult_after=true → MEB_VBlurColsRGBA divides by
        // the blurred alpha as it writes out). Neither needs its own
        // full-buffer walk anymore.
        t0 = MEB_Clock::now();
        if (do_blur) {
            const auto k = MakeBlurKernel((float)p.blur_amountF);
            GaussBlurRGBA(blurred_buf, outW, outH, k, in_data, true);
        }
        // else: blurred_buf holds the straight-colour copy written at ingest
        // — already in the right space for Phase 4, nothing to do.
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
        // sharp_alpha was extracted during ingest/derive; w's [0,1] clamp is
        // applied inline by the composite pass — no extra walks here.
        t0 = MEB_Clock::now();
        OctagonErode(sharp_alpha, weight, outW, outH, (float)p.blend_radiusF, in_data);

        const auto blend_k = MakeBlendKernel((float)p.blend_radiusF);
        if (!blend_k.empty()) GaussBlurPlane(weight, outW, outH, blend_k, in_data);
        DebugLogTiming("BlendRadius", t0, MEB_Clock::now());

        // -----------------------------------------------------------------
        // Phase 4: Composite and pack output.
        // Both layers are STRAIGHT colour here: sharp_buf as delivered (held
        // colour, zeroed only at the alpha=0 cliff), blurred_buf as recovered
        // by Phase 2's unpremultiply. Output must be straight too — AE
        // multiplies RGB by alpha itself at composite time, so we hand back
        // the blended straight colour untouched and let alpha do the fading.
        // Multiplying by out_alpha here (any variant — per-layer or final)
        // double-applies alpha once AE composites, which reads as a dark
        // halo on every source. See the Phase 2 comment.
        //
        // The blend itself must be ALPHA-WEIGHTED, not a plain lerp by w:
        // just outside the sharp matte, Blend Radius's feathered mask gives w
        // a small nonzero value while sharp_buf's RGB there is the zeroed
        // cliff — a plain lerp mixes that black in at weight w and draws a
        // thin dark line that gets stronger as Blend Radius rises. Weighting
        // each layer's colour by (blend weight × its own alpha) and
        // renormalising is the premultiplied-space blend expressed in
        // straight colour: where sharp coverage is 0 the sharp term drops
        // out entirely, and in the interior (both alphas 1) it reduces to
        // exactly the plain lerp.
        // out_rgb   = (sh_rgb·w·sh_a + bl_rgb·(1-w)·bl_a) / (w·sh_a + (1-w)·bl_a)
        // out_alpha = blurred_alpha  (unchanged — still Blur Amount's alpha)
        // -----------------------------------------------------------------
        // One threaded pass: clamp w, alpha-weighted blend, pack straight
        // into the output world row (see MEB_CompositeRow).
        t0 = MEB_Clock::now();
        std::vector<float> trackedOutRow;
        if (g_edgeX >= 0) trackedOutRow.resize((size_t)outW * 4);
        {
            MEB_CompositeRefcon crc;
            crc.sharp = sharp_buf; crc.blurred = blurred_buf; crc.weight = weight;
            crc.out_world = out_world; crc.fmt = fmt; crc.W = outW;
            crc.trackRow = (g_edgeX >= 0) ? g_edgeY : -1;
            crc.trackBuf = trackedOutRow.empty() ? nullptr : trackedOutRow.data();
            MEB_ForEachRow(in_data, outH, &crc, MEB_CompositeRow);
        }
        DebugLogTiming("Composite", t0, MEB_Clock::now());

        // DIAGNOSTIC (compiled out unless MEB_DEBUG_LOG) — dump w, Blur
        // Amount's alpha, sharp/blurred RGB, and the final composited RGBA
        // across the same tracked edge pixel range as the EDGE DUMP above.
        // Checks whether out_rgb and out_alpha stay consistent (out_alpha
        // comes only from Blur Amount's blur, while the RGB blend weight w
        // comes from Blend Radius's separate erosion+feather — if the two
        // disagree near the edge, out_rgb can show meaningful colour while
        // out_alpha has already dropped near zero, which is itself an
        // invalid-premult condition independent of anything Phase 2 does).
#if MEB_DEBUG_LOG
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
#endif // MEB_DEBUG_LOG

        DebugLogTiming("TOTAL", t_total0, MEB_Clock::now());
        DebugLog("---");
        // Buffers are pooled (g_pool) — nothing to free per frame.
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
            // GPU renders arrive on their OWN command — the enum is
            // SMART_RENDER_GPU, not GPU_SMART_RENDER, which is exactly why it
            // is easy to omit and why the effect then renders on the CPU with
            // no error at all. Same PF_SmartRenderExtra payload; SmartRender's
            // what_gpu fork routes it to MEB_SmartRenderGPU.
            case PF_Cmd_SMART_RENDER_GPU: err = SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra); break;
            // Per-GPU-device lifecycle (ModernEdgeBlur_GPU.cpp). Setup declines
            // quietly for frameworks this build does not render on, which is
            // how AE falls back to the CPU pipeline cleanly.
            case PF_Cmd_GPU_DEVICE_SETUP:   err = MEB_GPUDeviceSetup(in_data, out_data, (PF_GPUDeviceSetupExtra*)extra);     break;
            case PF_Cmd_GPU_DEVICE_SETDOWN: err = MEB_GPUDeviceSetdown(in_data, out_data, (PF_GPUDeviceSetdownExtra*)extra); break;
        }
    } catch (PF_Err &e) { err = e; }
    return err;
}
