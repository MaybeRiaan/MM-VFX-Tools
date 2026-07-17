/*  matte.h — matte driver: AE-free sampling + channel math (Phase 3).

    The render path (smart_render.cpp) checks out the Matte Layer param,
    unpacks its world into a premultiplied ARGB float plane, and builds an
    FS_MatteField describing that plane plus the Fit mapping. Per output pixel
    the compose loop calls SampleMatte() to get the blend weight, which it
    multiplies by Mix.

    Kept AE-free (plain float buffers, no PF types) so the channel extraction
    and Fit-to-Layer bilinear sampling are unit-testable without the SDK —
    tests/identity_test.cpp drives SampleMatte directly. The layer checkout
    and unpack stay in smart_render.cpp.

    Matte is sampled AS-IS: the Working Curve never applies to it (plan §0).
*/

#pragma once

namespace fs {

// FS_MATTE_CHAN_* mirrored here as plain ints so this header needs no AE
// includes. Keep in sync with params.h (Alpha / Alpha Inv / Luma / Luma Inv).
enum {
	FS_MATTE_CHAN_ALPHA_    = 1,
	FS_MATTE_CHAN_ALPHA_INV_,
	FS_MATTE_CHAN_LUMA_,
	FS_MATTE_CHAN_LUMA_INV_
};

// FS_MATTE_FIT_* mirrored likewise (Fit to Layer / Center / Top-Left).
enum {
	FS_MATTE_FIT_FIT_ = 1,
	FS_MATTE_FIT_CENTER_,
	FS_MATTE_FIT_TOPLEFT_
};

// One matte plane + the LAYER-SPACE geometry that maps an output pixel onto
// it. All mapping happens in layer coordinates (the resolution-independent
// pre-effect layer space AE exposes via each world's origin_x/origin_y and
// the checkout's ref_width/ref_height), so a preview at any resolution places
// the matte identically and mismatched-size layers line up by their frames.
//
//   data       : W*H*4 premult ARGB float (mm::UnpackRow layout: A,R,G,B).
//   W,H        : matte WORLD (buffer) dimensions.
//   mOriginX/Y : matte world's top-left in matte-LAYER coords (origin_x/y).
//   mRefW/H    : matte LAYER frame size (ref_width/ref_height, FULL-res).
//   outOriginX/Y: output world's top-left, in DOWNSAMPLED buffer coords.
//   srcRefW/H  : source LAYER frame size (FULL-res) — the frame Fit maps FROM.
//   dsx,dsy    : downsample ratio (num/den ≤ 1) for the current render. Buffer
//                coords (output pixel, origins) are DOWNSAMPLED; ref frames are
//                FULL-res. dsx/dsy reconcile the two so the mapping is
//                resolution-invariant. 1.0 at full res.
//   channel    : FS_MATTE_CHAN_* — which scalar to pull, with inversion.
//   fit        : FS_MATTE_FIT_* — Fit-to-Layer / Center / Top-Left placement.
//   active     : false when no matte layer selected → SampleMatte returns 1.
//
// Output→matte, per Fit mode. The output pixel + origin (downsampled) is
// scaled UP by 1/ds into full-res layer space; all Fit math runs there against
// the full-res ref frames; the resulting matte-layer coord is scaled back DOWN
// by ds into the (downsampled) matte buffer before subtracting mOrigin.
//   Fit-to-Layer: normalize across srcRef → scale across mRef. Stretch.
//   Center:       matte-layer placed 1:1, centered on the source frame.
//   Top-Left:     matte-layer placed 1:1 at the source frame's top-left.
struct FS_MatteField {
	const float *data     = nullptr;
	int          W        = 0;
	int          H        = 0;
	int          mOriginX  = 0;
	int          mOriginY  = 0;
	int          mRefW      = 0;
	int          mRefH      = 0;
	int          outOriginX = 0;
	int          outOriginY = 0;
	int          srcRefW    = 0;
	int          srcRefH    = 0;
	float        dsx        = 1.f;
	float        dsy        = 1.f;
	int          channel = FS_MATTE_CHAN_ALPHA_;
	int          fit     = FS_MATTE_FIT_FIT_;
	bool         active  = false;
};

// Extract the matte weight from an interpolated ARGB sample. RGB is
// PREMULTIPLIED (the layer's visible-over-black appearance) and `a` is the
// interpolated alpha.
//
// LUMA is gated by coverage — weight = straightLuma × alpha — so the driver's
// transparent regions contribute NO effect and its brightness modulates within
// covered regions ("paint bright where I want the effect; unmasked = none").
// Because the taps are premultiplied, premultLuma already equals
// straightLuma × alpha, so for non-inverted luma we read it directly. For
// LUMA INVERTED the inversion must apply to the STRAIGHT luma BEFORE the alpha
// gate — otherwise transparent areas (straightLuma undefined, premult 0) would
// invert to 1 and drive the effect everywhere the driver is empty. We recover
// straight luma by dividing the premult luma by alpha (guarded), invert, then
// re-gate by alpha.
// Rec.709 weights. Inputs in [0,1]; result in [0,1]. Not clamped here.
inline float MatteChannelValue(float a, float r, float g, float b, int channel)
{
	const float premLuma = 0.2126f * r + 0.7152f * g + 0.0722f * b;  // = straight·a
	switch (channel) {
		case FS_MATTE_CHAN_ALPHA_INV_:
			return 1.f - a;
		case FS_MATTE_CHAN_LUMA_:
			return premLuma;                       // straightLuma × alpha
		case FS_MATTE_CHAN_LUMA_INV_: {
			// (1 - straightLuma) × alpha, with straightLuma = premLuma / a.
			if (a <= 1e-6f) return 0.f;            // no coverage → no effect
			const float straight = premLuma / a;
			return (1.f - straight) * a;
		}
		case FS_MATTE_CHAN_ALPHA_:
		default:
			return a;
	}
}

// Bilinearly sample the matte for output pixel (ox, oy) and return the chosen
// channel value in [0,1]. No matte selected → 1.0 (effect everywhere).
// (ox,oy) are OUTPUT-buffer pixel coords; the field's origins/ref sizes carry
// the layer-space mapping (see FS_MatteField). Out-of-frame reads: Fit clamps
// to the matte's edge (it covers the frame); Center/Top-Left read as
// transparent (a matte genuinely smaller than the frame confines the effect).
float SampleMatte(const FS_MatteField &m, int ox, int oy);

} // namespace fs
