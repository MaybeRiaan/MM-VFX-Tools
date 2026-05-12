cmake_minimum_required(VERSION 3.20)

# Read current version from ModernEdgeBlur.h
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/ModernEdgeBlur.h" VERSION_H_CONTENT)
string(REGEX MATCH "#define[ \t]+MAJOR_VERSION[ \t]+([0-9]+)" _ ${VERSION_H_CONTENT})
set(VERSION_MAJOR ${CMAKE_MATCH_1})
string(REGEX MATCH "#define[ \t]+MINOR_VERSION[ \t]+([0-9]+)" _ ${VERSION_H_CONTENT})
set(VERSION_MINOR ${CMAKE_MATCH_1})
string(REGEX MATCH "#define[ \t]+BUG_VERSION[ \t]+([0-9]+)" _ ${VERSION_H_CONTENT})
set(VERSION_PATCH ${CMAKE_MATCH_1})
string(REGEX MATCH "#define[ \t]+BUILD_VERSION[ \t]+([0-9]+)" _ ${VERSION_H_CONTENT})
set(VERSION_BUILD ${CMAKE_MATCH_1})

project(ModernEdgeBlur VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH})

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Local SDK paths
set(SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
set(RESOURCES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")
set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Include directories
include_directories(
    ${SRC_DIR}
    ${SDK_ROOT}/Headers
    ${SDK_ROOT}/Headers/SP
    ${SDK_ROOT}/Util
    ${RESOURCES_DIR}
)

# Source files
set(SOURCES
    ${SRC_DIR}/ModernEdgeBlur.cpp
    ${SRC_DIR}/ModernEdgeBlur_Strings.cpp
    ${SDK_ROOT}/Util/AEGP_SuiteHandler.cpp
    ${SDK_ROOT}/Util/MissingSuiteError.cpp
)

set(HEADERS
    ${SRC_DIR}/ModernEdgeBlur.h
    ${SRC_DIR}/ModernEdgeBlur_Strings.h
)

# PiPL Resource file
set(PIPL_SOURCE ${RESOURCES_DIR}/ModernEdgeBlurPiPL.r)

# Find PiPLTool (local copy)
set(PIPLTOOL "${RESOURCES_DIR}/PiPLTool.exe")

if(NOT EXISTS ${PIPLTOOL})
    message(FATAL_ERROR "PiPLTool.exe not found at: ${PIPLTOOL}")
endif()

# Compile PiPL to binary resource
set(PIPL_BINARY ${CMAKE_CURRENT_BINARY_DIR}/ModernEdgeBlurPiPL.rc)

# Preprocess the .r file first
set(PIPL_PREPROCESSED ${CMAKE_CURRENT_BINARY_DIR}/ModernEdgeBlurPiPL.rr)

add_custom_command(
    OUTPUT ${PIPL_PREPROCESSED}
    COMMAND ${CMAKE_CXX_COMPILER} /I "${SDK_ROOT}/Headers" /I "${SRC_DIR}" 
            /D "MAJOR_VERSION=${VERSION_MAJOR}" 
            /D "MINOR_VERSION=${VERSION_MINOR}" 
            /D "BUG_VERSION=${VERSION_PATCH}" 
            /D "STAGE_VERSION=1"
            /D "BUILD_VERSION=${VERSION_BUILD}"
            /EP "${PIPL_SOURCE}" > "${PIPL_PREPROCESSED}"
    DEPENDS ${PIPL_SOURCE} ${SRC_DIR}/ModernEdgeBlur.h
    COMMENT "Preprocessing PiPL resource..."
    VERBATIM
)

# PiPLTool.exe doesn't quote-handle paths with spaces. Run it from the build dir
# with bare filenames so the space in "OFX DEV" never reaches its argv.
add_custom_command(
    OUTPUT ${PIPL_BINARY}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PIPLTOOL}" "${CMAKE_CURRENT_BINARY_DIR}/PiPLTool.exe"
    COMMAND cmd /c ".\\PiPLTool.exe ModernEdgeBlurPiPL.rr ModernEdgeBlurPiPL.rc.tmp"
    COMMAND ${CMAKE_CXX_COMPILER} /nologo /D "MSWindows" /EP "${PIPL_BINARY}.tmp" > "${PIPL_BINARY}"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${PIPL_PREPROCESSED}
    COMMENT "Compiling PiPL resource with PiPLTool..."
    VERBATIM
)

# Ensure the RC file is treated as a Windows resource
if(MSVC)
    set_source_files_properties(${PIPL_BINARY} PROPERTIES
        LANGUAGE RC
        GENERATED TRUE
    )
endif()

# Create the plugin as a shared library (.aex is just a renamed .dll)
add_library(${PROJECT_NAME} MODULE ${SOURCES} ${HEADERS} ${PIPL_BINARY})

# Set output name to .aex
set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    SUFFIX ".aex"
    OUTPUT_NAME "ModernEdgeBlur"
)

# No external libraries needed for basic plugin

# Windows-specific settings
if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _WINDOWS
        WIN32
        _USRDLL
        MSWindows
    )
    
    # Set subsystem to Windows
    set_target_properties(${PROJECT_NAME} PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS"
    )
endif()

# Post-build: Copy to After Effects plugins folder
set(AE_PLUGINS_DIR "C:/Program Files/Adobe/Adobe After Effects 2026/Support Files/Plug-ins")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Copying plugin to After Effects..."
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${PROJECT_NAME}>"
        "${AE_PLUGINS_DIR}/ModernEdgeBlur.aex"
    COMMENT "Installing plugin to After Effects plugins folder"
)

# Print build info
message(STATUS "Modern Edge Blur v${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH} Build ${VERSION_BUILD}")
message(STATUS "AE SDK: ${AE_SDK_ROOT}")
message(STATUS "Install to: ${AE_PLUGINS_DIR}")
