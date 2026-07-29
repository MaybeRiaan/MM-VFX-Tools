cmake_minimum_required(VERSION 3.20)

project(ModernEdgeBlur)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Local SDK paths
set(SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
set(RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")
set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

include_directories(
    ${SRC_DIR}
    ${SDK_ROOT}/Headers
    ${SDK_ROOT}/Headers/SP
    ${SDK_ROOT}/Util
    ${RESOURCES_DIR}
)

# SDK support sources compiled into every plugin target.
set(SDK_SOURCES
    ${SDK_ROOT}/Util/AEGP_SuiteHandler.cpp
    ${SDK_ROOT}/Util/MissingSuiteError.cpp
    ${SDK_ROOT}/Util/AEFX_SuiteHelper.c
    ${SDK_ROOT}/Util/Smart_Utils.cpp
)

set(PIPLTOOL "${RESOURCES_DIR}/PiPLTool.exe")
if(NOT EXISTS ${PIPLTOOL})
    message(FATAL_ERROR "PiPLTool.exe not found at: ${PIPLTOOL}")
endif()

set(AE_PLUGINS_DIR "C:/Program Files/Adobe/Adobe After Effects 2026/Support Files/Plug-ins")

# MediaCore common plug-ins folder. EdgeRepair installs HERE (not the AE
# folder) because its GPU path is a MediaCore GPUVideoFilter module: AE only
# scans .aex files for an `xGPUFilterEntry` GPU module when they load through
# the MediaCore host, and every working GPU effect on the test box (Adobe's
# own Fractal Noise/Transform/Lumetri live in AE's Plug-ins/Effects, but all
# THIRD-PARTY ones — Red Giant, Artlist, CLEngine) sits in this folder.
# Kernel assets (DirectX_Assets / CUDA_Assets) must sit NEXT TO the .aex, per
# the Premiere SDK readme. See docs/EdgeRepair.md Stage 6.
set(MEDIACORE_PLUGINS_DIR "C:/Program Files/Adobe/Common/Plug-ins/7.0/MediaCore")

# add_meb_plugin(TARGET PIPL_R <source files...>)
#   TARGET   — CMake target / OUTPUT_NAME of the .aex
#   PIPL_R   — the tool's PiPL .r file
#   remaining args (ARGN) — the tool's own .cpp sources (SDK_SOURCES added auto).
#   Builds the PiPL via the spaces-safe PiPLTool workaround (unique intermediate
#   names per tool so the two custom commands don't collide), compiles the tool
#   sources into a MODULE .aex, and deploys to the AE folder.
function(add_meb_plugin TARGET PIPL_R)
    set(PIPL_RR  "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}PiPL.rr")
    set(PIPL_RC  "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}PiPL.rc")

    add_custom_command(
        OUTPUT ${PIPL_RR}
        COMMAND ${CMAKE_CXX_COMPILER} /I "${SDK_ROOT}/Headers" /I "${SRC_DIR}"
                /D "MSWindows" /EP "${PIPL_R}" > "${PIPL_RR}"
        DEPENDS ${PIPL_R}
        COMMENT "Preprocessing ${TARGET} PiPL..."
        VERBATIM
    )

    # PiPLTool.exe splits argv on spaces — run it from the build dir with bare
    # filenames so the space in the project path never reaches its argv.
    add_custom_command(
        OUTPUT ${PIPL_RC}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PIPLTOOL}" "${CMAKE_CURRENT_BINARY_DIR}/PiPLTool.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PIPL_RR}" "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}PiPL.rr"
        COMMAND cmd /c ".\\PiPLTool.exe ${TARGET}PiPL.rr ${TARGET}PiPL.rc.tmp"
        COMMAND ${CMAKE_CXX_COMPILER} /nologo /D "MSWindows" /EP "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}PiPL.rc.tmp" > "${PIPL_RC}"
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS ${PIPL_RR}
        COMMENT "Compiling ${TARGET} PiPL with PiPLTool..."
        VERBATIM
    )

    if(MSVC)
        set_source_files_properties(${PIPL_RC} PROPERTIES LANGUAGE RC GENERATED TRUE)
    endif()

    add_library(${TARGET} MODULE ${ARGN} ${SDK_SOURCES} ${PIPL_RC})

    set_target_properties(${TARGET} PROPERTIES
        PREFIX ""
        SUFFIX ".aex"
        OUTPUT_NAME "${TARGET}"
    )

    if(WIN32)
        target_compile_definitions(${TARGET} PRIVATE _WINDOWS WIN32 _USRDLL MSWindows)
        set_target_properties(${TARGET} PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS")
    endif()

    if(DEFINED MODERN_EDGE_BLUR_THREADED)
        target_compile_definitions(${TARGET} PRIVATE
            MODERN_EDGE_BLUR_THREADED=${MODERN_EDGE_BLUR_THREADED})
    endif()

    # Install location: MEB_INSTALL_DIR_<TARGET> if set, else the AE folder.
    # (EdgeRepair sets it to MediaCore — see MEDIACORE_PLUGINS_DIR.)
    set(INSTALL_DIR "${AE_PLUGINS_DIR}")
    if(DEFINED MEB_INSTALL_DIR_${TARGET})
        set(INSTALL_DIR "${MEB_INSTALL_DIR_${TARGET}}")
    endif()
    set(MEB_INSTALL_DIR_${TARGET}_RESOLVED "${INSTALL_DIR}" PARENT_SCOPE)

    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND powershell.exe -NonInteractive -Command
            "Copy-Item -Path '$<TARGET_FILE:${TARGET}>' -Destination '${INSTALL_DIR}/${TARGET}.aex' -Force; Write-Host 'Deployed ${TARGET} to ${INSTALL_DIR}'"
        COMMENT "Installing ${TARGET} to ${INSTALL_DIR}"
        VERBATIM
    )
endfunction()

add_meb_plugin(ModernEdgeBlur "${RESOURCES_DIR}/ModernEdgeBlurPiPL.r"
    "${SRC_DIR}/ModernEdgeBlur.cpp"
    "${SRC_DIR}/ModernEdgeBlur_Strings.cpp")

# Shared suite helpers — compiled into each plugin target that lists them,
# same pattern as SDK_SOURCES. ModernEdgeBlur keeps its private copies for
# now (could migrate later; deliberately untouched).
set(MM_COMMON_SOURCES
    "${SRC_DIR}/common/MM_Common.cpp")

set(MEB_INSTALL_DIR_EdgeRepair "${MEDIACORE_PLUGINS_DIR}")
add_meb_plugin(EdgeRepair "${RESOURCES_DIR}/EdgeRepairPiPL.r"
    "${SRC_DIR}/EdgeRepair.cpp"
    "${SRC_DIR}/EdgeRepair_Strings.cpp"
    "${SRC_DIR}/EdgeRepair_GPU.cpp"
    "${SRC_DIR}/gpu/ER_GPUFilter.cpp"
    "${SDK_ROOT}/Util/DirectXUtils.cpp"
    ${MM_COMMON_SOURCES})

# ---------------------------------------------------------------------------
# EdgeRepair Stage 6 — MediaCore GPUVideoFilter module (ER_GPUFilter.cpp,
# exports xGPUFilterEntry): the mechanism AE 2026 actually GPU-renders
# effects through (docs/EdgeRepair.md "Stage 6 investigation state",
# 2026-07-29). Compiles against the PREMIERE PRO SDK headers, which are NOT
# redistributable: drop Examples/Headers into sdk/Premiere/Headers and
# Examples/Projects/GPUVideoFilter/Utils/PrGPUFilterModule.h into
# sdk/Premiere/Utils (both covered by the sdk/ gitignore). Only this one
# source file sees these headers — never mix them into AE-SDK TUs.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${SDK_ROOT}/Premiere/Headers/PrSDKGPUFilter.h" OR
   NOT EXISTS "${SDK_ROOT}/Premiere/Utils/PrGPUFilterModule.h")
    message(FATAL_ERROR "Premiere Pro SDK headers missing under sdk/Premiere/ "
        "(need Headers/* and Utils/PrGPUFilterModule.h) — see the Stage 6 "
        "section of docs/EdgeRepair.md")
endif()
set_source_files_properties("${SRC_DIR}/gpu/ER_GPUFilter.cpp" PROPERTIES
    INCLUDE_DIRECTORIES "${SDK_ROOT}/Premiere/Headers;${SDK_ROOT}/Premiere/Utils"
    COMPILE_DEFINITIONS "PRWIN_ENV")

# ---------------------------------------------------------------------------
# EdgeRepair Stage 6 — GPU (DirectX 12 backend).
# EdgeRepair_GPU.cpp + Adobe's DirectXUtils need d3d12/d3dcompiler; the
# kernels are precompiled at build time by fxc (Windows SDK) into
# DirectX_Assets/<kernel>.cso (compute shader) + .rs (root signature), the
# exact layout sdk/Util/DirectXUtils.cpp::GetShaderPath expects NEXT TO the
# .aex. CUDA/Metal kernel sources exist in src/gpu/ but are not built here
# (no CUDA toolkit on the build machines; Metal is Mac-only) — see
# docs/EdgeRepair.md Stage 6.
# ---------------------------------------------------------------------------
target_link_libraries(EdgeRepair d3d12 d3dcompiler)

# fxc lives in the Windows SDK bin, which is not on PATH for Ninja builds —
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

# One .cso/.rs pair per kernel entry point. Append new entry names here as
# the Stage 6 kernel port lands (all share ER_ROOT_SIGNATURE for now).
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
        COMMENT "fxc: ${ENTRY} → DirectX_Assets/${ASSET}.cso/.rs"
        VERBATIM
    )
    list(APPEND ER_KERNEL_OUTPUTS
        "${ER_DX_ASSETS}/${ASSET}.cso" "${ER_DX_ASSETS}/${ASSET}.rs")
endforeach()

add_custom_target(EdgeRepairShaders DEPENDS ${ER_KERNEL_OUTPUTS})
add_dependencies(EdgeRepair EdgeRepairShaders)

# Deploy the shader assets next to the deployed .aex (same POST_BUILD
# discipline as the plugin binary itself; fails harmlessly if AE holds them).
add_custom_command(TARGET EdgeRepair POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${ER_DX_ASSETS}" "${MEDIACORE_PLUGINS_DIR}/DirectX_Assets"
    COMMENT "Installing EdgeRepair DirectX_Assets next to the deployed .aex"
    VERBATIM
)

# ---------------------------------------------------------------------------
# CUDA backend — the framework AE-on-Windows actually offers effects
# (measured 2026-07-23; DX12 above serves Premiere hosts). The host side
# needs NO toolkit (runtime nvcuda.dll shim in src/gpu/ER_CudaShim.h); only
# the .cu → PTX step needs nvcc. When the CUDA toolkit is absent this whole
# block SKIPS gracefully: no PTX ships, device setup declines CUDA at
# runtime, AE falls back to CPU. PTX targets compute_75 (Turing/RTX-class
# baseline; the test box is an RTX 3070 Ti, sm_86) — PTX is a text format the
# driver JIT-compiles forward for newer GPUs. -allow-unsupported-compiler:
# CUDA 12.8 predates VS 2026's MSVC and hard-errors on its version check;
# safe here because nvcc only emits device PTX (no host code is kept).
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
    message(STATUS "nvcc: ${NVCC_EXE} — building CUDA_Assets PTX")

    # CUDA 12.8's nvcc cannot digest VS 2026's newest STL (hard static_assert:
    # "expected CUDA 13.2 or newer", plus EDG chokes on its C++23 patterns).
    # VS 2026 installs older MSVC toolsets side-by-side — hand nvcc the OLDEST
    # 14.4x (VS 2022-era v143) via -ccbin. If none exists the flag is simply
    # omitted and nvcc uses the default cl (may fail — install the "MSVC v143"
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
        # nvcc's own VS detection always lands on the NEWEST toolset, so give
        # it a fully consistent 14.4x environment instead: --use-local-env
        # makes it trust INCLUDE; VCToolsInstallDir covers its VS probing; the
        # PATH prepend satisfies its cl-in-PATH == ccbin consistency check.
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
        COMMENT "nvcc: EdgeRepairKernels.cu → CUDA_Assets/EdgeRepair_Kernels.ptx"
        VERBATIM
    )
    add_custom_target(EdgeRepairCudaPTX
        DEPENDS "${ER_CUDA_ASSETS}/EdgeRepair_Kernels.ptx")
    add_dependencies(EdgeRepair EdgeRepairCudaPTX)
    add_custom_command(TARGET EdgeRepair POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${ER_CUDA_ASSETS}" "${MEDIACORE_PLUGINS_DIR}/CUDA_Assets"
        COMMENT "Installing EdgeRepair CUDA_Assets next to the deployed .aex"
        VERBATIM
    )
else()
    message(STATUS "nvcc not found — CUDA PTX not built (CUDA declines at runtime; CPU/DX12 only)")
endif()

message(STATUS "ModernEdgeBlur.aex → ${AE_PLUGINS_DIR}")
message(STATUS "EdgeRepair.aex + kernel assets → ${MEDIACORE_PLUGINS_DIR} (GPUVideoFilter host)")
