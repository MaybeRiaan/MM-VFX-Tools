#pragma once

#ifndef EDGE_REPAIR_H
#define EDGE_REPAIR_H

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

#include "EdgeRepair_Strings.h"

// Keep in sync with resources/EdgeRepairPiPL.r (see Version / PiPL sync
// rules in Architecture.md — mismatch is AE error 8800b).
#define MAJOR_VERSION	1
#define MINOR_VERSION	0
#define BUG_VERSION		0
#define STAGE_VERSION	PF_Stage_DEVELOP
#define BUILD_VERSION	36

// ---------------------------------------------------------------------------
// Parameter ranges and defaults. Originally five; Softness (disk ID 106) was
// appended 2026-07-22 on user sign-off — six now. Premult stays internal-only
// (no knob); EDGE-BLUR softness stays owned by a separate suite plugin — the
// Softness param below is the distance-BAND inner ease, a different thing.
// ---------------------------------------------------------------------------

// Erode: how far (px) to shrink the trusted core inward from the alpha edge to
// find clean, uncontaminated interior colour. Clamped to 0..50 (2026-07-22):
// negative Erode (dilate) was removed because it has no legitimate use for the
// decontamination job — growing the core into the transparent exterior adds no
// colour (those pixels have alpha<=0, skipped by the fill), and growing it into
// the semi-transparent rim RE-TRUSTS the very contamination Erode exists to
// exclude. The core-matte grow branch (BuildCoreMatte, erode<0) is now
// unreachable from the UI but kept for the headless tests / possible future use.
#define ER_ERODE_MIN		  0.0
#define ER_ERODE_MAX		 50.0
#define ER_ERODE_DFLT		  2.0

// Extend: how far (px) the extended colour is USED outward from the core.
// The pull–push fill itself runs effectively unlimited; this clamps the band.
#define ER_EXTEND_MIN		  0.0
#define ER_EXTEND_MAX		500.0
#define ER_EXTEND_DFLT		 32.0

// Detail: frequency-separation texture preserve, % of the plate's high
// frequencies laid back over the extended colour base within the blend band.
#define ER_DETAIL_MIN		  0.0
#define ER_DETAIL_MAX		100.0
// Default lowered 100 -> 50 (2026-07-22): at 100 the plate's high frequencies
// are re-added at full strength over the extended base, and a same-scale
// contamination ring rides back on top (frequency separation cannot tell it
// from legitimate edge texture). 50 keeps texture while halving that re-add,
// a less aggressive out-of-box result. The knob still spans 0..100.
#define ER_DETAIL_DFLT		 50.0

// Alpha Choke: remap output alpha along the distance field. 0 = pure RGB
// operator (alpha untouched). Positive chokes inward, negative grows. px.
#define ER_CHOKE_MIN		-50.0
#define ER_CHOKE_MAX		 50.0
#define ER_CHOKE_DFLT		  0.0

// Softness: a fixed-PIXEL softness/ease width (2026-07-22, param #6). One
// unified slider shaping softness in two places (see EdgeRepair.cpp):
//   • Composited band: inner-ease width = Softness px, fed straight to
//     BlendWeight's inner arg — w eases 0->1 over the first Softness px out
//     from the core instead of snapping to full replacement.
//   • Distance Field matte: inward ramp width = Softness px (clamped to the
//     erode gap) — how much of the gap is solid white vs. a white->black
//     gradient toward the core.
// History: a fixed 15%-of-Extend fraction through v1.0.0.18; a 0..50 % slider
// v1.0.0.19-.21; switched to fixed PIXELS on user sign-off (px is predictable —
// it doesn't rescale when Extend/Erode change). Default 5 px ≈ the old 15% of
// the default Extend 32 (~4.8px), so the Composited look barely shifts (NOT
// byte-identical anymore — 5 vs 4.8px). 0 = hard (Composited: snaps at core;
// Distance Field: solid-white gap, hard cut); max 250 px. Disk ID 106.
#define ER_SOFT_MIN			  0.0
#define ER_SOFT_MAX			250.0
#define ER_SOFT_DFLT		  5.0

// View Mode popup (1-based in AE). ER_VIEW_RESULT is the composited output
// (label "Composited"); the code identifier / enum value (=1) / disk ID are
// LOCKED so existing .aep projects still resolve to this view. Extended Colour
// (opaque clean plate) and Distance Field (opaque luma matte) are now
// first-class deliverables; Core Matte stays diagnostic.
enum {
	ER_VIEW_RESULT = 1,		// label "Composited" (was "Result")
	ER_VIEW_CORE_MATTE,
	ER_VIEW_DISTANCE_FIELD,
	ER_VIEW_EXTENDED_COLOUR,
	ER_VIEW_NUM
};
#define ER_VIEW_DFLT		ER_VIEW_RESULT
// Popup item string — order must match the enum above. Label only: "Result"
// was renamed to "Composited" (2026-07-22); the enum value + disk ID are
// unchanged so saved projects keep resolving to the same view.
#define ER_VIEW_ITEMS		"Composited|Core Matte|Distance Field|Extended Colour"

// ---------------------------------------------------------------------------
// Parameter indices (0 = implicit input layer). Order here is the on-screen
// order and must never change once shipped.
// ---------------------------------------------------------------------------
enum {
	ER_INPUT = 0,
	ER_ERODE,
	ER_EXTEND,
	ER_DETAIL,
	ER_CHOKE,
	ER_VIEW,
	ER_SOFT,			// appended 2026-07-22 (last index — no prior shift)
	ER_NUM_PARAMS
};

// Disk IDs — forever. Never reorder, never reuse. New params append with new
// IDs only. (Values 101-105 per the locked brief; 106 = Softness, added
// 2026-07-22 on user sign-off — the first append past the original five.)
enum {
	ER_ERODE_DISK_ID  = 101,
	ER_EXTEND_DISK_ID = 102,
	ER_DETAIL_DISK_ID = 103,
	ER_CHOKE_DISK_ID  = 104,
	ER_VIEW_DISK_ID   = 105,
	ER_SOFT_DISK_ID   = 106
};

typedef struct ER_Params {
	PF_FpLong	erodeF;    // px inward, 0..50 (dilate removed 2026-07-22)
	PF_FpLong	extendF;   // px
	PF_FpLong	detailF;   // %
	PF_FpLong	chokeF;    // signed px
	A_long		viewL;     // ER_VIEW_* (1-based)
	PF_FpLong	softF;     // softness ease width in px (0..250)
} ER_Params;

extern "C" {
	DllExport PF_Err EffectMain(
		PF_Cmd		cmd,
		PF_InData	*in_data,
		PF_OutData	*out_data,
		PF_ParamDef	*params[],
		PF_LayerDef	*output,
		void		*extra);
}

#endif // EDGE_REPAIR_H
