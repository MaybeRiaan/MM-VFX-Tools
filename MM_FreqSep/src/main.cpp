/*  main.cpp — MM FreqSep ("Skin Retouch") entry points.

    Matte-driven frequency separation: split source into low (tone/color)
    and high (texture/detail) bands, process each, recombine, blend back.
    Full plan: docs/mm-freqsep-plan.md.

    This file owns the non-render commands: About, GlobalSetup (outflags —
    must mirror resources/MM_FreqSepPiPL.r literals exactly), ParamsSetup
    (all 11 params, disk IDs locked), and the registration entry points.
    Render lives in smart_render.cpp; blur kernel in blur.cpp; matte.cpp and
    curve.cpp are Phase 3/Working-Curve stubs.

    MFR-safe: no mutable global/static state anywhere in this plugin.
*/

#define NOMINMAX
#include "params.h"

static PF_Err
About(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *[], PF_LayerDef *)
{
	AEGP_SuiteHandler suites(in_data->pica_basicP);
	suites.ANSICallbacksSuite1()->sprintf(out_data->return_msg,
		"%s v%d.%d\rMatte-driven frequency separation retouching.",
		"Skin Retouch", MAJOR_VERSION, MINOR_VERSION);
	return PF_Err_NONE;
}

static PF_Err
GlobalSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *[], PF_LayerDef *)
{
	out_data->my_version = PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION,
									  STAGE_VERSION, BUILD_VERSION);
	// Must match resources/MM_FreqSepPiPL.r literals exactly (0x02000440 /
	// 0x08001400) or AE rejects the plugin with an outflags mismatch.
	out_data->out_flags  = PF_OutFlag_DEEP_COLOR_AWARE     |
						   PF_OutFlag_PIX_INDEPENDENT      |
						   PF_OutFlag_USE_OUTPUT_EXTENT    |
						   PF_OutFlag_SEND_UPDATE_PARAMS_UI;   // grey-out Edge Preservation
	out_data->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE         |
						   PF_OutFlag2_SUPPORTS_SMART_RENDER     |
						   PF_OutFlag2_SUPPORTS_THREADED_RENDERING;
	return PF_Err_NONE;
}

static PF_Err
ParamsSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *[], PF_LayerDef *)
{
	PF_Err      err = PF_Err_NONE;
	PF_ParamDef def;

	// --- Matte driver -----------------------------------------------------

	AEFX_CLR_STRUCT(def);
	// PF_LayerDefault_NONE → user starts with no matte; render treats that
	// as matte = 1.0 (effect applies everywhere).
	PF_ADD_LAYER("Matte Layer", PF_LayerDefault_NONE, FS_MATTE_LAYER_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Matte Channel",
		FS_MATTE_CHAN_NUM - 1, FS_MATTE_CHAN_DFLT,
		FS_MATTE_CHAN_ITEMS, FS_MATTE_CHANNEL_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Matte Fit",
		FS_MATTE_FIT_NUM - 1, FS_MATTE_FIT_DFLT,
		FS_MATTE_FIT_ITEMS, FS_MATTE_FIT_DISK_ID);

	// --- Separation --------------------------------------------------------

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Separation Radius",
		FS_RADIUS_MIN, FS_RADIUS_MAX, FS_RADIUS_MIN, FS_RADIUS_SLIDER_MAX,
		FS_RADIUS_DFLT, PF_Precision_TENTHS, 0, 0, FS_RADIUS_DISK_ID);

	// SUPERVISE so PF_Cmd_USER_CHANGED_PARAM fires when Method changes — that's
	// where we grey Edge Preservation in/out (Guided uses it, Gaussian doesn't).
	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUPX("Separation Method",
		FS_METHOD_NUM - 1, FS_METHOD_DFLT,
		FS_METHOD_ITEMS, PF_ParamFlag_SUPERVISE, FS_METHOD_DISK_ID);

	// Edge Preservation — the guided filter's regularizer, exposed 0-100.
	// Sits right after Method (its only relevant control). Registered DISABLED
	// because the default Method is Gaussian; FS_UpdateEdgeEnable toggles it
	// whenever Method changes.
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX_DISABLED("Edge Preservation",
		FS_EDGE_MIN, FS_EDGE_MAX, FS_EDGE_MIN, FS_EDGE_MAX,
		FS_EDGE_DFLT, PF_Precision_TENTHS, 0, 0, FS_EDGE_DISK_ID);

	// --- Band processing ----------------------------------------------------

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Low Band Smooth",
		FS_SMOOTH_MIN, FS_SMOOTH_MAX, FS_SMOOTH_MIN, FS_SMOOTH_MAX,
		FS_SMOOTH_DFLT, PF_Precision_TENTHS, 0, 0, FS_LO_SMOOTH_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("High Band Texture",
		FS_TEXTURE_MIN, FS_TEXTURE_MAX, FS_TEXTURE_MIN, FS_TEXTURE_MAX,
		FS_TEXTURE_DFLT, PF_Precision_TENTHS, 0, 0, FS_HI_TEXTURE_DISK_ID);

	// --- Output -------------------------------------------------------------

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Mix",
		FS_MIX_MIN, FS_MIX_MAX, FS_MIX_MIN, FS_MIX_MAX,
		FS_MIX_DFLT, PF_Precision_TENTHS, 0, 0, FS_MIX_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("View",
		FS_VIEW_NUM - 1, FS_VIEW_DFLT,
		FS_VIEW_ITEMS, FS_VIEW_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Working Curve",
		FS_CURVE_NUM - 1, FS_CURVE_DFLT,
		FS_CURVE_ITEMS, FS_CURVE_DISK_ID);

	out_data->num_params = FS_NUM_PARAMS;
	return err;
}

// ---------------------------------------------------------------------------
// Edge Preservation grey-out. Active only for Method = Guided Filter; greyed
// (PF_PUI_DISABLED) for Gaussian so artists never drag a dead control.
//
// Reads the CURRENT Method from params[] (already populated for both
// USER_CHANGED_PARAM and UPDATE_PARAMS_UI) and pushes the disabled bit onto
// the Edge param via PF_UpdateParamUI. UI-only: no value change, no undo, no
// re-render. Requires PF_OutFlag_SEND_UPDATE_PARAMS_UI (set in GlobalSetup).
// ---------------------------------------------------------------------------
static PF_Err
FS_UpdateEdgeEnable(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[])
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;

	const bool guided = (params[FS_METHOD]->u.pd.value == FS_METHOD_GUIDED);

	// Only the UI flag is touched — the sole PF_UpdateParamUI-permitted change
	// we make. Write it onto the live paramdef, then push via the suite.
	const PF_ParamUIFlags before = params[FS_EDGE]->ui_flags;
	if (guided) params[FS_EDGE]->ui_flags &= ~PF_PUI_DISABLED;
	else        params[FS_EDGE]->ui_flags |=  PF_PUI_DISABLED;

	if (params[FS_EDGE]->ui_flags == before)
		return PF_Err_NONE;   // already in the right state — nothing to push

	PF_ParamUtilsSuite3 *pusP = nullptr;
	ERR(AEFX_AcquireSuite(in_data, out_data,
		kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3, nullptr,
		reinterpret_cast<void**>(&pusP)));
	if (!err && pusP) {
		ERR(pusP->PF_UpdateParamUI(in_data->effect_ref, FS_EDGE, params[FS_EDGE]));
	}
	if (pusP) {
		ERR2(AEFX_ReleaseSuite(in_data, out_data,
			kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3, nullptr));
	}
	return err;
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------
extern "C" DllExport PF_Err
PluginDataEntryFunction2(PF_PluginDataPtr inPtr, PF_PluginDataCB2 inPluginDataCallBackPtr,
	SPBasicSuite *inSPBasicSuitePtr, const char *inHostName, const char *inHostVersion)
{
	PF_Err result = PF_Err_NONE;
	result = PF_REGISTER_EFFECT_EXT2(inPtr, inPluginDataCallBackPtr,
		"Skin Retouch",		// display name
		"MM Skin Retouch",	// match name — forever
		"MM",				// category
		AE_RESERVED_INFO, "EffectMain", "");
	return result;
}

PF_Err EffectMain(PF_Cmd cmd, PF_InData *in_data, PF_OutData *out_data,
	PF_ParamDef *params[], PF_LayerDef *output, void *extra)
{
	// No exceptions cross the plugin boundary — everything maps to a PF_Err.
	PF_Err err = PF_Err_NONE;
	try {
		switch (cmd) {
			case PF_Cmd_ABOUT:            err = About(in_data, out_data, params, output);					break;
			case PF_Cmd_GLOBAL_SETUP:     err = GlobalSetup(in_data, out_data, params, output);				break;
			case PF_Cmd_PARAMS_SETUP:     err = ParamsSetup(in_data, out_data, params, output);				break;
			case PF_Cmd_SMART_PRE_RENDER: err = FS_PreRender(in_data, out_data, (PF_PreRenderExtra*)extra);	break;
			case PF_Cmd_SMART_RENDER:     err = FS_SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra); break;

			// Grey Edge Preservation in/out with the Method popup. USER_CHANGED
			// fires on an interactive edit (SUPERVISE on the Method param);
			// UPDATE_PARAMS_UI fires on load / undo / preset so the disabled
			// state is correct without a user edit.
			case PF_Cmd_USER_CHANGED_PARAM:
				err = FS_UpdateEdgeEnable(in_data, out_data, params);
				break;
			case PF_Cmd_UPDATE_PARAMS_UI:
				err = FS_UpdateEdgeEnable(in_data, out_data, params);
				break;
		}
	} catch (PF_Err &e) { err = e; }
	  catch (...)       { err = PF_Err_INTERNAL_STRUCT_DAMAGED; }
	return err;
}
