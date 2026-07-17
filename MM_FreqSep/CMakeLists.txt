cmake_minimum_required(VERSION 3.20)

# C enabled alongside CXX: the SDK's AEFX_SuiteHelper.c is a C translation
# unit — without C in the language list CMake silently drops it and the
# AEFX_Acquire/ReleaseSuite symbols go unresolved at link.
project(MM_FreqSep LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Paths. The AE SDK is vendored under sdk/ (self-contained repo); no external
# path required. RESOURCES holds the PiPL .r and (Windows) PiPLTool.exe.
# ---------------------------------------------------------------------------
set(SDK_ROOT      "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
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

# Plugin's own translation units.
set(FS_SOURCES
    ${SRC_DIR}/main.cpp
    ${SRC_DIR}/smart_render.cpp
    ${SRC_DIR}/blur.cpp
    ${SRC_DIR}/guided.cpp
    ${SRC_DIR}/matte.cpp
    ${SRC_DIR}/curve.cpp
    ${SRC_DIR}/common/MM_Common.cpp
)

set(FS_PIPL_R "${RESOURCES_DIR}/MM_FreqSepPiPL.r")
set(TARGET    MM_FreqSep)

# ===========================================================================
# Windows — mirrors ModernEdgeBlur's PiPLTool.exe pipeline. The PiPL is built
# by preprocessing the .r with the MSVC compiler, running PiPLTool to emit an
# .rc, and compiling that .rc into the plugin's resources.
# ===========================================================================
if(WIN32)
    set(PIPLTOOL "${RESOURCES_DIR}/PiPLTool.exe")
    if(NOT EXISTS ${PIPLTOOL})
        message(FATAL_ERROR "PiPLTool.exe not found at: ${PIPLTOOL}")
    endif()

    set(AE_PLUGINS_DIR "C:/Program Files/Adobe/Adobe After Effects 2026/Support Files/Plug-ins"
        CACHE PATH "After Effects Plug-ins folder to deploy the .aex into")

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

    add_library(${TARGET} MODULE ${FS_SOURCES} ${SDK_SOURCES} ${PIPL_RC})

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
