/*  params.h — MM FreqSep ("Skin Retouch", Category "MM").

    Central header: SDK includes, version defines (keep in sync with
    resources/MM_FreqSepPiPL.r — mismatch is AE error 8800b), parameter
    ranges/defaults, param indices, disk IDs, and the checked-out param
    snapshot struct shared by PreRender and SmartRender.

    Params (Phase 4): Matte Layer/Channel/Fit, Separation Radius, Separation
    Method (Gaussian / Guided Filter), Edge Preservation (Guided ε, greyed for
    Gaussian), Low Band Smooth, High Band Texture, Mix, View are all ACTIVE.
    Working Curve remains registered-but-inert (curve.cpp is a stub, later
    phase). Low Band: Color Only was removed — it's being spun out into a
    standalone MM chroma-evening effect (see docs/mm-freqsep-plan.md).
*/

#pragma once

#ifndef MM_FREQSEP_PARAMS_H
#define MM_FREQSEP_PARAMS_H

#include <cmath>   // FS_EdgeToEps (exp/log) — exponential slider→eps mapping

typedef unsigned char		u_char;
typedef unsigned short		u_short;
typedef unsigned short		u_int16;
typedef unsigned long		u_long;
typedef short int			int16;
#define PF_TABLE_BITS		12
#define PF_TABLE_SZ_16		4096

#define PF_DEEP_COLOR_AWARE 1

#include "AEConfig.h"

#ifdef AE_OS_WIN
	typedef unsigned short PixelType;
	#include <Windows.h>
#endif

#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectCBSuites.h"
#include "String_Utils.h"
#include "AE_GeneralPlug.h"
#include "AEFX_ChannelDepthTpl.h"
#include "AEGP_SuiteHandler.h"
#include "AEFX_SuiteHelper.h"
#include "Smart_Utils.h"

// Keep in sync with resources/MM_FreqSepPiPL.r.
#define MAJOR_VERSION	1
#define MINOR_VERSION	0
#define BUG_VERSION		0
#define STAGE_VERSION	PF_Stage_DEVELOP
#define BUILD_VERSION	1

// ---------------------------------------------------------------------------
// Ranges and defaults
// ---------------------------------------------------------------------------

// Separation Radius IS the Gaussian sigma, in layer px at full resolution.
// Blur support (and therefore PreRender inflation) is ceil(3*sigma) per pass.
// Scaled by downsample_x in the render path — never use the raw value there.
#define FS_RADIUS_MIN		0.0
#define FS_RADIUS_MAX		250.0
#define FS_RADIUS_SLIDER_MAX 50.0
#define FS_RADIUS_DFLT		8.0

// Low Band Smooth (%): extra smoothing of the tone band after the split.
// This session it is a second Gaussian on the low band with
// sigma_extra = (smooth/100) * separation_radius. 0 = band untouched
// (required for the identity test). Edge-aware variant lands in Phase 4.
#define FS_SMOOTH_MIN		0.0
#define FS_SMOOTH_MAX		100.0
#define FS_SMOOTH_DFLT		0.0

// High Band Texture (%): gain on the detail band. 100 = unchanged.
#define FS_TEXTURE_MIN		0.0
#define FS_TEXTURE_MAX		200.0
#define FS_TEXTURE_DFLT		100.0

// Mix (%): final blend of processed result over the source.
#define FS_MIX_MIN			0.0
#define FS_MIX_MAX			100.0
#define FS_MIX_DFLT			100.0

// Matte Channel popup (1-based). LIVE — matte.cpp:132 MatteChannelValue picks
// the channel and smart_render.cpp:846 feeds it to both hosts. (The old "inert
// this session" note here outlived Phase 3 by some margin.)
enum {
	FS_MATTE_CHAN_ALPHA = 1,
	FS_MATTE_CHAN_ALPHA_INV,
	FS_MATTE_CHAN_LUMA,
	FS_MATTE_CHAN_LUMA_INV,
	FS_MATTE_CHAN_NUM
};
#define FS_MATTE_CHAN_DFLT	FS_MATTE_CHAN_ALPHA
#define FS_MATTE_CHAN_ITEMS	"Alpha|Alpha Inverted|Luma|Luma Inverted"

// Matte Fit popup (1-based). All three are LIVE — matte.cpp:88-112 implements
// the source→matte mapping per mode. Note they only diverge when the matte
// layer differs in size from the source layer; at equal dimensions all three
// reduce to mlx = lx. In-AE parity verified 2026-08-12 (F-07).
enum {
	FS_MATTE_FIT_FIT = 1,
	FS_MATTE_FIT_CENTER,
	FS_MATTE_FIT_TOPLEFT,
	FS_MATTE_FIT_NUM
};
#define FS_MATTE_FIT_DFLT	FS_MATTE_FIT_FIT
#define FS_MATTE_FIT_ITEMS	"Fit to Layer|Center|Top-Left"

// Separation Method popup (1-based). Gaussian = creamy/soft (halos on hard
// edges); Guided Filter = edge-aware (no halos). Both are deliberately kept —
// they are different looks. Bilateral was cut (redundant with Guided).
enum {
	FS_METHOD_GAUSSIAN = 1,
	FS_METHOD_GUIDED,
	FS_METHOD_NUM
};
#define FS_METHOD_DFLT		FS_METHOD_GAUSSIAN
#define FS_METHOD_ITEMS		"Gaussian|Guided Filter"

// Edge Preservation (%): the guided filter's regularizer eps, exposed
// perceptually. Only meaningful when Method = Guided Filter — greyed out for
// Gaussian (SEND_UPDATE_PARAMS_UI mechanism, see main.cpp). The slider is
// mapped EXPONENTIALLY to eps because eps lives in squared straight-color
// units and its useful range spans ~3 decades; linear would cram all the
// interesting behavior into the first few percent.
//   0%   → strongest edge preservation (tiny eps: keep almost every edge,
//          maximal halo suppression, least cross-edge smoothing)
//   100% → weakest (large eps: Guided approaches the matched Gaussian)
// eps(s) = EPS_MIN * (EPS_MAX/EPS_MIN)^(s/100).
#define FS_EDGE_MIN			0.0
#define FS_EDGE_MAX			100.0
#define FS_EDGE_DFLT		35.0
#define FS_EDGE_EPS_MIN		1.0e-4
#define FS_EDGE_EPS_MAX		1.0e-1

// View popup (1-based). High Band displays hi + 0.5 for inspection.
// Matte stubs to white until Phase 3.
enum {
	FS_VIEW_RESULT = 1,
	FS_VIEW_LOW,
	FS_VIEW_HIGH,
	FS_VIEW_MATTE,
	FS_VIEW_NUM
};
#define FS_VIEW_DFLT		FS_VIEW_RESULT
#define FS_VIEW_ITEMS		"Result|Low Band|High Band|Matte"

// Working Curve popup (1-based). Inert this session — curve.cpp is a stub.
enum {
	FS_CURVE_NONE = 1,
	FS_CURVE_SRGB,
	FS_CURVE_ACESCCT,
	FS_CURVE_NUM
};
#define FS_CURVE_DFLT		FS_CURVE_NONE
#define FS_CURVE_ITEMS		"None|sRGB|ACEScct"

// ---------------------------------------------------------------------------
// Param indices (0 = implicit input layer). On-screen order. Pre-release: no
// shipped comps exist except throwaway test comps, so this order (and the disk
// IDs below) may still be changed freely — Edge Preservation sits right after
// Separation Method, where it belongs. Once a real build ships, lock both.
// ---------------------------------------------------------------------------
enum {
	FS_INPUT = 0,
	FS_MATTE_LAYER,		// 1
	FS_MATTE_CHANNEL,	// 2
	FS_MATTE_FIT,		// 3
	FS_RADIUS,			// 4
	FS_METHOD,			// 5
	FS_EDGE,			// 6 — Edge Preservation (Guided ε); greyed for Gaussian
	FS_LO_SMOOTH,		// 7
	FS_HI_TEXTURE,		// 8
	FS_MIX,				// 9
	FS_VIEW,			// 10
	FS_CURVE,			// 11
	FS_NUM_PARAMS
};

// Disk IDs. Pre-release these can still change (see the index note above);
// they map on-screen order → saved value, so kept parallel to the indices for
// readability. Lock them at first ship.
enum {
	FS_MATTE_LAYER_DISK_ID     = 101,
	FS_MATTE_CHANNEL_DISK_ID   = 102,
	FS_MATTE_FIT_DISK_ID       = 103,
	FS_RADIUS_DISK_ID          = 104,
	FS_METHOD_DISK_ID          = 105,
	FS_EDGE_DISK_ID            = 106,
	FS_LO_SMOOTH_DISK_ID       = 107,
	FS_HI_TEXTURE_DISK_ID      = 108,
	FS_MIX_DISK_ID             = 109,
	FS_VIEW_DISK_ID            = 110,
	FS_CURVE_DISK_ID           = 111
};

// Snapshot of the checked-out param values for one render. Built fresh per
// PreRender/SmartRender call — never stored globally (MFR).
typedef struct FS_Params {
	PF_FpLong	radiusF;		// separation sigma, px (full-res; scale by downsample)
	PF_FpLong	smoothF;		// %
	PF_FpLong	textureF;		// %
	PF_FpLong	mixF;			// %
	A_long		matteChanL;		// FS_MATTE_CHAN_*
	A_long		matteFitL;		// FS_MATTE_FIT_*
	A_long		methodL;		// FS_METHOD_* (Gaussian / Guided)
	A_long		viewL;			// FS_VIEW_*
	A_long		curveL;			// FS_CURVE_*      (inert)
	PF_FpLong	edgeF;			// Edge Preservation %, 0-100 (Guided only)
} FS_Params;

// Edge Preservation slider % → guided-filter eps. Exponential (see FS_EDGE_*
// in this header): eps = EPS_MIN * (EPS_MAX/EPS_MIN)^(s/100). AE-free so both
// the render path and any test can share it.
inline double FS_EdgeToEps(double edgePct)
{
	double s = edgePct / 100.0;
	if (s < 0.0) s = 0.0; else if (s > 1.0) s = 1.0;
	const double ratio = FS_EDGE_EPS_MAX / FS_EDGE_EPS_MIN;
	// pow via exp/log — <cmath> is already pulled in by the SDK headers above.
	return FS_EDGE_EPS_MIN * exp(s * log(ratio));
}

extern "C" {
	DllExport PF_Err EffectMain(
		PF_Cmd		cmd,
		PF_InData	*in_data,
		PF_OutData	*out_data,
		PF_ParamDef	*params[],
		PF_LayerDef	*output,
		void		*extra);
}

// smart_render.cpp
PF_Err FS_PreRender(PF_InData *in_data, PF_OutData *out_data, PF_PreRenderExtra *extra);
PF_Err FS_SmartRender(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra);

// smart_render.cpp — Phase 5 GPU path (DX12 host in src/gpu/mm_gpu_dx.cpp;
// unsupported frameworks degrade to the CPU render, never to an error).
PF_Err FS_SmartRenderGPU(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra);
PF_Err FS_GPUDeviceSetup(PF_InData *in_data, PF_OutData *out_data, PF_GPUDeviceSetupExtra *extra);
PF_Err FS_GPUDeviceSetdown(PF_InData *in_data, PF_OutData *out_data, PF_GPUDeviceSetdownExtra *extra);

#endif // MM_FREQSEP_PARAMS_H
