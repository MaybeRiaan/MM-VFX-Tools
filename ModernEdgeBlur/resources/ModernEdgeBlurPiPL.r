#include "AEConfig.h"
#include "AE_EffectVers.h"

// Version information from ModernEdgeBlur.h — keep STAGE_VERSION in sync.
#define	MAJOR_VERSION	1
#define	MINOR_VERSION	2
#define	BUG_VERSION		0
#define	STAGE_VERSION	0	// PF_Stage_DEVELOP (enum value 0) — must match ModernEdgeBlur.h
#define	BUILD_VERSION	10

// Portable pattern from the SDK's own SmartyPantsPiPL.r: on Mac (AE_OS_WIN
// undefined) pull in AE_General.r for Rez; on Windows the cl /EP + PiPLTool
// path supplies the definitions and this include is skipped. Do NOT force-
// define AE_OS_WIN above — that would break the Mac Rez build.
#ifndef AE_OS_WIN
	#include "AE_General.r"
#endif
	
resource 'PiPL' (16000) {
	{	/* array properties: 12 elements */
		/* [1] */
		Kind {
			AEEffect
		},
		/* [2] */
		Name {
			"Modern Edge Blur"
		},
		/* [3] */
		Category {
			"MM"
		},
#ifdef AE_OS_WIN
    #if defined(AE_PROC_INTELx64)
		CodeWin64X86 {"EffectMain"},
    #elif defined(AE_PROC_ARM64)
		CodeWinARM64 {"EffectMain"},
    #endif
#elif defined(AE_OS_MAC)
		CodeMacIntel64 {"EffectMain"},
		CodeMacARM64 {"EffectMain"},
#endif
		/* [6] */
		AE_PiPL_Version {
			2,
			0
		},
		/* [7] */
		AE_Effect_Spec_Version {
			PF_PLUG_IN_VERSION,
			PF_PLUG_IN_SUBVERS
		},
		/* [8] */
		AE_Effect_Version {
			((MAJOR_VERSION * 524288) + (MINOR_VERSION * 32768) + (BUG_VERSION * 2048) + (STAGE_VERSION * 512) + BUILD_VERSION)
		},
		/* [9] */
		AE_Effect_Info_Flags {
			0
		},
		/* [10] */
		// MUST match what GlobalSetup() sets at runtime, or AE rejects with
		// "global outflags mismatch". See ModernEdgeBlur.cpp GlobalSetup.
		// out_flags  = DEEP_COLOR_AWARE | PIX_INDEPENDENT | USE_OUTPUT_EXTENT
		//            = 0x02000000 | 0x400 | 0x40 = 0x02000440
		// (USE_OUTPUT_EXTENT is deliberately kept even though Adobe's AE GPU
		//  sample omits it — ../../docs/AE_GPU_Effects.md §8 records that it was
		//  tested and is irrelevant to the GPU gate, and it is part of how this
		//  effect's render region already behaves.)
		AE_Effect_Global_OutFlags {
		0x02000440
		},
		// out_flags2 = FLOAT_COLOR_AWARE | SUPPORTS_SMART_RENDER
		//            | SUPPORTS_THREADED_RENDERING | SUPPORTS_GPU_RENDER_F32
		//            | SUPPORTS_DIRECTX_RENDERING
		//            = 0x1000 | 0x400 | 0x08000000 | 0x02000000 | 0x20000000
		//            = 0x2A001400
		// The PiPL declares the SUPERSET: GlobalSetup only ORs the two GPU bits
		// in for non-Premiere hosts, and AE tolerates the PiPL being wider than
		// the runtime flags. It does NOT tolerate the reverse — any narrower and
		// AE rejects the plugin at load with "global outflags mismatch" (25::16).
		AE_Effect_Global_OutFlags_2 {
		0x2A001400
		},
		/* [11] */
		AE_Effect_Match_Name {
			"MM Modern Edge Blur"
		},
		/* [12] */
		AE_Reserved_Info {
			0
		},
		/* [13] */
		AE_Effect_Support_URL {
			""
		}
	}
};
