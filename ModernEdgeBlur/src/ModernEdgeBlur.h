#pragma once

#ifndef MODERN_EDGE_BLUR_H
#define MODERN_EDGE_BLUR_H

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

#include "ModernEdgeBlur_Strings.h"

#define MAJOR_VERSION	1
#define MINOR_VERSION	2
#define BUG_VERSION		0
#define STAGE_VERSION	PF_Stage_DEVELOP
#define BUILD_VERSION	5

// ---------------------------------------------------------------------------
// Parameter ranges and defaults
// ---------------------------------------------------------------------------

#define MEB_SHIFT_EDGE_MIN		-50.0
#define MEB_SHIFT_EDGE_MAX		 50.0
#define MEB_SHIFT_EDGE_DFLT		  0.0

#define MEB_BLUR_AMOUNT_MIN		  0.0
#define MEB_BLUR_AMOUNT_MAX		100.0
#define MEB_BLUR_AMOUNT_DFLT	  4.0

#define MEB_BLEND_RADIUS_MIN	  0.0
#define MEB_BLEND_RADIUS_MAX	100.0
#define MEB_BLEND_RADIUS_DFLT	  4.0

// ---------------------------------------------------------------------------
// Parameter indices (0 = implicit input layer)
// ---------------------------------------------------------------------------
enum {
	MEB_INPUT = 0,
	MEB_SHIFT_EDGE,
	MEB_BLUR_AMOUNT,
	MEB_BLEND_RADIUS,
	MEB_NUM_PARAMS
};

// Disk IDs — never reorder, never reuse
enum {
	MEB_SHIFT_EDGE_DISK_ID   = 1,
	MEB_BLUR_AMOUNT_DISK_ID  = 2,
	MEB_BLEND_RADIUS_DISK_ID = 3
};

typedef struct MEB_Params {
	PF_FpLong	shift_edgeF;
	PF_FpLong	blur_amountF;
	PF_FpLong	blend_radiusF;
} MEB_Params;


extern "C" {
	DllExport PF_Err EffectMain(
		PF_Cmd		cmd,
		PF_InData	*in_data,
		PF_OutData	*out_data,
		PF_ParamDef	*params[],
		PF_LayerDef	*output,
		void		*extra);
}

#endif // MODERN_EDGE_BLUR_H
