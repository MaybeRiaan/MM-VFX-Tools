cmake_minimum_required(VERSION 3.20)

project(GrainTransfer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

set(PIPLTOOL "${AE_SDK_ROOT}/Resources/PiPLtool.exe")
if(NOT EXISTS ${PIPLTOOL})
    message(FATAL_ERROR "PiPLtool.exe not found at: ${PIPLTOOL}")
endif()

set(AE_PLUGINS_DIR "C:/Program Files/Adobe/Adobe After Effects 2026/Support Files/Plug-ins")

set(PIPL_R   "${RESOURCES_DIR}/GrainTransferPiPL.r")
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
    ${PIPL_RC}
)

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

message(STATUS "GrainTransfer.aex → ${AE_PLUGINS_DIR}")
