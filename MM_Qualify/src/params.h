/*  params.h — MM Qualify ("Qualify", Category "MM").

    Central header: SDK includes, version defines (keep in sync with
    resources/MM_QualifyPiPL.r — mismatch is AE error 8800b), parameter
    ranges/defaults, param indices, disk IDs, the checked-out param snapshot
    struct shared by PreRender and SmartRender, and the host-side resolution
    of popups into the kernel-space QK_Gates block.

    The plugin is the plan's v2 descoped Resolve-parity HSL qualifier
    (docs/hsl_keyer_plugin_plan.md): three ANDed gates (Hue with seam-free
    wraparound, Saturation, Luma), Resolve's matte-finesse chain, temporal
    stabilize, garbage matte, all params standard AE controls -> natively
    keyframeable. MFR-safe: no mutable global state anywhere.
*/

#pragma once

#ifndef MM_QUALIFY_PARAMS_H
#define MM_QUALIFY_PARAMS_H

typedef unsigned char		u_char;
typedef unsigned short		u_short;
typedef unsigned short		u_int16;
typedef unsigned long		u_long;
typedef short int			int16;
#define PF_TABLE_BITS		12
#define PF_TABLE_SZ_16		4096

#define PF_DEEP_COLOR_AWARE 1

#include "AEConfig.h"

#ifdef AE_OS_WIN
	typedef unsigned short PixelType;
	#include <Windows.h>
#endif

#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectCBSuites.h"
#include "String_Utils.h"
#include "AE_GeneralPlug.h"
#include "AEFX_ChannelDepthTpl.h"
#include "AEGP_SuiteHandler.h"
#include "AEFX_SuiteHelper.h"
#include "Smart_Utils.h"

// The shared per-pixel core, in its C++ spelling. QK_Gates and every gate/
// cleanup/compose function come from here — the same file the GPU kernels
// compile (src/qk_core.h header comment explains the contract).
#include "qk_cpu_prologue.h"
#include "qk_core.h"

// Keep in sync with resources/MM_QualifyPiPL.r.
#define MAJOR_VERSION	1
#define MINOR_VERSION	0
#define BUG_VERSION		0
#define STAGE_VERSION	PF_Stage_DEVELOP
#define BUILD_VERSION	1

// ---------------------------------------------------------------------------
// Popups (1-based) and ranges/defaults
// ---------------------------------------------------------------------------

// Color Space Override (Advanced) — tells the qualifier how to read the
// footage when building the matte. The image is NEVER touched, only the
// matte-building copy. Plain-language, two choices:
//   Display — footage is display / log encoded (sRGB, Rec.709, or any log).
//             Key the pixels directly; they're already perceptually spread.
//             The right default for the vast majority of AE work.
//   Linear  — footage is scene-linear (ACEScg / linear). Internally lift to
//             a log space before keying so darks aren't crushed together.
//             This is how Resolve's ACES timeline qualifies linear footage.
enum {
	QK_QIN_ASIS_P = 1,
	QK_QIN_LINLOG_P,
	QK_QIN_NUM
};
#define QK_QIN_DFLT		QK_QIN_ASIS_P
#define QK_QIN_ITEMS	"Display|Linear"

// Luma weights (Advanced). Rec.709 is right for the overwhelming majority of
// AE work; the alternates are there for correctness on wide-gamut / SD
// pipelines. The plan's "working primaries" cannot be queried from inside an
// AE effect (the host does not expose working-space primaries).
enum {
	QK_LW_709 = 1,
	QK_LW_2020,
	QK_LW_601,
	QK_LW_AVG,
	QK_LW_NUM
};
#define QK_LW_DFLT		QK_LW_709
#define QK_LW_ITEMS		"Rec.709|Rec.2020|Rec.601|Average"

// Output popup — order matches qk_core.h QK_OUT_* (value - 1).
enum {
	QK_OUTPUT_KEYED = 1,
	QK_OUTPUT_MATTE,
	QK_OUTPUT_MATTE_INV,
	QK_OUTPUT_HILITE,
	QK_OUTPUT_RAW,
	QK_OUTPUT_NUM
};
#define QK_OUTPUT_DFLT	QK_OUTPUT_KEYED
#define QK_OUTPUT_ITEMS	"Original + Alpha|Matte|Matte Inverted|Highlight|Qualify Raw"

// Garbage channel — order matches qk_core.h QK_GCHAN_* (used 1-based).
enum {
	QK_GCHAN_ALPHA_P = 1,
	QK_GCHAN_ALPHA_INV_P,
	QK_GCHAN_LUMA_P,
	QK_GCHAN_LUMA_INV_P,
	QK_GCHAN_NUM
};
#define QK_GCHAN_DFLT	QK_GCHAN_ALPHA_P
#define QK_GCHAN_ITEMS	"Alpha|Alpha Inverted|Luma|Luma Inverted"

// Hue gate: Center (angle, unbounded — animating across the 0 seam is smooth,
// all hue math wraps) + Width (the total arc kept, degrees) + independent
// Soft In / Soft Out. The host converts Center/Width -> the In/Out edges the
// gate math uses (QK_BuildGates), so the seam-free kernel is unchanged.
// Default Width 360 == full circle (all hues pass) -> passthrough at reset.
#define QK_HUE_CENTER_DFLT		0.0
#define QK_HUE_WIDTH_MIN		0.0
#define QK_HUE_WIDTH_MAX		360.0
#define QK_HUE_WIDTH_DFLT		360.0
#define QK_HUE_SOFT_MIN			0.0
#define QK_HUE_SOFT_MAX			180.0
#define QK_HUE_SOFT_DFLT		5.0

// Sat gate: bounds 0..1 on the slider, valid to 2 (defensive headroom).
#define QK_SAT_MIN				0.0
#define QK_SAT_VALID_MAX		2.0
#define QK_SAT_SLIDER_MAX		1.0
#define QK_SAT_IN_DFLT			0.0
#define QK_SAT_OUT_DFLT			1.0
#define QK_SAT_SOFT_MAX			1.0
#define QK_SAT_SOFT_DFLT		0.0

// Luma gate: HDR-aware — bounds settable to 8.0 (float-safe superwhites).
#define QK_LUM_MIN				0.0
#define QK_LUM_VALID_MAX		8.0
#define QK_LUM_SLIDER_MAX		1.0
#define QK_LUM_IN_DFLT			0.0
#define QK_LUM_OUT_DFLT			1.0
#define QK_LUM_SOFT_VALID_MAX	4.0
#define QK_LUM_SOFT_SLIDER_MAX	1.0
#define QK_LUM_SOFT_DFLT		0.0

// Matte finesse. Denoise / Blur Radius ARE Gaussian sigmas in full-res layer
// px (downsample-scaled in the render path — never use raw values there).
// Denoise is deliberately fine: the useful range for killing key sizzle is
// well under 3px, so the slider tops out at 5 with hundredths precision (the
// old 0..10/tenths jumped too coarsely — user feedback).
#define QK_DENOISE_MIN			0.0
#define QK_DENOISE_MAX			5.0
#define QK_DENOISE_DFLT			0.0

#define QK_CLEAN_MIN			0.0
#define QK_CLEAN_MAX			100.0
#define QK_CLEAN_DFLT			0.0

#define QK_CLIP_MIN				0.0
#define QK_CLIP_MAX				100.0
#define QK_BLACK_CLIP_DFLT		0.0
#define QK_WHITE_CLIP_DFLT		100.0

#define QK_BLUR_MIN				0.0
#define QK_BLUR_MAX				50.0
#define QK_BLUR_DFLT			0.0

#define QK_RATIO_MIN			-100.0
#define QK_RATIO_MAX			100.0
#define QK_RATIO_DFLT			0.0

#define QK_SMOOTH_MIN			0.0
#define QK_SMOOTH_MAX			100.0
#define QK_ATTACK_DFLT			0.0
#define QK_RELEASE_DFLT			50.0

// Temporal stabilize: IIR warm-started QK_TEMPORAL_WINDOW frames back —
// stateless (MFR-safe) yet converged to the true IIR for the k range
// QK_SmoothK produces. Frames are checked out under QK_CHECKOUT_HIST0 + i.
#define QK_TEMPORAL_WINDOW		6

// checkout_layer ids. Input reuses its param index; garbage and the history
// frames need their own non-negative ids.
enum {
	QK_CHECKOUT_INPUT   = 0,	// == QK_INPUT
	QK_CHECKOUT_GARBAGE = 1,
	QK_CHECKOUT_HIST0   = 2		// .. QK_CHECKOUT_HIST0 + QK_TEMPORAL_WINDOW - 1
};

// ---------------------------------------------------------------------------
// Param indices (0 = implicit input layer). On-screen order. Pre-release:
// order and disk IDs may still change freely; lock both at first ship.
// ---------------------------------------------------------------------------
// Layout (top to bottom): three Sample eyedroppers FIRST (Set / Add /
// Subtract — mirroring Resolve's pick/+pick/-pick buttons, each a plain
// color swatch since AE does not forward Ctrl/Alt modifiers to an effect's
// color param); then the three gates (Hue = Center/Width/softs); Output;
// Matte Finesse; Garbage Matte; and an Advanced group holding the
// color-science controls (Color Space Override, Luma Weights).
enum {
	QK_INPUT = 0,
	QK_TOPIC_QUAL,			// 1  "Qualifier"
	QK_SAMPLE_COLOR,		// 2  (supervised) eyedropper: SET the range
	QK_SAMPLE_ADD,			// 3  (supervised) eyedropper: ADD to the range
	QK_SAMPLE_SUB,			// 4  (supervised) eyedropper: SUBTRACT from range
	QK_TOPIC_HUE,			// 5  "Hue"
	QK_HUE_ENABLE,			// 6
	QK_HUE_CENTER,			// 7  (angle)
	QK_HUE_WIDTH,			// 8
	QK_HUE_IN_SOFT,			// 9
	QK_HUE_OUT_SOFT,		// 10
	QK_TOPIC_HUE_END,		// 11
	QK_TOPIC_SAT,			// 12 "Saturation"
	QK_SAT_ENABLE,			// 13
	QK_SAT_IN,				// 14
	QK_SAT_OUT,				// 15
	QK_SAT_IN_SOFT,			// 16
	QK_SAT_OUT_SOFT,		// 17
	QK_TOPIC_SAT_END,		// 18
	QK_TOPIC_LUM,			// 19 "Luma"
	QK_LUM_ENABLE,			// 20
	QK_LUM_IN,				// 21
	QK_LUM_OUT,				// 22
	QK_LUM_IN_SOFT,			// 23
	QK_LUM_OUT_SOFT,		// 24
	QK_TOPIC_LUM_END,		// 25
	QK_TOPIC_QUAL_END,		// 26
	QK_OUTPUT,				// 27
	QK_TOPIC_FINESSE,		// 28 "Matte Finesse"
	QK_DENOISE,				// 29
	QK_CLEAN_BLACK,			// 30
	QK_CLEAN_WHITE,			// 31
	QK_BLACK_CLIP,			// 32
	QK_WHITE_CLIP,			// 33
	QK_BLUR,				// 34
	QK_RATIO,				// 35
	QK_TEMPORAL,			// 36 (supervised — greys Attack/Release)
	QK_ATTACK,				// 37
	QK_RELEASE,				// 38
	QK_TOPIC_FINESSE_END,	// 39
	QK_TOPIC_GARBAGE,		// 40 "Garbage Matte"
	QK_GARBAGE_LAYER,		// 41
	QK_GARBAGE_CHAN,		// 42
	QK_TOPIC_GARBAGE_END,	// 43
	QK_TOPIC_ADV,			// 44 "Advanced (Color Science)"
	QK_QIN,					// 45 Color Space Override
	QK_LUMA_WEIGHTS,		// 46
	QK_TOPIC_ADV_END,		// 47
	QK_NUM_PARAMS
};

// Disk IDs. Pre-release these can still change; keep parallel to indices.
// NOTE: reused numbers from the previous layout freely — no shipped comps
// exist yet, so old saved values are not a concern.
enum {
	QK_TOPIC_QUAL_DISK_ID = 101,
	QK_SAMPLE_COLOR_DISK_ID,		// 102
	QK_SAMPLE_ADD_DISK_ID,			// 103
	QK_SAMPLE_SUB_DISK_ID,			// 104
	QK_TOPIC_HUE_DISK_ID,			// 105
	QK_HUE_ENABLE_DISK_ID,			// 106
	QK_HUE_CENTER_DISK_ID,			// 107
	QK_HUE_WIDTH_DISK_ID,			// 108
	QK_HUE_IN_SOFT_DISK_ID,			// 109
	QK_HUE_OUT_SOFT_DISK_ID,		// 110
	QK_TOPIC_HUE_END_DISK_ID,		// 111
	QK_TOPIC_SAT_DISK_ID,			// 112
	QK_SAT_ENABLE_DISK_ID,			// 113
	QK_SAT_IN_DISK_ID,				// 114
	QK_SAT_OUT_DISK_ID,				// 115
	QK_SAT_IN_SOFT_DISK_ID,			// 116
	QK_SAT_OUT_SOFT_DISK_ID,		// 117
	QK_TOPIC_SAT_END_DISK_ID,		// 118
	QK_TOPIC_LUM_DISK_ID,			// 119
	QK_LUM_ENABLE_DISK_ID,			// 120
	QK_LUM_IN_DISK_ID,				// 121
	QK_LUM_OUT_DISK_ID,				// 122
	QK_LUM_IN_SOFT_DISK_ID,			// 123
	QK_LUM_OUT_SOFT_DISK_ID,		// 124
	QK_TOPIC_LUM_END_DISK_ID,		// 125
	QK_TOPIC_QUAL_END_DISK_ID,		// 126
	QK_OUTPUT_DISK_ID,				// 127
	QK_TOPIC_FINESSE_DISK_ID,		// 128
	QK_DENOISE_DISK_ID,				// 129
	QK_CLEAN_BLACK_DISK_ID,			// 130
	QK_CLEAN_WHITE_DISK_ID,			// 131
	QK_BLACK_CLIP_DISK_ID,			// 132
	QK_WHITE_CLIP_DISK_ID,			// 133
	QK_BLUR_DISK_ID,				// 134
	QK_RATIO_DISK_ID,				// 135
	QK_TEMPORAL_DISK_ID,			// 136
	QK_ATTACK_DISK_ID,				// 137
	QK_RELEASE_DISK_ID,				// 138
	QK_TOPIC_FINESSE_END_DISK_ID,	// 139
	QK_TOPIC_GARBAGE_DISK_ID,		// 140
	QK_GARBAGE_LAYER_DISK_ID,		// 141
	QK_GARBAGE_CHAN_DISK_ID,		// 142
	QK_TOPIC_GARBAGE_END_DISK_ID,	// 143
	QK_TOPIC_ADV_DISK_ID,			// 144
	QK_QIN_DISK_ID,					// 145
	QK_LUMA_WEIGHTS_DISK_ID,		// 146
	QK_TOPIC_ADV_END_DISK_ID		// 147
};

// Snapshot of the checked-out param values for one render. Built fresh per
// PreRender/SmartRender call — never stored globally (MFR). Hue is stored as
// Center/Width on the UI; QK_BuildGates converts to the In/Out the gate math
// uses, so the seam-free kernel never sees Center/Width.
typedef struct QK_Params {
	// gates
	A_long		hueEnL, satEnL, lumEnL;
	PF_FpLong	hueCenterF, hueWidthF, hueInSoftF, hueOutSoftF;
	PF_FpLong	satInF, satOutF, satInSoftF, satOutSoftF;
	PF_FpLong	lumInF, lumOutF, lumInSoftF, lumOutSoftF;
	A_long		qinL;			// QK_QIN_*_P (1-based)
	A_long		lumaWL;			// QK_LW_*
	// output
	A_long		outputL;		// QK_OUTPUT_* (1-based)
	// finesse
	PF_FpLong	denoiseF;		// sigma, full-res px
	PF_FpLong	cleanBlackF, cleanWhiteF;	// %
	PF_FpLong	blackClipF, whiteClipF;		// %
	PF_FpLong	blurF;			// sigma, full-res px
	PF_FpLong	ratioF;			// -100..100
	A_long		temporalL;		// 0/1
	PF_FpLong	attackF, releaseF;			// %
	// garbage
	A_long		garbageChanL;	// QK_GCHAN_*_P
} QK_Params;

// Host-side popup resolution -> kernel-space QK_Gates. The ONE place popup
// values become numbers; CPU render, GPU job assembly and the eyedropper all
// go through this so they cannot disagree. Hue Center/Width -> In/Out here.
inline void QK_BuildGates(const QK_Params &p, QK_Gates *g)
{
	g->hueEn = p.hueEnL ? 1.f : 0.f;
	// Center C, total Width W -> forward arc [C - W/2, C + W/2]. W == 360
	// makes In/Out 360 apart, which the gate reads as the full circle.
	g->hueIn  = (float)(p.hueCenterF - p.hueWidthF * 0.5);
	g->hueOut = (float)(p.hueCenterF + p.hueWidthF * 0.5);
	g->hueInSoft = (float)p.hueInSoftF; g->hueOutSoft = (float)p.hueOutSoftF;
	g->satEn = p.satEnL ? 1.f : 0.f;
	g->satIn = (float)p.satInF;         g->satOut = (float)p.satOutF;
	g->satInSoft = (float)p.satInSoftF; g->satOutSoft = (float)p.satOutSoftF;
	g->lumEn = p.lumEnL ? 1.f : 0.f;
	g->lumIn = (float)p.lumInF;         g->lumOut = (float)p.lumOutF;
	g->lumInSoft = (float)p.lumInSoftF; g->lumOutSoft = (float)p.lumOutSoftF;

	switch (p.lumaWL) {
		case QK_LW_2020: g->lumR = 0.2627f;      g->lumG = 0.6780f;      g->lumB = 0.0593f;      break;
		case QK_LW_601:  g->lumR = 0.299f;       g->lumG = 0.587f;       g->lumB = 0.114f;       break;
		case QK_LW_AVG:  g->lumR = 1.f / 3.f;    g->lumG = 1.f / 3.f;    g->lumB = 1.f / 3.f;    break;
		case QK_LW_709:
		default:         g->lumR = 0.2126f;      g->lumG = 0.7152f;      g->lumB = 0.0722f;      break;
	}
	g->model = (float)QK_MODEL_HSL;     // HSL always (no HSV toggle — matches Resolve)
	g->qin   = (float)(p.qinL - 1);     // 1-based popup -> QK_QIN_*
}

extern "C" {
	DllExport PF_Err EffectMain(
		PF_Cmd		cmd,
		PF_InData	*in_data,
		PF_OutData	*out_data,
		PF_ParamDef	*params[],
		PF_LayerDef	*output,
		void		*extra);
}

// smart_render.cpp
PF_Err QK_PreRender(PF_InData *in_data, PF_OutData *out_data, PF_PreRenderExtra *extra);
PF_Err QK_SmartRender(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra);
PF_Err QK_ReadParams(PF_InData *in_data, QK_Params *p);

// smart_render.cpp — GPU path (DX12 host in src/gpu/mm_gpu_dx.cpp;
// unsupported frameworks degrade to the CPU render, never to an error).
PF_Err QK_SmartRenderGPU(PF_InData *in_data, PF_OutData *out_data, PF_SmartRenderExtra *extra);
PF_Err QK_GPUDeviceSetup(PF_InData *in_data, PF_OutData *out_data, PF_GPUDeviceSetupExtra *extra);
PF_Err QK_GPUDeviceSetdown(PF_InData *in_data, PF_OutData *out_data, PF_GPUDeviceSetdownExtra *extra);

// sampling.cpp — eyedropper picking. changedIndex is the swatch that fired
// (QK_SAMPLE_COLOR / QK_SAMPLE_ADD / QK_SAMPLE_SUB); the picked color is read
// from that swatch and the op (set/add/subtract) is chosen from which one it
// was. Returns without changing gates when the color equals the swatch
// default (a reset restoring the default must NOT re-key — the reset->black
// bug). PF_Cmd_USER_CHANGED_PARAM.
enum { QK_PICK_SET = 0, QK_PICK_ADD, QK_PICK_SUB };
PF_Err QK_ApplySample(PF_InData *in_data, PF_OutData *out_data,
                      PF_ParamDef *params[], A_long changedIndex);

#endif // MM_QUALIFY_PARAMS_H
