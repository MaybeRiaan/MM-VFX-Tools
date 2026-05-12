/*	ModernEdgeBlur.cpp	*/

#include "ModernEdgeBlur.h"
#include <cmath>

// Per-render data passed to iterate callbacks via refcon. Stack-allocated
// inside SmartRender so each frame thread gets its own — MFR-safe.
struct EdgeBlurRenderInfo {
	EdgeBlurInfo	params;
	A_long			input_width;
	A_long			input_height;
};


static PF_Err
About(
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output)
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
GlobalSetup(
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output)
{
	out_data->my_version = PF_VERSION(	MAJOR_VERSION,
										MINOR_VERSION,
										BUG_VERSION,
										STAGE_VERSION,
										BUILD_VERSION);

	out_data->out_flags  =	PF_OutFlag_DEEP_COLOR_AWARE		|
							PF_OutFlag_PIX_INDEPENDENT		|
							PF_OutFlag_USE_OUTPUT_EXTENT;

	out_data->out_flags2 =	PF_OutFlag2_FLOAT_COLOR_AWARE			|
							PF_OutFlag2_SUPPORTS_SMART_RENDER		|
							PF_OutFlag2_SUPPORTS_THREADED_RENDERING;

	return PF_Err_NONE;
}


static PF_Err
ParamsSetup(
	PF_InData		*in_data,
	PF_OutData		*out_data,
	PF_ParamDef		*params[],
	PF_LayerDef		*output)
{
	PF_Err		err = PF_Err_NONE;
	PF_ParamDef	def;

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX(	"Edge Radius",
							EDGE_BLUR_EDGE_RADIUS_MIN,
							EDGE_BLUR_EDGE_RADIUS_MAX,
							EDGE_BLUR_EDGE_RADIUS_MIN,
							EDGE_BLUR_EDGE_RADIUS_MAX,
							EDGE_BLUR_EDGE_RADIUS_DFLT,
							PF_Precision_HUNDREDTHS,
							0, 0,
							EDGE_RADIUS_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX(	"Edge Offset",
							EDGE_BLUR_EDGE_OFFSET_MIN,
							EDGE_BLUR_EDGE_OFFSET_MAX,
							EDGE_BLUR_EDGE_OFFSET_MIN,
							EDGE_BLUR_EDGE_OFFSET_MAX,
							EDGE_BLUR_EDGE_OFFSET_DFLT,
							PF_Precision_HUNDREDTHS,
							0, 0,
							EDGE_OFFSET_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX(	"Blur Amount",
							EDGE_BLUR_BLUR_AMOUNT_MIN,
							EDGE_BLUR_BLUR_AMOUNT_MAX,
							EDGE_BLUR_BLUR_AMOUNT_MIN,
							EDGE_BLUR_BLUR_AMOUNT_MAX,
							EDGE_BLUR_BLUR_AMOUNT_DFLT,
							PF_Precision_HUNDREDTHS,
							0, 0,
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
							0, 0,
							DILATE_AMT_DISK_ID);

	out_data->num_params = EDGE_BLUR_NUM_PARAMS;

	return err;
}


// Map an alpha value (0..1 normalized) to a test heat-map output.
// Full opaque or full transparent → 0. Closer to alpha=0.5 → 1. The width of the
// non-zero band is controlled by edge_radius (the param value, in slider units).
// This is a stand-in for the real edge-weight calculation so we can visually
// confirm the radius slider has effect across all bit depths.
static PF_FpLong
TestEdgeWeight(PF_FpLong alpha_norm, PF_FpLong edge_radius)
{
	if (alpha_norm <= 0.0 || alpha_norm >= 1.0) {
		return 0.0;
	}
	// Distance from the middle of the alpha range. 0 at center, 0.5 at edges.
	const PF_FpLong dist = std::fabs(alpha_norm - 0.5);
	// Normalize edge_radius (slider 0..20) to a falloff width 0..0.5.
	const PF_FpLong falloff = edge_radius / (EDGE_BLUR_EDGE_RADIUS_MAX * 2.0);
	if (falloff <= 0.0) {
		return 0.0;
	}
	const PF_FpLong w = 1.0 - (dist / falloff);
	return (w < 0.0) ? 0.0 : ((w > 1.0) ? 1.0 : w);
}


static PF_Err
EdgeBlurFunc8(
	void		*refcon,
	A_long		xL,
	A_long		yL,
	PF_Pixel8	*inP,
	PF_Pixel8	*outP)
{
	const EdgeBlurRenderInfo *info = static_cast<EdgeBlurRenderInfo*>(refcon);
	const PF_FpLong alpha_norm = (PF_FpLong)inP->alpha / (PF_FpLong)PF_MAX_CHAN8;
	const PF_FpLong w = TestEdgeWeight(alpha_norm, info->params.edge_radiusF);
	const A_u_char v = (A_u_char)(w * PF_MAX_CHAN8);
	outP->alpha = PF_MAX_CHAN8;
	outP->red   = v;
	outP->green = v;
	outP->blue  = v;
	return PF_Err_NONE;
}


static PF_Err
EdgeBlurFunc16(
	void		*refcon,
	A_long		xL,
	A_long		yL,
	PF_Pixel16	*inP,
	PF_Pixel16	*outP)
{
	const EdgeBlurRenderInfo *info = static_cast<EdgeBlurRenderInfo*>(refcon);
	const PF_FpLong alpha_norm = (PF_FpLong)inP->alpha / (PF_FpLong)PF_MAX_CHAN16;
	const PF_FpLong w = TestEdgeWeight(alpha_norm, info->params.edge_radiusF);
	const A_u_short v = (A_u_short)(w * PF_MAX_CHAN16);
	outP->alpha = PF_MAX_CHAN16;
	outP->red   = v;
	outP->green = v;
	outP->blue  = v;
	return PF_Err_NONE;
}


static PF_Err
EdgeBlurFuncFloat(
	void			*refcon,
	A_long			xL,
	A_long			yL,
	PF_PixelFloat	*inP,
	PF_PixelFloat	*outP)
{
	const EdgeBlurRenderInfo *info = static_cast<EdgeBlurRenderInfo*>(refcon);
	const PF_FpLong alpha_norm = inP->alpha;	// 32-bit float is already 0..1 (HDR may exceed)
	const PF_FpLong w = TestEdgeWeight(alpha_norm, info->params.edge_radiusF);
	const PF_FpShort v = (PF_FpShort)w;
	outP->alpha = 1.0f;
	outP->red   = v;
	outP->green = v;
	outP->blue  = v;
	return PF_Err_NONE;
}


// Read all params at the given time into an EdgeBlurInfo. Called from both
// PreRender (to size the input region) and SmartRender (to drive the iterate).
static PF_Err
ReadParams(
	PF_InData		*in_data,
	EdgeBlurInfo	*out_params)
{
	PF_Err		err = PF_Err_NONE;
	PF_ParamDef	def;

	AEFX_CLR_STRUCT(def);

	#define CHECKOUT_FLOAT(IDX, FIELD)											\
		AEFX_CLR_STRUCT(def);													\
		ERR(PF_CHECKOUT_PARAM(in_data, IDX,										\
			in_data->current_time, in_data->time_step, in_data->time_scale,		\
			&def));																\
		out_params->FIELD = def.u.fs_d.value;									\
		ERR2(PF_CHECKIN_PARAM(in_data, &def));

	#define CHECKOUT_POPUP(IDX, FIELD)											\
		AEFX_CLR_STRUCT(def);													\
		ERR(PF_CHECKOUT_PARAM(in_data, IDX,										\
			in_data->current_time, in_data->time_step, in_data->time_scale,		\
			&def));																\
		out_params->FIELD = def.u.pd.value - 1;	/* popup is 1-based */			\
		ERR2(PF_CHECKIN_PARAM(in_data, &def));

	#define CHECKOUT_BOOL(IDX, FIELD)											\
		AEFX_CLR_STRUCT(def);													\
		ERR(PF_CHECKOUT_PARAM(in_data, IDX,										\
			in_data->current_time, in_data->time_step, in_data->time_scale,		\
			&def));																\
		out_params->FIELD = def.u.bd.value;										\
		ERR2(PF_CHECKIN_PARAM(in_data, &def));

	PF_Err err2 = PF_Err_NONE;
	CHECKOUT_FLOAT(EDGE_BLUR_EDGE_RADIUS, edge_radiusF);
	CHECKOUT_FLOAT(EDGE_BLUR_EDGE_OFFSET, edge_offsetF);
	CHECKOUT_FLOAT(EDGE_BLUR_BLUR_AMOUNT, blur_amountF);
	CHECKOUT_POPUP(EDGE_BLUR_BLUR_TYPE,   blur_type);
	CHECKOUT_BOOL (EDGE_BLUR_SUB_PIXEL,   sub_pixel);
	CHECKOUT_BOOL (EDGE_BLUR_DILATE_EN,   dilate_en);
	CHECKOUT_FLOAT(EDGE_BLUR_DILATE_AMT,  dilate_amtF);

	#undef CHECKOUT_FLOAT
	#undef CHECKOUT_POPUP
	#undef CHECKOUT_BOOL

	return err;
}


// How many pixels of input we need beyond each side of the requested output
// region, so the blur convolution has the source it needs at the boundary.
static A_long
InflationPixels(const EdgeBlurInfo &p)
{
	// Edge_radius and blur_amount are slider units (interpreted as pixels for
	// now). Dilation also reads neighbors. Add 1 for safety against rounding.
	PF_FpLong total = p.edge_radiusF + p.blur_amountF;
	if (p.dilate_en) {
		total += p.dilate_amtF;
	}
	return (A_long)std::ceil(total) + 1;
}


static PF_Err
PreRender(
	PF_InData			*in_data,
	PF_OutData			*out_data,
	PF_PreRenderExtra	*extra)
{
	PF_Err				err = PF_Err_NONE;
	EdgeBlurInfo		params;
	AEFX_CLR_STRUCT(params);

	ERR(ReadParams(in_data, &params));

	const A_long inflate = InflationPixels(params);

	PF_RenderRequest req = extra->input->output_request;
	req.rect.left   -= inflate;
	req.rect.top    -= inflate;
	req.rect.right  += inflate;
	req.rect.bottom += inflate;
	req.preserve_rgb_of_zero_alpha = TRUE;

	PF_CheckoutResult in_result;
	ERR(extra->cb->checkout_layer(	in_data->effect_ref,
									EDGE_BLUR_INPUT,
									EDGE_BLUR_INPUT,
									&req,
									in_data->current_time,
									in_data->time_step,
									in_data->time_scale,
									&in_result));

	UnionLRect(&in_result.result_rect,     &extra->output->result_rect);
	UnionLRect(&in_result.max_result_rect, &extra->output->max_result_rect);

	return err;
}


static PF_Err
SmartRender(
	PF_InData				*in_data,
	PF_OutData				*out_data,
	PF_SmartRenderExtra		*extra)
{
	PF_Err		err  = PF_Err_NONE;
	PF_Err		err2 = PF_Err_NONE;

	PF_EffectWorld	*input_world  = NULL;
	PF_EffectWorld	*output_world = NULL;

	AEGP_SuiteHandler	suites(in_data->pica_basicP);
	PF_WorldSuite2		*wsP = NULL;

	ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, EDGE_BLUR_INPUT, &input_world));
	ERR(extra->cb->checkout_output(in_data->effect_ref, &output_world));

	ERR(AEFX_AcquireSuite(	in_data,
							out_data,
							kPFWorldSuite,
							kPFWorldSuiteVersion2,
							"Couldn't load PFWorldSuite2.",
							(void**)&wsP));

	EdgeBlurRenderInfo	info;
	AEFX_CLR_STRUCT(info);
	ERR(ReadParams(in_data, &info.params));
	if (input_world) {
		info.input_width  = input_world->width;
		info.input_height = input_world->height;
	}

	PF_PixelFormat format = PF_PixelFormat_INVALID;
	if (!err && wsP) {
		ERR(wsP->PF_GetPixelFormat(output_world, &format));
	}

	if (!err) {
		switch (format) {
			case PF_PixelFormat_ARGB128:
				ERR(suites.IterateFloatSuite2()->iterate(	in_data,
															0,
															output_world->height,
															input_world,
															NULL,
															(void*)&info,
															EdgeBlurFuncFloat,
															output_world));
				break;

			case PF_PixelFormat_ARGB64:
				ERR(suites.Iterate16Suite2()->iterate(		in_data,
															0,
															output_world->height,
															input_world,
															NULL,
															(void*)&info,
															EdgeBlurFunc16,
															output_world));
				break;

			case PF_PixelFormat_ARGB32:
				ERR(suites.Iterate8Suite2()->iterate(		in_data,
															0,
															output_world->height,
															input_world,
															NULL,
															(void*)&info,
															EdgeBlurFunc8,
															output_world));
				break;

			default:
				err = PF_Err_BAD_CALLBACK_PARAM;
				break;
		}
	}

	if (wsP) {
		ERR2(AEFX_ReleaseSuite(	in_data,
								out_data,
								kPFWorldSuite,
								kPFWorldSuiteVersion2,
								"Couldn't release PFWorldSuite2."));
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
		"Modern Edge Blur",
		"MM Modern Edge Blur",
		"MM",
		AE_RESERVED_INFO,
		"EffectMain",
		"");

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
	PF_Err err = PF_Err_NONE;

	try {
		switch (cmd) {
			case PF_Cmd_ABOUT:
				err = About(in_data, out_data, params, output);
				break;

			case PF_Cmd_GLOBAL_SETUP:
				err = GlobalSetup(in_data, out_data, params, output);
				break;

			case PF_Cmd_PARAMS_SETUP:
				err = ParamsSetup(in_data, out_data, params, output);
				break;

			case PF_Cmd_SMART_PRE_RENDER:
				err = PreRender(in_data, out_data, (PF_PreRenderExtra*)extra);
				break;

			case PF_Cmd_SMART_RENDER:
				err = SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra);
				break;
		}
	} catch (PF_Err &thrown_err) {
		err = thrown_err;
	}
	return err;
}
