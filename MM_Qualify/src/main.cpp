/*  main.cpp — MM Qualify ("Qualify") entry points.

    The DaVinci Resolve HSL qualifier rebuilt inside After Effects: three
    ANDed 1D gates (Hue / Saturation / Luma, uniform In / Out / In Soft /
    Out Soft controls), Resolve's matte-finesse chain, temporal stabilize,
    garbage matte. Full plan: docs/hsl_keyer_plugin_plan.md.

    This file owns the non-render commands: About, GlobalSetup (outflags —
    must mirror resources/MM_QualifyPiPL.r literals exactly), ParamsSetup
    (all 45 params, disk IDs pre-release), the Attack/Release grey-out, and
    the eyedropper dispatch (sampling.cpp). Render lives in smart_render.cpp.

    Every gate parameter is a standard AE float/angle param — natively
    keyframeable and expression-drivable, the headline differentiator over
    Resolve (plan §1).

    MFR-safe: no mutable global/static state anywhere in this plugin.
*/

#define NOMINMAX
#include "params.h"

static PF_Err
About(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *[], PF_LayerDef *)
{
	AEGP_SuiteHandler suites(in_data->pica_basicP);
	suites.ANSICallbacksSuite1()->sprintf(out_data->return_msg,
		"%s v%d.%d\rHSL qualifier: Resolve-style keying with keyframeable gates.",
		"Qualify", MAJOR_VERSION, MINOR_VERSION);
	return PF_Err_NONE;
}

static PF_Err
GlobalSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *[], PF_LayerDef *)
{
	out_data->my_version = PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION,
									  STAGE_VERSION, BUILD_VERSION);
	// Must match resources/MM_QualifyPiPL.r literals exactly (0x06008442 /
	// 0x2A001400) or AE rejects the plugin with an outflags mismatch.
	// WIDE_TIME_INPUT: temporal stabilize checks out source frames at t-1..
	// t-QK_TEMPORAL_WINDOW in PreRender.
	// CUSTOM_UI: the viewer eyedropper (picker.cpp) takes comp/layer-window
	// events so a click lands on the IMAGE and can be mapped to a layer pixel.
	out_data->out_flags  = PF_OutFlag_DEEP_COLOR_AWARE     |
						   PF_OutFlag_PIX_INDEPENDENT      |
						   PF_OutFlag_USE_OUTPUT_EXTENT    |
						   PF_OutFlag_SEND_UPDATE_PARAMS_UI |   // grey Attack/Release
						   PF_OutFlag_CUSTOM_UI             |   // viewer eyedropper
						   PF_OutFlag_WIDE_TIME_INPUT;
	out_data->out_flags2 = PF_OutFlag2_FLOAT_COLOR_AWARE         |
						   PF_OutFlag2_SUPPORTS_SMART_RENDER     |
						   PF_OutFlag2_SUPPORTS_THREADED_RENDERING |
						   PF_OutFlag2_SUPPORTS_GPU_RENDER_F32     |
						   PF_OutFlag2_SUPPORTS_DIRECTX_RENDERING;
	return PF_Err_NONE;
}

static PF_Err
ParamsSetup(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *[], PF_LayerDef *)
{
	PF_Err      err = PF_Err_NONE;
	PF_ParamDef def;

	// ======================= Qualifier ====================================

	AEFX_CLR_STRUCT(def);
	PF_ADD_TOPIC("Qualifier", QK_TOPIC_QUAL_DISK_ID);

	// --- Sampling FIRST — the first thing the artist reaches for. ONE button
	// arms the viewer eyedropper (picker.cpp); the next click/drag in the comp
	// or layer viewer builds the key from the PRE-EFFECT pixel, so the current
	// Output view (Selection Overlay et al) cannot contaminate a sample.
	// Because a comp/layer custom UI DOES receive modifiers — unlike a colour
	// param — the operation is the modifier, not three separate swatches:
	//     click = Set   ·   Ctrl-click = Add   ·   Alt-click = Subtract
	// Dragging GROWS the selection across everything swept (Resolve's feel).
	// SUPERVISE so the press fires PF_Cmd_USER_CHANGED_PARAM -> QK_ArmPicker.
	AEFX_CLR_STRUCT(def);
	def.flags = PF_ParamFlag_SUPERVISE;
	PF_ADD_BUTTON("Sample", "Click / drag in viewer",
		PF_ParamFlag_SUPERVISE, PF_PUI_NONE, QK_SAMPLE_BUTTON_DISK_ID);

	// --- Hue gate. Center is an ANGLE (unbounded — animating across the 0°
	// seam is smooth; all hue math wraps in qk_core.h). Width is the total
	// arc kept; Soft In/Out feather each edge independently. Default Width
	// 360 == full circle (all hues pass) -> passthrough at reset.
	AEFX_CLR_STRUCT(def);
	PF_ADD_TOPIC("Hue", QK_TOPIC_HUE_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_CHECKBOXX("Hue Enable", TRUE, 0, QK_HUE_ENABLE_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_ANGLE("Hue Center", QK_HUE_CENTER_DFLT, QK_HUE_CENTER_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Hue Width",
		QK_HUE_WIDTH_MIN, QK_HUE_WIDTH_MAX, QK_HUE_WIDTH_MIN, QK_HUE_WIDTH_MAX,
		QK_HUE_WIDTH_DFLT, PF_Precision_TENTHS, 0, 0, QK_HUE_WIDTH_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Hue Soft In",
		QK_HUE_SOFT_MIN, QK_HUE_SOFT_MAX, QK_HUE_SOFT_MIN, QK_HUE_SOFT_MAX,
		QK_HUE_SOFT_DFLT, PF_Precision_TENTHS, 0, 0, QK_HUE_IN_SOFT_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Hue Soft Out",
		QK_HUE_SOFT_MIN, QK_HUE_SOFT_MAX, QK_HUE_SOFT_MIN, QK_HUE_SOFT_MAX,
		QK_HUE_SOFT_DFLT, PF_Precision_TENTHS, 0, 0, QK_HUE_OUT_SOFT_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_END_TOPIC(QK_TOPIC_HUE_END_DISK_ID);

	// --- Saturation gate.
	AEFX_CLR_STRUCT(def);
	PF_ADD_TOPIC("Saturation", QK_TOPIC_SAT_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_CHECKBOXX("Sat Enable", TRUE, 0, QK_SAT_ENABLE_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Sat In",
		QK_SAT_MIN, QK_SAT_VALID_MAX, QK_SAT_MIN, QK_SAT_SLIDER_MAX,
		QK_SAT_IN_DFLT, PF_Precision_THOUSANDTHS, 0, 0, QK_SAT_IN_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Sat Out",
		QK_SAT_MIN, QK_SAT_VALID_MAX, QK_SAT_MIN, QK_SAT_SLIDER_MAX,
		QK_SAT_OUT_DFLT, PF_Precision_THOUSANDTHS, 0, 0, QK_SAT_OUT_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Sat In Soft",
		QK_SAT_MIN, QK_SAT_SOFT_MAX, QK_SAT_MIN, QK_SAT_SOFT_MAX,
		QK_SAT_SOFT_DFLT, PF_Precision_THOUSANDTHS, 0, 0, QK_SAT_IN_SOFT_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Sat Out Soft",
		QK_SAT_MIN, QK_SAT_SOFT_MAX, QK_SAT_MIN, QK_SAT_SOFT_MAX,
		QK_SAT_SOFT_DFLT, PF_Precision_THOUSANDTHS, 0, 0, QK_SAT_OUT_SOFT_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_END_TOPIC(QK_TOPIC_SAT_END_DISK_ID);

	// --- Luma gate. HDR-aware: bounds valid to 8.0 (sliders show 0..1).
	AEFX_CLR_STRUCT(def);
	PF_ADD_TOPIC("Luma", QK_TOPIC_LUM_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_CHECKBOXX("Luma Enable", TRUE, 0, QK_LUM_ENABLE_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Luma In",
		QK_LUM_MIN, QK_LUM_VALID_MAX, QK_LUM_MIN, QK_LUM_SLIDER_MAX,
		QK_LUM_IN_DFLT, PF_Precision_THOUSANDTHS, 0, 0, QK_LUM_IN_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Luma Out",
		QK_LUM_MIN, QK_LUM_VALID_MAX, QK_LUM_MIN, QK_LUM_SLIDER_MAX,
		QK_LUM_OUT_DFLT, PF_Precision_THOUSANDTHS, 0, 0, QK_LUM_OUT_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Luma In Soft",
		QK_LUM_MIN, QK_LUM_SOFT_VALID_MAX, QK_LUM_MIN, QK_LUM_SOFT_SLIDER_MAX,
		QK_LUM_SOFT_DFLT, PF_Precision_THOUSANDTHS, 0, 0, QK_LUM_IN_SOFT_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Luma Out Soft",
		QK_LUM_MIN, QK_LUM_SOFT_VALID_MAX, QK_LUM_MIN, QK_LUM_SOFT_SLIDER_MAX,
		QK_LUM_SOFT_DFLT, PF_Precision_THOUSANDTHS, 0, 0, QK_LUM_OUT_SOFT_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_END_TOPIC(QK_TOPIC_LUM_END_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_END_TOPIC(QK_TOPIC_QUAL_END_DISK_ID);

	// ======================= Output =======================================

	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Output",
		QK_OUTPUT_NUM - 1, QK_OUTPUT_DFLT, QK_OUTPUT_ITEMS, QK_OUTPUT_DISK_ID);

	// ======================= Matte Finesse ================================
	// Fixed chain order matching Resolve so muscle memory transfers:
	// Denoise (pre-filter) -> Clean Black -> Clean White -> Clips ->
	// [Temporal] -> Blur Radius -> In/Out Ratio (plan §2; temporal sits
	// before the blur — see smart_render.cpp's chain note).

	AEFX_CLR_STRUCT(def);
	PF_ADD_TOPIC("Matte Finesse", QK_TOPIC_FINESSE_DISK_ID);

	// Denoise: hundredths precision, 0..5 range — the useful sizzle-killing
	// band is well under 3px, so fine control matters more than reach.
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Denoise",
		QK_DENOISE_MIN, QK_DENOISE_MAX, QK_DENOISE_MIN, QK_DENOISE_MAX,
		QK_DENOISE_DFLT, PF_Precision_HUNDREDTHS, 0, 0, QK_DENOISE_DISK_ID);

	// Edge shaping: Blur softens the matte edge, then In/Out Ratio chokes or
	// spreads that softened edge. (Clean Black/White were removed — Blur +
	// Ratio do the shaping, the clips below do the endpoint hardening.)
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Blur Radius",
		QK_BLUR_MIN, QK_BLUR_MAX, QK_BLUR_MIN, QK_BLUR_MAX,
		QK_BLUR_DFLT, PF_Precision_TENTHS, 0, 0, QK_BLUR_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("In/Out Ratio",
		QK_RATIO_MIN, QK_RATIO_MAX, QK_RATIO_MIN, QK_RATIO_MAX,
		QK_RATIO_DFLT, PF_Precision_TENTHS, 0, 0, QK_RATIO_DISK_ID);

	// Alpha clip: hard matte black/white point. Black Clip lifts the
	// transparent floor to solid 0; White Clip pulls the opaque ceiling to 1.
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("Black Clip",
		QK_CLIP_MIN, QK_CLIP_MAX, QK_CLIP_MIN, QK_CLIP_MAX,
		QK_BLACK_CLIP_DFLT, PF_Precision_TENTHS, 0, 0, QK_BLACK_CLIP_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX("White Clip",
		QK_CLIP_MIN, QK_CLIP_MAX, QK_CLIP_MIN, QK_CLIP_MAX,
		QK_WHITE_CLIP_DFLT, PF_Precision_TENTHS, 0, 0, QK_WHITE_CLIP_DISK_ID);

	// SUPERVISE so USER_CHANGED_PARAM fires — Attack/Release grey in/out.
	AEFX_CLR_STRUCT(def);
	PF_ADD_CHECKBOXX("Temporal Stabilize", FALSE, PF_ParamFlag_SUPERVISE,
		QK_TEMPORAL_DISK_ID);

	// Registered DISABLED because Temporal Stabilize defaults off;
	// QK_UpdateTemporalEnable toggles them whenever the checkbox changes.
	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX_DISABLED("Attack Smoothing",
		QK_SMOOTH_MIN, QK_SMOOTH_MAX, QK_SMOOTH_MIN, QK_SMOOTH_MAX,
		QK_ATTACK_DFLT, PF_Precision_TENTHS, 0, 0, QK_ATTACK_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_FLOAT_SLIDERX_DISABLED("Release Smoothing",
		QK_SMOOTH_MIN, QK_SMOOTH_MAX, QK_SMOOTH_MIN, QK_SMOOTH_MAX,
		QK_RELEASE_DFLT, PF_Precision_TENTHS, 0, 0, QK_RELEASE_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_END_TOPIC(QK_TOPIC_FINESSE_END_DISK_ID);

	// ======================= Garbage Matte ================================

	AEFX_CLR_STRUCT(def);
	PF_ADD_TOPIC("Garbage Matte", QK_TOPIC_GARBAGE_DISK_ID);

	AEFX_CLR_STRUCT(def);
	// PF_LayerDefault_NONE -> no garbage by default; render treats that as
	// garbage = 1.0 (qualifier acts everywhere).
	PF_ADD_LAYER("Garbage Layer", PF_LayerDefault_NONE, QK_GARBAGE_LAYER_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Garbage Channel",
		QK_GCHAN_NUM - 1, QK_GCHAN_DFLT, QK_GCHAN_ITEMS, QK_GARBAGE_CHAN_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_END_TOPIC(QK_TOPIC_GARBAGE_END_DISK_ID);

	// ======================= Advanced (Color Science) =====================
	// Tucked at the bottom, collapsed by default (PF_ParamFlag_START_COLLAPSED
	// on the group). Defaults (Display / Rec.709) are right for the vast
	// majority of AE work; these only matter for scene-linear/wide-gamut
	// pipelines. Both transform only the qualification path — never the image.
	AEFX_CLR_STRUCT(def);
	def.flags = PF_ParamFlag_START_COLLAPSED;
	PF_ADD_TOPIC("Advanced (Color Science)", QK_TOPIC_ADV_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Color Space Override",
		QK_QIN_NUM - 1, QK_QIN_DFLT, QK_QIN_ITEMS, QK_QIN_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_ADD_POPUP("Luma Weights",
		QK_LW_NUM - 1, QK_LW_DFLT, QK_LW_ITEMS, QK_LUMA_WEIGHTS_DISK_ID);

	AEFX_CLR_STRUCT(def);
	PF_END_TOPIC(QK_TOPIC_ADV_END_DISK_ID);

	out_data->num_params = QK_NUM_PARAMS;
	return err;
}

// ---------------------------------------------------------------------------
// Attack/Release grey-out. Active only while Temporal Stabilize is checked.
// UI-only: no value change, no undo, no re-render. Requires
// PF_OutFlag_SEND_UPDATE_PARAMS_UI (set in GlobalSetup).
// ---------------------------------------------------------------------------
static PF_Err
QK_UpdateTemporalEnable(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[])
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;

	const bool on = (params[QK_TEMPORAL]->u.bd.value != 0);
	const A_long targets[2] = { QK_ATTACK, QK_RELEASE };

	PF_ParamUtilsSuite3 *pusP = nullptr;
	for (int i = 0; i < 2 && !err; ++i) {
		PF_ParamDef *pd = params[targets[i]];
		const PF_ParamUIFlags before = pd->ui_flags;
		if (on) pd->ui_flags &= ~PF_PUI_DISABLED;
		else    pd->ui_flags |=  PF_PUI_DISABLED;
		if (pd->ui_flags == before) continue;

		if (!pusP) {
			ERR(AEFX_AcquireSuite(in_data, out_data,
				kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3, nullptr,
				reinterpret_cast<void**>(&pusP)));
			if (err || !pusP) break;
		}
		ERR(pusP->PF_UpdateParamUI(in_data->effect_ref, targets[i], pd));
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
		"Qualify",			// display name
		"MM Qualify",		// match name — forever
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
			case PF_Cmd_SMART_PRE_RENDER: err = QK_PreRender(in_data, out_data, (PF_PreRenderExtra*)extra);	break;
			case PF_Cmd_SMART_RENDER:     err = QK_SmartRender(in_data, out_data, (PF_SmartRenderExtra*)extra); break;

			// GPU path. DEVICE_SETUP compiles nothing at render time — PSOs
			// are built once per device from DXIL embedded at build time.
			case PF_Cmd_GPU_DEVICE_SETUP:   err = QK_GPUDeviceSetup(in_data, out_data, (PF_GPUDeviceSetupExtra*)extra);     break;
			case PF_Cmd_GPU_DEVICE_SETDOWN: err = QK_GPUDeviceSetdown(in_data, out_data, (PF_GPUDeviceSetdownExtra*)extra); break;
			case PF_Cmd_SMART_RENDER_GPU:   err = QK_SmartRenderGPU(in_data, out_data, (PF_SmartRenderExtra*)extra);        break;

			// Sequence data — transient picker state only (picker.cpp).
			// RESETUP arrives after a load/duplicate; the flattened copy holds
			// no live state, so both paths just allocate a zeroed block.
			case PF_Cmd_SEQUENCE_SETUP:    err = QK_SeqSetup(in_data, out_data);     break;
			case PF_Cmd_SEQUENCE_RESETUP:  err = QK_SeqUnflatten(in_data, out_data); break;
			case PF_Cmd_SEQUENCE_SETDOWN:  err = QK_SeqSetdown(in_data, out_data);   break;
			case PF_Cmd_SEQUENCE_FLATTEN:  err = QK_SeqFlatten(in_data, out_data);   break;

			// Viewer eyedropper: clicks/drags in the comp or layer window.
			case PF_Cmd_EVENT:
				err = QK_HandleEvent(in_data, out_data, params,
				                     static_cast<PF_EventExtra*>(extra));
				break;

			// Sample button (arms the picker) + Attack/Release grey-out. The
			// UI pass also fires on load/undo/preset so the disabled state is
			// correct without a user edit.
			case PF_Cmd_USER_CHANGED_PARAM: {
				const PF_UserChangedParamExtra *uc =
					static_cast<const PF_UserChangedParamExtra*>(extra);
				if (uc && uc->param_index == QK_SAMPLE_BUTTON)
					err = QK_ArmPicker(in_data, out_data);
				else if (uc && uc->param_index == QK_TEMPORAL)
					err = QK_UpdateTemporalEnable(in_data, out_data, params);
				break;
			}
			case PF_Cmd_UPDATE_PARAMS_UI:
				err = QK_UpdateTemporalEnable(in_data, out_data, params);
				break;
		}
	} catch (PF_Err &e) { err = e; }
	  catch (...)       { err = PF_Err_INTERNAL_STRUCT_DAMAGED; }
	return err;
}
