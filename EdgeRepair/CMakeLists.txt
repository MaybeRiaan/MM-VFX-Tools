cmake_minimum_required(VERSION 3.20)

project(EdgeRepair)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Suite helpers (version cross-check + shared AE deploy dir). OPTIONAL so this
# project still configures if the folder is lifted out of the suite.
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/MMSuite.cmake" OPTIONAL)

# ---------------------------------------------------------------------------
# Edge Repair - edge extension / decontamination, with a CUDA GPU path.
#
# Split out of the ModernEdgeBlur project on 2026-08-02. The two plugins had
# shared one CMake project since Edge Repair was started, but never shared any
# code: src/common/ was included only by EdgeRepair.cpp, and Modern Edge Blur's
# sources pull in nothing but their own header and the STL. Everything the old
# combined file did for Edge Repair - PiPL, common sources, fxc shader assets,
# nvcc PTX, the optional Premiere module - lives here now, unchanged.
#
# GPU: CUDA is the backend After Effects actually offers on Windows/NVIDIA
# (measured 2026-07-23) and the one this suite maintains. DX12 is retained as
# the Premiere-host path. See ../docs/AE_GPU_Effects.md SS1.1-1.2.
# ---------------------------------------------------------------------------

# The AE SDK lives OUTSIDE this repo and is never committed (its license
# prohibits redistribution). One shared install serves the whole suite.
set(AE_SDK_ROOT "E:/OFX DEV/AE SDK/AfterEffectsSDK_25.6_61_win/ae25.6_61.64bit.AfterEffectsSDK/Examples"
    CACHE PATH "After Effects SDK Examples directory")

if(NOT EXISTS "${AE_SDK_ROOT}/Headers/AE_Effect.h")
    message(FATAL_ERROR "AE SDK not found at: ${AE_SDK_ROOT} (override with -DAE_SDK_ROOT=...)")
endif()

# The PREMIERE SDK is a separate download with no shared install, needed only
# for the optional Premiere GPUVideoFilter module below. Stays vendored under
# sdk/Premiere/ (gitignored, like any SDK).
set(PREMIERE_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/sdk/Premiere"
    CACHE PATH "Premiere Pro SDK root (only for EDGEREPAIR_PREMIERE_GPU=ON)")

set(SRC_DIR       "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")

include_directories(
    ${SRC_DIR}
    ${AE_SDK_ROOT}/Headers
    ${AE_SDK_ROOT}/Headers/SP
    ${AE_SDK_ROOT}/Util
    ${RESOURCES_DIR}
)

set(SDK_SOURCES
    ${AE_SDK_ROOT}/Util/AEGP_SuiteHandler.cpp
    ${AE_SDK_ROOT}/Util/MissingSuiteError.cpp
    ${AE_SDK_ROOT}/Util/AEFX_SuiteHelper.c
    ${AE_SDK_ROOT}/Util/Smart_Utils.cpp
)

set(PIPLTOOL "${AE_SDK_ROOT}/Resources/PiPLtool.exe")
if(NOT EXISTS ${PIPLTOOL})
    message(FATAL_ERROR "PiPLtool.exe not found at: ${PIPLTOOL}")
endif()

if(COMMAND mm_ae_plugins_dir)
    mm_ae_plugins_dir()
else()
    set(AE_PLUGINS_DIR "C:/Program Files/Adobe/Adobe After Effects 2026/Support Files/Plug-ins"
        CACHE PATH "After Effects Plug-ins folder to deploy the .aex into")
endif()

# 2026-07-30: AE's own Plug-ins folder is correct. EdgeRepair was briefly
# installed to Common/Plug-ins/7.0/MediaCore to chase the GPUVideoFilter
# mechanism; that turned out to be the PREMIERE GPU path. AE drives GPU through
# the documented AE-SDK F32 path (GPU_DEVICE_SETUP + SMART_RENDER_GPU), verified
# by building Adobe's own SDK_Invert_ProcAmp, which loads from AE's Plug-ins
# folder and GPU-renders on CUDA. Kernel assets must sit NEXT TO the .aex.
set(MEDIACORE_PLUGINS_DIR "C:/Program Files/Adobe/Common/Plug-ins/7.0/MediaCore")

set(PIPL_R "${RESOURCES_DIR}/EdgeRepairPiPL.r")

# Header and PiPL each carry their own copy of the version defines; AE rejects
# the plugin at load if they disagree. Catch it here instead.
if(COMMAND mm_check_plugin_version)
    mm_check_plugin_version(NAME EdgeRepair
        HEADER "${SRC_DIR}/EdgeRepair.h"
        PIPL   "${PIPL_R}")
endif()

# ---------------------------------------------------------------------------
# OPTIONAL: Premiere Pro GPU module (src/gpu/ER_GPUFilter.cpp, exports
# xGPUFilterEntry - a MediaCore GPUVideoFilter).
#
# NOT needed for After Effects, and inert when installed in AE's Plug-ins
# folder. Kept because it is written and working-ish, for if Premiere support is
# ever wanted. It compiles against the PREMIERE PRO SDK headers, which are NOT
# redistributable: drop Examples/Headers into sdk/Premiere/Headers and
# Examples/Projects/GPUVideoFilter/Utils/PrGPUFilterModule.h into
# sdk/Premiere/Utils. Only this one source file may see those headers - never
# mix them into AE-SDK translation units.
#
# Default OFF: an AE-only build must not need the Premiere SDK at all, and a
# missing Premiere SDK must not block a build on another machine.
# ---------------------------------------------------------------------------
option(EDGEREPAIR_PREMIERE_GPU
    "Build the Premiere GPUVideoFilter module (not needed for After Effects)"
    OFF)

set(ER_GPU_SOURCES "")
if(EDGEREPAIR_PREMIERE_GPU)
    if(NOT EXISTS "${PREMIERE_SDK_ROOT}/Headers/PrSDKGPUFilter.h" OR
       NOT EXISTS "${PREMIERE_SDK_ROOT}/Utils/PrGPUFilterModule.h")
        message(FATAL_ERROR "EDGEREPAIR_PREMIERE_GPU=ON but the Premiere Pro SDK "
            "headers are missing under ${PREMIERE_SDK_ROOT} (need Headers/* and "
            "Utils/PrGPUFilterModule.h). Drop a copy there or pass "
            "-DPREMIERE_SDK_ROOT=... - see ../docs/AE_GPU_Effects.md SS6")
    endif()
    list(APPEND ER_GPU_SOURCES "${SRC_DIR}/gpu/ER_GPUFilter.cpp")
    message(STATUS "EdgeRepair: building Premiere GPUVideoFilter module (AE does not use it)")
else()
    message(STATUS "EdgeRepair: Premiere GPUVideoFilter module SKIPPED (AE-only build)")
endif()

# ---------------------------------------------------------------------------
# PiPL resource. PiPLTool.exe splits argv on spaces, and this repo's path
# contains one - so it runs from the build dir with bare filenames, and the
# space never reaches its argv. Do not "simplify" this by passing full paths.
# ---------------------------------------------------------------------------
set(PIPL_RR "${CMAKE_CURRENT_BINARY_DIR}/EdgeRepairPiPL.rr")
set(PIPL_RC "${CMAKE_CURRENT_BINARY_DIR}/EdgeRepairPiPL.rc")

add_custom_command(
    OUTPUT ${PIPL_RR}
    COMMAND ${CMAKE_CXX_COMPILER} /I "${AE_SDK_ROOT}/Headers" /I "${SRC_DIR}"
            /D "MSWindows" /EP "${PIPL_R}" > "${PIPL_RR}"
    DEPENDS ${PIPL_R}
    COMMENT "Preprocessing EdgeRepair PiPL..."
    VERBATIM
)

add_custom_command(
    OUTPUT ${PIPL_RC}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PIPLTOOL}" "${CMAKE_CURRENT_BINARY_DIR}/PiPLTool.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PIPL_RR}" "${CMAKE_CURRENT_BINARY_DIR}/EdgeRepairPiPL.rr"
    COMMAND cmd /c ".\\PiPLTool.exe EdgeRepairPiPL.rr EdgeRepairPiPL.rc.tmp"
    COMMAND ${CMAKE_CXX_COMPILER} /nologo /D "MSWindows" /EP "${CMAKE_CURRENT_BINARY_DIR}/EdgeRepairPiPL.rc.tmp" > "${PIPL_RC}"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${PIPL_RR}
    COMMENT "Compiling EdgeRepair PiPL with PiPLTool..."
    VERBATIM
)

if(MSVC)
    set_source_files_properties(${PIPL_RC} PROPERTIES LANGUAGE RC GENERATED TRUE)
endif()

add_library(EdgeRepair MODULE
    "${SRC_DIR}/EdgeRepair.cpp"
    "${SRC_DIR}/EdgeRepair_Strings.cpp"
    "${SRC_DIR}/EdgeRepair_GPU.cpp"
    "${SRC_DIR}/common/MM_Common.cpp"
    ${ER_GPU_SOURCES}
    "${AE_SDK_ROOT}/Util/DirectXUtils.cpp"
    ${SDK_SOURCES}
    ${PIPL_RC}
)

set_target_properties(EdgeRepair PROPERTIES
    PREFIX ""
    SUFFIX ".aex"
    OUTPUT_NAME "EdgeRepair"
)

if(WIN32)
    target_compile_definitions(EdgeRepair PRIVATE _WINDOWS WIN32 _USRDLL MSWindows)
    set_target_properties(EdgeRepair PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS")
endif()

if(EDGEREPAIR_PREMIERE_GPU)
    set_source_files_properties("${SRC_DIR}/gpu/ER_GPUFilter.cpp" PROPERTIES
        INCLUDE_DIRECTORIES "${PREMIERE_SDK_ROOT}/Headers;${PREMIERE_SDK_ROOT}/Utils"
        COMPILE_DEFINITIONS "PRWIN_ENV")
endif()

# Deploy. Non-fatal by design: AE holds a file lock on a loaded .aex, and a
# failed copy must not fail the build - check timestamps before trusting a test.
add_custom_command(TARGET EdgeRepair POST_BUILD
    COMMAND powershell.exe -NonInteractive -Command
        "Copy-Item -Path '$<TARGET_FILE:EdgeRepair>' -Destination '${AE_PLUGINS_DIR}/EdgeRepair.aex' -Force; Write-Host 'Deployed EdgeRepair to ${AE_PLUGINS_DIR}'"
    COMMENT "Installing EdgeRepair to ${AE_PLUGINS_DIR}"
    VERBATIM
)

# ---------------------------------------------------------------------------
# DirectX 12 backend (Premiere-host path; see the GPU note at the top).
# EdgeRepair_GPU.cpp + Adobe's DirectXUtils need d3d12/d3dcompiler; the kernels
# are precompiled at build time by fxc (Windows SDK) into
# DirectX_Assets/<kernel>.cso (compute shader) + .rs (root signature), the exact
# layout DirectXUtils.cpp::GetShaderPath expects NEXT TO the .aex.
#
# Worth keeping even though CUDA is the maintained backend: fxc type-checks the
# WHOLE translation unit, so one compiled entry point validates the entire
# shared kernel header against HLSL's rules on every build. That has already
# caught a construct CUDA and C++ both accept and HLSL rejects.
# See ../docs/AE_GPU_Effects.md SS4.
# ---------------------------------------------------------------------------
target_link_libraries(EdgeRepair d3d12 d3dcompiler)

# fxc lives in the Windows SDK bin, which is not on PATH for Ninja builds -
# glob the installed SDK versions and take the newest.
file(GLOB FXC_CANDIDATES
    "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/fxc.exe")
if(FXC_CANDIDATES)
    list(SORT FXC_CANDIDATES)
    list(GET FXC_CANDIDATES -1 FXC_EXE)
else()
    find_program(FXC_EXE fxc REQUIRED)
endif()
message(STATUS "fxc: ${FXC_EXE}")

set(ER_HLSL "${SRC_DIR}/gpu/EdgeRepairKernels.hlsl")
set(ER_DX_ASSETS "${CMAKE_CURRENT_BINARY_DIR}/DirectX_Assets")

# One .cso/.rs pair per kernel entry point. Append new entry names here as the
# kernel port lands (all share ER_ROOT_SIGNATURE for now).
set(ER_KERNEL_ENTRIES ER_Passthrough)
set(ER_KERNEL_OUTPUTS "")
foreach(ENTRY ${ER_KERNEL_ENTRIES})
    string(REGEX REPLACE "^ER_" "EdgeRepair_" ASSET ${ENTRY})
    add_custom_command(
        OUTPUT "${ER_DX_ASSETS}/${ASSET}.cso" "${ER_DX_ASSETS}/${ASSET}.rs"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ER_DX_ASSETS}"
        COMMAND "${FXC_EXE}" -nologo -T cs_5_1 -E ${ENTRY}
                -Fo "${ER_DX_ASSETS}/${ASSET}.cso" "${ER_HLSL}"
        COMMAND "${FXC_EXE}" -nologo -T rootsig_1_0 -E ER_ROOT_SIGNATURE
                -Fo "${ER_DX_ASSETS}/${ASSET}.rs" "${ER_HLSL}"
        DEPENDS "${ER_HLSL}" "${SRC_DIR}/gpu/ER_KernelMath.h"
        COMMENT "fxc: ${ENTRY} -> DirectX_Assets/${ASSET}.cso/.rs"
        VERBATIM
    )
    list(APPEND ER_KERNEL_OUTPUTS
        "${ER_DX_ASSETS}/${ASSET}.cso" "${ER_DX_ASSETS}/${ASSET}.rs")
endforeach()

add_custom_target(EdgeRepairShaders DEPENDS ${ER_KERNEL_OUTPUTS})
add_dependencies(EdgeRepair EdgeRepairShaders)

add_custom_command(TARGET EdgeRepair POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${ER_DX_ASSETS}" "${AE_PLUGINS_DIR}/DirectX_Assets"
    COMMENT "Installing EdgeRepair DirectX_Assets next to the deployed .aex"
    VERBATIM
)

# ---------------------------------------------------------------------------
# CUDA backend - the framework AE-on-Windows actually offers effects (measured
# 2026-07-23) and the one this suite maintains. The host side needs NO toolkit
# (runtime nvcuda.dll shim in src/gpu/ER_CudaShim.h); only the .cu -> PTX step
# needs nvcc. When the CUDA toolkit is absent this whole block SKIPS gracefully:
# no PTX ships, device setup declines CUDA at runtime, AE falls back to CPU.
#
# PTX targets compute_75 (Turing/RTX-class baseline; the test box is an RTX 3070
# Ti, sm_86) - PTX is a text format the driver JIT-compiles forward for newer
# GPUs. -allow-unsupported-compiler: CUDA 12.8 predates VS 2026's MSVC and
# hard-errors on its version check; safe here because nvcc only emits device PTX
# (no host code is kept).
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

    # CUDA 12.8's nvcc cannot digest VS 2026's newest STL (hard static_assert:
    # "expected CUDA 13.2 or newer", plus EDG chokes on its C++23 patterns).
    # VS 2026 installs older MSVC toolsets side-by-side - hand nvcc the OLDEST
    # 14.4x (VS 2022-era v143) via -ccbin. If none exists the flag is simply
    # omitted and nvcc uses the default cl (may fail - install the "MSVC v143"
    # component or CUDA 13.2+ in that case).
    file(GLOB ER_CUDA_CCBIN_CANDIDATES
        "C:/Program Files/Microsoft Visual Studio/*/*/VC/Tools/MSVC/14.4*/bin/Hostx64/x64/cl.exe")
    set(ER_NVCC_CCBIN "")
    set(ER_NVCC_ENV "")
    if(ER_CUDA_CCBIN_CANDIDATES)
        list(SORT ER_CUDA_CCBIN_CANDIDATES)
        list(GET ER_CUDA_CCBIN_CANDIDATES 0 ER_CUDA_CCBIN)
        message(STATUS "nvcc host compiler (ccbin): ${ER_CUDA_CCBIN}")
        set(ER_NVCC_CCBIN -ccbin "${ER_CUDA_CCBIN}")

        # ccbin alone is not enough: the build environment's INCLUDE (set by
        # vcvars for the NEWEST toolset) would still hand the old cl the new
        # STL. Rebuild INCLUDE for the nvcc step from the chosen toolset's own
        # headers + the newest installed Windows SDK.
        get_filename_component(ER_CCBIN_DIR "${ER_CUDA_CCBIN}" DIRECTORY)
        get_filename_component(ER_MSVC_ROOT "${ER_CCBIN_DIR}/../../.." ABSOLUTE)
        file(GLOB ER_WINSDK_INC_CANDIDATES
            "C:/Program Files (x86)/Windows Kits/10/Include/10.*")
        list(SORT ER_WINSDK_INC_CANDIDATES)
        list(GET ER_WINSDK_INC_CANDIDATES -1 ER_WINSDK_INC)
        file(TO_NATIVE_PATH "${ER_MSVC_ROOT}/include" ER_INC_MSVC)
        file(TO_NATIVE_PATH "${ER_WINSDK_INC}/ucrt"   ER_INC_UCRT)
        file(TO_NATIVE_PATH "${ER_WINSDK_INC}/shared" ER_INC_SHARED)
        file(TO_NATIVE_PATH "${ER_WINSDK_INC}/um"     ER_INC_UM)
        file(TO_NATIVE_PATH "${ER_MSVC_ROOT}" ER_MSVC_ROOT_NATIVE)
        file(TO_NATIVE_PATH "${ER_CCBIN_DIR}" ER_CCBIN_DIR_NATIVE)
        # nvcc's own VS detection always lands on the NEWEST toolset, so give it
        # a fully consistent 14.4x environment instead: --use-local-env makes it
        # trust INCLUDE; VCToolsInstallDir covers its VS probing; the PATH
        # prepend satisfies its cl-in-PATH == ccbin consistency check.
        set(ER_NVCC_ENV ${CMAKE_COMMAND} -E env
            --modify "PATH=path_list_prepend:${ER_CCBIN_DIR_NATIVE}"
            "INCLUDE=${ER_INC_MSVC}\;${ER_INC_UCRT}\;${ER_INC_SHARED}\;${ER_INC_UM}"
            "VCToolsInstallDir=${ER_MSVC_ROOT_NATIVE}\\")
    endif()

    set(ER_CUDA_ASSETS "${CMAKE_CURRENT_BINARY_DIR}/CUDA_Assets")
    add_custom_command(
        OUTPUT "${ER_CUDA_ASSETS}/EdgeRepair_Kernels.ptx"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ER_CUDA_ASSETS}"
        COMMAND ${ER_NVCC_ENV} "${NVCC_EXE}" -ptx -arch=compute_75
                --use-local-env
                -allow-unsupported-compiler ${ER_NVCC_CCBIN} -o
                "${ER_CUDA_ASSETS}/EdgeRepair_Kernels.ptx"
                "${SRC_DIR}/gpu/EdgeRepairKernels.cu"
        DEPENDS "${SRC_DIR}/gpu/EdgeRepairKernels.cu"
                "${SRC_DIR}/gpu/ER_KernelMath.h"
        COMMENT "nvcc: EdgeRepairKernels.cu -> CUDA_Assets/EdgeRepair_Kernels.ptx"
        VERBATIM
    )
    add_custom_target(EdgeRepairCudaPTX
        DEPENDS "${ER_CUDA_ASSETS}/EdgeRepair_Kernels.ptx")
    add_dependencies(EdgeRepair EdgeRepairCudaPTX)
    add_custom_command(TARGET EdgeRepair POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${ER_CUDA_ASSETS}" "${AE_PLUGINS_DIR}/CUDA_Assets"
        COMMENT "Installing EdgeRepair CUDA_Assets next to the deployed .aex"
        VERBATIM
    )
else()
    message(STATUS "nvcc not found - CUDA PTX not built (CUDA declines at runtime; CPU only)")
endif()

message(STATUS "EdgeRepair.aex + kernel assets -> ${AE_PLUGINS_DIR}")
