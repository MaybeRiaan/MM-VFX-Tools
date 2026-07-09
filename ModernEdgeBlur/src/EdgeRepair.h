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
#define BUILD_VERSION	9

// ---------------------------------------------------------------------------
// Parameter ranges and defaults. The locked FIVE — Premult is internal-only
// (no knob); no Softness param (a separate suite plugin owns edge shaping).
// ---------------------------------------------------------------------------

// Erode: single SIGNED control — positive erodes the core inward, negative
// dilates it. px.
#define ER_ERODE_MIN		-50.0
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
#define ER_DETAIL_DFLT		100.0

// Alpha Choke: remap output alpha along the distance field. 0 = pure RGB
// operator (alpha untouched). Positive chokes inward, negative grows. px.
#define ER_CHOKE_MIN		-50.0
#define ER_CHOKE_MAX		 50.0
#define ER_CHOKE_DFLT		  0.0

// View Mode popup (1-based in AE). Result is the composited output; the rest
// are diagnostics wired up across Stages 2-3.
enum {
	ER_VIEW_RESULT = 1,
	ER_VIEW_CORE_MATTE,
	ER_VIEW_DISTANCE_FIELD,
	ER_VIEW_EXTENDED_COLOUR,
	ER_VIEW_NUM
};
#define ER_VIEW_DFLT		ER_VIEW_RESULT
// Popup item string — order must match the enum above.
#define ER_VIEW_ITEMS		"Result|Core Matte|Distance Field|Extended Colour"

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
	ER_NUM_PARAMS
};

// Disk IDs — forever. Never reorder, never reuse. New params append with new
// IDs only. (Values 101-105 per the locked brief.)
enum {
	ER_ERODE_DISK_ID  = 101,
	ER_EXTEND_DISK_ID = 102,
	ER_DETAIL_DISK_ID = 103,
	ER_CHOKE_DISK_ID  = 104,
	ER_VIEW_DISK_ID   = 105
};

typedef struct ER_Params {
	PF_FpLong	erodeF;    // signed px
	PF_FpLong	extendF;   // px
	PF_FpLong	detailF;   // %
	PF_FpLong	chokeF;    // signed px
	A_long		viewL;     // ER_VIEW_* (1-based)
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
