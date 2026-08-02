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
# sources include nothing but their own header and the STL — that independence
# is what made the split mechanical, and it is worth preserving.
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

set(PIPL_R "${RESOURCES_DIR}/ModernEdgeBlurPiPL.r")

# Header and PiPL each carry their own copy of the version defines; AE rejects
# the plugin at load if they disagree. Catch it here instead.
if(COMMAND mm_check_plugin_version)
    mm_check_plugin_version(NAME ModernEdgeBlur
        HEADER "${SRC_DIR}/ModernEdgeBlur.h"
        PIPL   "${PIPL_R}")
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

message(STATUS "ModernEdgeBlur.aex -> ${AE_PLUGINS_DIR}")
