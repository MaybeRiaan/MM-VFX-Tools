cmake_minimum_required(VERSION 3.20)

project(ModernEdgeBlur)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Suite helpers (version cross-check + shared AE deploy dir). OPTIONAL so this
# project still configures if the folder is lifted out of the suite.
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/MMSuite.cmake" OPTIONAL)

# ---------------------------------------------------------------------------
# This project builds ONE plugin. Until 2026-08-02 it also built Edge Repair
# from this same file, which is why the build logic used to be wrapped in an
# add_meb_plugin() function; Edge Repair now lives in ../EdgeRepair with its
# own CMakeLists, and the two share no code at all. Modern Edge Blur's own
# sources include nothing but their own headers, the AE SDK and the STL — that
# independence is what made the split mechanical, and it is worth preserving.
# (The GPU path added in 2026-08 keeps it: its CUDA host side is a runtime
# nvcuda.dll shim, so it links nothing extra and needs no toolkit to build.)
# ---------------------------------------------------------------------------

# The AE SDK lives OUTSIDE this repo and is never committed (its license
# prohibits redistribution). One shared install serves the whole suite.
set(AE_SDK_ROOT "E:/OFX DEV/AE SDK/AfterEffectsSDK_25.6_61_win/ae25.6_61.64bit.AfterEffectsSDK/Examples"
    CACHE PATH "After Effects SDK Examples directory")

if(NOT EXISTS "${AE_SDK_ROOT}/Headers/AE_Effect.h")
    message(FATAL_ERROR "AE SDK not found at: ${AE_SDK_ROOT} (override with -DAE_SDK_ROOT=...)")
endif()

set(SRC_DIR       "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")

include_directories(
    ${SRC_DIR}
    ${AE_SDK_ROOT}/Headers
    ${AE_SDK_ROOT}/Headers/SP
    ${AE_SDK_ROOT}/Util
    ${RESOURCES_DIR}
)

# SDK support sources compiled into the plugin target.
set(SDK_SOURCES
    ${AE_SDK_ROOT}/Util/AEGP_SuiteHandler.cpp
    ${AE_SDK_ROOT}/Util/MissingSuiteError.cpp
    ${AE_SDK_ROOT}/Util/AEFX_SuiteHelper.c
    ${AE_SDK_ROOT}/Util/Smart_Utils.cpp
)

if(COMMAND mm_ae_plugins_dir)
    mm_ae_plugins_dir()
else()
    # Standalone fallback, used only when this folder is built outside the
    # suite (cmake/MMSuite.cmake absent, so mm_ae_plugins_dir is undefined).
    if(APPLE)
        set(AE_PLUGINS_DIR "/Applications/Adobe After Effects 2026/Plug-ins"
            CACHE PATH "After Effects Plug-ins folder to deploy plugins into")
    else()
        set(AE_PLUGINS_DIR "C:/Program Files/Adobe/Adobe After Effects 2026/Support Files/Plug-ins"
            CACHE PATH "After Effects Plug-ins folder to deploy plugins into")
    endif()
endif()

set(PIPL_R "${RESOURCES_DIR}/ModernEdgeBlurPiPL.r")

# Header and PiPL each carry their own copy of the version defines; AE rejects
# the plugin at load if they disagree. Catch it here instead.
if(COMMAND mm_check_plugin_version)
    mm_check_plugin_version(NAME ModernEdgeBlur
        HEADER "${SRC_DIR}/ModernEdgeBlur.h"
        PIPL   "${PIPL_R}")
endif()

# ===========================================================================
# Windows - the .aex: PiPLTool resource pipeline, DXIL/PTX kernel assets, and
# the deploy into AE's Plug-ins folder. Everything from here to the macOS
# branch at the bottom of this file is Windows-only.
# ===========================================================================
if(WIN32)

set(PIPLTOOL "${AE_SDK_ROOT}/Resources/PiPLtool.exe")
if(NOT EXISTS ${PIPLTOOL})
    message(FATAL_ERROR "PiPLtool.exe not found at: ${PIPLTOOL}")
endif()


# ---------------------------------------------------------------------------
# PiPL resource. Preprocess the .r with cl, run PiPLTool, preprocess again into
# a .rc the resource compiler can take.
#
# PiPLTool.exe splits argv on spaces, and this repo's path contains one - so it
# runs from the build dir with bare filenames, and the space never reaches its
# argv. Do not "simplify" this by passing full paths.
# ---------------------------------------------------------------------------
set(PIPL_RR "${CMAKE_CURRENT_BINARY_DIR}/ModernEdgeBlurPiPL.rr")
set(PIPL_RC "${CMAKE_CURRENT_BINARY_DIR}/ModernEdgeBlurPiPL.rc")

add_custom_command(
    OUTPUT ${PIPL_RR}
    COMMAND ${CMAKE_CXX_COMPILER} /I "${AE_SDK_ROOT}/Headers" /I "${SRC_DIR}"
            /D "MSWindows" /EP "${PIPL_R}" > "${PIPL_RR}"
    DEPENDS ${PIPL_R}
    COMMENT "Preprocessing ModernEdgeBlur PiPL..."
    VERBATIM
)

add_custom_command(
    OUTPUT ${PIPL_RC}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PIPLTOOL}" "${CMAKE_CURRENT_BINARY_DIR}/PiPLTool.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PIPL_RR}" "${CMAKE_CURRENT_BINARY_DIR}/ModernEdgeBlurPiPL.rr"
    COMMAND cmd /c ".\\PiPLTool.exe ModernEdgeBlurPiPL.rr ModernEdgeBlurPiPL.rc.tmp"
    COMMAND ${CMAKE_CXX_COMPILER} /nologo /D "MSWindows" /EP "${CMAKE_CURRENT_BINARY_DIR}/ModernEdgeBlurPiPL.rc.tmp" > "${PIPL_RC}"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${PIPL_RR}
    COMMENT "Compiling ModernEdgeBlur PiPL with PiPLTool..."
    VERBATIM
)

if(MSVC)
    set_source_files_properties(${PIPL_RC} PROPERTIES LANGUAGE RC GENERATED TRUE)
endif()

add_library(ModernEdgeBlur MODULE
    "${SRC_DIR}/ModernEdgeBlur.cpp"
    "${SRC_DIR}/ModernEdgeBlur_Strings.cpp"
    "${SRC_DIR}/ModernEdgeBlur_GPU.cpp"
    ${SDK_SOURCES}
    ${PIPL_RC}
)

set_target_properties(ModernEdgeBlur PROPERTIES
    PREFIX ""
    SUFFIX ".aex"
    OUTPUT_NAME "ModernEdgeBlur"
)

if(WIN32)
    target_compile_definitions(ModernEdgeBlur PRIVATE _WINDOWS WIN32 _USRDLL MSWindows)
    set_target_properties(ModernEdgeBlur PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS")
endif()

# Legacy switch kept from the pre-split build. No source reads it today; it is
# retained so an old -DMODERN_EDGE_BLUR_THREADED=... command line still
# configures rather than erroring.
if(DEFINED MODERN_EDGE_BLUR_THREADED)
    target_compile_definitions(ModernEdgeBlur PRIVATE
        MODERN_EDGE_BLUR_THREADED=${MODERN_EDGE_BLUR_THREADED})
endif()

# Deploy. Non-fatal by design: AE holds a file lock on a loaded .aex, and a
# failed copy must not fail the build - check timestamps before trusting a test.
add_custom_command(TARGET ModernEdgeBlur POST_BUILD
    COMMAND powershell.exe -NonInteractive -Command
        "Copy-Item -Path '$<TARGET_FILE:ModernEdgeBlur>' -Destination '${AE_PLUGINS_DIR}/ModernEdgeBlur.aex' -Force; Write-Host 'Deployed ModernEdgeBlur to ${AE_PLUGINS_DIR}'"
    COMMENT "Installing ModernEdgeBlur to ${AE_PLUGINS_DIR}"
    VERBATIM
)

# ---------------------------------------------------------------------------
# GPU backend - CUDA.
#
# CUDA is the framework After Effects actually offers effects on Windows/NVIDIA
# (measured 2026-07-23) and the one this suite maintains; see
# ../docs/AE_GPU_Effects.md SS1.1-1.2. The HOST side needs NO toolkit - it goes
# through a runtime nvcuda.dll shim (src/gpu/MEB_CudaShim.h) and links nothing
# extra - so ModernEdgeBlur_GPU.cpp is always compiled in. Only the .cu -> PTX
# step below needs nvcc, and when the CUDA toolkit is absent this whole block
# SKIPS gracefully: no PTX ships, device setup declines CUDA at runtime, and AE
# renders on the CPU exactly as it does today.
#
# PTX targets compute_75 (Turing/RTX-class baseline; the test box is an RTX 3070
# Ti, sm_86) - PTX is a text format the driver JIT-compiles forward for newer
# GPUs. -allow-unsupported-compiler: CUDA 12.8 predates VS 2026's MSVC and
# hard-errors on its version check; safe here because nvcc only emits device PTX
# (no host code is kept). The -ccbin / INCLUDE dance below is the same recipe
# EdgeRepair/CMakeLists.txt documents - see there for why each piece is needed.
# ---------------------------------------------------------------------------
file(GLOB NVCC_CANDIDATES
    "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v*/bin/nvcc.exe")
if(NVCC_CANDIDATES)
    list(SORT NVCC_CANDIDATES)
    list(GET NVCC_CANDIDATES -1 NVCC_EXE)
else()
    find_program(NVCC_EXE nvcc)
endif()

if(NVCC_EXE)
    message(STATUS "nvcc: ${NVCC_EXE} - building CUDA_Assets PTX")

    file(GLOB MEB_CUDA_CCBIN_CANDIDATES
        "C:/Program Files/Microsoft Visual Studio/*/*/VC/Tools/MSVC/14.4*/bin/Hostx64/x64/cl.exe")
    set(MEB_NVCC_CCBIN "")
    set(MEB_NVCC_ENV "")
    if(MEB_CUDA_CCBIN_CANDIDATES)
        list(SORT MEB_CUDA_CCBIN_CANDIDATES)
        list(GET MEB_CUDA_CCBIN_CANDIDATES 0 MEB_CUDA_CCBIN)
        message(STATUS "nvcc host compiler (ccbin): ${MEB_CUDA_CCBIN}")
        set(MEB_NVCC_CCBIN -ccbin "${MEB_CUDA_CCBIN}")

        get_filename_component(MEB_CCBIN_DIR "${MEB_CUDA_CCBIN}" DIRECTORY)
        get_filename_component(MEB_MSVC_ROOT "${MEB_CCBIN_DIR}/../../.." ABSOLUTE)
        file(GLOB MEB_WINSDK_INC_CANDIDATES
            "C:/Program Files (x86)/Windows Kits/10/Include/10.*")
        list(SORT MEB_WINSDK_INC_CANDIDATES)
        list(GET MEB_WINSDK_INC_CANDIDATES -1 MEB_WINSDK_INC)
        file(TO_NATIVE_PATH "${MEB_MSVC_ROOT}/include" MEB_INC_MSVC)
        file(TO_NATIVE_PATH "${MEB_WINSDK_INC}/ucrt"   MEB_INC_UCRT)
        file(TO_NATIVE_PATH "${MEB_WINSDK_INC}/shared" MEB_INC_SHARED)
        file(TO_NATIVE_PATH "${MEB_WINSDK_INC}/um"     MEB_INC_UM)
        file(TO_NATIVE_PATH "${MEB_MSVC_ROOT}" MEB_MSVC_ROOT_NATIVE)
        file(TO_NATIVE_PATH "${MEB_CCBIN_DIR}" MEB_CCBIN_DIR_NATIVE)
        set(MEB_NVCC_ENV ${CMAKE_COMMAND} -E env
            --modify "PATH=path_list_prepend:${MEB_CCBIN_DIR_NATIVE}"
            "INCLUDE=${MEB_INC_MSVC}\;${MEB_INC_UCRT}\;${MEB_INC_SHARED}\;${MEB_INC_UM}"
            "VCToolsInstallDir=${MEB_MSVC_ROOT_NATIVE}\\")
    endif()

    set(MEB_CUDA_ASSETS "${CMAKE_CURRENT_BINARY_DIR}/CUDA_Assets")
    add_custom_command(
        OUTPUT "${MEB_CUDA_ASSETS}/ModernEdgeBlur_Kernels.ptx"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MEB_CUDA_ASSETS}"
        COMMAND ${MEB_NVCC_ENV} "${NVCC_EXE}" -ptx -arch=compute_75
                --use-local-env
                -allow-unsupported-compiler ${MEB_NVCC_CCBIN} -o
                "${MEB_CUDA_ASSETS}/ModernEdgeBlur_Kernels.ptx"
                "${SRC_DIR}/gpu/ModernEdgeBlurKernels.cu"
        DEPENDS "${SRC_DIR}/gpu/ModernEdgeBlurKernels.cu"
                "${SRC_DIR}/gpu/MEB_KernelPasses.h"
                "${SRC_DIR}/gpu/MEB_KernelMath.h"
        COMMENT "nvcc: ModernEdgeBlurKernels.cu -> CUDA_Assets/ModernEdgeBlur_Kernels.ptx"
        VERBATIM
    )
    add_custom_target(ModernEdgeBlurCudaPTX
        DEPENDS "${MEB_CUDA_ASSETS}/ModernEdgeBlur_Kernels.ptx")
    add_dependencies(ModernEdgeBlur ModernEdgeBlurCudaPTX)

    # Kernel assets must sit NEXT TO the .aex (AE_GPU_Effects.md SS2.5).
    # copy_directory MERGES, so this coexists with EdgeRepair's own
    # CUDA_Assets deploy into the same folder - the two PTX files have
    # different names by design.
    add_custom_command(TARGET ModernEdgeBlur POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${MEB_CUDA_ASSETS}" "${AE_PLUGINS_DIR}/CUDA_Assets"
        COMMENT "Installing ModernEdgeBlur CUDA_Assets next to the deployed .aex"
        VERBATIM
    )
else()
    message(STATUS "nvcc not found - CUDA PTX not built (CUDA declines at runtime; CPU only)")
endif()

# ---------------------------------------------------------------------------
# HLSL validation pass. There is NO DirectX 12 host for this plugin and the
# .cso produced here is deliberately NOT deployed - nothing loads it.
#
# It is compiled anyway because fxc type-checks the WHOLE translation unit, so
# one compiled entry point validates the entire shared kernel math header
# against HLSL's rules on every build (../docs/AE_GPU_Effects.md SS4 item 5).
# That has already caught, in EdgeRepair, a construct CUDA and C++ both accept
# and HLSL rejects. It is a compile-time assertion, not an asset - which is
# also why a failure here is a real failure worth stopping the build for.
# ---------------------------------------------------------------------------
file(GLOB FXC_CANDIDATES
    "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/fxc.exe")
if(FXC_CANDIDATES)
    list(SORT FXC_CANDIDATES)
    list(GET FXC_CANDIDATES -1 FXC_EXE)
else()
    find_program(FXC_EXE fxc)
endif()

if(FXC_EXE)
    message(STATUS "fxc: ${FXC_EXE} - validating the shared kernel math against HLSL")
    set(MEB_HLSL "${SRC_DIR}/gpu/ModernEdgeBlurKernels.hlsl")
    set(MEB_HLSL_CHECK "${CMAKE_CURRENT_BINARY_DIR}/hlsl_check")
    add_custom_command(
        OUTPUT "${MEB_HLSL_CHECK}/ModernEdgeBlur_Passthrough.cso"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MEB_HLSL_CHECK}"
        COMMAND "${FXC_EXE}" -nologo -T cs_5_1 -E MEB_Passthrough
                -I "${SRC_DIR}/gpu"
                -Fo "${MEB_HLSL_CHECK}/ModernEdgeBlur_Passthrough.cso" "${MEB_HLSL}"
        COMMAND "${FXC_EXE}" -nologo -T rootsig_1_0 -E MEB_ROOT_SIGNATURE
                -I "${SRC_DIR}/gpu"
                -Fo "${MEB_HLSL_CHECK}/ModernEdgeBlur_Passthrough.rs" "${MEB_HLSL}"
        DEPENDS "${MEB_HLSL}" "${SRC_DIR}/gpu/MEB_KernelMath.h"
        COMMENT "fxc: type-checking MEB_KernelMath.h against HLSL"
        VERBATIM
    )
    add_custom_target(ModernEdgeBlurHLSLCheck
        DEPENDS "${MEB_HLSL_CHECK}/ModernEdgeBlur_Passthrough.cso")
    add_dependencies(ModernEdgeBlur ModernEdgeBlurHLSLCheck)
else()
    message(STATUS "fxc not found - skipping the HLSL type-check of the shared kernel math")
endif()

message(STATUS "ModernEdgeBlur.aex -> ${AE_PLUGINS_DIR}")


# ===========================================================================
# macOS - the .plugin bundle: a Rez-compiled PiPL in Resources, universal
# (x86_64 + arm64), matching the SDK's own Xcode sample layout. Rez ships with
# the Xcode command-line tools.
#
# CPU PATH ONLY, and that is a complete plugin rather than a degraded one: the
# CPU pipeline is the reference both platforms are validated against. There is
# no Metal host in this suite yet (../docs/gpu-kernel-rules.md SS4), so
# ModernEdgeBlur_GPU.cpp compiles to its decline stubs here, AE is never offered a
# device, and every frame takes the reference path.
#
# NOTE: cannot be compiled or verified on the Windows dev machine.
# ===========================================================================
elseif(APPLE)
    find_program(REZ_TOOL Rez REQUIRED)

    add_library(ModernEdgeBlur MODULE
        "${SRC_DIR}/ModernEdgeBlur.cpp"
        "${SRC_DIR}/ModernEdgeBlur_Strings.cpp"
        "${SRC_DIR}/ModernEdgeBlur_GPU.cpp"
        ${SDK_SOURCES}
    )

    set_target_properties(ModernEdgeBlur PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "plugin"
        OUTPUT_NAME "ModernEdgeBlur"
        MACOSX_BUNDLE_INFO_PLIST "${RESOURCES_DIR}/Info.plist"
        OSX_ARCHITECTURES "x86_64;arm64")

    target_compile_definitions(ModernEdgeBlur PRIVATE macintosh)

    # The PiPL goes into the bundle's Resources as a Rez-compiled .rsrc. AE
    # reads it to find the effect's name, category and entry point; without it
    # the bundle loads and does nothing.
    add_custom_command(TARGET ModernEdgeBlur POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_CONTENT_DIR:ModernEdgeBlur>/Resources"
        COMMAND ${REZ_TOOL}
            -useDF
            -i "${AE_SDK_ROOT}/Headers"
            -i "${AE_SDK_ROOT}/Resources"
            -arch x86_64
            -o "$<TARGET_BUNDLE_CONTENT_DIR:ModernEdgeBlur>/Resources/ModernEdgeBlur.rsrc"
            "${PIPL_R}"
        COMMENT "Compiling ModernEdgeBlur PiPL with Rez"
        VERBATIM)

    # Deploy. Pass -DAE_PLUGINS_DIR=... if AE is not at the default location.
    add_custom_command(TARGET ModernEdgeBlur POST_BUILD
        COMMAND sh -c "if [ -w \"${AE_PLUGINS_DIR}\" ]; then rm -rf \"${AE_PLUGINS_DIR}/ModernEdgeBlur.plugin\" && cp -R \"$<TARGET_BUNDLE_DIR:ModernEdgeBlur>\" \"${AE_PLUGINS_DIR}/ModernEdgeBlur.plugin\" && echo 'Deployed ModernEdgeBlur -> ${AE_PLUGINS_DIR}'; else echo 'AE plugins folder missing or not writable; skipping deploy (pass -DAE_PLUGINS_DIR=<writable dir>, or copy by hand)'; fi"
        COMMENT "Installing ModernEdgeBlur.plugin -> ${AE_PLUGINS_DIR}"
        VERBATIM)

    message(STATUS "ModernEdgeBlur.plugin (macOS bundle, CPU path) -> ${AE_PLUGINS_DIR}")

else()
    message(FATAL_ERROR "ModernEdgeBlur targets Windows and macOS only.")
endif()
