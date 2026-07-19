/*  sampling.cpp — eyedropper range picking (Resolve's pick / +pick / -pick,
    plan §1).

    Three color swatches (QK_SAMPLE_COLOR / _ADD / _SUB), each with its own
    eyedropper, mirror Resolve's three picker buttons. AE does not forward
    Ctrl/Alt modifiers to an effect's color param, so the operation is chosen
    by WHICH swatch fired rather than by a click modifier. Whichever the user
    eyedrops runs `QK_ApplySample` from PF_Cmd_USER_CHANGED_PARAM:

      Set (QK_SAMPLE_COLOR) — center each gate on the sample (hue width 20°,
                              sat/luma ±0.1), enable all three gates.
      Add (QK_SAMPLE_ADD)   — widen the nearest edge of each gate to include
                              the sample (+ a small pad).
      Sub (QK_SAMPLE_SUB)   — pull the nearest edge past the sample to exclude
                              it. A full-circle hue gate becomes "everything
                              except the sample's neighborhood".

    The sample runs through the SAME qualification math the render uses
    (QK_QualifyIn1 / QK_RGBToHS / luma weights via QK_BuildGates), and Hue is
    written back as Center/Width (the UI form; QK_BuildGates converts to the
    gate's In/Out).

    RESET GUARD (fixes reset->black): resetting the effect restores every
    swatch to its default grey, which fires USER_CHANGED_PARAM on the
    supervised swatches. If we re-keyed on that, reset would produce a tight
    key on grey and black the frame. So a pick whose color EQUALS the swatch
    default is treated as "no pick" and changes nothing. A genuine eyedrop of
    a pixel that happens to be exactly the default grey is vanishingly rare
    and recoverable with a nudge.

    Writes go through uu.change_flags = PF_ChangeFlag_CHANGED_VALUE — the
    standard supervised-param mechanism, so keyframing/undo behave normally.

    Sample from Output = Original + Alpha (or the Layer panel) so the pick
    reflects the input, not the matte. Region mean±stddev drag-sampling
    belongs to the custom-ECW polish phase.
*/

#define NOMINMAX
#include "params.h"

namespace {

const double kHueHalf   = 10.0;   // Set: hue half-width, degrees (20° total)
const double kHuePad    = 2.0;    // Add/Sub: hue pad, degrees
const double kLinHalf   = 0.10;   // Set: sat/luma half-width
const double kLinPad    = 0.025;  // Add/Sub: sat/luma pad

// The swatch default (matches PF_ADD_COLOR in main.cpp). A pick equal to this
// is a reset, not a user sample — see RESET GUARD above.
const A_u_char kDefR = 128, kDefG = 128, kDefB = 128;

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

PF_Err QK_ApplySample(PF_InData *in_data, PF_OutData *out_data,
                      PF_ParamDef *params[], A_long changedIndex)
{
	(void)in_data; (void)out_data;

	// Which swatch fired -> which operation, and where to read the color.
	A_long mode;
	if      (changedIndex == QK_SAMPLE_ADD) mode = QK_PICK_ADD;
	else if (changedIndex == QK_SAMPLE_SUB) mode = QK_PICK_SUB;
	else                                    mode = QK_PICK_SET;

	const PF_Pixel &c = params[changedIndex]->u.cd.value;

	// RESET GUARD: a swatch restored to its default is a reset, not a pick.
	if (c.red == kDefR && c.green == kDefG && c.blue == kDefB)
		return PF_Err_NONE;

	// Snapshot the qualification settings straight off the live params.
	QK_Params p;
	p.qinL   = params[QK_QIN]->u.pd.value;
	p.lumaWL = params[QK_LUMA_WEIGHTS]->u.pd.value;
	p.hueEnL = p.satEnL = p.lumEnL = 1;
	p.hueCenterF = p.hueWidthF = p.hueInSoftF = p.hueOutSoftF = 0.0;
	p.satInF = p.satOutF = p.satInSoftF = p.satOutSoftF = 0.0;
	p.lumInF = p.lumOutF = p.lumInSoftF = p.lumOutSoftF = 0.0;
	QK_Gates g;
	QK_BuildGates(p, &g);

	// The 8-bit sample, through the exact qualification path.
	const int qin = (int)g.qin;
	const float tr = QK_QualifyIn1((float)c.red   / 255.f, qin);
	const float tg = QK_QualifyIn1((float)c.green / 255.f, qin);
	const float tb = QK_QualifyIn1((float)c.blue  / 255.f, qin);

	float hF, sF;
	QK_RGBToHS(tr, tg, tb, (int)g.model, hF, sF);
	const double h = hF, s = sF;
	const double y = g.lumR * tr + g.lumG * tg + g.lumB * tb;

	// ---- hue (circular), stored as Center + Width -----------------------
	const double center = (double)params[QK_HUE_CENTER]->u.ad.value / 65536.0;
	const double width  = params[QK_HUE_WIDTH]->u.fs_d.value;

	if (mode == QK_PICK_SET) {
		WriteAngle(params, QK_HUE_CENTER, h);
		WriteFloat(params, QK_HUE_WIDTH, 2.0 * kHueHalf);
		WriteBool(params, QK_HUE_ENABLE, true);
		WriteBool(params, QK_SAT_ENABLE, true);
		WriteBool(params, QK_LUM_ENABLE, true);
	} else {
		// Signed angular distance of the sample from the current center,
		// wrapped to [-180, 180]. Inside the band => |d| <= width/2.
		double d = WrapDeg(h - center);
		if (d > 180.0) d -= 360.0;
		const double half = width * 0.5;
		if (mode == QK_PICK_ADD) {
			if (width < 360.0 && std::fabs(d) > half) {
				// Widen symmetrically so the sample lands just inside an edge;
				// keep the center put (predictable), cap at the full circle.
				double newHalf = std::fabs(d) + kHuePad;
				double newWidth = 2.0 * newHalf;
				if (newWidth > 360.0) newWidth = 360.0;
				WriteFloat(params, QK_HUE_WIDTH, newWidth);
			}
		} else {   // subtract
			if (width >= 360.0) {
				// Full circle -> a band that excludes the sample's
				// neighborhood: center opposite the sample, width just under
				// full so a gap opens around it.
				WriteAngle(params, QK_HUE_CENTER, WrapDeg(h + 180.0));
				WriteFloat(params, QK_HUE_WIDTH, 360.0 - 2.0 * kHuePad);
			} else if (std::fabs(d) <= half) {
				// Narrow so the sample falls just outside the nearer edge.
				double newHalf = std::fabs(d) - kHuePad;
				if (newHalf < 0.5) newHalf = 0.5;
				WriteFloat(params, QK_HUE_WIDTH, 2.0 * newHalf);
			}
		}
	}

	// ---- saturation / luma (linear) --------------------------------------
	ApplyLinear(params, QK_SAT_IN, QK_SAT_OUT, s, mode, QK_SAT_VALID_MAX);
	ApplyLinear(params, QK_LUM_IN, QK_LUM_OUT, y, mode, QK_LUM_VALID_MAX);

	return PF_Err_NONE;
}
