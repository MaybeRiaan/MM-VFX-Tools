/*  ModernEdgeBlur.cpp  —  v1.2

    Pipeline (per frame):
      1. [Shift Edge]  Stubbed — morphological grow/shrink on alpha. No-op when = 0 (default).
      2. [Blur Amount] Gaussian blur full RGBA via AE's built-in convolve → blurred base (A).
                       Fallback: hand-rolled separable Gaussian (sigma = radius * 0.25).
      3. [Blend Radius] Gaussian blur sharp alpha only → weight w = clamp(blurred_alpha, 0, 1).
                        Hand-rolled kernel (sigma = radius, support = ceil(radius*3)).
      4. [Composite]   out_rgb   = lerp(A_rgb, sharp_rgb, w)
                       out_alpha = A_alpha  (blurred alpha = soft feathered edge)

    Passthrough: Blur=0 and Blend=0 → input copied directly to output, held RGB preserved.
    Premult: preserve_rgb_of_zero_alpha=FALSE. Outside-matte pixels are (0,0,0,0).
             Gaussian sees zero colour outside matte → no dark halo.
    No input inflation — src_world is exactly output size.
*/

#define NOMINMAX
#include "ModernEdgeBlur.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>

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
// Phase 1: Morphological shift on alpha (circular kernel, brute-force)
// radius > 0 → dilate (grow alpha); radius < 0 → erode (shrink alpha)
// Operates in-place on a float[H*W*4] ARGB buffer (channel 0 = alpha).
// ---------------------------------------------------------------------------
static void ShiftEdgeAlpha(float *buf, A_long W, A_long H, float radius)
{
    if (std::abs(radius) < 0.5f) return;

    const bool dilate = (radius > 0.f);
    const float r = std::abs(radius);
    const int   ri = (int)std::ceil(r);
    const float r2 = r * r;

    // Pre-extract alpha plane
    std::vector<float> alpha_in((size_t)W * H);
    for (A_long i = 0; i < W * H; ++i) alpha_in[i] = buf[i * 4 + 0];

    std::vector<float> alpha_out((size_t)W * H);

    for (A_long y = 0; y < H; ++y) {
        for (A_long x = 0; x < W; ++x) {
            float best = dilate ? 0.f : 1.f;
            for (int dy = -ri; dy <= ri; ++dy) {
                for (int dx = -ri; dx <= ri; ++dx) {
                    if ((float)(dx*dx + dy*dy) > r2) continue;
                    const A_long sx = clampL(x + dx, 0, W - 1);
                    const A_long sy = clampL(y + dy, 0, H - 1);
                    const float  a  = alpha_in[(size_t)sy * W + sx];
                    if (dilate) { if (a > best) best = a; }
                    else        { if (a < best) best = a; }
                }
            }
            alpha_out[(size_t)y * W + x] = best;
        }
    }

    // Write shifted alpha back; RGB unchanged
    for (A_long i = 0; i < W * H; ++i) buf[i * 4 + 0] = alpha_out[i];
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

// Separable Gaussian blur of a single float plane [H*W], in-place.
static void GaussBlurPlane(float *buf, A_long W, A_long H, const std::vector<float> &k)
{
    if (k.empty() || W <= 0 || H <= 0) return;
    const int half = (int)(k.size() / 2);
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

// Gaussian blur a PF_EffectWorld in-place using AE's built-in convolve.
// Two separable 1D passes (horizontal then vertical).
// Falls back to our own plane-by-plane blur if AE's convolve fails.
static void GaussBlurWorld(PF_EffectWorld *world, float radius, PF_InData *in_data,
                           A_long W, A_long H, PF_PixelFormat fmt)
{
    if (radius < 0.5f) return;

    if (in_data && in_data->utils && in_data->utils->gaussian_kernel && in_data->utils->convolve) {
        const A_long max_dia = 255;
        std::vector<A_long> kbuf(max_dia, 0);
        A_long diameter = 0;

        PF_Err kerr = (*in_data->utils->gaussian_kernel)(
            in_data->effect_ref, (A_FpLong)radius,
            PF_KernelFlag_1D | PF_KernelFlag_NORMALIZED | PF_KernelFlag_USE_LONG,
            1.0, &diameter, kbuf.data());

        if (kerr == PF_Err_NONE && diameter > 0 && diameter <= max_dia) {
            kerr = (*in_data->utils->convolve)(
                in_data->effect_ref, world, nullptr,
                PF_KernelFlag_1D | PF_KernelFlag_NORMALIZED | PF_KernelFlag_USE_LONG | PF_KernelFlag_HORIZONTAL,
                diameter, kbuf.data(), kbuf.data(), kbuf.data(), kbuf.data(), world);

            if (kerr == PF_Err_NONE) {
                kerr = (*in_data->utils->convolve)(
                    in_data->effect_ref, world, nullptr,
                    PF_KernelFlag_1D | PF_KernelFlag_NORMALIZED | PF_KernelFlag_USE_LONG | PF_KernelFlag_VERTICAL,
                    diameter, kbuf.data(), kbuf.data(), kbuf.data(), kbuf.data(), world);
            }

            if (kerr == PF_Err_NONE) return;
        }
    }

    // Fallback: our own separable blur.
    const size_t N = (size_t)W * H;
    const auto k = MakeBlurKernel(radius);
    if (k.empty()) return;

    std::vector<float> tmp(N * 4);
    for (A_long y = 0; y < H; ++y)
        UnpackRow(reinterpret_cast<const char*>(world->data) + y * world->rowbytes,
                  tmp.data() + (size_t)y * W * 4, W, fmt);

    std::vector<float> plane(N);
    for (int c = 0; c < 4; ++c) {
        for (size_t i = 0; i < N; ++i) plane[i] = tmp[i * 4 + c];
        GaussBlurPlane(plane.data(), W, H, k);
        for (size_t i = 0; i < N; ++i) tmp[i * 4 + c] = plane[i];
    }

    for (A_long y = 0; y < H; ++y)
        PackRow(tmp.data() + (size_t)y * W * 4,
                reinterpret_cast<char*>(world->data) + y * world->rowbytes, W, fmt);
}

// Gaussian blur a single alpha plane extracted from a packed ARGB buffer.
// Returns a new heap float[H*W] containing the blurred alpha values.
static float* GaussBlurAlphaOnly(const float *argb, A_long W, A_long H, float radius)
{
    const size_t N = (size_t)W * H;
    float *alpha = new (std::nothrow) float[N];
    if (!alpha) return nullptr;

    for (size_t i = 0; i < N; ++i) alpha[i] = argb[i * 4 + 0];

    const auto k = MakeBlendKernel(radius);
    if (!k.empty()) GaussBlurPlane(alpha, W, H, k);

    return alpha;
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
    req.preserve_rgb_of_zero_alpha = TRUE;

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
        float *sharp_buf = UnpackWorld(src_world, fmt);
        if (!sharp_buf) { err = PF_Err_OUT_OF_MEMORY; goto cleanup; }

        // Phase 1: Shift Edge — stubbed, only runs when non-zero (default=0).
        if (std::abs(p.shift_edgeF) >= 0.5f) {
            ShiftEdgeAlpha(sharp_buf, outW, outH, (float)p.shift_edgeF);
        }

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
            goto cleanup;
        }

        // -----------------------------------------------------------------
        // Phase 2: Blurred base — Gaussian blur of premultiplied data.
        // Blurring premult is correct: alpha and RGB are blurred together,
        // so the blurred RGB is always proportional to blurred alpha at the edge.
        // Zero-alpha pixels outside the matte have RGB=0 in honest premult,
        // so they contribute no colour to the blur kernel — no halo.
        // -----------------------------------------------------------------
        // Phase 2: Blur src_world in-place using AE's convolve, then unpack.
        // We need a mutable copy of src_world to blur — create a new world.
        PF_EffectWorld blur_world;
        AEFX_CLR_STRUCT(blur_world);
        bool blur_world_created = false;

        if (do_blur) {
            ERR(wsP->PF_NewWorld(in_data->effect_ref, outW, outH, FALSE, fmt, &blur_world));
            if (!err) {
                blur_world_created = true;
                // Copy src_world into blur_world row by row
                for (A_long y = 0; y < outH; ++y) {
                    const void *srcRow = reinterpret_cast<const char*>(src_world->data) + y * src_world->rowbytes;
                    void *dstRow = reinterpret_cast<char*>(blur_world.data) + y * blur_world.rowbytes;
                    std::memcpy(dstRow, srcRow, (size_t)outW * (blur_world.rowbytes / outW > 0 ? blur_world.rowbytes / outW : 4));
                }
                GaussBlurWorld(&blur_world, (float)p.blur_amountF, in_data, outW, outH, fmt);
            }
        }

        float *blurred_buf = blur_world_created
            ? UnpackWorld(&blur_world, fmt)
            : new (std::nothrow) float[outN * 4];

        if (!blurred_buf) {
            delete[] sharp_buf;
            if (blur_world_created) wsP->PF_DisposeWorld(in_data->effect_ref, &blur_world);
            err = PF_Err_OUT_OF_MEMORY;
            goto cleanup;
        }

        if (!blur_world_created) {
            std::memcpy(blurred_buf, sharp_buf, outN * 4 * sizeof(float));
        }

        if (blur_world_created) wsP->PF_DisposeWorld(in_data->effect_ref, &blur_world);

        // -----------------------------------------------------------------
        // Phase 3: Blend weight — Gaussian blur of the sharp alpha only.
        // Extract alpha before premult (it is unchanged by premult).
        // w = pow(blurred_alpha, 3): 1 deep inside, 0 at edge/outside.
        // -----------------------------------------------------------------
        float *weight = GaussBlurAlphaOnly(sharp_buf, outW, outH, (float)p.blend_radiusF);
        if (!weight) {
            delete[] sharp_buf;
            delete[] blurred_buf;
            err = PF_Err_OUT_OF_MEMORY;
            goto cleanup;
        }

        for (size_t i = 0; i < outN; ++i) {
            weight[i] = clampF(weight[i], 0.f, 1.f);
        }

        // -----------------------------------------------------------------
        // Phase 4: Composite and pack output.
        // Both sharp_buf and blurred_buf are premultiplied here.
        // out_rgb   = lerp(blurred_rgb, sharp_rgb, w)
        // out_alpha = blurred_alpha
        // -----------------------------------------------------------------
        {
            std::vector<float> out_row((size_t)outW * 4);

            for (A_long y = 0; y < outH; ++y) {
                const float *sh  = sharp_buf   + (size_t)y * outW * 4;
                const float *bl  = blurred_buf + (size_t)y * outW * 4;
                const float *wt  = weight      + (size_t)y * outW;
                float       *dst = out_row.data();

                for (A_long x = 0; x < outW; ++x) {
                    const float w   = wt[x];
                    const float inv = 1.f - w;
                    dst[0] = bl[0];
                    dst[1] = sh[1] * w + bl[1] * inv;
                    dst[2] = sh[2] * w + bl[2] * inv;
                    dst[3] = sh[3] * w + bl[3] * inv;
                    sh += 4; bl += 4; dst += 4;
                }

                void *outRow = reinterpret_cast<char*>(out_world->data) + y * out_world->rowbytes;
                PackRow(out_row.data(), outRow, outW, fmt);
            }
        }

        delete[] weight;
        delete[] blurred_buf;
        delete[] sharp_buf;
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
