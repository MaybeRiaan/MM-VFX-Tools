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

    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND powershell.exe -NonInteractive -Command
            "Copy-Item -Path '$<TARGET_FILE:${TARGET}>' -Destination '${AE_PLUGINS_DIR}/${TARGET}.aex' -Force; Write-Host 'Deployed ${TARGET} to AE plugins folder'"
        COMMENT "Installing ${TARGET} to After Effects plugins folder"
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

add_meb_plugin(EdgeRepair "${RESOURCES_DIR}/EdgeRepairPiPL.r"
    "${SRC_DIR}/EdgeRepair.cpp"
    "${SRC_DIR}/EdgeRepair_Strings.cpp"
    ${MM_COMMON_SOURCES})

message(STATUS "ModernEdgeBlur.aex, EdgeRepair.aex → ${AE_PLUGINS_DIR}")
