cmake_minimum_required(VERSION 3.20)

project(GrainTransfer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Suite helpers (version cross-check + shared AE deploy dir). OPTIONAL so this
# project still configures if the folder is lifted out of the suite.
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/MMSuite.cmake" OPTIONAL)

# The AE SDK lives OUTSIDE this repo and is never committed (see .gitignore).
# Override with -DAE_SDK_ROOT=... if the SDK moves.
set(AE_SDK_ROOT "E:/OFX DEV/AE SDK/AfterEffectsSDK_25.6_61_win/ae25.6_61.64bit.AfterEffectsSDK/Examples"
    CACHE PATH "After Effects SDK Examples directory")

set(SRC_DIR       "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")
set(KISSFFT_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/third_party/kissfft")

if(NOT EXISTS "${AE_SDK_ROOT}/Headers/AE_Effect.h")
    message(FATAL_ERROR "AE SDK not found at: ${AE_SDK_ROOT}")
endif()

include_directories(
    ${SRC_DIR}
    ${AE_SDK_ROOT}/Headers
    ${AE_SDK_ROOT}/Headers/SP
    ${AE_SDK_ROOT}/Util
    ${AE_SDK_ROOT}/Resources
    ${RESOURCES_DIR}
    ${KISSFFT_DIR}
)

# SDK support sources compiled into the plugin target (same pattern as the
# rest of the suite).
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
        set(AE_PLUGINS_DIR "/Applications/Adobe After Effects 2025/Plug-ins")
    else()
        set(AE_PLUGINS_DIR "C:/Program Files/Adobe/Adobe After Effects 2026/Support Files/Plug-ins")
    endif()
endif()

set(PIPL_R   "${RESOURCES_DIR}/GrainTransferPiPL.r")

# Header and PiPL each carry their own copy of the version defines; AE rejects
# the plugin at load if they disagree. Catch it here instead.
if(COMMAND mm_check_plugin_version)
    mm_check_plugin_version(NAME GrainTransfer
        HEADER "${SRC_DIR}/GrainTransfer.h"
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


set(PIPL_RR  "${CMAKE_CURRENT_BINARY_DIR}/GrainTransferPiPL.rr")
set(PIPL_RC  "${CMAKE_CURRENT_BINARY_DIR}/GrainTransferPiPL.rc")

add_custom_command(
    OUTPUT ${PIPL_RR}
    COMMAND ${CMAKE_CXX_COMPILER} /I "${AE_SDK_ROOT}/Headers" /I "${SRC_DIR}"
            /D "MSWindows" /EP "${PIPL_R}" > "${PIPL_RR}"
    DEPENDS ${PIPL_R}
    COMMENT "Preprocessing GrainTransfer PiPL..."
    VERBATIM
)

# PiPLtool.exe splits argv on spaces — run it from the build dir with bare
# filenames so the spaces in the project/SDK paths never reach its argv.
add_custom_command(
    OUTPUT ${PIPL_RC}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PIPLTOOL}" "${CMAKE_CURRENT_BINARY_DIR}/PiPLtool.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PIPL_RR}" "${CMAKE_CURRENT_BINARY_DIR}/GrainTransferPiPL.rr"
    COMMAND cmd /c ".\\PiPLtool.exe GrainTransferPiPL.rr GrainTransferPiPL.rc.tmp"
    COMMAND ${CMAKE_CXX_COMPILER} /nologo /D "MSWindows" /EP "${CMAKE_CURRENT_BINARY_DIR}/GrainTransferPiPL.rc.tmp" > "${PIPL_RC}"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${PIPL_RR}
    COMMENT "Compiling GrainTransfer PiPL with PiPLtool..."
    VERBATIM
)

if(MSVC)
    set_source_files_properties(${PIPL_RC} PROPERTIES LANGUAGE RC GENERATED TRUE)
endif()

# ===========================================================================
# CUDA kernels -> PTX.
#
# The plugin links NO CUDA libraries: gpu/gt_cuda_shim.h loads nvcuda.dll (a
# DRIVER component) at runtime. Only the .cu -> PTX step needs nvcc, and when
# the toolkit is absent this whole block SKIPS: no PTX ships, device setup
# declines CUDA at runtime, AE falls back to the CPU reference. A build machine
# without CUDA therefore still produces a working plugin — one that renders on
# the CPU.
#
# PTX targets compute_75 (Turing/RTX-class baseline). PTX is a text format the
# driver JIT-compiles forward for newer GPUs, so one asset serves every card
# from Turing up.
#
# This mirrors MM_Qualify/CMakeLists.txt and EdgeRepair's before it — see
# EdgeRepair for the full reasoning behind the -ccbin dance below.
# ===========================================================================
# -DGT_NVCC_EXE=NOTFOUND forces the no-toolkit path, which is how the graceful
# skip below gets TESTED on a machine that HAS the toolkit. Asserting that the
# skip works is not the same as exercising it.
if(NOT DEFINED GT_NVCC_EXE)
    file(GLOB GT_NVCC_CANDIDATES
        "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v*/bin/nvcc.exe")
    if(GT_NVCC_CANDIDATES)
        list(SORT GT_NVCC_CANDIDATES)
        list(GET GT_NVCC_CANDIDATES -1 GT_NVCC_EXE)
    else()
        find_program(GT_NVCC_EXE nvcc)
    endif()
endif()

set(GT_KERNEL_FILES
    "${SRC_DIR}/gt_core.h"
    "${SRC_DIR}/gpu/kernels/gt_stamp.kernel")

if(GT_NVCC_EXE)
    message(STATUS "nvcc: ${GT_NVCC_EXE} - building CUDA_Assets PTX")

    # CUDA 12.8's nvcc cannot digest VS 2026's newest STL (hard static_assert:
    # "expected CUDA 13.2 or newer", plus EDG chokes on its C++23 patterns).
    # VS 2026 installs older MSVC toolsets side-by-side — hand nvcc the OLDEST
    # 14.4x (VS 2022-era v143) via -ccbin. If none exists the flag is omitted
    # and nvcc uses the default cl, which may fail; install the "MSVC v143"
    # component or CUDA 13.2+ in that case.
    file(GLOB GT_CUDA_CCBIN_CANDIDATES
        "C:/Program Files/Microsoft Visual Studio/*/*/VC/Tools/MSVC/14.4*/bin/Hostx64/x64/cl.exe")
    set(GT_NVCC_CCBIN "")
    set(GT_NVCC_ENV "")
    if(GT_CUDA_CCBIN_CANDIDATES)
        list(SORT GT_CUDA_CCBIN_CANDIDATES)
        list(GET GT_CUDA_CCBIN_CANDIDATES 0 GT_CUDA_CCBIN)
        message(STATUS "nvcc host compiler (ccbin): ${GT_CUDA_CCBIN}")
        set(GT_NVCC_CCBIN -ccbin "${GT_CUDA_CCBIN}")

        # ccbin alone is not enough: the build environment's INCLUDE (set by
        # vcvars for the NEWEST toolset) would still hand the old cl the new
        # STL. Rebuild INCLUDE for the nvcc step from the chosen toolset's own
        # headers plus the newest installed Windows SDK.
        get_filename_component(GT_CCBIN_DIR "${GT_CUDA_CCBIN}" DIRECTORY)
        get_filename_component(GT_MSVC_ROOT "${GT_CCBIN_DIR}/../../.." ABSOLUTE)
        file(GLOB GT_WINSDK_INC_CANDIDATES
            "C:/Program Files (x86)/Windows Kits/10/Include/10.*")
        list(SORT GT_WINSDK_INC_CANDIDATES)
        list(GET GT_WINSDK_INC_CANDIDATES -1 GT_WINSDK_INC)
        file(TO_NATIVE_PATH "${GT_MSVC_ROOT}/include" GT_INC_MSVC)
        file(TO_NATIVE_PATH "${GT_WINSDK_INC}/ucrt"   GT_INC_UCRT)
        file(TO_NATIVE_PATH "${GT_WINSDK_INC}/shared" GT_INC_SHARED)
        file(TO_NATIVE_PATH "${GT_WINSDK_INC}/um"     GT_INC_UM)
        file(TO_NATIVE_PATH "${GT_MSVC_ROOT}" GT_MSVC_ROOT_NATIVE)
        file(TO_NATIVE_PATH "${GT_CCBIN_DIR}" GT_CCBIN_DIR_NATIVE)
        # nvcc's own VS detection always lands on the NEWEST toolset, so give
        # it a fully consistent 14.4x environment instead: --use-local-env
        # makes it trust INCLUDE; VCToolsInstallDir covers its VS probing; the
        # PATH prepend satisfies its cl-in-PATH == ccbin consistency check.
        set(GT_NVCC_ENV ${CMAKE_COMMAND} -E env
            --modify "PATH=path_list_prepend:${GT_CCBIN_DIR_NATIVE}"
            "INCLUDE=${GT_INC_MSVC}\;${GT_INC_UCRT}\;${GT_INC_SHARED}\;${GT_INC_UM}"
            "VCToolsInstallDir=${GT_MSVC_ROOT_NATIVE}\\")
    endif()

    set(GT_CUDA_ASSETS "${CMAKE_CURRENT_BINARY_DIR}/CUDA_Assets")
    add_custom_command(
        OUTPUT "${GT_CUDA_ASSETS}/GrainTransfer_Kernels.ptx"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GT_CUDA_ASSETS}"
        COMMAND ${GT_NVCC_ENV} "${GT_NVCC_EXE}" -ptx -arch=compute_75
                --use-local-env
                -allow-unsupported-compiler ${GT_NVCC_CCBIN} -o
                "${GT_CUDA_ASSETS}/GrainTransfer_Kernels.ptx"
                "${SRC_DIR}/gpu/cuda/gt_kernels.cu"
        # An edit to the kernel body or to gt_core.h must rebuild the PTX, or
        # the shipped asset silently drifts from the CPU path it mirrors.
        DEPENDS "${SRC_DIR}/gpu/cuda/gt_kernels.cu"
                "${SRC_DIR}/gpu/kernels/gt_prologue_cuda.h"
                ${GT_KERNEL_FILES}
        COMMENT "nvcc: gt_kernels.cu -> CUDA_Assets/GrainTransfer_Kernels.ptx"
        VERBATIM
    )
    add_custom_target(gt_cuda_ptx
        DEPENDS "${GT_CUDA_ASSETS}/GrainTransfer_Kernels.ptx")
else()
    message(STATUS "nvcc not found - CUDA_Assets PTX will NOT be built. "
                   "The plugin still builds and runs; After Effects will "
                   "render it on the CPU (GPU device setup declines).")
endif()

add_library(GrainTransfer MODULE
    "${SRC_DIR}/GrainTransfer.cpp"
    "${SRC_DIR}/GrainTransfer_Strings.cpp"
    "${SRC_DIR}/common/GT_Common.cpp"
    "${SRC_DIR}/core/GT_FFT.cpp"
    "${SRC_DIR}/core/GT_Grain.cpp"
    "${SRC_DIR}/core/GT_Response.cpp"
    "${SRC_DIR}/core/GT_Stamp.cpp"
    "${SRC_DIR}/gpu/gt_gpu_cuda.cpp"
    "${KISSFFT_DIR}/kiss_fft.c"
    "${KISSFFT_DIR}/kiss_fftnd.c"
    ${SDK_SOURCES}
    ${PIPL_RC}
)

if(GT_NVCC_EXE)
    add_dependencies(GrainTransfer gt_cuda_ptx)
endif()

set_target_properties(GrainTransfer PROPERTIES
    PREFIX ""
    SUFFIX ".aex"
    OUTPUT_NAME "GrainTransfer"
)

if(WIN32)
    target_compile_definitions(GrainTransfer PRIVATE _WINDOWS WIN32 _USRDLL MSWindows)
    set_target_properties(GrainTransfer PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS")
endif()

add_custom_command(TARGET GrainTransfer POST_BUILD
    COMMAND powershell.exe -NonInteractive -Command
        "Copy-Item -Path '$<TARGET_FILE:GrainTransfer>' -Destination '${AE_PLUGINS_DIR}/GrainTransfer.aex' -Force; Write-Host 'Deployed GrainTransfer to AE plugins folder'"
    COMMENT "Installing GrainTransfer to After Effects plugins folder"
    VERBATIM
)

# Kernel assets must sit NEXT TO the .aex, in AE's own Plug-ins folder — not
# Common/Plug-ins/7.0/MediaCore, which is Premiere's (../docs/AE_GPU_Effects.md
# §2.5). A stale PTX here is the classic half-finished deploy: device setup
# fails to resolve an entry point and declines the whole backend rather than
# discovering it mid-render, where the worlds are device memory and there is no
# CPU to fall back to.
if(GT_NVCC_EXE)
    add_custom_command(TARGET GrainTransfer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${GT_CUDA_ASSETS}" "${AE_PLUGINS_DIR}/CUDA_Assets"
        COMMENT "Installing GrainTransfer CUDA_Assets next to the deployed .aex"
        VERBATIM)
endif()

message(STATUS "GrainTransfer.aex → ${AE_PLUGINS_DIR}")


# ===========================================================================
# macOS - the .plugin bundle: a Rez-compiled PiPL in Resources, universal
# (x86_64 + arm64), matching the SDK's own Xcode sample layout. Rez ships with
# the Xcode command-line tools.
#
# CPU PATH ONLY, and that is a complete plugin rather than a degraded one: the
# CPU pipeline is the reference both platforms are validated against. There is
# no Metal host in this suite yet (../docs/gpu-kernel-rules.md SS4), so
# gt_gpu_cuda.cpp (excluded from this branch entirely; its host header is #ifdef _WIN32) compiles to its decline stubs here, AE is never offered a
# device, and every frame takes the reference path.
#
# NOTE: cannot be compiled or verified on the Windows dev machine.
# ===========================================================================
elseif(APPLE)
    find_program(REZ_TOOL Rez REQUIRED)

    add_library(GrainTransfer MODULE
        "${SRC_DIR}/GrainTransfer.cpp"
        "${SRC_DIR}/GrainTransfer_Strings.cpp"
        "${SRC_DIR}/common/GT_Common.cpp"
        "${SRC_DIR}/core/GT_FFT.cpp"
        "${SRC_DIR}/core/GT_Grain.cpp"
        "${SRC_DIR}/core/GT_Response.cpp"
        "${SRC_DIR}/core/GT_Stamp.cpp"
        "${KISSFFT_DIR}/kiss_fft.c"
        "${KISSFFT_DIR}/kiss_fftnd.c"
        ${SDK_SOURCES}
    )

    set_target_properties(GrainTransfer PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "plugin"
        OUTPUT_NAME "GrainTransfer"
        MACOSX_BUNDLE_INFO_PLIST "${RESOURCES_DIR}/Info.plist"
        OSX_ARCHITECTURES "x86_64;arm64")

    target_compile_definitions(GrainTransfer PRIVATE macintosh)

    # The PiPL goes into the bundle's Resources as a Rez-compiled .rsrc. AE
    # reads it to find the effect's name, category and entry point; without it
    # the bundle loads and does nothing.
    add_custom_command(TARGET GrainTransfer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_CONTENT_DIR:GrainTransfer>/Resources"
        COMMAND ${REZ_TOOL}
            -useDF
            -i "${AE_SDK_ROOT}/Headers"
            -i "${AE_SDK_ROOT}/Resources"
            -arch x86_64
            -o "$<TARGET_BUNDLE_CONTENT_DIR:GrainTransfer>/Resources/GrainTransfer.rsrc"
            "${PIPL_R}"
        COMMENT "Compiling GrainTransfer PiPL with Rez"
        VERBATIM)

    # Deploy. Pass -DAE_PLUGINS_DIR=... if AE is not at the default location.
    add_custom_command(TARGET GrainTransfer POST_BUILD
        COMMAND sh -c "if [ -w \"${AE_PLUGINS_DIR}\" ]; then rm -rf \"${AE_PLUGINS_DIR}/GrainTransfer.plugin\" && cp -R \"$<TARGET_BUNDLE_DIR:GrainTransfer>\" \"${AE_PLUGINS_DIR}/GrainTransfer.plugin\" && echo 'Deployed GrainTransfer -> ${AE_PLUGINS_DIR}'; else echo 'AE plugins folder missing or not writable; skipping deploy (pass -DAE_PLUGINS_DIR=<writable dir>, or copy by hand)'; fi"
        COMMENT "Installing GrainTransfer.plugin -> ${AE_PLUGINS_DIR}"
        VERBATIM)

    message(STATUS "GrainTransfer.plugin (macOS bundle, CPU path) -> ${AE_PLUGINS_DIR}")

else()
    message(FATAL_ERROR "GrainTransfer targets Windows and macOS only.")
endif()
