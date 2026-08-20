#pragma once

#ifndef GRAIN_TRANSFER_H
#define GRAIN_TRANSFER_H

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

#include "GrainTransfer_Strings.h"

// Keep in sync with resources/GrainTransferPiPL.r — AE compares the packed
// AE_Effect_Version from both sides and rejects on any drift (error 8800b).
#define MAJOR_VERSION	1
#define MINOR_VERSION	0
#define BUG_VERSION		0
#define STAGE_VERSION	PF_Stage_DEVELOP
#define BUILD_VERSION	18

// ---------------------------------------------------------------------------
// Parameter ranges and defaults — the locked SEVEN (brief table confirmed).
// ---------------------------------------------------------------------------

#define GT_AMOUNT_MIN		  0.0
#define GT_AMOUNT_MAX		200.0
#define GT_AMOUNT_DFLT		100.0

#define GT_SIZE_MIN			 25.0
#define GT_SIZE_MAX			400.0
#define GT_SIZE_DFLT		100.0

#define GT_RESPONSE_MIN		  0.0
#define GT_RESPONSE_MAX		100.0
#define GT_RESPONSE_DFLT	100.0

#define GT_SEED_MIN			0
#define GT_SEED_MAX			9999
#define GT_SEED_DFLT		0

// Working Space popup (1-based). Auto queries the project working space via
// the colour-settings suite; the manual entries exist for the CM-bypassed /
// Preserve-RGB workflow and as a version safety net.
enum {
	GT_WS_AUTO = 1,
	GT_WS_DISPLAY,        // "Display / Bypassed"
	GT_WS_SCENE_LINEAR,
	GT_WS_ACES,
	GT_WS_NUM
};
#define GT_WS_DFLT			GT_WS_AUTO
#define GT_WS_ITEMS			"Auto|Display / Bypassed|Scene-Linear|ACES"

// ---------------------------------------------------------------------------
// Parameter indices (0 = implicit input layer). On-screen order — never
// change once shipped.
// ---------------------------------------------------------------------------
enum {
	GT_INPUT = 0,
	GT_SOURCE,       // Grain Source layer
	GT_AMOUNT,
	GT_SIZE,
	GT_RESPONSE,
	GT_SEED,
	GT_WORKSPACE,
	GT_PREVIEW,
	GT_NUM_PARAMS
};

// Disk IDs — forever. Never reorder, never reuse; new params append with new
// IDs only. (101–107 per the locked brief.)
enum {
	GT_SOURCE_DISK_ID    = 101,
	GT_AMOUNT_DISK_ID    = 102,
	GT_SIZE_DISK_ID      = 103,
	GT_RESPONSE_DISK_ID  = 104,
	GT_SEED_DISK_ID      = 105,
	GT_WORKSPACE_DISK_ID = 106,
	GT_PREVIEW_DISK_ID   = 107
};

typedef struct GT_Params {
	PF_FpLong	amountF;      // %
	PF_FpLong	sizeF;        // %
	PF_FpLong	responseF;    // %
	A_long		seedL;
	A_long		workspaceL;   // GT_WS_* (1-based popup)
	PF_Boolean	previewB;
} GT_Params;

extern "C" {
	DllExport PF_Err EffectMain(
		PF_Cmd		cmd,
		PF_InData	*in_data,
		PF_OutData	*out_data,
		PF_ParamDef	*params[],
		PF_LayerDef	*output,
		void		*extra);
}

#endif // GRAIN_TRANSFER_H
