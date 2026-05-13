/*******************************************************************/
/*                                                                 */
/*                      ADOBE CONFIDENTIAL                         */
/*                   _ _ _ _ _ _ _ _ _ _ _ _ _                     */
/*                                                                 */
/* Copyright 2007-2023 Adobe Inc.                                  */
/* All Rights Reserved.                                            */
/*                                                                 */
/* NOTICE:  All information contained herein is, and remains the   */
/* property of Adobe Inc. and its suppliers, if                    */
/* any.  The intellectual and technical concepts contained         */
/* herein are proprietary to Adobe Inc. and its                    */
/* suppliers and may be covered by U.S. and Foreign Patents,       */
/* patents in process, and are protected by trade secret or        */
/* copyright law.  Dissemination of this information or            */
/* reproduction of this material is strictly forbidden unless      */
/* prior written permission is obtained from Adobe Inc.            */
/* Incorporated.                                                   */
/*                                                                 */
/*******************************************************************/

/*
	ModernEdgeBlur.h
*/

#pragma once

#ifndef MODERN_EDGE_BLUR_H
#define MODERN_EDGE_BLUR_H

typedef unsigned char		u_char;
typedef unsigned short		u_short;
typedef unsigned short		u_int16;
typedef unsigned long		u_long;
typedef short int			int16;
#define PF_TABLE_BITS	12
#define PF_TABLE_SZ_16	4096

#define PF_DEEP_COLOR_AWARE 1	// make sure we get 16bpc pixels;
								// AE_Effect.h checks for this.

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

/* Versioning information */

#define	MAJOR_VERSION	1
#define	MINOR_VERSION	1
#define	BUG_VERSION		0
#define	STAGE_VERSION	PF_Stage_DEVELOP
#define	BUILD_VERSION	11


/* Parameter defaults */

#define	EDGE_BLUR_EDGE_RADIUS_MIN		0.0
#define	EDGE_BLUR_EDGE_RADIUS_MAX		50.0
#define	EDGE_BLUR_EDGE_RADIUS_DFLT		3.0

#define	EDGE_BLUR_BLUR_AMOUNT_MIN		0.0
#define	EDGE_BLUR_BLUR_AMOUNT_MAX		50.0
#define	EDGE_BLUR_BLUR_AMOUNT_DFLT		5.0

#define	EDGE_BLUR_DILATE_AMT_MIN		0.0
#define	EDGE_BLUR_DILATE_AMT_MAX		10.0
#define	EDGE_BLUR_DILATE_AMT_DFLT		2.0

enum {
	EDGE_BLUR_INPUT = 0,
	EDGE_BLUR_EDGE_RADIUS,
	EDGE_BLUR_BLUR_AMOUNT,
	EDGE_BLUR_DILATE_EN,
	EDGE_BLUR_DILATE_AMT,
	EDGE_BLUR_NUM_PARAMS
};

enum {
	EDGE_RADIUS_DISK_ID = 1,
	BLUR_AMOUNT_DISK_ID,
	DILATE_EN_DISK_ID,
	DILATE_AMT_DISK_ID
};

typedef struct EdgeBlurInfo{
	PF_FpLong	edge_radiusF;
	PF_FpLong	blur_amountF;
	PF_Boolean	dilate_en;
	PF_FpLong	dilate_amtF;
} EdgeBlurInfo, *EdgeBlurInfoP, **EdgeBlurInfoH;


extern "C" {

	DllExport
	PF_Err
	EffectMain(
		PF_Cmd			cmd,
		PF_InData		*in_data,
		PF_OutData		*out_data,
		PF_ParamDef		*params[],
		PF_LayerDef		*output,
		void			*extra);

}

#endif // MODERN_EDGE_BLUR_H
