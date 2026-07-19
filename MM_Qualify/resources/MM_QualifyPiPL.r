#include "AEConfig.h"
#include "AE_EffectVers.h"

// Version information from src/params.h — keep STAGE_VERSION in sync.
#define	MAJOR_VERSION	1
#define	MINOR_VERSION	0
#define	BUG_VERSION		0
#define	STAGE_VERSION	0	// PF_Stage_DEVELOP (enum value 0) — must match params.h
#define	BUILD_VERSION	1

// Portable pattern from the SDK's SmartyPantsPiPL.r: on Mac (AE_OS_WIN
// undefined) pull in AE_General.r for Rez; on Windows the cl /EP + PiPLTool
// path supplies the definitions and this include is skipped. Do NOT force-
// define AE_OS_WIN here — that would break the Mac Rez build.
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
			"Qualify"
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
		// "global outflags mismatch". See src/main.cpp GlobalSetup.
		// out_flags  = WIDE_TIME_INPUT | USE_OUTPUT_EXTENT | PIX_INDEPENDENT
		//              | DEEP_COLOR_AWARE | SEND_UPDATE_PARAMS_UI
		//            = 0x2 | 0x40 | 0x400 | 0x02000000 | 0x04000000 = 0x06000442
		AE_Effect_Global_OutFlags {
		0x06000442
		},
		// out_flags2 = SUPPORTS_SMART_RENDER | FLOAT_COLOR_AWARE
		//              | SUPPORTS_GPU_RENDER_F32 | SUPPORTS_THREADED_RENDERING
		//              | SUPPORTS_DIRECTX_RENDERING
		//            = 0x400 | 0x1000 | 0x02000000 | 0x08000000 | 0x20000000 = 0x2A001400
		AE_Effect_Global_OutFlags_2 {
		0x2A001400
		},
		/* [11] */
		AE_Effect_Match_Name {
			"MM Qualify"
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
