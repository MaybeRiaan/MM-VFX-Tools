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

/*	ModernEdgeBlur.cpp	*/

#include "ModernEdgeBlur.h"

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
        "%s v%d.%d.%d.%d\r%s",
        STR(StrID_Name),
        MAJOR_VERSION,
        MINOR_VERSION,
        BUG_VERSION,
        BUILD_VERSION,
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
							EDGE_BLUR_EDGE_RADIUS_MIN, 
							EDGE_BLUR_EDGE_RADIUS_MAX, 
							EDGE_BLUR_EDGE_RADIUS_MIN, 
							EDGE_BLUR_EDGE_RADIUS_MAX, 
							EDGE_BLUR_EDGE_RADIUS_DFLT,
							PF_Precision_HUNDREDTHS,
							0,
							0,
							EDGE_RADIUS_DISK_ID);

	AEFX_CLR_STRUCT(def);

	PF_ADD_FLOAT_SLIDERX(	"Edge Offset", 
							EDGE_BLUR_EDGE_OFFSET_MIN, 
							EDGE_BLUR_EDGE_OFFSET_MAX, 
							EDGE_BLUR_EDGE_OFFSET_MIN, 
							EDGE_BLUR_EDGE_OFFSET_MAX, 
							EDGE_BLUR_EDGE_OFFSET_DFLT,
							PF_Precision_HUNDREDTHS,
							0,
							0,
							EDGE_OFFSET_DISK_ID);

	AEFX_CLR_STRUCT(def);

	PF_ADD_FLOAT_SLIDERX(	"Blur Amount", 
							EDGE_BLUR_BLUR_AMOUNT_MIN, 
							EDGE_BLUR_BLUR_AMOUNT_MAX, 
							EDGE_BLUR_BLUR_AMOUNT_MIN, 
							EDGE_BLUR_BLUR_AMOUNT_MAX, 
							EDGE_BLUR_BLUR_AMOUNT_DFLT,
							PF_Precision_HUNDREDTHS,
							0,
							0,
							BLUR_AMOUNT_DISK_ID);

	AEFX_CLR_STRUCT(def);

	PF_ADD_POPUP(	"Blur Type",
					2,
					BLUR_TYPE_BOX + 1,
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
							EDGE_BLUR_DILATE_AMT_MIN, 
							EDGE_BLUR_DILATE_AMT_MAX, 
							EDGE_BLUR_DILATE_AMT_MIN, 
							EDGE_BLUR_DILATE_AMT_MAX, 
							EDGE_BLUR_DILATE_AMT_DFLT,
							PF_Precision_HUNDREDTHS,
							0,
							0,
							DILATE_AMT_DISK_ID);
	
	out_data->num_params = EDGE_BLUR_NUM_PARAMS;

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
		// Step 1: Edge Detection Heat Map
		// Edge pixels are semi-transparent (alpha between 1 and max-1)
		A_u_short alpha = inP->alpha;
		
		if (alpha > 0 && alpha < PF_MAX_CHAN16) {
			// This is an edge pixel - output WHITE
			outP->alpha = PF_MAX_CHAN16;
			outP->red = PF_MAX_CHAN16;
			outP->green = PF_MAX_CHAN16;
			outP->blue = PF_MAX_CHAN16;
		} else {
			// Fully transparent or fully opaque - output BLACK
			outP->alpha = PF_MAX_CHAN16;
			outP->red = 0;
			outP->green = 0;
			outP->blue = 0;
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
		// Step 1: Edge Detection Heat Map
		// Edge pixels are semi-transparent (alpha between 1 and 254)
		A_u_char alpha = inP->alpha;
		
		if (alpha > 0 && alpha < PF_MAX_CHAN8) {
			// This is an edge pixel - output WHITE
			outP->alpha = PF_MAX_CHAN8;
			outP->red = PF_MAX_CHAN8;
			outP->green = PF_MAX_CHAN8;
			outP->blue = PF_MAX_CHAN8;
		} else {
			// Fully transparent or fully opaque - output BLACK
			outP->alpha = PF_MAX_CHAN8;
			outP->red = 0;
			outP->green = 0;
			outP->blue = 0;
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
	ebiP.edge_radiusF	= params[EDGE_BLUR_EDGE_RADIUS]->u.fs_d.value;
	ebiP.edge_offsetF	= params[EDGE_BLUR_EDGE_OFFSET]->u.fs_d.value;
	ebiP.blur_amountF	= params[EDGE_BLUR_BLUR_AMOUNT]->u.fs_d.value;
	ebiP.blur_type		= params[EDGE_BLUR_BLUR_TYPE]->u.pd.value - 1; // Convert to 0-based
	ebiP.sub_pixel		= params[EDGE_BLUR_SUB_PIXEL]->u.bd.value;
	ebiP.dilate_en		= params[EDGE_BLUR_DILATE_EN]->u.bd.value;
	ebiP.dilate_amtF	= params[EDGE_BLUR_DILATE_AMT]->u.fs_d.value;
	
	if (PF_WORLD_IS_DEEP(output)){
		ERR(suites.Iterate16Suite2()->iterate(	in_data,
												0,								// progress base
												linesL,							// progress final
												&params[EDGE_BLUR_INPUT]->u.ld,	// src
												NULL,							// area - null for all pixels
												(void*)&ebiP,					// refcon - your custom data pointer
												EdgeBlurFunc16,					// pixel function pointer
												output));
	} else {
		ERR(suites.Iterate8Suite2()->iterate(	in_data,
												0,								// progress base
												linesL,							// progress final
												&params[EDGE_BLUR_INPUT]->u.ld,	// src 
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
		"MM Modern Edge Blur", // Match Name
		"MM", // Category
		AE_RESERVED_INFO, // Reserved Info
		"EffectMain",	// Entry point
		"");	// support URL

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

