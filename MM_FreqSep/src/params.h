/*  params.h — MM FreqSep ("Skin Retouch", Category "MM").

    Central header: SDK includes, version defines (keep in sync with
    resources/MM_FreqSepPiPL.r — mismatch is AE error 8800b), parameter
    ranges/defaults, param indices, disk IDs, and the checked-out param
    snapshot struct shared by PreRender and SmartRender.

    Session scope (Phase 1+2): params 1-11 all registered; ACTIVE this
    session are Separation Radius, Low Band Smooth, High Band Texture, Mix,
    View. Matte Layer/Channel/Fit, Separation Method (non-Gaussian), Color
    Only, and Working Curve are registered-but-inert so their disk IDs are
    locked from the first shipped build.
*/

#pragma once

#ifndef MM_FREQSEP_PARAMS_H
#define MM_FREQSEP_PARAMS_H

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

// Matte Channel popup (1-based). Inert this session (Phase 3).
enum {
	FS_MATTE_CHAN_ALPHA = 1,
	FS_MATTE_CHAN_ALPHA_INV,
	FS_MATTE_CHAN_LUMA,
	FS_MATTE_CHAN_LUMA_INV,
	FS_MATTE_CHAN_NUM
};
#define FS_MATTE_CHAN_DFLT	FS_MATTE_CHAN_ALPHA
#define FS_MATTE_CHAN_ITEMS	"Alpha|Alpha Inverted|Luma|Luma Inverted"

// Matte Fit popup (1-based). v1 ships Fit only; all inert this session.
enum {
	FS_MATTE_FIT_FIT = 1,
	FS_MATTE_FIT_CENTER,
	FS_MATTE_FIT_TOPLEFT,
	FS_MATTE_FIT_NUM
};
#define FS_MATTE_FIT_DFLT	FS_MATTE_FIT_FIT
#define FS_MATTE_FIT_ITEMS	"Fit to Layer|Center|Top-Left"

// Separation Method popup (1-based). Only Gaussian runs this session;
// Bilateral/Guided fall back to Gaussian until Phase 4.
enum {
	FS_METHOD_GAUSSIAN = 1,
	FS_METHOD_BILATERAL,
	FS_METHOD_GUIDED,
	FS_METHOD_NUM
};
#define FS_METHOD_DFLT		FS_METHOD_GAUSSIAN
#define FS_METHOD_ITEMS		"Gaussian|Bilateral|Guided Filter"

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
// Param indices (0 = implicit input layer). On-screen order — never change
// once shipped.
// ---------------------------------------------------------------------------
enum {
	FS_INPUT = 0,
	FS_USE_LAYER_MASKS,	// 1 — use the effect layer's own alpha (masks) as matte
	FS_MATTE_LAYER,		// 2
	FS_MATTE_CHANNEL,	// 3
	FS_MATTE_FIT,		// 4
	FS_RADIUS,			// 5
	FS_METHOD,			// 6
	FS_LO_SMOOTH,		// 7
	FS_LO_COLOR_ONLY,	// 8
	FS_HI_TEXTURE,		// 9
	FS_MIX,				// 10
	FS_VIEW,			// 11
	FS_CURVE,			// 12
	FS_NUM_PARAMS
};

// Disk IDs — forever. Never reorder, never reuse; new params append with new
// IDs only.
enum {
	FS_MATTE_LAYER_DISK_ID     = 101,
	FS_MATTE_CHANNEL_DISK_ID   = 102,
	FS_MATTE_FIT_DISK_ID       = 103,
	FS_RADIUS_DISK_ID          = 104,
	FS_METHOD_DISK_ID          = 105,
	FS_LO_SMOOTH_DISK_ID       = 106,
	FS_LO_COLOR_ONLY_DISK_ID   = 107,
	FS_HI_TEXTURE_DISK_ID      = 108,
	FS_MIX_DISK_ID             = 109,
	FS_VIEW_DISK_ID            = 110,
	FS_CURVE_DISK_ID           = 111,
	FS_USE_LAYER_MASKS_DISK_ID = 112
};

// Snapshot of the checked-out param values for one render. Built fresh per
// PreRender/SmartRender call — never stored globally (MFR).
typedef struct FS_Params {
	PF_FpLong	radiusF;		// separation sigma, px (full-res; scale by downsample)
	PF_FpLong	smoothF;		// %
	PF_FpLong	textureF;		// %
	PF_FpLong	mixF;			// %
	A_long		matteChanL;		// FS_MATTE_CHAN_* (inert)
	A_long		matteFitL;		// FS_MATTE_FIT_*  (inert)
	A_long		methodL;		// FS_METHOD_*     (Gaussian only this session)
	A_long		colorOnlyB;		// checkbox 0/1    (inert)
	A_long		viewL;			// FS_VIEW_*
	A_long		curveL;			// FS_CURVE_*      (inert)
	A_long		useLayerMasksB;	// checkbox 0/1 — use effect layer's own alpha
} FS_Params;

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

#endif // MM_FREQSEP_PARAMS_H
