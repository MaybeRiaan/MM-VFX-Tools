/*  fs_common.cpp — see fs_common.h.

    Semantics follow the row pack/unpack pattern Modern Edge Blur established
    privately, with one deliberate difference: PackRow leaves float32
    unclamped so HDR values survive a round trip.
*/

#include "fs_common.h"

namespace fs {

size_t PixelBytes(PF_PixelFormat fmt)
{
    switch (fmt) {
        case PF_PixelFormat_ARGB128: return sizeof(PF_PixelFloat); // 16
        case PF_PixelFormat_ARGB64:  return sizeof(PF_Pixel16);    // 8
        case PF_PixelFormat_ARGB32:  return sizeof(PF_Pixel8);     // 4
        default:                     return 0;
    }
}

void UnpackRow(const void *srcRow, float *dst, A_long W, PF_PixelFormat fmt)
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

void PackRow(const float *src, void *dstRow, A_long W, PF_PixelFormat fmt)
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
                p->alpha = (A_u_short)(ClampF(src[0],0,1)*M + .5f);
                p->red   = (A_u_short)(ClampF(src[1],0,1)*M + .5f);
                p->green = (A_u_short)(ClampF(src[2],0,1)*M + .5f);
                p->blue  = (A_u_short)(ClampF(src[3],0,1)*M + .5f);
            }
            break;
        }
        default: {
            PF_Pixel8 *p = static_cast<PF_Pixel8*>(dstRow);
            const float M = (float)PF_MAX_CHAN8;
            for (A_long x = 0; x < W; ++x, ++p, src += 4) {
                p->alpha = (A_u_char)(ClampF(src[0],0,1)*M + .5f);
                p->red   = (A_u_char)(ClampF(src[1],0,1)*M + .5f);
                p->green = (A_u_char)(ClampF(src[2],0,1)*M + .5f);
                p->blue  = (A_u_char)(ClampF(src[3],0,1)*M + .5f);
            }
            break;
        }
    }
}

void ForEachRow(PF_InData *in_data, A_long rows, void *refcon,
                PF_Err (*cb)(void *, A_long, A_long, A_long))
{
    if (in_data && in_data->utils && in_data->utils->iterate_generic) {
        if ((*in_data->utils->iterate_generic)(rows, refcon, cb) == PF_Err_NONE) return;
    }
    for (A_long y = 0; y < rows; ++y) cb(refcon, 0, y, rows);
}

} // namespace fs
