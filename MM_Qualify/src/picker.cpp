/*  picker.cpp — the viewer eyedropper: click/drag on the IMAGE to build the
    key, always reading the PRE-EFFECT pixel.

    WHY THIS EXISTS (the load-bearing constraint)
    ---------------------------------------------
    AE's standard colour-swatch eyedropper samples the COMPOSITED effect
    output and never tells the plugin where the user clicked. With the default
    Selection Overlay view that means picking reads the dimmed/desaturated
    overlay colour and mis-keys — you cannot pick accurately from the view you
    actually want to work in.

    The fix has two halves, and both are needed:

      1. WHERE the click lands. A custom UI registered for the COMP and LAYER
         windows receives PF_Event_DO_CLICK / PF_Event_DRAG with the click
         point, which frame_to_source maps to a layer coordinate. (An ECW
         custom UI would NOT do: there the point is in Effect-Controls panel
         space — it describes your control's row, not the picture.)

      2. WHAT is read there. AEGP_NewFromUpstreamOfEffect builds render
         options whose EffectsToRender stops AT this effect's index, i.e. the
         layer as it enters MM Qualify. Rendering that and reading one pixel
         gives the true input colour REGARDLESS of the current Output popup.
         Overlay/Matte/Keyed are display modes only; they cannot contaminate a
         sample. That is a structural guarantee, not a convention.

    The synchronous render on the UI thread is the sanctioned use of this call
    — AE_GeneralPlug.h's RenderSuite5 note carves out exactly this case ("the
    plugin requires this frame to make database change (e.g. an auto color
    button on an effect)" AND "the user explicitly requested this operation").

    ONE FRAME PER GESTURE
    ---------------------
    Rendering the upstream frame is expensive, so it happens ONCE at press.
    The receipt is held for the whole gesture, every drag sample reads a pixel
    out of that held world, and release checks it in. Drag therefore costs a
    pixel read rather than a render — and every sample in a gesture comes from
    one consistent frame, so a sweep cannot straddle a time change.

    The frame is requested at downsample 1:1 so the sampled coordinate matches
    full-res layer space no matter what resolution the viewer is set to.

    MFR: all gesture state lives in sequence data (QK_SeqData), never in a
    global or static. Events are UI-thread, but the rule is kept anyway.
*/

#define NOMINMAX
#include "params.h"

namespace {

// AEGP plugin id for our render/world calls. Registered lazily per call from
// in_data->effect_ref; AE hands the same id back for the same plugin.
const A_char *kPickerMatchName = "MM Qualify";

// ---------------------------------------------------------------------------
// Sequence data helpers
// ---------------------------------------------------------------------------

QK_SeqData *LockSeq(PF_InData *in_data)
{
	if (!in_data->sequence_data) return nullptr;
	return reinterpret_cast<QK_SeqData*>(
		PF_LOCK_HANDLE(in_data->sequence_data));
}

void UnlockSeq(PF_InData *in_data)
{
	if (in_data->sequence_data) PF_UNLOCK_HANDLE(in_data->sequence_data);
}

// ---------------------------------------------------------------------------
// Pixel read out of a checked-out AEGP world, promoted to float.
// The upstream frame can come back 8/16/32-bit depending on the project's
// depth, so all three are handled. 8/16 are straight normalisations; 32 is
// already float. Premultiplied alpha is divided out so the sampled colour is
// the pixel's own colour, not its colour scaled by coverage — sampling a
// semi-transparent edge should key on the material, not on the blend.
// ---------------------------------------------------------------------------
PF_Err ReadWorldPixel(AEGP_SuiteHandler &suites, AEGP_WorldH worldH,
                      A_long x, A_long y,
                      PF_FpLong *rP, PF_FpLong *gP, PF_FpLong *bP)
{
	PF_Err          err = PF_Err_NONE;
	AEGP_WorldType  type = AEGP_WorldType_NONE;
	A_long          w = 0, h = 0;
	A_u_long        rowBytes = 0;

	ERR(suites.WorldSuite3()->AEGP_GetType(worldH, &type));
	ERR(suites.WorldSuite3()->AEGP_GetSize(worldH, &w, &h));
	ERR(suites.WorldSuite3()->AEGP_GetRowBytes(worldH, &rowBytes));
	if (err) return err;

	if (x < 0 || y < 0 || x >= w || y >= h) return PF_Err_INVALID_INDEX;

	double r = 0, g = 0, b = 0, a = 1.0;

	switch (type) {
		case AEGP_WorldType_8: {
			PF_Pixel8 *base = nullptr;
			ERR(suites.WorldSuite3()->AEGP_GetBaseAddr8(worldH, &base));
			if (err || !base) break;
			const PF_Pixel8 *px = reinterpret_cast<const PF_Pixel8*>(
				reinterpret_cast<const char*>(base) + (size_t)y * rowBytes) + x;
			r = px->red / 255.0; g = px->green / 255.0;
			b = px->blue / 255.0; a = px->alpha / 255.0;
			break;
		}
		case AEGP_WorldType_16: {
			PF_Pixel16 *base = nullptr;
			ERR(suites.WorldSuite3()->AEGP_GetBaseAddr16(worldH, &base));
			if (err || !base) break;
			const PF_Pixel16 *px = reinterpret_cast<const PF_Pixel16*>(
				reinterpret_cast<const char*>(base) + (size_t)y * rowBytes) + x;
			// AE's 16-bit is 0..32768 (MAX_CHAN16), not 0..65535.
			r = px->red / (double)PF_MAX_CHAN16;
			g = px->green / (double)PF_MAX_CHAN16;
			b = px->blue / (double)PF_MAX_CHAN16;
			a = px->alpha / (double)PF_MAX_CHAN16;
			break;
		}
		case AEGP_WorldType_32: {
			PF_PixelFloat *base = nullptr;
			ERR(suites.WorldSuite3()->AEGP_GetBaseAddr32(worldH, &base));
			if (err || !base) break;
			const PF_PixelFloat *px = reinterpret_cast<const PF_PixelFloat*>(
				reinterpret_cast<const char*>(base) + (size_t)y * rowBytes) + x;
			r = px->red; g = px->green; b = px->blue; a = px->alpha;
			break;
		}
		default:
			return PF_Err_BAD_CALLBACK_PARAM;
	}
	if (err) return err;

	// Un-premultiply. Below the epsilon there is no colour information to
	// recover, so leave the (essentially black) value alone rather than
	// amplifying noise.
	if (a > 1e-6 && a < 1.0) { r /= a; g /= a; b /= a; }

	*rP = r; *gP = g; *bP = b;
	return PF_Err_NONE;
}

// ---------------------------------------------------------------------------
// Check out ONE upstream (pre-effect) frame at the current time, 1:1.
// Caller owns the receipt and must AEGP_CheckinFrame it.
// ---------------------------------------------------------------------------
PF_Err CheckoutUpstreamFrame(PF_InData *in_data, AEGP_SuiteHandler &suites,
                             AEGP_FrameReceiptH *receiptPH,
                             AEGP_WorldH *worldPH)
{
	PF_Err                    err = PF_Err_NONE;
	AEGP_PluginID             pluginId = 0;
	AEGP_EffectRefH           effectH = nullptr;
	AEGP_LayerRenderOptionsH  optionsH = nullptr;

	*receiptPH = nullptr;
	*worldPH   = nullptr;

	ERR(suites.UtilitySuite6()->AEGP_RegisterWithAEGP(
		nullptr, kPickerMatchName, &pluginId));

	// A ref to THIS effect instance — the anchor for "everything upstream of
	// me". in_data->effect_ref is the PF-side handle AE gave us for this call.
	ERR(suites.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(
		pluginId, in_data->effect_ref, &effectH));

	// EffectsToRender = this effect's index => the layer AS IT ENTERS us.
	ERR(suites.LayerRenderOptionsSuite2()->AEGP_NewFromUpstreamOfEffect(
		pluginId, effectH, &optionsH));

	if (!err && optionsH) {
		A_Time t;
		t.value = in_data->current_time;
		t.scale = in_data->time_scale;
		ERR(suites.LayerRenderOptionsSuite2()->AEGP_SetTime(optionsH, t));

		// 1:1 so sampled coords are full-res layer coords regardless of the
		// viewer's resolution setting.
		ERR(suites.LayerRenderOptionsSuite2()->AEGP_SetDownsampleFactor(
			optionsH, 1, 1));

		// Float when we can get it — the qualification math is float and the
		// gates care about superwhite/HDR values that 8-bit would clip.
		ERR(suites.LayerRenderOptionsSuite2()->AEGP_SetWorldType(
			optionsH, AEGP_WorldType_32));
	}

	ERR(suites.RenderSuite5()->AEGP_RenderAndCheckoutLayerFrame(
		optionsH, nullptr, nullptr, receiptPH));

	if (!err && *receiptPH)
		ERR(suites.RenderSuite5()->AEGP_GetReceiptWorld(*receiptPH, worldPH));

	// Options and the effect ref are ours to clean up; the receipt is not.
	if (optionsH) suites.LayerRenderOptionsSuite2()->AEGP_Dispose(optionsH);
	if (effectH)  suites.EffectSuite4()->AEGP_DisposeEffect(effectH);

	return err;
}

// ---------------------------------------------------------------------------
// Screen point -> layer pixel. frame_to_source takes the window point into
// the source (layer) coordinate space of the effect's layer.
// ---------------------------------------------------------------------------
PF_Err ScreenToLayer(PF_EventExtra *extra, const PF_Point &screen,
                     A_long *xP, A_long *yP)
{
	PF_Err        err = PF_Err_NONE;
	PF_FixedPoint pt;
	pt.x = (PF_Fixed)(screen.h << 16);
	pt.y = (PF_Fixed)(screen.v << 16);

	if (!extra->cbs.frame_to_source) return PF_Err_BAD_CALLBACK_PARAM;
	ERR(extra->cbs.frame_to_source(extra->cbs.refcon, extra->contextH, &pt));
	if (err) return err;

	*xP = (A_long)(pt.x >> 16);
	*yP = (A_long)(pt.y >> 16);
	return PF_Err_NONE;
}

// One sample: read the pixel and fold it into the gates.
PF_Err SampleAt(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[],
                AEGP_SuiteHandler &suites, AEGP_WorldH worldH,
                A_long x, A_long y, A_long mode, QK_PickEnvelope *env)
{
	PF_Err    err = PF_Err_NONE;
	PF_FpLong r = 0, g = 0, b = 0;

	err = ReadWorldPixel(suites, worldH, x, y, &r, &g, &b);
	// Off-image drags are normal at the edges of a sweep — ignore them rather
	// than aborting the gesture.
	if (err == PF_Err_INVALID_INDEX) return PF_Err_NONE;
	if (err) return err;

	return QK_ApplySampleRGB(in_data, out_data, params, mode, r, g, b, env);
}

} // namespace

// ---------------------------------------------------------------------------
// Sample button — arms the picker. The next click in the comp/layer viewer
// starts a gesture; until then nothing is intercepted, so normal viewer
// interaction is untouched.
// ---------------------------------------------------------------------------
PF_Err QK_ArmPicker(PF_InData *in_data, PF_OutData *out_data)
{
	QK_SeqData *seq = LockSeq(in_data);
	if (!seq) return PF_Err_BAD_CALLBACK_PARAM;

	seq->armedB   = 1;
	seq->gestureB = 0;
	AEFX_CLR_STRUCT(seq->env);

	UnlockSeq(in_data);

	// Nudge the viewer so the armed cursor takes effect immediately.
	out_data->out_flags |= PF_OutFlag_REFRESH_UI;
	return PF_Err_NONE;
}

// ---------------------------------------------------------------------------
// The event handler.
//
//   DO_CLICK  — if armed: latch the modifier -> mode, check out ONE upstream
//               frame, take the first sample, and ask AE for drags.
//   DRAG      — sample from the HELD frame; on last_time, check in + disarm.
//
// The receipt is carried across events in do_click.continue_refcon, which AE
// round-trips from the originating click to every drag of that gesture — the
// mechanism that exists precisely for this.
// ---------------------------------------------------------------------------
PF_Err QK_HandleEvent(PF_InData *in_data, PF_OutData *out_data,
                      PF_ParamDef *params[], PF_EventExtra *extra)
{
	PF_Err err = PF_Err_NONE;

	if (!extra || !extra->contextH) return PF_Err_NONE;

	// NEW_CONTEXT is where a comp/layer custom UI subscribes to events —
	// register_ui is REJECTED during GLOBAL_SETUP (AE errors out and the
	// effect fails to initialise). PF_OutFlag_CUSTOM_UI at global setup only
	// says "I have a custom UI"; this call says WHICH windows send us events.
	// Per the SDK the extra fields are meaningless for NEW_CONTEXT beyond
	// contextH/e_type, so we do nothing else here.
	if (extra->e_type == PF_Event_NEW_CONTEXT) {
		PF_CustomUIInfo ci;
		AEFX_CLR_STRUCT(ci);
		ci.events = PF_CustomEFlag_COMP | PF_CustomEFlag_LAYER;
		return PF_REGISTER_UI(in_data, &ci);
	}

	// Only the image windows carry a meaningful click point.
	const PF_WindowType wtype = (*extra->contextH)->w_type;
	if (wtype != PF_Window_COMP && wtype != PF_Window_LAYER) return PF_Err_NONE;

	if (extra->e_type != PF_Event_DO_CLICK &&
	    extra->e_type != PF_Event_DRAG &&
	    extra->e_type != PF_Event_ADJUST_CURSOR)
		return PF_Err_NONE;

	QK_SeqData *seq = LockSeq(in_data);
	if (!seq) return PF_Err_NONE;

	// Not armed and not mid-gesture: let the event through to AE untouched, so
	// normal viewer interaction (pan, selection) is completely unaffected.
	if (!seq->armedB && !seq->gestureB) { UnlockSeq(in_data); return PF_Err_NONE; }

	// Armed: show the eyedropper so the user can SEE the picker is live.
	if (extra->e_type == PF_Event_ADJUST_CURSOR) {
		extra->u.adjust_cursor.set_cursor = PF_Cursor_EYEDROPPER;
		extra->evt_out_flags = PF_EO_HANDLED_EVENT;
		UnlockSeq(in_data);
		return PF_Err_NONE;
	}

	AEGP_SuiteHandler suites(in_data->pica_basicP);
	PF_DoClickEventInfo *click = &extra->u.do_click;

	A_long x = 0, y = 0;
	err = ScreenToLayer(extra, click->screen_point, &x, &y);

	if (extra->e_type == PF_Event_DO_CLICK) {
		// ---- press: latch mode, check out the upstream frame once ---------
		A_long mode = QK_PICK_SET;
		if      (click->modifiers & PF_Mod_CMD_CTRL_KEY) mode = QK_PICK_ADD;
		else if (click->modifiers & PF_Mod_OPT_ALT_KEY)  mode = QK_PICK_SUB;

		AEGP_FrameReceiptH receiptH = nullptr;
		AEGP_WorldH        worldH   = nullptr;
		if (!err) err = CheckoutUpstreamFrame(in_data, suites, &receiptH, &worldH);

		if (!err && worldH) {
			seq->gestureB = 1;
			seq->modeL    = mode;
			AEFX_CLR_STRUCT(seq->env);

			err = SampleAt(in_data, out_data, params, suites, worldH,
			               x, y, mode, &seq->env);

			// Hold the receipt for the drags of this gesture.
			click->continue_refcon[0] = reinterpret_cast<A_intptr_t>(receiptH);
			click->continue_refcon[1] = reinterpret_cast<A_intptr_t>(worldH);
			click->send_drag = TRUE;
		} else if (receiptH) {
			// Render failed after checkout — do not leak the frame.
			suites.RenderSuite5()->AEGP_CheckinFrame(receiptH);
			seq->armedB = 0;
		}
		extra->evt_out_flags = PF_EO_HANDLED_EVENT;

	} else {
		// ---- drag: sample from the held frame -----------------------------
		AEGP_FrameReceiptH receiptH =
			reinterpret_cast<AEGP_FrameReceiptH>(click->continue_refcon[0]);
		AEGP_WorldH worldH =
			reinterpret_cast<AEGP_WorldH>(click->continue_refcon[1]);

		if (worldH && !err)
			err = SampleAt(in_data, out_data, params, suites, worldH,
			               x, y, seq->modeL, &seq->env);

		if (click->last_time) {
			// ---- release: end the gesture, disarm -------------------------
			if (receiptH) suites.RenderSuite5()->AEGP_CheckinFrame(receiptH);
			click->continue_refcon[0] = 0;
			click->continue_refcon[1] = 0;
			seq->gestureB = 0;
			seq->armedB   = 0;
			AEFX_CLR_STRUCT(seq->env);
		}
		extra->evt_out_flags = PF_EO_HANDLED_EVENT;
	}

	UnlockSeq(in_data);
	return err;
}

// ---------------------------------------------------------------------------
// Sequence data. Holds ONLY transient picker state — nothing render-affecting,
// nothing worth persisting. Flatten writes a zeroed copy so a project saved
// mid-gesture reopens disarmed rather than with a dangling receipt.
// ---------------------------------------------------------------------------
PF_Err QK_SeqSetup(PF_InData *in_data, PF_OutData *out_data)
{
	PF_Handle h = PF_NEW_HANDLE(sizeof(QK_SeqData));
	if (!h) return PF_Err_OUT_OF_MEMORY;

	QK_SeqData *seq = reinterpret_cast<QK_SeqData*>(PF_LOCK_HANDLE(h));
	if (!seq) { PF_DISPOSE_HANDLE(h); return PF_Err_OUT_OF_MEMORY; }
	AEFX_CLR_STRUCT(*seq);
	PF_UNLOCK_HANDLE(h);

	out_data->sequence_data = h;
	return PF_Err_NONE;
}

PF_Err QK_SeqSetdown(PF_InData *in_data, PF_OutData *out_data)
{
	if (in_data->sequence_data) PF_DISPOSE_HANDLE(in_data->sequence_data);
	out_data->sequence_data = nullptr;
	return PF_Err_NONE;
}

PF_Err QK_SeqFlatten(PF_InData *in_data, PF_OutData *out_data)
{
	PF_Handle h = PF_NEW_HANDLE(sizeof(QK_SeqData));
	if (!h) return PF_Err_OUT_OF_MEMORY;

	QK_SeqData *flat = reinterpret_cast<QK_SeqData*>(PF_LOCK_HANDLE(h));
	if (!flat) { PF_DISPOSE_HANDLE(h); return PF_Err_OUT_OF_MEMORY; }
	AEFX_CLR_STRUCT(*flat);		// transient state is never persisted
	PF_UNLOCK_HANDLE(h);

	out_data->sequence_data = h;
	return PF_Err_NONE;
}

PF_Err QK_SeqUnflatten(PF_InData *in_data, PF_OutData *out_data)
{
	return QK_SeqSetup(in_data, out_data);
}
