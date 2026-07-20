/*  sampling.cpp — turning a sampled colour into gate values (Resolve's
    pick / +pick / -pick, plan §1).

    The colour arrives from picker.cpp as float RGB read out of the UPSTREAM
    (pre-effect) frame — never from the composited view, so what the viewer is
    currently showing (Selection Overlay, Matte, anything) cannot contaminate
    a sample. That is the whole reason the picker is a comp/layer-window
    custom UI rather than a standard colour swatch; see picker.cpp.

    Modifier at press picks the op:
      Set (plain click)  — seed the envelope from this sample.
      Add (Ctrl/Cmd)     — union the sample into the CURRENT gate.
      Sub (Alt/Opt)      — carve the sample out of the current gate.

    DRAG GROWS THE SELECTION. A gesture (press .. release) accumulates a
    QK_PickEnvelope; each sample unions into it and the gates are rewritten
    from the envelope. Growth is therefore monotonic and independent of drag
    direction and speed — sweeping back over ground already covered cannot
    shrink the key. Sub is the exception: it is applied per-sample against the
    live gate (carving has no meaningful "envelope").

    The sample runs through the SAME qualification math the render uses
    (QK_QualifyIn1 / QK_RGBToHS / luma weights via QK_BuildGates), and Hue is
    written back as Center/Width (the UI form; QK_BuildGates converts to the
    gate's In/Out the seam-free kernel wants).
*/

#define NOMINMAX
#include "params.h"

namespace {

const double kHueHalf   = 10.0;   // Set (single click): hue half-width, degrees
const double kHuePad    = 2.0;    // pad added around a swept hue extent
const double kLinHalf   = 0.10;   // Set (single click): sat/luma half-width
const double kLinPad    = 0.025;  // pad added around a swept sat/luma extent

inline double WrapDeg(double x) { return x - 360.0 * std::floor(x / 360.0); }

// Signed angular difference a-b wrapped to [-180, 180].
inline double SignedDeltaDeg(double a, double b)
{
	double d = WrapDeg(a - b);
	if (d > 180.0) d -= 360.0;
	return d;
}

inline void WriteFloat(PF_ParamDef *params[], A_long idx, double v)
{
	params[idx]->u.fs_d.value = v;
	params[idx]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
}

inline void WriteAngle(PF_ParamDef *params[], A_long idx, double deg)
{
	params[idx]->u.ad.value = (PF_Fixed)(deg * 65536.0);
	params[idx]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
}

inline void WriteBool(PF_ParamDef *params[], A_long idx, bool v)
{
	params[idx]->u.bd.value = v ? 1 : 0;
	params[idx]->uu.change_flags = PF_ChangeFlag_CHANGED_VALUE;
}

// Subtract: pull the nearest edge past the sample to exclude it. Unchanged
// from the swatch-era behaviour — carving is inherently per-sample.
void SubtractLinear(PF_ParamDef *params[], A_long inIdx, A_long outIdx,
                    double v, double vmax)
{
	double lo = params[inIdx]->u.fs_d.value;
	double hi = params[outIdx]->u.fs_d.value;
	if (lo > hi) { double t = lo; lo = hi; hi = t; }

	if (v >= lo && v <= hi) {
		if (v - lo < hi - v) lo = (v + kLinPad < hi) ? v + kLinPad : hi;
		else                 hi = (v - kLinPad > lo) ? v - kLinPad : lo;
	}
	if (lo < 0.0)  lo = 0.0;
	if (hi > vmax) hi = vmax;
	if (lo > hi)   lo = hi;
	WriteFloat(params, inIdx, lo);
	WriteFloat(params, outIdx, hi);
}

// Write the accumulated envelope out to the gate params. Called after every
// sample of a Set/Add gesture, so the key updates live under the cursor.
void WriteEnvelope(PF_ParamDef *params[], const QK_PickEnvelope &e)
{
	// --- hue: anchor + signed extent -> Center/Width -----------------------
	const double half   = 0.5 * (e.huePosF - e.hueNegF) + kHuePad;
	const double center = e.hueAnchorF + 0.5 * (e.huePosF + e.hueNegF);
	double width = 2.0 * half;
	if (width > 360.0) width = 360.0;
	WriteAngle(params, QK_HUE_CENTER, WrapDeg(center));
	WriteFloat(params, QK_HUE_WIDTH, width);

	// --- sat / luma: plain min/max + pad, clamped to the valid span --------
	double sLo = e.satMinF - kLinPad, sHi = e.satMaxF + kLinPad;
	if (sLo < QK_SAT_MIN)       sLo = QK_SAT_MIN;
	if (sHi > QK_SAT_VALID_MAX) sHi = QK_SAT_VALID_MAX;
	if (sLo > sHi)              sLo = sHi;
	WriteFloat(params, QK_SAT_IN,  sLo);
	WriteFloat(params, QK_SAT_OUT, sHi);

	double yLo = e.lumMinF - kLinPad, yHi = e.lumMaxF + kLinPad;
	if (yLo < QK_LUM_MIN)       yLo = QK_LUM_MIN;
	if (yHi > QK_LUM_VALID_MAX) yHi = QK_LUM_VALID_MAX;
	if (yLo > yHi)              yLo = yHi;
	WriteFloat(params, QK_LUM_IN,  yLo);
	WriteFloat(params, QK_LUM_OUT, yHi);
}

} // namespace

PF_Err QK_ApplySampleRGB(PF_InData *in_data, PF_OutData *out_data,
                         PF_ParamDef *params[], A_long mode,
                         PF_FpLong r, PF_FpLong g, PF_FpLong b,
                         QK_PickEnvelope *env)
{
	(void)in_data; (void)out_data;
	if (!env) return PF_Err_BAD_CALLBACK_PARAM;

	// Snapshot the qualification settings straight off the live params, so the
	// sample is read exactly the way the render reads pixels.
	QK_Params p;
	p.qinL   = params[QK_QIN]->u.pd.value;
	p.lumaWL = params[QK_LUMA_WEIGHTS]->u.pd.value;
	p.hueEnL = p.satEnL = p.lumEnL = 1;
	p.hueCenterF = p.hueWidthF = p.hueInSoftF = p.hueOutSoftF = 0.0;
	p.satInF = p.satOutF = p.satInSoftF = p.satOutSoftF = 0.0;
	p.lumInF = p.lumOutF = p.lumInSoftF = p.lumOutSoftF = 0.0;
	QK_Gates gates;
	QK_BuildGates(p, &gates);

	const int qin = (int)gates.qin;
	const float tr = QK_QualifyIn1((float)r, qin);
	const float tg = QK_QualifyIn1((float)g, qin);
	const float tb = QK_QualifyIn1((float)b, qin);

	float hF, sF;
	QK_RGBToHS(tr, tg, tb, (int)gates.model, hF, sF);
	const double h = hF, s = sF;
	const double y = gates.lumR * tr + gates.lumG * tg + gates.lumB * tb;

	// ---- subtract: per-sample carve against the live gate -----------------
	if (mode == QK_PICK_SUB) {
		const double center = (double)params[QK_HUE_CENTER]->u.ad.value / 65536.0;
		const double width  = params[QK_HUE_WIDTH]->u.fs_d.value;
		const double d      = SignedDeltaDeg(h, center);
		const double half   = width * 0.5;

		if (width >= 360.0) {
			// Full circle -> open a gap around the sample: recentre opposite
			// it and pull the width just under full.
			WriteAngle(params, QK_HUE_CENTER, WrapDeg(h + 180.0));
			WriteFloat(params, QK_HUE_WIDTH, 360.0 - 2.0 * kHuePad);
		} else if (std::fabs(d) <= half) {
			double newHalf = std::fabs(d) - kHuePad;
			if (newHalf < 0.5) newHalf = 0.5;
			WriteFloat(params, QK_HUE_WIDTH, 2.0 * newHalf);
		}
		SubtractLinear(params, QK_SAT_IN, QK_SAT_OUT, s, QK_SAT_VALID_MAX);
		SubtractLinear(params, QK_LUM_IN, QK_LUM_OUT, y, QK_LUM_VALID_MAX);
		return PF_Err_NONE;
	}

	// ---- set / add: accumulate into the envelope, then rewrite ------------
	if (!env->validB) {
		// First sample of the gesture seeds the envelope.
		if (mode == QK_PICK_ADD) {
			// Add starts from the CURRENT gate so the existing key is kept and
			// grown. Seed the extent with the live Center/Width, anchored on
			// the current centre.
			const double center = (double)params[QK_HUE_CENTER]->u.ad.value / 65536.0;
			const double width  = params[QK_HUE_WIDTH]->u.fs_d.value;
			const double half   = (width >= 360.0) ? 180.0 : width * 0.5;
			env->hueAnchorF = center;
			env->hueNegF    = -half;
			env->huePosF    =  half;

			double sLo = params[QK_SAT_IN]->u.fs_d.value;
			double sHi = params[QK_SAT_OUT]->u.fs_d.value;
			if (sLo > sHi) { double t = sLo; sLo = sHi; sHi = t; }
			env->satMinF = sLo; env->satMaxF = sHi;

			double yLo = params[QK_LUM_IN]->u.fs_d.value;
			double yHi = params[QK_LUM_OUT]->u.fs_d.value;
			if (yLo > yHi) { double t = yLo; yLo = yHi; yHi = t; }
			env->lumMinF = yLo; env->lumMaxF = yHi;

			// Then union this sample in (relative to the anchor).
			const double d = SignedDeltaDeg(h, env->hueAnchorF);
			if (d < env->hueNegF) env->hueNegF = d;
			if (d > env->huePosF) env->huePosF = d;
			if (s < env->satMinF) env->satMinF = s;
			if (s > env->satMaxF) env->satMaxF = s;
			if (y < env->lumMinF) env->lumMinF = y;
			if (y > env->lumMaxF) env->lumMaxF = y;
		} else {
			// Set: this sample IS the selection. A lone click (no drag) must
			// still give a usable band, so seed a small width around it; a
			// drag then grows outward from here.
			env->hueAnchorF = h;
			env->hueNegF    = -(kHueHalf - kHuePad);
			env->huePosF    =  (kHueHalf - kHuePad);
			env->satMinF    = s - (kLinHalf - kLinPad);
			env->satMaxF    = s + (kLinHalf - kLinPad);
			env->lumMinF    = y - (kLinHalf - kLinPad);
			env->lumMaxF    = y + (kLinHalf - kLinPad);
		}
		env->validB = 1;

		WriteBool(params, QK_HUE_ENABLE, true);
		WriteBool(params, QK_SAT_ENABLE, true);
		WriteBool(params, QK_LUM_ENABLE, true);
	} else {
		// Later samples in the gesture: union in. Hue extent is measured
		// against the anchor so wraparound is handled by the signed delta.
		const double d = SignedDeltaDeg(h, env->hueAnchorF);
		if (d < env->hueNegF) env->hueNegF = d;
		if (d > env->huePosF) env->huePosF = d;
		if (s < env->satMinF) env->satMinF = s;
		if (s > env->satMaxF) env->satMaxF = s;
		if (y < env->lumMinF) env->lumMinF = y;
		if (y > env->lumMaxF) env->lumMaxF = y;
	}

	WriteEnvelope(params, *env);
	return PF_Err_NONE;
}
