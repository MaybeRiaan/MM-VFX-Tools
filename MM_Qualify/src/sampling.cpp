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

    DRAG GROWS (OR ERODES) THE SELECTION. A gesture (press .. release)
    accumulates a QK_PickEnvelope; each sample unions into it — or, for Sub,
    carves out of it — and the gates are rewritten from the envelope. Change is
    therefore monotonic and independent of drag direction and speed: sweeping
    back over ground already covered cannot undo it.

    ALL THREE modes go through that accumulator. Sub reached it late: it used
    to re-read the live gate out of params[] per sample and carve all three
    gates at once. Because the gates are ANDed, carving three to exclude one
    colour took a huge bite — after which everything swept over was already
    outside the key and the drag had nothing left to do, so it looked frozen.
    The Subtract geometry note below covers the rule that replaced it.

    The sample runs through the SAME qualification math the render uses
    (QK_QualifyIn1 / QK_RGBToHS / luma weights via QK_BuildGates), and Hue is
    written back as Center/Width (the UI form; QK_BuildGates converts to the
    gate's In/Out the seam-free kernel wants).
*/

#define NOMINMAX
#include "params.h"

#include <cstdio>
#include <string>

namespace {

const double kHueHalf   = 10.0;   // Set (single click): hue half-width, degrees
const double kHuePad    = 2.0;    // pad added around a swept hue extent
const double kLinHalf   = 0.10;   // Set (single click): sat/luma half-width
const double kLinPad    = 0.025;  // pad added around a swept sat/luma extent

// Hue gap notched out of an unbounded gate by a Subtract — sized to match the
// band a Set click selects, so add and subtract cover comparable ground. NOT
// used for edge retreats; see the Subtract geometry note below for why that
// distinction is load-bearing.
const double kHueGap    = kHueHalf - kHuePad;

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

// Seed the envelope from the gate as it stands right now. Used by BOTH Add
// and Subtract at the first sample of a gesture: each one starts from the
// existing key and grows or erodes it from there.
void SeedEnvelopeFromGate(PF_ParamDef *params[], QK_PickEnvelope *e)
{
	const double center = (double)params[QK_HUE_CENTER]->u.ad.value / 65536.0;
	const double width  = params[QK_HUE_WIDTH]->u.fs_d.value;
	const double half   = (width >= 360.0) ? 180.0 : width * 0.5;
	e->hueAnchorF = center;
	e->hueNegF    = -half;
	e->huePosF    =  half;

	double sLo = params[QK_SAT_IN]->u.fs_d.value;
	double sHi = params[QK_SAT_OUT]->u.fs_d.value;
	if (sLo > sHi) { double t = sLo; sLo = sHi; sHi = t; }
	e->satMinF = sLo; e->satMaxF = sHi;

	double yLo = params[QK_LUM_IN]->u.fs_d.value;
	double yHi = params[QK_LUM_OUT]->u.fs_d.value;
	if (yLo > yHi) { double t = yLo; yLo = yHi; yHi = t; }
	e->lumMinF = yLo; e->lumMaxF = yHi;
}

// --- Subtract geometry -----------------------------------------------------
//
// The three gates are ANDed, so a pixel leaves the selection the moment ONE
// gate rejects it. Carving all three to remove a single colour therefore
// throws away three half-spaces where one would do. Three rules follow, and
// between them they are also what lets a drag scrub:
//
//   1. If the sample is ALREADY outside any enabled gate, it is already
//      deselected. Do nothing at all.
//
//   2. NEVER close an open bound. A bound parked at its slider extreme is
//      OPEN — QK_BuildGates maps it to +/-BIG, i.e. no limit at all — and
//      closing one is not a trim, it is a brand new limit that drops an
//      unbounded set of pixels in a single click. Sat Out at 1.0 is the
//      dangerous one: pulling it to 0.975 silently evicts every pixel whose
//      saturation exceeds that, which per the params.h note is a large and
//      important class (near-black and near-white chromatic pixels). Such a
//      carve is priced kCostInf so it is chosen only if nothing else can.
//      Hue is the exception: a full circle is carved by NOTCHING a gap, which
//      is finite and safe because hue wraps.
//
//   3. Otherwise carve exactly ONE gate — whichever loses the smallest
//      FRACTION of its own span. Because the gates are ANDed, the selected
//      volume goes as the product of the spans, so the smallest fractional
//      loss is the smallest loss of key.
//
// Two different sizes are in play, and conflating them wipes the key:
//   · Retreating an EDGE past a sample needs only a small clearance
//     (kHuePad / kLinPad). A Set click builds a band 20 degrees wide, so
//     retreating a Set-sized 10 degrees from its centre makes the two edges
//     meet and the gate collapses to nothing.
//   · Notching a GAP in a full circle has no edge to retreat, so the gap size
//     is a free choice — and there it SHOULD match what a Set click selects
//     (kHueGap), which is what makes add and subtract cover comparable ground.

const double kNoCarve = -1.0;   // sample is already outside this gate
const double kCostInf = 1.0e9;  // gate is carvable only by closing an open bound

// Clearance a carve leaves between the retreated edge and the sample, as a
// FRACTION of that gate's span rather than a fixed step. Deliberate, on two
// counts:
//   · A fixed 0.025 is 7% of a wide sat gate but 45% of a narrow luma one, so
//     on narrow gates the clearance alone did most of the damage.
//   · A small proportional clearance is what lets a DRAG scrub. Each sample
//     retreats the edge just past itself, leaving the next sample along still
//     inside and able to take another bite. A large clearance swallows the
//     whole swept band on the FIRST sample, and the rest of the drag then
//     finds every sample already outside and correctly does nothing — which
//     is precisely how "subtract does not drag" looked.
const double kMarginFrac = 0.02;

double LinMargin(double span)
{
	double m = kMarginFrac * span;
	if (m > kLinPad) m = kLinPad;   // never exceed the old fixed clearance
	if (m < 1.0e-6)  m = 1.0e-6;    // but always strictly clear the sample
	return m;
}

double HueMargin(double span)
{
	double m = kMarginFrac * span;
	if (m > kHuePad) m = kHuePad;
	if (m < 1.0e-4)  m = 1.0e-4;
	return m;
}

// Which edge a carve would retreat.
enum { QK_SIDE_NONE = 0, QK_SIDE_LO = -1, QK_SIDE_HI = 1 };

struct CarvePlan {
	double	cost;    // fraction of the gate's span lost, or kNoCarve / kCostInf
	int		side;    // QK_SIDE_*
	double	margin;  // clearance the carve will leave; carried so the carve
					 // cannot compute a different one than the price assumed
					 // (they diverge on open gates, where the span used for
					 // pricing is the slider range, not hi-lo)
};

// A bound within QK_OPEN_EPS of its slider extreme is open — the SAME test
// QK_BuildGates uses, so what we consider open is exactly what the render
// treats as unbounded.
bool OpenLo(double lo, double vmin) { return lo <= vmin + QK_OPEN_EPS; }
bool OpenHi(double hi, double vmax) { return hi >= vmax - QK_OPEN_EPS; }

CarvePlan LinearCarvePlan(double lo, double hi, double v,
                          double vmin, double vmax)
{
	CarvePlan plan = { kCostInf, QK_SIDE_NONE, 0.0 };

	const bool openLo = OpenLo(lo, vmin);
	const bool openHi = OpenHi(hi, vmax);

	// Inside test against the TRUE gate: an open side reaches to infinity, so
	// a sample past the slider range (saturation over 1.0 on a near-black
	// chromatic pixel, luma over 1.0 in HDR) is legitimately inside.
	if (!openLo && v < lo) { plan.cost = kNoCarve; return plan; }
	if (!openHi && v > hi) { plan.cost = kNoCarve; return plan; }

	// Fraction of the gate's own span. With one side open the span is
	// unbounded, so the slider range stands in for it — conservative, and it
	// keeps the number comparable with a closed gate's.
	const double span = (openLo || openHi) ? (vmax - vmin) : (hi - lo);
	if (span <= 1e-9) { plan.cost = kNoCarve; return plan; }
	const double margin = LinMargin(span);

	// COST IS WHAT THE CARVE ACTUALLY REMOVES — the gap to the edge PLUS the
	// clearance. Pricing the gap alone under-rates a carve on a narrow gate,
	// where the clearance is the larger half of the bite, and that mispricing
	// is what kept steering carves into gates they then nearly annihilated.
	// A cost above 1.0 means the carve would empty the gate; nothing special
	// is needed for that, it simply loses to any partial carve.
	double best = kCostInf;

	// Raise the lower edge past the sample. Only if that edge is closed, and
	// only if the new bound stays inside the slider range — pushing it past
	// the top would leave a degenerate gate that reads as inverted.
	if (!openLo && v + margin <= vmax) {
		best = (v - lo) + margin;
		plan.side = QK_SIDE_LO;
	}
	// Lower the upper edge past the sample, same conditions mirrored.
	if (!openHi && v - margin >= vmin) {
		const double removed = (hi - v) + margin;
		if (removed < best) { best = removed; plan.side = QK_SIDE_HI; }
	}

	if (plan.side == QK_SIDE_NONE) return plan;   // stays kCostInf
	plan.cost = best / span;
	return plan;
}

CarvePlan HueCarvePlan(const QK_PickEnvelope &e, double h)
{
	CarvePlan plan = { kCostInf, QK_SIDE_NONE, 0.0 };
	const double span = e.huePosF - e.hueNegF;

	// Full circle: every hue is inside, and the carve notches a finite gap
	// rather than retreating an edge. Safe, and usually the right gate to
	// spend on a default key.
	if (span >= 360.0 - 1e-6) {
		plan.cost   = (2.0 * kHueGap) / 360.0;
		plan.side   = QK_SIDE_NONE;   // gap, not an edge retreat
		plan.margin = kHueGap;
		return plan;
	}
	if (span <= 1e-9) { plan.cost = kNoCarve; return plan; }

	const double d = SignedDeltaDeg(h, e.hueAnchorF);
	if (d < e.hueNegF || d > e.huePosF) { plan.cost = kNoCarve; return plan; }

	const double margin = HueMargin(span);
	const double loRemoved = (d - e.hueNegF) + margin;
	const double hiRemoved = (e.huePosF - d) + margin;
	if (loRemoved < hiRemoved) { plan.cost = loRemoved / span; plan.side = QK_SIDE_LO; }
	else                       { plan.cost = hiRemoved / span; plan.side = QK_SIDE_HI; }
	return plan;
}

void CarveHueAt(QK_PickEnvelope *e, double h, int side, double margin)
{
	if (e->huePosF - e->hueNegF >= 360.0 - 1e-6) {
		e->hueAnchorF = WrapDeg(h + 180.0);
		e->hueNegF    = -(180.0 - kHueGap);
		e->huePosF    =  (180.0 - kHueGap);
		return;
	}
	const double d = SignedDeltaDeg(h, e->hueAnchorF);
	if (side == QK_SIDE_LO) e->hueNegF = d + margin;
	else                    e->huePosF = d - margin;
	if (e->hueNegF > e->huePosF) {
		const double m = 0.5 * (e->hueNegF + e->huePosF);
		e->hueNegF = m; e->huePosF = m;
	}
}

void CarveLinearAt(double *lo, double *hi, double v, int side, double margin)
{
	if (side == QK_SIDE_LO) *lo = v + margin;
	else                    *hi = v - margin;
	if (*lo > *hi) { const double m = 0.5 * (*lo + *hi); *lo = m; *hi = m; }
}

// Optional trace for the Subtract path. OFF unless MM_QUALIFY_PICK_LOG is set
// to something other than 0, and then one line per sample into
// %TEMP%\MM_Qualify_pick.log. Subtract is the one mode whose result is hard
// to read off the screen — a bite that lands on a gate you were not watching
// looks identical to no bite at all — so this prints the whole decision:
// the sample, every gate before, each gate's cost, and every gate after.
// ---------------------------------------------------------------------------
#ifdef AE_OS_WIN
bool PickLogOn()
{
	static const bool on = [] {
		char v[8];
		const DWORD n = ::GetEnvironmentVariableA("MM_QUALIFY_PICK_LOG", v, (DWORD)sizeof(v));
		return n > 0 && n < (DWORD)sizeof(v) && v[0] != '0';
	}();
	return on;
}

void PickLogCarve(double h, double s, double y,
                  const QK_PickEnvelope &before, const QK_PickEnvelope &after,
                  bool hueEn, bool satEn, bool lumEn,
                  double costH, double costS, double costY)
{
	if (!PickLogOn()) return;

	char path[MAX_PATH];
	const DWORD n = ::GetTempPathA(MAX_PATH, path);
	if (n == 0 || n > MAX_PATH) return;
	std::string file(path);
	file += "MM_Qualify_pick.log";

	FILE *f = nullptr;
	if (fopen_s(&f, file.c_str(), "a") != 0 || !f) return;

	const char *hit = "NONE (sample already outside a gate, or no gate enabled)";
	if      (before.hueNegF != after.hueNegF || before.huePosF != after.huePosF ||
	         before.hueAnchorF != after.hueAnchorF)                   hit = "HUE";
	else if (before.satMinF != after.satMinF || before.satMaxF != after.satMaxF) hit = "SAT";
	else if (before.lumMinF != after.lumMinF || before.lumMaxF != after.lumMaxF) hit = "LUM";

	fprintf(f,
		"SUB h=%7.2f s=%.4f y=%.4f en=%d%d%d | "
		"cost H=%7.4f S=%7.4f Y=%7.4f (neg = outside) | carved %s\n"
		"    before hue[anchor=%7.2f neg=%7.2f pos=%7.2f] sat[%.4f %.4f] lum[%.4f %.4f]\n"
		"    after  hue[anchor=%7.2f neg=%7.2f pos=%7.2f] sat[%.4f %.4f] lum[%.4f %.4f]\n",
		h, s, y, hueEn ? 1 : 0, satEn ? 1 : 0, lumEn ? 1 : 0,
		costH, costS, costY, hit,
		before.hueAnchorF, before.hueNegF, before.huePosF,
		before.satMinF, before.satMaxF, before.lumMinF, before.lumMaxF,
		after.hueAnchorF, after.hueNegF, after.huePosF,
		after.satMinF, after.satMaxF, after.lumMinF, after.lumMaxF);
	fclose(f);
}
#else
inline void PickLogCarve(double, double, double, const QK_PickEnvelope &,
                         const QK_PickEnvelope &, bool, bool, bool,
                         double, double, double) {}
#endif

// Carve the sample out of the envelope — the mirror of the union in
// QK_ApplySampleRGB. Disabled gates take no part: they cannot reject a pixel,
// so they cannot be the cheapest way to reject one either.
void CarveEnvelope(QK_PickEnvelope *e, double h, double s, double y,
                   bool hueEn, bool satEn, bool lumEn,
                   double *costHP, double *costSP, double *costYP)
{
	// The sample is NOT clamped into slider space. Pinning it to the extreme is
	// what made a sample sitting past an open bound score as a free carve of
	// that bound; each plan below handles the open case honestly instead.
	const CarvePlan planH = hueEn ? HueCarvePlan(*e, h)
	                              : CarvePlan{ kNoCarve, QK_SIDE_NONE, 0.0 };
	const CarvePlan planS = satEn ? LinearCarvePlan(e->satMinF, e->satMaxF, s,
	                                                QK_SAT_MIN, QK_SAT_SLIDER_MAX)
	                              : CarvePlan{ kNoCarve, QK_SIDE_NONE, 0.0 };
	const CarvePlan planY = lumEn ? LinearCarvePlan(e->lumMinF, e->lumMaxF, y,
	                                                QK_LUM_MIN, QK_LUM_SLIDER_MAX)
	                              : CarvePlan{ kNoCarve, QK_SIDE_NONE, 0.0 };
	if (costHP) *costHP = planH.cost;
	if (costSP) *costSP = planS.cost;
	if (costYP) *costYP = planY.cost;

	// Rule 1 — already rejected somewhere, so the pixel is not selected and
	// there is nothing to remove. kCostInf is NOT this case: it means inside
	// but only carvable by closing an open bound.
	if ((hueEn && planH.cost == kNoCarve) ||
	    (satEn && planS.cost == kNoCarve) ||
	    (lumEn && planY.cost == kNoCarve)) return;

	// Rule 3 — smallest fractional loss wins. A kCostInf plan loses to any
	// real one and is never acted on, so an open bound stays open.
	double best = kCostInf;
	if (hueEn && planH.cost < best) best = planH.cost;
	if (satEn && planS.cost < best) best = planS.cost;
	if (lumEn && planY.cost < best) best = planY.cost;
	if (best >= kCostInf) return;

	if      (hueEn && planH.cost == best) CarveHueAt(e, h, planH.side, planH.margin);
	else if (satEn && planS.cost == best) CarveLinearAt(&e->satMinF, &e->satMaxF, s, planS.side, planS.margin);
	else if (lumEn && planY.cost == best) CarveLinearAt(&e->lumMinF, &e->lumMaxF, y, planY.side, planY.margin);
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

// Write the envelope out VERBATIM — no outward pad. WriteEnvelope's padding
// exists to make a union generous; applying it to a carve would re-grow the
// gate by kHuePad/kLinPad on every sample and cancel the erosion out. The
// carve has already applied its own inward pad.
void WriteEnvelopeExact(PF_ParamDef *params[], const QK_PickEnvelope &e)
{
	double width = e.huePosF - e.hueNegF;
	if (width < QK_HUE_WIDTH_MIN) width = QK_HUE_WIDTH_MIN;
	if (width > QK_HUE_WIDTH_MAX) width = QK_HUE_WIDTH_MAX;
	const double center = e.hueAnchorF + 0.5 * (e.huePosF + e.hueNegF);
	WriteAngle(params, QK_HUE_CENTER, WrapDeg(center));
	WriteFloat(params, QK_HUE_WIDTH, width);

	double sLo = e.satMinF, sHi = e.satMaxF;
	if (sLo < QK_SAT_MIN)       sLo = QK_SAT_MIN;
	if (sHi > QK_SAT_VALID_MAX) sHi = QK_SAT_VALID_MAX;
	if (sLo > sHi)              sLo = sHi;
	WriteFloat(params, QK_SAT_IN,  sLo);
	WriteFloat(params, QK_SAT_OUT, sHi);

	double yLo = e.lumMinF, yHi = e.lumMaxF;
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

	// ---- subtract: carve the gesture accumulator --------------------------
	// Seeded once from the live gate, then eroded sample by sample. The
	// accumulator keeps a drag off params[], whose contents are AE's
	// checked-out copy for THIS event and need not carry the previous sample's
	// write; the geometry above is what makes each bite small enough that the
	// drag has anything left to erode. See the Subtract geometry note.
	if (mode == QK_PICK_SUB) {
		if (!env->validB) {
			SeedEnvelopeFromGate(params, env);
			env->validB = 1;
		}
		const bool hueEn = params[QK_HUE_ENABLE]->u.bd.value != 0;
		const bool satEn = params[QK_SAT_ENABLE]->u.bd.value != 0;
		const bool lumEn = params[QK_LUM_ENABLE]->u.bd.value != 0;
		const QK_PickEnvelope before = *env;
		double costH = 0, costS = 0, costY = 0;
		CarveEnvelope(env, h, s, y, hueEn, satEn, lumEn, &costH, &costS, &costY);
		PickLogCarve(h, s, y, before, *env, hueEn, satEn, lumEn, costH, costS, costY);
		WriteEnvelopeExact(params, *env);
		return PF_Err_NONE;
	}

	// ---- set / add: accumulate into the envelope, then rewrite ------------
	if (!env->validB) {
		// First sample of the gesture seeds the envelope.
		if (mode == QK_PICK_ADD) {
			// Add starts from the CURRENT gate so the existing key is kept and
			// grown, then unions this sample in (relative to the anchor).
			SeedEnvelopeFromGate(params, env);
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
