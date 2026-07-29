#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
	#define AE_OS_WIN
#endif

// Version information from EdgeRepair.h — keep STAGE_VERSION in sync.
#define	MAJOR_VERSION	1
#define	MINOR_VERSION	0
#define	BUG_VERSION		0
#define	STAGE_VERSION	0	// PF_Stage_DEVELOP (enum value 0) — must match EdgeRepair.h
#define	BUILD_VERSION	31

#ifndef AE_OS_WIN
	#include <AE_General.r>
#endif

resource 'PiPL' (16000) {
	{	/* array properties: 12 elements */
		/* [1] */
		Kind {
			AEEffect
		},
		/* [2] */
		Name {
			"Edge Repair"
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
		// "global outflags mismatch". See EdgeRepair.cpp GlobalSetup.
		// out_flags  = DEEP_COLOR_AWARE | PIX_INDEPENDENT
		//            | USE_OUTPUT_EXTENT
		//            = 0x02000000 | 0x400 | 0x40 = 0x02000440
		// (USE_OUTPUT_EXTENT RESTORED 2026-07-29. Removing it on 2026-07-24 was
		//  inferred from a sweep of shipping binaries; Adobe's own
		//  GPUVideoFilter samples both SET it — SDK_ProcAmp uses 0x40 alone,
		//  Vignette 0x440 and its header says it runs in AE and Premiere.
		//  Samples beat inference from stripped third-party binaries.)
		AE_Effect_Global_OutFlags {
		0x02000440
		},
		// out_flags2 = FLOAT_COLOR_AWARE | SUPPORTS_SMART_RENDER
		//            | SUPPORTS_THREADED_RENDERING | SUPPORTS_GPU_RENDER_F32
		//            = 0x1000 | 0x400 | 0x08000000 | 0x02000000 = 0x0A001400
		// (SUPPORTS_QUERY_DYNAMIC_FLAGS (bit 0) was added 2026-07-24 as GPU-gate
		//  experiment 2 and REMOVED 2026-07-29: it never opened the gate and it
		//  caused an unsuppressable AE error 25::228 on every apply. See the
		//  GlobalSetup note in EdgeRepair.cpp before ever re-adding it.)
		// (No SUPPORTS_DIRECTX_RENDERING: it made AE-on-Windows/CUDA offer
		//  what_gpu=NONE at PreRender — see GlobalSetup note in EdgeRepair.cpp.)
		AE_Effect_Global_OutFlags_2 {
		0x0A001400
		},
		/* [11] */
		AE_Effect_Match_Name {
			"MM Edge Repair"
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
