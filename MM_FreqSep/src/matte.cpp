/*  matte.cpp — see matte.h. AE-free bilinear matte sampling + channel
    extraction. The layer checkout / world unpack lives in smart_render.cpp;
    this file is pure buffer math so it stays unit-testable without the SDK.
*/

#include "matte.h"

#include <cmath>
#include <cstddef>

namespace fs {

// Bilinear tap of the premultiplied matte plane at continuous matte coords
// (mx, my). Returns the interpolated PREMULTIPLIED ARGB — i.e. the color as it
// would composite over black, which is the layer's visible appearance. We do
// NOT un-premultiply here:
//   - Alpha channel wants alpha; premult vs straight is irrelevant to it.
//   - Luma channel wants the driver layer's VISIBLE brightness. Premultiplied
//     luma == what you see on that layer over black, which is the intuitive
//     "paint bright where I want the effect" behavior. Un-premultiplying was
//     the bug: dividing RGB by a near-zero alpha exploded into artifacts, and
//     guarding the divide made transparent areas contribute full weight but
//     zero color — a hard luma cut at every alpha edge.
//
// Edge policy depends on `clampEdge`:
//   true  (Fit-to-Layer): clamp taps to the nearest edge texel. The matte is
//         meant to cover the whole frame, so the boundary reads the edge
//         value, not fade toward transparent (which would un-mask a rim).
//   false (Center / Top-Left): out-of-bounds contributes transparent black,
//         so a matte genuinely smaller than the frame confines the effect to
//         where it actually covers.
static void SampleBilinear(const FS_MatteField &m, float mx, float my,
						   bool clampEdge, float &a, float &r, float &g, float &b)
{
	a = r = g = b = 0.f;

	// Pixel centers at integer+0.5; shift so floor gives the lower-left tap.
	const float fx = mx - 0.5f;
	const float fy = my - 0.5f;
	const int x0 = (int)std::floor(fx);
	const int y0 = (int)std::floor(fy);
	const float tx = fx - (float)x0;
	const float ty = fy - (float)y0;

	const float wx[2] = { 1.f - tx, tx };
	const float wy[2] = { 1.f - ty, ty };

	for (int j = 0; j < 2; ++j) {
		int yy = y0 + j;
		if (clampEdge) { yy = yy < 0 ? 0 : (yy >= m.H ? m.H - 1 : yy); }
		else if (yy < 0 || yy >= m.H) continue;
		for (int i = 0; i < 2; ++i) {
			int xx = x0 + i;
			if (clampEdge) { xx = xx < 0 ? 0 : (xx >= m.W ? m.W - 1 : xx); }
			else if (xx < 0 || xx >= m.W) continue;   // outside → transparent
			const float w = wx[i] * wy[j];
			const float *p = m.data + ((size_t)yy * m.W + xx) * 4;
			a += w * p[0];   // premultiplied ARGB, interpolated directly
			r += w * p[1];
			g += w * p[2];
			b += w * p[3];
		}
	}
}

// Map output pixel (ox,oy) → continuous matte-WORLD coords, entirely through
// FULL-RES layer space so the result is independent of render resolution and
// tiling. The key correctness point (the half↔third-res jump): buffer coords
// (output pixel, both origins) are DOWNSAMPLED, but ref_width/height are
// FULL-res, so the two must be reconciled by the downsample ratio — mixing
// them is what made Fit drift with resolution.
//
// Step 1: output pixel (downsampled) → FULL-res source-layer coord: add the
//         (downsampled) output origin, then scale up by 1/ds.
// Step 2: source-layer → matte-layer coord (full-res), per Fit mode.
// Step 3: matte-layer coord → matte-WORLD pixel: scale DOWN by ds into the
//         downsampled matte buffer, then subtract the matte world origin.
static void OutputToMatte(const FS_MatteField &m, int ox, int oy,
						  float &mx, float &my)
{
	// Step 1 — full-res source-layer coordinate of this output pixel center.
	const float invdsx = (m.dsx > 0.f) ? 1.f / m.dsx : 1.f;
	const float invdsy = (m.dsy > 0.f) ? 1.f / m.dsy : 1.f;
	const float lx = ((float)ox + 0.5f + (float)m.outOriginX) * invdsx;
	const float ly = ((float)oy + 0.5f + (float)m.outOriginY) * invdsy;

	// Step 2 — source-layer → matte-layer (both full-res).
	float mlx, mly;
	switch (m.fit) {
		case FS_MATTE_FIT_CENTER_: {
			// Matte frame centered on the source frame, 1:1.
			const float offx = ((float)m.srcRefW - (float)m.mRefW) * 0.5f;
			const float offy = ((float)m.srcRefH - (float)m.mRefH) * 0.5f;
			mlx = lx - offx;
			mly = ly - offy;
			break;
		}
		case FS_MATTE_FIT_TOPLEFT_:
			// Matte frame at the source frame's top-left, 1:1.
			mlx = lx;
			mly = ly;
			break;
		case FS_MATTE_FIT_FIT_:
		default: {
			// Stretch the matte frame across the source frame: normalize the
			// layer coord by the source frame, scale by the matte frame.
			const float u = lx / (float)(m.srcRefW > 0 ? m.srcRefW : 1);
			const float v = ly / (float)(m.srcRefH > 0 ? m.srcRefH : 1);
			mlx = u * (float)m.mRefW;
			mly = v * (float)m.mRefH;
			break;
		}
	}

	// Step 3 — matte-layer (full-res) → downsampled matte-world pixel.
	mx = mlx * m.dsx - (float)m.mOriginX;
	my = mly * m.dsy - (float)m.mOriginY;
}

float SampleMatte(const FS_MatteField &m, int ox, int oy)
{
	if (!m.active || !m.data || m.W <= 0 || m.H <= 0) return 1.0f;

	float mx, my;
	OutputToMatte(m, ox, oy, mx, my);

	// Fit-to-Layer clamps to the matte's edge texels (it covers the frame);
	// Center/Top-Left let out-of-bounds read as transparent (smaller matte).
	const bool clampEdge = (m.fit == FS_MATTE_FIT_FIT_);

	float a, r, g, b;
	SampleBilinear(m, mx, my, clampEdge, a, r, g, b);
	return MatteChannelValue(a, r, g, b, m.channel);
}

} // namespace fs
