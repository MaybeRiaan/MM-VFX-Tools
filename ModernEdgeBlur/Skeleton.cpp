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

/*	Skeleton.cpp	

	This is a compiling husk of a project. Fill it in with interesting
	pixel processing code.
	
	Revision History

	Version		Change													Engineer	Date
	=======		======													========	======
	1.0			(seemed like a good idea at the time)					bbb			6/1/2002

	1.0			Okay, I'm leaving the version at 1.0,					bbb			2/15/2006
				for obvious reasons; you're going to 
				copy these files directly! This is the
				first XCode version, though.

	1.0			Let's simplify this barebones sample					zal			11/11/2010

	1.0			Added new entry point									zal			9/18/2017
	1.1			Added 'Support URL' to PiPL and entry point				cjr			3/31/2023

*/

#include "Skeleton.h"

static PF_Err 
About (	
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output )
{
	AEGP_SuiteHandler suites(in_data->pica_basicP);
	
	suites.ANSICallbacksSuite1()->sprintf(
        out_data->return_msg,
        "%s v%d.%d\r%s",
        STR(StrID_Name),
        MAJOR_VERSION,
        MINOR_VERSION,
        STR(StrID_Description));
        
	return PF_Err_NONE;
}

static PF_Err 
GlobalSetup (	
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output )
{
	out_data->my_version = PF_VERSION(	MAJOR_VERSION, 
										MINOR_VERSION,
										BUG_VERSION, 
										STAGE_VERSION, 
										BUILD_VERSION);

	out_data->out_flags =  PF_OutFlag_DEEP_COLOR_AWARE;	// just 16bpc, not 32bpc
	
	return PF_Err_NONE;
}

static PF_Err 
ParamsSetup (	
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output )
{
	PF_Err		err		= PF_Err_NONE;
	PF_ParamDef	def;	

	AEFX_CLR_STRUCT(def);

	PF_ADD_FLOAT_SLIDERX(	"Edge Radius", 
							SKELETON_EDGE_RADIUS_MIN, 
							SKELETON_EDGE_RADIUS_MAX, 
							SKELETON_EDGE_RADIUS_MIN, 
							SKELETON_EDGE_RADIUS_MAX, 
							SKELETON_EDGE_RADIUS_DFLT,
							PF_Precision_HUNDREDTHS,
							0,
							0,
							EDGE_RADIUS_DISK_ID);

	AEFX_CLR_STRUCT(def);

	PF_ADD_FLOAT_SLIDERX(	"Edge Offset", 
							SKELETON_EDGE_OFFSET_MIN, 
							SKELETON_EDGE_OFFSET_MAX, 
							SKELETON_EDGE_OFFSET_MIN, 
							SKELETON_EDGE_OFFSET_MAX, 
							SKELETON_EDGE_OFFSET_DFLT,
							PF_Precision_HUNDREDTHS,
							0,
							0,
							EDGE_OFFSET_DISK_ID);

	AEFX_CLR_STRUCT(def);

	PF_ADD_FLOAT_SLIDERX(	"Blur Amount", 
							SKELETON_BLUR_AMOUNT_MIN, 
							SKELETON_BLUR_AMOUNT_MAX, 
							SKELETON_BLUR_AMOUNT_MIN, 
							SKELETON_BLUR_AMOUNT_MAX, 
							SKELETON_BLUR_AMOUNT_DFLT,
							PF_Precision_HUNDREDTHS,
							0,
							0,
							BLUR_AMOUNT_DISK_ID);

	AEFX_CLR_STRUCT(def);

	PF_ADD_POPUP(	"Blur Type",
					2,
					1,
					"Box|Gaussian",
					BLUR_TYPE_DISK_ID);

	AEFX_CLR_STRUCT(def);

	PF_ADD_CHECKBOXX(	"Sub-Pixel Quality",
						FALSE,
						0,
						SUB_PIXEL_DISK_ID);

	AEFX_CLR_STRUCT(def);

	PF_ADD_CHECKBOXX(	"Enable Color Dilation",
						FALSE,
						0,
						DILATE_EN_DISK_ID);

	AEFX_CLR_STRUCT(def);

	PF_ADD_FLOAT_SLIDERX(	"Dilation Amount", 
							SKELETON_DILATE_AMT_MIN, 
							SKELETON_DILATE_AMT_MAX, 
							SKELETON_DILATE_AMT_MIN, 
							SKELETON_DILATE_AMT_MAX, 
							SKELETON_DILATE_AMT_DFLT,
							PF_Precision_HUNDREDTHS,
							0,
							0,
							DILATE_AMT_DISK_ID);
	
	out_data->num_params = SKELETON_NUM_PARAMS;

	return err;
}

// Helper: Calculate edge weight based on alpha value
static PF_FpLong
CalculateEdgeWeight(
	A_long		alpha,
	A_long		max_val,
	PF_FpLong	edge_radius,
	PF_FpLong	edge_offset)
{
	// Edge detection: alpha between 1 and max-1 indicates edge
	if (alpha <= 0 || alpha >= max_val) {
		return 0.0; // Fully transparent or opaque = no edge
	}
	
	// Normalize alpha to 0.0-1.0
	PF_FpLong alpha_norm = (PF_FpLong)alpha / (PF_FpLong)max_val;
	
	// Calculate distance from edge center (0.5)
	PF_FpLong dist_from_center = fabs(alpha_norm - 0.5);
	
	// Apply offset
	dist_from_center -= (edge_offset / 100.0);
	
	// Calculate weight based on radius
	PF_FpLong weight = 0.0;
	if (edge_radius > 0.0) {
		weight = 1.0 - (dist_from_center / (edge_radius / 100.0));
		weight = MAX(0.0, MIN(1.0, weight));
	}
	
	return weight;
}

static PF_Err
EdgeBlurFunc16 (
	void		*refcon, 
	A_long		xL, 
	A_long		yL, 
	PF_Pixel16	*inP, 
	PF_Pixel16	*outP)
{
	PF_Err			err = PF_Err_NONE;
	EdgeBlurInfo	*ebiP = reinterpret_cast<EdgeBlurInfo*>(refcon);
					
	if (ebiP){
		// Calculate edge weight for this pixel
		PF_FpLong edge_weight = CalculateEdgeWeight(
			inP->alpha,
			PF_MAX_CHAN16,
			ebiP->edge_radiusF,
			ebiP->edge_offsetF);
		
		// For now, just pass through with edge detection
		// Full blur implementation will be added in subsequent steps
		outP->alpha = inP->alpha;
		outP->red = inP->red;
		outP->green = inP->green;
		outP->blue = inP->blue;
		
		// Visual debug: tint edges red based on weight
		if (edge_weight > 0.0) {
			A_long tint = (A_long)(edge_weight * PF_MAX_CHAN16 * 0.3);
			outP->red = MIN(outP->red + tint, PF_MAX_CHAN16);
		}
	}

	return err;
}

static PF_Err
EdgeBlurFunc8 (
	void		*refcon, 
	A_long		xL, 
	A_long		yL, 
	PF_Pixel8	*inP, 
	PF_Pixel8	*outP)
{
	PF_Err			err = PF_Err_NONE;
	EdgeBlurInfo	*ebiP = reinterpret_cast<EdgeBlurInfo*>(refcon);
					
	if (ebiP){
		// Calculate edge weight for this pixel
		PF_FpLong edge_weight = CalculateEdgeWeight(
			inP->alpha,
			PF_MAX_CHAN8,
			ebiP->edge_radiusF,
			ebiP->edge_offsetF);
		
		// For now, just pass through with edge detection
		// Full blur implementation will be added in subsequent steps
		outP->alpha = inP->alpha;
		outP->red = inP->red;
		outP->green = inP->green;
		outP->blue = inP->blue;
		
		// Visual debug: tint edges red based on weight
		if (edge_weight > 0.0) {
			A_long tint = (A_long)(edge_weight * PF_MAX_CHAN8 * 0.3);
			outP->red = MIN(outP->red + tint, PF_MAX_CHAN8);
		}
	}

	return err;
}

static PF_Err 
Render (
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output )
{
	PF_Err				err		= PF_Err_NONE;
	AEGP_SuiteHandler	suites(in_data->pica_basicP);

	/*	Edge Blur Processing */
	EdgeBlurInfo		ebiP;
	AEFX_CLR_STRUCT(ebiP);
	A_long				linesL	= 0;

	linesL = output->extent_hint.bottom - output->extent_hint.top;
	
	// Gather parameters
	ebiP.edge_radiusF	= params[SKELETON_EDGE_RADIUS]->u.fs_d.value;
	ebiP.edge_offsetF	= params[SKELETON_EDGE_OFFSET]->u.fs_d.value;
	ebiP.blur_amountF	= params[SKELETON_BLUR_AMOUNT]->u.fs_d.value;
	ebiP.blur_type		= params[SKELETON_BLUR_TYPE]->u.pd.value - 1; // Convert to 0-based
	ebiP.sub_pixel		= params[SKELETON_SUB_PIXEL]->u.bd.value;
	ebiP.dilate_en		= params[SKELETON_DILATE_EN]->u.bd.value;
	ebiP.dilate_amtF	= params[SKELETON_DILATE_AMT]->u.fs_d.value;
	
	if (PF_WORLD_IS_DEEP(output)){
		ERR(suites.Iterate16Suite2()->iterate(	in_data,
												0,								// progress base
												linesL,							// progress final
												&params[SKELETON_INPUT]->u.ld,	// src 
												NULL,							// area - null for all pixels
												(void*)&ebiP,					// refcon - your custom data pointer
												EdgeBlurFunc16,					// pixel function pointer
												output));
	} else {
		ERR(suites.Iterate8Suite2()->iterate(	in_data,
												0,								// progress base
												linesL,							// progress final
												&params[SKELETON_INPUT]->u.ld,	// src 
												NULL,							// area - null for all pixels
												(void*)&ebiP,					// refcon - your custom data pointer
												EdgeBlurFunc8,					// pixel function pointer
												output));	
	}

	return err;
}


extern "C" DllExport
PF_Err PluginDataEntryFunction2(
	PF_PluginDataPtr inPtr,
	PF_PluginDataCB2 inPluginDataCallBackPtr,
	SPBasicSuite* inSPBasicSuitePtr,
	const char* inHostName,
	const char* inHostVersion)
{
	PF_Err result = PF_Err_INVALID_CALLBACK;

	result = PF_REGISTER_EFFECT_EXT2(
		inPtr,
		inPluginDataCallBackPtr,
		"Modern Edge Blur", // Name
		"ADBE Modern Edge Blur", // Match Name
		"Sample Plug-ins", // Category
		AE_RESERVED_INFO, // Reserved Info
		"EffectMain",	// Entry point
		"https://www.adobe.com");	// support URL

	return result;
}


PF_Err
EffectMain(
	PF_Cmd			cmd,
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output,
	void			*extra)
{
	PF_Err		err = PF_Err_NONE;
	
	try {
		switch (cmd) {
			case PF_Cmd_ABOUT:

				err = About(in_data,
							out_data,
							params,
							output);
				break;
				
			case PF_Cmd_GLOBAL_SETUP:

				err = GlobalSetup(	in_data,
									out_data,
									params,
									output);
				break;
				
			case PF_Cmd_PARAMS_SETUP:

				err = ParamsSetup(	in_data,
									out_data,
									params,
									output);
				break;
				
			case PF_Cmd_RENDER:

				err = Render(	in_data,
								out_data,
								params,
								output);
				break;
		}
	}
	catch(PF_Err &thrown_err){
		err = thrown_err;
	}
	return err;
}

