# MM VFX Tools — suite-level build.
#
# Configures every plugin in one tree so a single build produces and deploys
# all five .aex files to After Effects for testing:
#
#     cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#     cmake --build build
#
# or just `.\build.ps1` from here, which handles the vcvars setup.
#
# This is ADDITIVE. Every project still configures and builds standalone from
# its own folder exactly as before — that is the contract, and build.ps1 in each
# project folder is unchanged. Nothing here alters how any plugin compiles;
# projects are pulled in with add_subdirectory, so each keeps its own compile
# flags, SDK wiring, kernel toolchain and POST_BUILD deploy.
#
# What the suite level adds:
#   * one command to build and deploy everything (see mm_publish below)
#   * one AE_PLUGINS_DIR override for all five deploys at once
#   * a configure-time check that each plugin's header and PiPL versions agree,
#     which AE otherwise reports at load as a misleading "version mismatch"
#   * one printed table of what is about to be installed

cmake_minimum_required(VERSION 3.20)

project(MMVFXTools LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(MMSuite)

mm_ae_plugins_dir()

# Per-project switches — build a subset without editing anything.
#   cmake -S . -B build -DMM_BUILD_MM_FREQSEP=OFF
option(MM_BUILD_GRAINTRANSFER  "Build Grain Transfer"                      ON)
option(MM_BUILD_MM_QUALIFY     "Build MM Qualify (HSL Key)"                ON)
option(MM_BUILD_MM_FREQSEP     "Build MM FreqSep"                          ON)
option(MM_BUILD_MODERNEDGEBLUR "Build Modern Edge Blur"                    ON)
option(MM_BUILD_EDGEREPAIR     "Build Edge Repair"                         ON)

set(MM_ENABLED_PROJECTS "")

if(MM_BUILD_GRAINTRANSFER)
    add_subdirectory(GrainTransfer)
    list(APPEND MM_ENABLED_PROJECTS GrainTransfer)
endif()

if(MM_BUILD_MM_QUALIFY)
    add_subdirectory(MM_Qualify)
    list(APPEND MM_ENABLED_PROJECTS MM_Qualify)
endif()

if(MM_BUILD_MM_FREQSEP)
    add_subdirectory(MM_FreqSep)
    list(APPEND MM_ENABLED_PROJECTS MM_FreqSep)
endif()

if(MM_BUILD_MODERNEDGEBLUR)
    add_subdirectory(ModernEdgeBlur)
    list(APPEND MM_ENABLED_PROJECTS ModernEdgeBlur)
endif()

if(MM_BUILD_EDGEREPAIR)
    add_subdirectory(EdgeRepair)
    list(APPEND MM_ENABLED_PROJECTS EdgeRepair)
endif()

# ---------------------------------------------------------------------------
# mm_publish — redeploy every built .aex into AE without recompiling.
#
# Exists because the deploy is the step that fails independently of the build:
# AE holds an open file lock on a loaded .aex, so a build that ran while AE was
# open compiled fine and silently did not install (each project treats the
# failed copy as non-fatal, by design — see MIGRATION.md's verification notes).
# The fix is not to rebuild; it is to close AE and copy again.
#
#     cmake --build build --target mm_publish
# ---------------------------------------------------------------------------
# One target per project since the 2026-08-02 ModernEdgeBlur/EdgeRepair split.
set(MM_PUBLISH_TARGETS ${MM_ENABLED_PROJECTS})

set(MM_PUBLISH_COMMANDS "")
foreach(t ${MM_PUBLISH_TARGETS})
    if(TARGET ${t})
        list(APPEND MM_PUBLISH_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:${t}>" "${AE_PLUGINS_DIR}/${t}.aex")
    endif()
endforeach()

if(MM_PUBLISH_COMMANDS)
    add_custom_target(mm_publish
        ${MM_PUBLISH_COMMANDS}
        COMMENT "Publishing all plugins to ${AE_PLUGINS_DIR} (close AE first)"
        VERBATIM)
    add_dependencies(mm_publish ${MM_PUBLISH_TARGETS})
endif()

# EdgeRepair's CUDA/DirectX kernel assets must sit NEXT TO its .aex, so they
# ride along with its own POST_BUILD step rather than being re-copied here.
# mm_publish depends on the target, which is enough to keep them in sync.

mm_report_versions()

message(STATUS "MM VFX Tools: deploying to ${AE_PLUGINS_DIR}")
message(STATUS "MM VFX Tools: projects — ${MM_ENABLED_PROJECTS}")
