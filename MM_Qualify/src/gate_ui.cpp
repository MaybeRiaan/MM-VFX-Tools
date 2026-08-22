/*  gate_ui.cpp — the Hue / Saturation / Luma gate strips: custom Drawbot
    controls in the Effect Controls panel, drawn under each gate's twirl.

    WHAT THEY ARE
    -------------
    One horizontal strip per gate. The background is a generic ramp of that
    gate's axis (hue spectrum, grey-to-colour, black-to-white) — deliberately
    NOT sampled from the frame, so a redraw never needs a render and
    PF_Event_DRAW stays synchronous and cheap. Columns the gate rejects are
    dimmed, columns it passes are full brightness, and the response curve is
    stroked across the strip: a flat-topped trapezoid whose sloped shoulders
    ARE the softness, exactly as Lumetri and Resolve draw it.

    Four handles per strip:

        softIn edge     In edge          Out edge     softOut edge
             |             |                |              |
             +-------------+================+--------------+
                  ramp up       selected         ramp down

    THE PARAMS REMAIN THE STORAGE
    -----------------------------
    A strip owns NO state. It draws whatever the gate params say right now,
    and a handle drag writes straight back into those same params with
    PF_ChangeFlag_CHANGED_VALUE. So the numeric sliders below and the strip
    can never disagree, and keyframes and expressions keep working untouched —
    scrubbing an animated Hue Width just redraws the strip at that time. This
    is the arrangement docs/hsl_keyer_plugin_plan.md section 3 called for: the
    widget is a view, never a second source of truth.

    The response curve comes from QK_HueGate / QK_LinGate in qk_core.h — the
    SAME functions the CPU and GPU render paths call. The picture cannot drift
    from the key, including the open-bound behaviour (a handle parked at its
    extreme means "no limit", and the strip draws that as a shoulder running
    off the end rather than as a wall).

    MFR: no mutable global state. The only cross-event state is which handle is
    being dragged, carried in do_click.continue_refcon, which AE round-trips
    from the originating click to every drag of that gesture.
*/

#define NOMINMAX
#include "params.h"

#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Look
// ---------------------------------------------------------------------------
const float kDimFloor     = 0.20f;  // brightness of a fully rejected column
const float kRampInset    = 3.0f;   // px between the control frame and the ramp
const float kHandleHalfW  = 4.0f;   // px, half width of a handle marker
const float kHandleH      = 8.0f;   // px, height of a handle marker
const float kHandleGrabPx = 10.0f;  // px, how close a click must be to grab one
                                    // (deliberately wider than the marker —
                                    //  the grab should be easier than the aim)
const int   kMinStripW    = 32;     // below this there is nothing worth drawing

// TWO HANDLE ROWS, and the reason is functional rather than decorative.
// Softness is measured outward from its hard edge, so at zero softness the
// soft handle sits at EXACTLY the same x as the hard one — stacked on a single
// row it was unreachable, and softness could never be dialled up from zero by
// dragging. Splitting them by row makes the hit test two-dimensional, so both
// are always grabbable even when they coincide horizontally:
//
//     +--[In]------------------[Out]--+   <- top row: the selection edges
//     |        (ramp + curve)         |
//     +--[soft]--------------[soft]---+   <- bottom row: the falloff edges
//
// A circular strip gets a THIRD zone between them for the centre handle,
// which slides the whole band — selection and softness together — without
// changing its shape. That is only meaningful where the axis wraps: on a
// linear strip the band has real ends to bump into, so sat and luma keep the
// simple two-zone split and no centre handle.
const float kRowSplitLinear = 0.50f;  // linear: hard above, soft below
const float kRowSplitHard   = 0.34f;  // circular: hard edges above this
const float kRowSplitCenter = 0.66f;  // circular: centre between, soft below
const float kCenterHalfW    = 6.0f;   // px, the centre marker is wider

// Which handle. CENTER exists only on a circular strip — see kRowSplit* below.
enum { QK_H_SOFT_IN = 0, QK_H_IN, QK_H_OUT, QK_H_SOFT_OUT, QK_H_CENTER,
       QK_H_COUNT };

// Vertical zones a click can land in.
enum { QK_ROW_HARD = 0, QK_ROW_CENTER, QK_ROW_SOFT };

// One strip's wiring: the control's own param index, the four value params it
// edits, and the axis it spans.
struct GateSpec {
	A_long	uiParam;
	A_long	pA, pB;				// hue: CENTER (angle), WIDTH | lin: IN, OUT
	A_long	pSoftIn, pSoftOut;
	A_long	pEnable;
	double	axisMin, axisMax;
	double	softMax;
	bool	circular;
};

const GateSpec kGates[] = {
	{ QK_HUE_UI, QK_HUE_CENTER, QK_HUE_WIDTH, QK_HUE_IN_SOFT, QK_HUE_OUT_SOFT,
	  QK_HUE_ENABLE, 0.0, 360.0, QK_HUE_SOFT_MAX, true },
	{ QK_SAT_UI, QK_SAT_IN, QK_SAT_OUT, QK_SAT_IN_SOFT, QK_SAT_OUT_SOFT,
	  QK_SAT_ENABLE, QK_SAT_MIN, QK_SAT_SLIDER_MAX, QK_SAT_SOFT_MAX, false },
	{ QK_LUM_UI, QK_LUM_IN, QK_LUM_OUT, QK_LUM_IN_SOFT, QK_LUM_OUT_SOFT,
	  QK_LUM_ENABLE, QK_LUM_MIN, QK_LUM_SLIDER_MAX, QK_LUM_SOFT_VALID_MAX, false },
};
const int kGateCount = sizeof(kGates) / sizeof(kGates[0]);

// The strip this param index belongs to, or -1.
int GateForParam(A_long index)
{
	for (int i = 0; i < kGateCount; ++i)
		if (kGates[i].uiParam == index) return i;
	return -1;
}

// ---------------------------------------------------------------------------
// Gate values, in axis units
// ---------------------------------------------------------------------------
struct GateEdges {
	double in, out;			// handle positions (hue may be unwrapped)
	double softIn, softOut;
	double gIn, gOut;		// the same edges as the GATE sees them, i.e. with
							// an extreme handle expanded to the open-bound
							// sentinel, so the drawn curve matches the render
};

double AngleOf(PF_ParamDef *params[], A_long idx)
{
	return (double)params[idx]->u.ad.value / 65536.0;
}

double Wrap360(double x) { return x - 360.0 * std::floor(x / 360.0); }

GateEdges ReadEdges(PF_ParamDef *params[], const GateSpec &g)
{
	GateEdges e;
	if (g.circular) {
		const double c = AngleOf(params, g.pA);
		const double w = params[g.pB]->u.fs_d.value;
		e.in  = c - w * 0.5;
		e.out = c + w * 0.5;
		// Hue has no open bound: QK_HueGate reads a 360 span as the full
		// circle on its own, so the gate edges ARE the handle edges.
		e.gIn  = e.in;
		e.gOut = e.out;
	} else {
		e.in  = params[g.pA]->u.fs_d.value;
		e.out = params[g.pB]->u.fs_d.value;
		// The same open-bound test QK_BuildGates applies, so a handle parked
		// at an extreme draws as "no limit" instead of as a wall at 0.0 / 1.0.
		e.gIn  = (e.in  <= g.axisMin + QK_OPEN_EPS) ? -QK_BOUND_BIG : e.in;
		e.gOut = (e.out >= g.axisMax - QK_OPEN_EPS) ?  QK_BOUND_BIG : e.out;
	}
	e.softIn  = params[g.pSoftIn]->u.fs_d.value;
	e.softOut = params[g.pSoftOut]->u.fs_d.value;
	return e;
}

// The gate's own response, from the very functions the render calls.
float Response(const GateSpec &g, const GateEdges &e, double v)
{
	if (g.circular)
		return QK_HueGate((float)v, (float)e.gIn, (float)e.gOut,
						  (float)e.softIn, (float)e.softOut);
	return QK_LinGate((float)v, (float)e.gIn, (float)e.gOut,
					  (float)e.softIn, (float)e.softOut);
}

// Handle positions in axis units. Soft handles sit OUTSIDE their edge, which
// is where the falloff lives (qk_core.h: softness extends outward).
void HandleValues(const GateEdges &e, double out[QK_H_COUNT])
{
	out[QK_H_SOFT_IN]  = e.in  - e.softIn;
	out[QK_H_IN]       = e.in;
	out[QK_H_OUT]      = e.out;
	out[QK_H_SOFT_OUT] = e.out + e.softOut;
	// Midpoint of the hard edges. For hue that IS the Hue Center param, since
	// the edges are derived from it as centre +/- width/2.
	out[QK_H_CENTER]   = 0.5 * (e.in + e.out);
}

// ---------------------------------------------------------------------------
// Axis <-> pixels
// ---------------------------------------------------------------------------
struct Strip { float left, top, width, height; };

// Axis value -> x within the strip. Circular axes wrap into view.
float ValueToX(const GateSpec &g, const Strip &st, double v)
{
	double t;
	if (g.circular) t = Wrap360(v) / 360.0;
	else            t = (v - g.axisMin) / (g.axisMax - g.axisMin);
	if (t < 0.0) t = 0.0;
	if (t > 1.0) t = 1.0;
	return st.left + (float)(t * st.width);
}

double XToValue(const GateSpec &g, const Strip &st, float x)
{
	double t = (st.width > 0.0f) ? (double)(x - st.left) / (double)st.width : 0.0;
	if (t < 0.0) t = 0.0;
	if (t > 1.0) t = 1.0;
	return g.axisMin + t * (g.axisMax - g.axisMin);
}

// Vertical bands within a strip. Hard handles hug the top edge, soft handles
// the bottom, and the response curve gets the space between them.
float RowTopY(const Strip &st)    { return st.top; }
float RowBottomY(const Strip &st) { return st.top + st.height - kHandleH; }
float CurveTopY(const Strip &st)  { return st.top + kHandleH + 1.0f; }
float CurveBotY(const Strip &st)  { return st.top + st.height - kHandleH - 1.0f; }

float RowCenterY(const Strip &st) { return st.top + st.height * 0.5f - kHandleH * 0.5f; }

// Which zone a click at y belongs to.
int RowForY(const GateSpec &g, const Strip &st, float y)
{
	const float t = (st.height > 0.0f) ? (y - st.top) / st.height : 0.0f;
	if (!g.circular) return (t > kRowSplitLinear) ? QK_ROW_SOFT : QK_ROW_HARD;
	if (t < kRowSplitHard)   return QK_ROW_HARD;
	if (t < kRowSplitCenter) return QK_ROW_CENTER;
	return QK_ROW_SOFT;
}

Strip StripFromFrame(const PF_UnionableRect &fr)
{
	Strip st;
	st.left   = (float)fr.left + kRampInset;
	st.top    = (float)fr.top  + kRampInset;
	st.width  = (float)(fr.right - fr.left) - 2.0f * kRampInset;
	st.height = (float)(fr.bottom - fr.top) - 2.0f * kRampInset;
	return st;
}

// ---------------------------------------------------------------------------
// Ramp colours. Generic by design — no frame is ever sampled.
// ---------------------------------------------------------------------------
void HSLToRGB(double h, double sl, double l, float &r, float &g, float &b)
{
	h = Wrap360(h) / 60.0;
	const double c = (1.0 - std::fabs(2.0 * l - 1.0)) * sl;
	const double x = c * (1.0 - std::fabs(std::fmod(h, 2.0) - 1.0));
	const double m = l - 0.5 * c;
	double rr = 0.0, gg = 0.0, bb = 0.0;
	if      (h < 1.0) { rr = c; gg = x; }
	else if (h < 2.0) { rr = x; gg = c; }
	else if (h < 3.0) { gg = c; bb = x; }
	else if (h < 4.0) { gg = x; bb = c; }
	else if (h < 5.0) { rr = x; bb = c; }
	else              { rr = c; bb = x; }
	r = (float)(rr + m); g = (float)(gg + m); b = (float)(bb + m);
}

// The strip background at axis value v. Saturation is tinted by the CURRENT
// hue centre so the bar means something at a glance; that reads a param, not
// the frame, so it costs nothing and never needs a render.
void RampColor(int gate, double v, double hueCenter, float &r, float &g, float &b)
{
	if      (gate == 0) HSLToRGB(v, 1.0, 0.5, r, g, b);
	else if (gate == 1) HSLToRGB(hueCenter, v, 0.5, r, g, b);
	else                { r = (float)v; g = (float)v; b = (float)v; }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
struct Drawbot {
	DRAWBOT_Suites      suites;
	DRAWBOT_DrawRef     draw;
	DRAWBOT_SupplierRef supplier;
	DRAWBOT_SurfaceRef  surface;
};

PF_Err MakePen(Drawbot &d, float r, float g, float b, float a, float w,
			   DRAWBOT_PenRef *out)
{
	DRAWBOT_ColorRGBA c;
	c.red = r; c.green = g; c.blue = b; c.alpha = a;
	return d.suites.supplier_suiteP->NewPen(d.supplier, &c, w, out);
}

void FillRect(Drawbot &d, const DRAWBOT_RectF32 &rect,
			  float r, float g, float b, float a)
{
	DRAWBOT_ColorRGBA c;
	c.red = r; c.green = g; c.blue = b; c.alpha = a;
	d.suites.surface_suiteP->PaintRect(d.surface, &c, &rect);
}

// The ramp, dimmed per column by the gate response. One buffer column per
// screen column, so the picture is exact at any panel width and DrawImage can
// blit it 1:1 (it takes an origin only, never a destination size).
PF_Err DrawRamp(Drawbot &d, int gi, const GateEdges &e,
				const Strip &st, double hueCenter, bool enabled)
{
	PF_Err err = PF_Err_NONE;
	const GateSpec &g = kGates[gi];
	const int W = (int)st.width;
	const int H = (int)st.height;
	if (W < kMinStripW || H < 1) return PF_Err_NONE;

	DRAWBOT_Boolean prefersBGRA = FALSE;
	d.suites.supplier_suiteP->PrefersPixelLayoutBGRA(d.supplier, &prefersBGRA);

	std::vector<unsigned char> buf((size_t)W * (size_t)H * 4);
	const int rowBytes = W * 4;

	for (int x = 0; x < W; ++x) {
		const double v = XToValue(g, st, st.left + (float)x + 0.5f);
		float r = 0.0f, gg = 0.0f, b = 0.0f;
		RampColor(gi, v, hueCenter, r, gg, b);

		float lit = enabled ? Response(g, e, v) : 1.0f;
		lit = kDimFloor + (1.0f - kDimFloor) * lit;
		if (!enabled) lit *= 0.45f;		// a disabled gate reads as inert

		const unsigned char R = (unsigned char)(255.0f * (r  * lit) + 0.5f);
		const unsigned char G = (unsigned char)(255.0f * (gg * lit) + 0.5f);
		const unsigned char B = (unsigned char)(255.0f * (b  * lit) + 0.5f);

		for (int y = 0; y < H; ++y) {
			unsigned char *px = &buf[(size_t)y * rowBytes + (size_t)x * 4];
			if (prefersBGRA) { px[0] = B;   px[1] = G; px[2] = R; px[3] = 255; }
			else             { px[0] = 255; px[1] = R; px[2] = G; px[3] = B;   }
		}
	}

	DRAWBOT_ImageRef img = NULL;
	ERR(d.suites.supplier_suiteP->NewImageFromBuffer(
			d.supplier, W, H, rowBytes,
			prefersBGRA ? kDRAWBOT_PixelLayout_32BGRA_Straight
						: kDRAWBOT_PixelLayout_32ARGB_Straight,
			&buf[0], &img));
	if (!err && img) {
		DRAWBOT_PointF32 origin;
		origin.x = st.left;
		origin.y = st.top;
		ERR(d.suites.surface_suiteP->DrawImage(d.surface, img, &origin, 1.0f));
		d.suites.supplier_suiteP->ReleaseObject(
			reinterpret_cast<DRAWBOT_ObjectRef>(img));
	}
	return err;
}

// The response curve stroked across the strip: the trapezoid outline whose
// shoulders are the softness.
PF_Err DrawCurve(Drawbot &d, const GateSpec &g, const GateEdges &e,
				 const Strip &st, bool enabled)
{
	PF_Err          err  = PF_Err_NONE;
	DRAWBOT_PathRef path = NULL;
	DRAWBOT_PenRef  pen  = NULL;

	const int W = (int)st.width;
	if (W < kMinStripW) return PF_Err_NONE;

	ERR(d.suites.supplier_suiteP->NewPath(d.supplier, &path));
	if (err || !path) return err;

	// Sampling every column keeps hue wraparound honest for free: whatever the
	// gate returns at that hue is what gets plotted, seam included.
	const float cTop = CurveTopY(st);
	const float cBot = CurveBotY(st);
	for (int x = 0; x < W; ++x) {
		const float  px = st.left + (float)x + 0.5f;
		const double v  = XToValue(g, st, px);
		const float  r  = enabled ? Response(g, e, v) : 0.0f;
		const float  py = cBot - r * (cBot - cTop);
		if (x == 0) ERR(d.suites.path_suiteP->MoveTo(path, px, py));
		else        ERR(d.suites.path_suiteP->LineTo(path, px, py));
	}

	ERR(MakePen(d, 1.0f, 1.0f, 1.0f, enabled ? 0.85f : 0.30f, 1.0f, &pen));
	if (!err && pen) ERR(d.suites.surface_suiteP->StrokePath(d.surface, pen, path));

	if (pen)  d.suites.supplier_suiteP->ReleaseObject(reinterpret_cast<DRAWBOT_ObjectRef>(pen));
	if (path) d.suites.supplier_suiteP->ReleaseObject(reinterpret_cast<DRAWBOT_ObjectRef>(path));
	return err;
}

// The four handles, on their two rows. Hard-edge handles are bright with a
// stem dropped through the strip so the selection boundary is unmistakable;
// soft-edge handles are dimmer and sit on the bottom row. The soft pair is
// ALWAYS drawn, including at zero softness where it coincides with its hard
// edge — being reachable there is the whole point of the split.
void DrawHandle(Drawbot &d, float cx, float top, float halfW,
                float lum, bool enabled)
{
	DRAWBOT_RectF32 r;
	r.left   = cx - halfW - 1.0f;
	r.top    = top - 1.0f;
	r.width  = (halfW + 1.0f) * 2.0f;
	r.height = kHandleH + 2.0f;
	// Dark surround first, so a handle stays legible over a bright ramp.
	FillRect(d, r, 0.05f, 0.05f, 0.05f, enabled ? 0.85f : 0.30f);

	r.left   = cx - halfW;
	r.top    = top;
	r.width  = halfW * 2.0f;
	r.height = kHandleH;
	FillRect(d, r, lum, lum, lum, enabled ? 1.0f : 0.35f);
}

void DrawHandles(Drawbot &d, const GateSpec &g, const GateEdges &e,
				 const Strip &st, bool enabled)
{
	double hv[QK_H_COUNT];
	HandleValues(e, hv);

	// Stems: a thin line the full height under each hard edge, and a fainter
	// one under the centre. Drawn first so the markers cap them.
	const int stems[3] = { QK_H_IN, QK_H_OUT, QK_H_CENTER };
	for (int k = 0; k < 3; ++k) {
		if (stems[k] == QK_H_CENTER && !g.circular) continue;
		DRAWBOT_RectF32 r;
		r.left   = ValueToX(g, st, hv[stems[k]]) - 0.5f;
		r.top    = st.top;
		r.width  = 1.0f;
		r.height = st.height;
		const float a = (stems[k] == QK_H_CENTER) ? 0.30f : 0.55f;
		FillRect(d, r, 1.0f, 1.0f, 1.0f, enabled ? a : a * 0.4f);
	}

	for (int i = 0; i < QK_H_COUNT; ++i) {
		if (i == QK_H_CENTER && !g.circular) continue;
		const float cx = ValueToX(g, st, hv[i]);
		switch (i) {
			case QK_H_IN:
			case QK_H_OUT:
				DrawHandle(d, cx, RowTopY(st), kHandleHalfW, 0.97f, enabled);
				break;
			case QK_H_CENTER:
				// Wider and mid-toned: a different job from the edges, so it
				// should not be mistaken for one of them.
				DrawHandle(d, cx, RowCenterY(st), kCenterHalfW, 0.80f, enabled);
				break;
			default:
				DrawHandle(d, cx, RowBottomY(st), kHandleHalfW, 0.62f, enabled);
				break;
		}
	}
}

} // namespace

// ---------------------------------------------------------------------------
// PF_Event_DRAW for one gate strip.
// ---------------------------------------------------------------------------
PF_Err QK_GateUIDraw(PF_InData *in_data, PF_OutData *out_data,
					 PF_ParamDef *params[], PF_EventExtra *extra)
{
	PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;

	const int gi = GateForParam(extra->effect_win.index);
	if (gi < 0 || extra->effect_win.area != PF_EA_CONTROL) return PF_Err_NONE;
	const GateSpec &g = kGates[gi];

	Drawbot d;
	AEFX_CLR_STRUCT(d);
	ERR(AEFX_AcquireDrawbotSuites(in_data, out_data, &d.suites));

	PF_EffectCustomUISuite1 *csP = NULL;
	ERR(AEFX_AcquireSuite(in_data, out_data, kPFEffectCustomUISuite,
						  kPFEffectCustomUISuiteVersion1, NULL, (void**)&csP));
	if (!err && csP) {
		ERR((*csP->PF_GetDrawingReference)(extra->contextH, &d.draw));
		AEFX_ReleaseSuite(in_data, out_data, kPFEffectCustomUISuite,
						  kPFEffectCustomUISuiteVersion1, NULL);
	}
	if (!err && d.draw) {
		ERR(d.suites.drawbot_suiteP->GetSupplier(d.draw, &d.supplier));
		ERR(d.suites.drawbot_suiteP->GetSurface(d.draw, &d.surface));
	}

	if (!err && d.supplier && d.surface) {
		const Strip st = StripFromFrame(extra->effect_win.current_frame);
		if (st.width >= (float)kMinStripW && st.height >= 6.0f) {
			const GateEdges e   = ReadEdges(params, g);
			const bool      on  = params[g.pEnable]->u.bd.value != 0;
			const double    hue = AngleOf(params, QK_HUE_CENTER);

			ERR(DrawRamp(d, gi, e, st, hue, on));
			ERR(DrawCurve(d, g, e, st, on));
			if (!err) DrawHandles(d, g, e, st, on);
		}
	}

	ERR2(AEFX_ReleaseDrawbotSuites(in_data, out_data));

	if (!err) extra->evt_out_flags = PF_EO_HANDLED_EVENT;
	return err;
}

// Ask AE to repaint this control now.
//
// Writing a param with PF_ChangeFlag_CHANGED_VALUE re-renders the COMP, which
// is why the viewer tracked a drag while the strip itself sat frozen until the
// mouse came up: nothing had told the Effect Controls panel that its custom
// area was stale. PF_InvalidateRect marks it dirty (NULL rect = the whole
// window) and PF_EO_UPDATE_NOW promotes the repaint from "next idle" to
// "immediately after this event returns", which is what makes a drag track.
// Documented as valid only while handling a NON-draw event, hence its use
// from the click and drag paths and never from QK_GateUIDraw.
void RepaintControl(PF_InData *in_data, PF_OutData *out_data,
                    PF_EventExtra *extra)
{
	try {
		AEGP_SuiteHandler suites(in_data->pica_basicP);
		if (suites.AppSuite6()->PF_InvalidateRect(extra->contextH, NULL) == PF_Err_NONE) {
			extra->evt_out_flags |= PF_EO_UPDATE_NOW;
			return;
		}
	} catch (...) {
		// Suite missing (older host / Premiere): fall through.
	}
	// Coarser but always available — redraws the ECW along with the viewers.
	out_data->out_flags |= PF_OutFlag_REFRESH_UI;
}

// ---------------------------------------------------------------------------
// Drag on a strip: write the grabbed handle straight into the gate params.
// ---------------------------------------------------------------------------
PF_Err QK_GateUIDrag(PF_InData *in_data, PF_OutData *out_data,
					 PF_ParamDef *params[], PF_EventExtra *extra)
{
	PF_DoClickEventInfo *click = &extra->u.do_click;

	const int gi = (int)click->continue_refcon[0] - 1;	// 0 means "not ours"
	const int hi = (int)click->continue_refcon[1];
	if (gi < 0 || gi >= kGateCount || hi < 0 || hi >= QK_H_COUNT)
		return PF_Err_NONE;
	const GateSpec &g = kGates[gi];

	const Strip st = StripFromFrame(extra->effect_win.current_frame);
	if (st.width < (float)kMinStripW) return PF_Err_NONE;

	// CONTINUOUS ROTATION ON A CIRCULAR AXIS.
	//
	// Hue has no ends — 0 and 360 are the same place — so mapping pointer x
	// straight onto the axis and clamping at the strip edge was wrong twice
	// over: it stopped the drag dead at a boundary that does not exist, and it
	// capped a single gesture's travel at the width of the strip. Accumulating
	// the pointer DELTA instead keeps turning the wheel for as long as the
	// mouse keeps moving, through the seam as many times as you like, and it
	// stops mattering whether the pointer is still over the strip at all.
	//
	// Slots 2 and 3 of continue_refcon carry the gesture's last x and its
	// running angle (in 1/65536 degrees, the same fixed point PF_Fixed uses);
	// AE round-trips all four from the originating click to every drag.
	//
	// Linear axes keep the absolute mapping: saturation and luma have real
	// ends, so clamping there is the correct behaviour, not a limitation.
	double v;
	if (g.circular) {
		const double     degPerPx = 360.0 / (double)st.width;
		const A_intptr_t nowX     = (A_intptr_t)click->screen_point.h;
		const double     dx       = (double)(nowX - click->continue_refcon[2]);
		double accum = (double)click->continue_refcon[3] / 65536.0 + dx * degPerPx;
		accum = Wrap360(accum);
		click->continue_refcon[2] = nowX;
		click->continue_refcon[3] = (A_intptr_t)(accum * 65536.0);
		v = accum;
	} else {
		v = XToValue(g, st, (float)click->screen_point.h);
	}

	const GateEdges e = ReadEdges(params, g);

	if (g.circular) {
		if (hi == QK_H_CENTER) {
			// Slide the whole band. Width and both softness values are left
			// alone, and the hard edges are DERIVED from centre +/- width/2,
			// so moving this one param translates selection and falloff
			// together without altering the shape of either.
			params[g.pA]->u.ad.value      = (PF_Fixed)(Wrap360(v) * 65536.0);
			params[g.pA]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
		} else if (hi == QK_H_IN || hi == QK_H_OUT) {
			// Hue is stored as Centre + Width, so moving ONE hard edge
			// rewrites BOTH params: the opposite edge is held and the pair
			// re-derived. All of it through Wrap360, so dragging across the
			// 0/360 seam behaves.
			double newIn  = e.in;
			double newOut = e.out;
			if (hi == QK_H_IN) newIn = v; else newOut = v;
			double w = Wrap360(newOut - newIn);
			if (w < 1e-6) w = 0.0;
			const double c = Wrap360(newIn + w * 0.5);
			params[g.pA]->u.ad.value      = (PF_Fixed)(c * 65536.0);
			params[g.pA]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
			params[g.pB]->u.fs_d.value    = w;
			params[g.pB]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
		} else {
			// Soft handles measure OUTWARD from their edge, the short way
			// round; dragging inside the band reads as zero softness rather
			// than as nearly a full turn of it.
			const double edge = (hi == QK_H_SOFT_IN) ? e.in : e.out;
			double dist = (hi == QK_H_SOFT_IN) ? Wrap360(edge - v)
											   : Wrap360(v - edge);
			if (dist > 180.0)      dist = 0.0;
			if (dist > g.softMax)  dist = g.softMax;
			const A_long idx = (hi == QK_H_SOFT_IN) ? g.pSoftIn : g.pSoftOut;
			params[idx]->u.fs_d.value    = dist;
			params[idx]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
		}
	} else {
		// Linear strips have no centre handle (RowForY never offers one), so
		// its arrival would mean a logic error upstream — bail rather than let
		// the switch below fold it into soft-out.
		if (hi == QK_H_CENTER) return PF_Err_NONE;

		A_long idx = -1;
		double val = 0.0;
		switch (hi) {
			case QK_H_IN:
				val = (v > e.out) ? e.out : v;
				if (val < g.axisMin) val = g.axisMin;
				idx = g.pA;
				break;
			case QK_H_OUT:
				val = (v < e.in) ? e.in : v;
				if (val > g.axisMax) val = g.axisMax;
				idx = g.pB;
				break;
			case QK_H_SOFT_IN:
				val = e.in - v;
				if (val < 0.0) val = 0.0;
				if (val > g.softMax) val = g.softMax;
				idx = g.pSoftIn;
				break;
			default:
				val = v - e.out;
				if (val < 0.0) val = 0.0;
				if (val > g.softMax) val = g.softMax;
				idx = g.pSoftOut;
				break;
		}
		params[idx]->u.fs_d.value    = val;
		params[idx]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
	}

	if (click->last_time) {
		click->continue_refcon[0] = 0;
		click->continue_refcon[1] = 0;
		click->continue_refcon[2] = 0;
		click->continue_refcon[3] = 0;
	}
	extra->evt_out_flags |= PF_EO_HANDLED_EVENT;
	RepaintControl(in_data, out_data, extra);
	return PF_Err_NONE;
}

// ---------------------------------------------------------------------------
// Click on a strip: grab the nearest handle, then apply immediately so a bare
// click (no drag) still moves something.
// ---------------------------------------------------------------------------
PF_Err QK_GateUIClick(PF_InData *in_data, PF_OutData *out_data,
					  PF_ParamDef *params[], PF_EventExtra *extra)
{
	const int gi = GateForParam(extra->effect_win.index);
	if (gi < 0 || extra->effect_win.area != PF_EA_CONTROL) return PF_Err_NONE;
	const GateSpec &g = kGates[gi];

	const Strip st = StripFromFrame(extra->effect_win.current_frame);
	if (st.width < (float)kMinStripW) return PF_Err_NONE;

	PF_DoClickEventInfo *click = &extra->u.do_click;
	const float x = (float)click->screen_point.h;
	const float y = (float)click->screen_point.v;

	const GateEdges e = ReadEdges(params, g);
	double hv[QK_H_COUNT];
	HandleValues(e, hv);

	// WHICH ZONE decides which handles are even candidates, and that is what
	// makes a zero-width softness reachable: at zero the soft handle shares
	// its hard handle's x exactly, so x alone could never separate them.
	// Click high for the selection edges, low for softness, and — on hue —
	// anywhere through the middle to slide the whole band.
	const int row = RowForY(g, st, y);

	int bestI = -1;
	if (row == QK_ROW_CENTER) {
		// One candidate, and no distance test: anywhere in the middle band
		// grabs it. Sliding the band is the point, not hitting a target.
		bestI = QK_H_CENTER;
	} else {
		const bool soft = (row == QK_ROW_SOFT);
		const int  cand[2] = { soft ? QK_H_SOFT_IN : QK_H_IN,
							   soft ? QK_H_SOFT_OUT : QK_H_OUT };

		// Nearest of the two in PIXELS, so the grab radius feels the same on
		// every strip regardless of the axis units underneath.
		float bestD = kHandleGrabPx;
		for (int k = 0; k < 2; ++k) {
			const float dx = std::fabs(ValueToX(g, st, hv[cand[k]]) - x);
			if (dx < bestD) { bestD = dx; bestI = cand[k]; }
		}
		// Missing both still grabs the nearer one, so clicking bare strip
		// does something useful rather than nothing.
		if (bestI < 0) {
			const float d0 = std::fabs(ValueToX(g, st, hv[cand[0]]) - x);
			const float d1 = std::fabs(ValueToX(g, st, hv[cand[1]]) - x);
			bestI = (d0 <= d1) ? cand[0] : cand[1];
		}
	}

	click->continue_refcon[0] = (A_intptr_t)(gi + 1);
	click->continue_refcon[1] = (A_intptr_t)bestI;
	// Seed the rotation accumulator at the press, so the first drag's delta
	// comes out zero and the handle starts from where it was clicked rather
	// than jumping by a frame's worth of travel.
	click->continue_refcon[2] = (A_intptr_t)click->screen_point.h;
	click->continue_refcon[3] = (A_intptr_t)(XToValue(g, st, x) * 65536.0);
	click->send_drag          = TRUE;
	extra->evt_out_flags     |= PF_EO_HANDLED_EVENT;

	// The drag path applies the value and asks for the repaint, so a bare
	// click gets both without duplicating either.
	return QK_GateUIDrag(in_data, out_data, params, extra);
}
