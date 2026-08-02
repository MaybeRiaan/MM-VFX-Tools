cmake_minimum_required(VERSION 3.20)

# C enabled alongside CXX: the SDK's AEFX_SuiteHelper.c is a C translation
# unit — without C in the language list CMake silently drops it and the
# AEFX_Acquire/ReleaseSuite symbols go unresolved at link.
project(MM_FreqSep LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Suite helpers (version cross-check + shared AE deploy dir). OPTIONAL so this
# project still configures if the folder is lifted out of the suite.
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/MMSuite.cmake" OPTIONAL)

# ---------------------------------------------------------------------------
# Paths. The AE SDK lives OUTSIDE this repo and is never committed — one
# shared install serves the whole suite (same convention as GrainTransfer).
# Override with -DAE_SDK_ROOT=... if the SDK moves. RESOURCES holds the
# PiPL .r; PiPLtool.exe comes from the SDK's own Resources.
# ---------------------------------------------------------------------------
set(AE_SDK_ROOT "E:/OFX DEV/AE SDK/AfterEffectsSDK_25.6_61_win/ae25.6_61.64bit.AfterEffectsSDK/Examples"
    CACHE PATH "After Effects SDK Examples directory")
set(SDK_ROOT      "${AE_SDK_ROOT}")

if(NOT EXISTS "${SDK_ROOT}/Headers/AE_Effect.h")
    message(FATAL_ERROR "AE SDK not found at: ${SDK_ROOT} (override with -DAE_SDK_ROOT=...)")
endif()
set(RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")
set(SRC_DIR       "${CMAKE_CURRENT_SOURCE_DIR}/src")

include_directories(
    ${SRC_DIR}
    ${SDK_ROOT}/Headers
    ${SDK_ROOT}/Headers/SP
    ${SDK_ROOT}/Util
    ${RESOURCES_DIR}
)

# SDK support sources compiled into the plugin target.
set(SDK_SOURCES
    ${SDK_ROOT}/Util/AEGP_SuiteHandler.cpp
    ${SDK_ROOT}/Util/MissingSuiteError.cpp
    ${SDK_ROOT}/Util/AEFX_SuiteHelper.c
    ${SDK_ROOT}/Util/Smart_Utils.cpp
)

# Plugin's own translation units. mm_gpu_dx.cpp (DX12 host) is appended in
# the Windows branch; the Metal host joins the Apple branch when written.
set(FS_SOURCES
    ${SRC_DIR}/main.cpp
    ${SRC_DIR}/smart_render.cpp
    ${SRC_DIR}/blur.cpp
    ${SRC_DIR}/guided.cpp
    ${SRC_DIR}/matte.cpp
    ${SRC_DIR}/curve.cpp
    ${SRC_DIR}/fs_common.cpp
)

set(FS_PIPL_R "${RESOURCES_DIR}/MM_FreqSepPiPL.r")
set(TARGET    MM_FreqSep)

# Header and PiPL each carry their own copy of the version defines; AE rejects
# the plugin at load if they disagree. Catch it here instead.
if(COMMAND mm_check_plugin_version)
    mm_check_plugin_version(NAME MM_FreqSep
        HEADER "${SRC_DIR}/params.h"
        PIPL   "${FS_PIPL_R}")
endif()

# ===========================================================================
# Windows — mirrors ModernEdgeBlur's PiPLTool.exe pipeline. The PiPL is built
# by preprocessing the .r with the MSVC compiler, running PiPLTool to emit an
# .rc, and compiling that .rc into the plugin's resources.
# ===========================================================================
if(WIN32)
    set(PIPLTOOL "${SDK_ROOT}/Resources/PiPLtool.exe")
    if(NOT EXISTS ${PIPLTOOL})
        message(FATAL_ERROR "PiPLtool.exe not found at: ${PIPLTOOL}")
    endif()

    if(COMMAND mm_ae_plugins_dir)
        mm_ae_plugins_dir()
    else()
        set(AE_PLUGINS_DIR "C:/Program Files/Adobe/Adobe After Effects 2026/Support Files/Plug-ins"
            CACHE PATH "After Effects Plug-ins folder to deploy the .aex into")
    endif()

    set(PIPL_RR "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}PiPL.rr")
    set(PIPL_RC "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}PiPL.rc")

    add_custom_command(
        OUTPUT ${PIPL_RR}
        COMMAND ${CMAKE_CXX_COMPILER} /I "${SDK_ROOT}/Headers" /I "${SRC_DIR}"
                /D "MSWindows" /EP "${FS_PIPL_R}" > "${PIPL_RR}"
        DEPENDS ${FS_PIPL_R}
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

    # =======================================================================
    # GPU compute shaders (Phase 5): each platform-free .kernel body is
    # compiled per entry point by dxc (prologue + body via the src/gpu/dx/
    # shims) into DXIL embedded as C headers — no runtime shader compiler, no
    # loose .cso assets next to the .aex. dxc.exe ships in the Windows SDK
    # that vcvars64 puts on PATH (dxil.dll beside it signs the output).
    # =======================================================================
    find_program(DXC_EXECUTABLE dxc)
    if(NOT DXC_EXECUTABLE)
        file(GLOB _dxc_candidates
            "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxc.exe")
        if(_dxc_candidates)
            list(SORT _dxc_candidates)
            list(GET _dxc_candidates -1 DXC_EXECUTABLE)
        endif()
    endif()
    if(NOT DXC_EXECUTABLE)
        message(FATAL_ERROR "dxc.exe not found (needed for the GPU kernels). Install a Windows 10/11 SDK.")
    endif()
    message(STATUS "dxc: ${DXC_EXECUTABLE}")

    set(FS_DXIL_DIR "${CMAKE_CURRENT_BINARY_DIR}/fs_dxil")
    file(MAKE_DIRECTORY ${FS_DXIL_DIR})

    # wrapper=entry pairs; every entry point compiles separately (-E).
    set(FS_SHADER_ENTRIES
        fs_gauss_cs.hlsl=fs_gauss_pass
        fs_guided_cs.hlsl=fs_guided_box
        fs_guided_cs.hlsl=fs_guided_squares
        fs_guided_cs.hlsl=fs_guided_ab
        fs_guided_cs.hlsl=fs_guided_final
        fs_compose_cs.hlsl=fs_compose
        fs_tile_cs.hlsl=fs_load_tile
        fs_tile_cs.hlsl=fs_store_tile
        fs_tile_cs.hlsl=fs_fill_ones
        fs_matte_cs.hlsl=fs_matte
    )

    file(GLOB FS_KERNEL_FILES "${SRC_DIR}/gpu/kernels/*.kernel"
                              "${SRC_DIR}/gpu/kernels/fs_prologue_hlsl.h")

    set(FS_DXIL_HEADERS "")
    foreach(pair ${FS_SHADER_ENTRIES})
        string(REPLACE "=" ";" pair_list ${pair})
        list(GET pair_list 0 wrapper)
        list(GET pair_list 1 entry)
        set(hdr "${FS_DXIL_DIR}/${entry}.h")
        add_custom_command(
            OUTPUT ${hdr}
            COMMAND ${DXC_EXECUTABLE} -T cs_6_0 -E ${entry} -O3
                    -Fh "${hdr}" -Vn kFS_DXIL_${entry}
                    "${SRC_DIR}/gpu/dx/${wrapper}"
            DEPENDS "${SRC_DIR}/gpu/dx/${wrapper}" ${FS_KERNEL_FILES}
            COMMENT "dxc ${wrapper} -E ${entry}"
            VERBATIM
        )
        list(APPEND FS_DXIL_HEADERS ${hdr})
    endforeach()

    add_custom_target(fs_shaders DEPENDS ${FS_DXIL_HEADERS})

    list(APPEND FS_SOURCES ${SRC_DIR}/gpu/mm_gpu_dx.cpp)

    add_library(${TARGET} MODULE ${FS_SOURCES} ${SDK_SOURCES} ${PIPL_RC})
    add_dependencies(${TARGET} fs_shaders)
    target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(${TARGET} PRIVATE d3d12.lib dxgi.lib)

    set_target_properties(${TARGET} PROPERTIES
        PREFIX ""
        SUFFIX ".aex"
        OUTPUT_NAME "${TARGET}")

    target_compile_definitions(${TARGET} PRIVATE _WINDOWS WIN32 _USRDLL MSWindows)
    set_target_properties(${TARGET} PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS")

    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND powershell.exe -NonInteractive -Command
            "if (Test-Path '${AE_PLUGINS_DIR}') { Copy-Item -Path '$<TARGET_FILE:${TARGET}>' -Destination '${AE_PLUGINS_DIR}/${TARGET}.aex' -Force; Write-Host 'Deployed ${TARGET} to AE plugins folder' } else { Write-Host 'AE plugins folder not found; skipping deploy' }"
        COMMENT "Installing ${TARGET} to After Effects plugins folder"
        VERBATIM)

    message(STATUS "${TARGET}.aex -> ${AE_PLUGINS_DIR}")

# ===========================================================================
# macOS — bundle plugin (.plugin) with a Rez-compiled PiPL, matching the
# SDK's Xcode sample layout. Rez ships with Xcode command-line tools.
# NOTE: cannot be compiled/tested on the Windows dev machine; wired up so the
# Xcode/Ninja generators produce a correct target when built on a Mac.
# ===========================================================================
elseif(APPLE)
    find_program(REZ_TOOL Rez REQUIRED)

    add_library(${TARGET} MODULE ${FS_SOURCES} ${SDK_SOURCES})

    set_target_properties(${TARGET} PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "plugin"
        OUTPUT_NAME "${TARGET}"
        MACOSX_BUNDLE_INFO_PLIST "${RESOURCES_DIR}/Info.plist")

    target_compile_definitions(${TARGET} PRIVATE macintosh)
    set_target_properties(${TARGET} PROPERTIES
        OSX_ARCHITECTURES "x86_64;arm64")

    # Compile the PiPL into the bundle's Resources with Rez. AE_General.r and
    # the SDK headers supply the resource type definitions.
    set(PIPL_RSRC "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.rsrc")
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${REZ_TOOL}
            -i "${SDK_ROOT}/Headers"
            -i "${SDK_ROOT}/Resources"
            -arch x86_64
            -o "$<TARGET_BUNDLE_CONTENT_DIR:${TARGET}>/Resources/${TARGET}.rsrc"
            "${FS_PIPL_R}"
        COMMENT "Compiling ${TARGET} PiPL with Rez"
        VERBATIM)

    message(STATUS "${TARGET}.plugin (macOS bundle)")

else()
    message(FATAL_ERROR "MM_FreqSep targets Windows and macOS only.")
endif()
