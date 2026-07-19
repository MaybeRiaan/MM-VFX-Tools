/*  sampling.cpp — eyedropper range picking (Resolve's picker / +picker /
    -picker, plan §1).

    Fires from PF_Cmd_USER_CHANGED_PARAM when the supervised "Sample" color
    swatch changes (i.e. the user eyedropped a color; Ctrl-click gives AE's
    native 5x5 average). The sample runs through the SAME qualification math
    the render uses — QK_QualifyIn1 / QK_RGBToHS / luma weights via
    QK_BuildGates — then updates the gate params according to the Picker
    popup:

      Set Range      — center each gate on the sample (hue ±10°, sat/luma
                       ±0.1), enable all three gates. Softness untouched.
      Add To Range   — widen the nearest edge of each gate to include the
                       sample (+ a small pad). Gates already containing the
                       sample are untouched.
      Subtract From  — pull the nearest edge past the sample to exclude it.
                       A full-circle hue gate becomes "everything except
                       the sample's neighborhood".

    Writes go through uu.change_flags = PF_ChangeFlag_CHANGED_VALUE — the
    standard supervised-param mechanism, so keyframing/undo behave normally.

    Notes/limits (v1, standard params only — plan §3): the AE eyedropper
    samples the rendered comp/layer at 8bpc in the working space. Sample with
    Output = Original + Alpha (or from the Layer panel) so the pick reflects
    the input, not the matte. Region mean±stddev drag-sampling belongs to the
    custom-ECW polish phase.
*/

#define NOMINMAX
#include "params.h"

namespace {

const double kHueHalf   = 10.0;   // Set: hue half-width, degrees
const double kHuePad    = 2.0;    // Add/Sub: hue pad, degrees
const double kLinHalf   = 0.10;   // Set: sat/luma half-width
const double kLinPad    = 0.025;  // Add/Sub: sat/luma pad

inline double WrapDeg(double x) { return x - 360.0 * std::floor(x / 360.0); }

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

inline double ReadAngle(PF_ParamDef *params[], A_long idx)
{
	return (double)params[idx]->u.ad.value / 65536.0;
}

// Linear-gate edit shared by Sat and Luma. lo/hi are the CURRENT bounds.
void ApplyLinear(PF_ParamDef *params[], A_long inIdx, A_long outIdx,
                 double v, A_long mode, double vmax)
{
	double lo = params[inIdx]->u.fs_d.value;
	double hi = params[outIdx]->u.fs_d.value;
	if (lo > hi) { double t = lo; lo = hi; hi = t; }

	if (mode == QK_PICK_SET) {
		lo = v - kLinHalf; hi = v + kLinHalf;
	} else if (mode == QK_PICK_ADD) {
		if (v - kLinPad < lo) lo = v - kLinPad;
		if (v + kLinPad > hi) hi = v + kLinPad;
	} else {   // subtract — only when the sample is inside
		if (v >= lo && v <= hi) {
			if (v - lo < hi - v) lo = (v + kLinPad < hi) ? v + kLinPad : hi;
			else                 hi = (v - kLinPad > lo) ? v - kLinPad : lo;
		}
	}
	if (lo < 0.0) lo = 0.0;
	if (hi > vmax) hi = vmax;
	if (lo > hi) lo = hi;
	WriteFloat(params, inIdx, lo);
	WriteFloat(params, outIdx, hi);
}

} // namespace

PF_Err QK_ApplySample(PF_InData *in_data, PF_OutData *out_data, PF_ParamDef *params[])
{
	(void)in_data; (void)out_data;

	// Snapshot the qualification settings straight off the live params —
	// they are fully populated for USER_CHANGED_PARAM.
	QK_Params p;
	p.qinL   = params[QK_QIN]->u.pd.value;
	p.modelL = params[QK_MODEL]->u.pd.value;
	p.lumaWL = params[QK_LUMA_WEIGHTS]->u.pd.value;
	p.hueEnL = p.satEnL = p.lumEnL = 1;
	p.hueInF = p.hueOutF = p.hueInSoftF = p.hueOutSoftF = 0.0;
	p.satInF = p.satOutF = p.satInSoftF = p.satOutSoftF = 0.0;
	p.lumInF = p.lumOutF = p.lumInSoftF = p.lumOutSoftF = 0.0;
	QK_Gates g;
	QK_BuildGates(p, &g);

	// The 8-bit sample, through the exact qualification path.
	const PF_Pixel &c = params[QK_SAMPLE_COLOR]->u.cd.value;
	const int qin = (int)g.qin;
	const float tr = QK_QualifyIn1((float)c.red   / 255.f, qin);
	const float tg = QK_QualifyIn1((float)c.green / 255.f, qin);
	const float tb = QK_QualifyIn1((float)c.blue  / 255.f, qin);

	float hF, sF;
	QK_RGBToHS(tr, tg, tb, (int)g.model, hF, sF);
	const double h = hF, s = sF;
	const double y = g.lumR * tr + g.lumG * tg + g.lumB * tb;

	const A_long mode = params[QK_PICKER_MODE]->u.pd.value;

	// ---- hue (circular) --------------------------------------------------
	const double hueIn  = ReadAngle(params, QK_HUE_IN);
	const double hueOut = ReadAngle(params, QK_HUE_OUT);
	const double w = WrapDeg(hueOut - hueIn);          // 0 == full circle
	const double d = WrapDeg(h - WrapDeg(hueIn));      // sample vs In edge

	if (mode == QK_PICK_SET) {
		WriteAngle(params, QK_HUE_IN,  WrapDeg(h - kHueHalf));
		WriteAngle(params, QK_HUE_OUT, WrapDeg(h + kHueHalf));
		WriteBool(params, QK_HUE_ENABLE, true);
		WriteBool(params, QK_SAT_ENABLE, true);
		WriteBool(params, QK_LUM_ENABLE, true);
	} else if (mode == QK_PICK_ADD) {
		// Extend the nearer edge to include the sample. Writes preserve the
		// param's absolute (unbounded) value so keyframes stay continuous.
		if (w > 0.0 && d > w) {
			const double dOut = d - w;          // past Out, going forward
			const double dIn  = 360.0 - d;      // before In, going back
			if (dOut <= dIn) {
				double grow = dOut + kHuePad;
				if (w + grow >= 360.0) grow = 360.0 - w;   // full circle cap
				WriteAngle(params, QK_HUE_OUT, hueOut + grow);
			} else {
				double grow = dIn + kHuePad;
				if (w + grow >= 360.0) grow = 360.0 - w;
				WriteAngle(params, QK_HUE_IN, hueIn - grow);
			}
		}
	} else {   // subtract
		if (w <= 0.0) {
			// Full circle -> everything except the sample's neighborhood.
			WriteAngle(params, QK_HUE_IN,  WrapDeg(h + kHuePad));
			WriteAngle(params, QK_HUE_OUT, WrapDeg(h + 360.0 - kHuePad));
		} else if (d <= w) {
			// Inside: pull the nearer edge past the sample, never past the
			// other edge (a subtract can shrink the arc to ~0 but not flip it).
			if (d < w - d) {
				double shrinkTo = w - d - kHuePad;
				if (shrinkTo < 0.01) shrinkTo = 0.01;
				WriteAngle(params, QK_HUE_IN, hueIn + (w - shrinkTo));
			} else {
				double shrinkTo = d - kHuePad;
				if (shrinkTo < 0.01) shrinkTo = 0.01;
				WriteAngle(params, QK_HUE_OUT, hueOut - (w - shrinkTo));
			}
		}
	}

	// ---- saturation / luma (linear) --------------------------------------
	ApplyLinear(params, QK_SAT_IN, QK_SAT_OUT, s, mode, QK_SAT_VALID_MAX);
	ApplyLinear(params, QK_LUM_IN, QK_LUM_OUT, y, mode, QK_LUM_VALID_MAX);

	return PF_Err_NONE;
}
