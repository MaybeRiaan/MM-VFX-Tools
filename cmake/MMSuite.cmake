# MMSuite.cmake — helpers shared by every plugin in the MM VFX Tools suite.
#
# Included by each project's CMakeLists with OPTIONAL, so a project folder
# lifted out of the suite still configures (it just loses the suite-level
# checks). Nothing here changes how a plugin is compiled or linked — it is
# deployment location and version bookkeeping only, the two things that were
# copy-pasted five ways before the suite existed.

include_guard(GLOBAL)

# ---------------------------------------------------------------------------
# mm_ae_plugins_dir()
#
# Defines AE_PLUGINS_DIR once, as a cache var, so `-DAE_PLUGINS_DIR=...` at the
# suite root redirects every plugin's deploy at the same time (testing against a
# second AE install, or a sandbox folder instead of Program Files). Projects
# used to hardcode this string individually; three of five had already drifted
# into making it a cache var, two had not.
# ---------------------------------------------------------------------------
function(mm_ae_plugins_dir)
    set(AE_PLUGINS_DIR "C:/Program Files/Adobe/Adobe After Effects 2026/Support Files/Plug-ins"
        CACHE PATH "After Effects Plug-ins folder to deploy .aex files into")
endfunction()

# ---------------------------------------------------------------------------
# mm_check_plugin_version(NAME <label> HEADER <path> PIPL <path>)
#
# THE point of this file. Every plugin carries its version FIVE times over two
# files: MAJOR/MINOR/BUG/STAGE/BUILD_VERSION in a C header, and the same five in
# its PiPL .r resource. AE packs both into one integer at load and compares
# them. If they differ by even one build number it refuses the plugin with
#
#     "version mismatch. Code version is 1.0 and PiPL version is 1.0"
#
# which prints only MAJOR.MINOR and so shows two identical-looking versions as
# the reason it will not load. That message has cost real time in this suite
# more than once.
#
# This turns that runtime rejection into a configure-time error naming the two
# files and both quadruples. The files are added to CMAKE_CONFIGURE_DEPENDS, so
# a version bump (build.ps1 rewrites both) re-runs the check on the next build
# rather than going stale.
#
# STAGE is compared across its two spellings: the header uses the enum
# (PF_Stage_DEVELOP), the .r must use the integer, because the resource
# preprocessor has no enum. Values per AE_Effect.h: DEVELOP 0, ALPHA 1,
# BETA 2, RELEASE 3.
#
# Sets MM_VERSION_<NAME> in the caller's scope and registers the plugin in the
# global MM_PLUGIN_VERSIONS property for mm_report_versions().
# ---------------------------------------------------------------------------
function(mm_check_plugin_version)
    cmake_parse_arguments(MMV "" "NAME;HEADER;PIPL" "" ${ARGN})

    foreach(required NAME HEADER PIPL)
        if(NOT MMV_${required})
            message(FATAL_ERROR "mm_check_plugin_version: ${required} is required")
        endif()
    endforeach()

    foreach(f "${MMV_HEADER}" "${MMV_PIPL}")
        if(NOT EXISTS "${f}")
            message(FATAL_ERROR "mm_check_plugin_version(${MMV_NAME}): no such file: ${f}")
        endif()
    endforeach()

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${MMV_HEADER}" "${MMV_PIPL}")

    file(READ "${MMV_HEADER}" _hdr)
    file(READ "${MMV_PIPL}"   _rsrc)

    foreach(part MAJOR MINOR BUG STAGE BUILD)
        foreach(side hdr rsrc)
            if(NOT "${_${side}}" MATCHES "#define[ \t]+${part}_VERSION[ \t]+([A-Za-z0-9_]+)")
                message(FATAL_ERROR
                    "mm_check_plugin_version(${MMV_NAME}): no ${part}_VERSION define in "
                    "${side} file. Both the header and the PiPL must define all five.")
            endif()
            set(_${side}_${part} "${CMAKE_MATCH_1}")
        endforeach()
    endforeach()

    # Normalize the stage enum to its integer on both sides.
    set(_stage_names PF_Stage_DEVELOP PF_Stage_ALPHA PF_Stage_BETA PF_Stage_RELEASE)
    foreach(side hdr rsrc)
        list(FIND _stage_names "${_${side}_STAGE}" _idx)
        if(NOT _idx EQUAL -1)
            set(_${side}_STAGE "${_idx}")
        endif()
    endforeach()

    set(_hdr_v  "${_hdr_MAJOR}.${_hdr_MINOR}.${_hdr_BUG}.${_hdr_BUILD}")
    set(_rsrc_v "${_rsrc_MAJOR}.${_rsrc_MINOR}.${_rsrc_BUG}.${_rsrc_BUILD}")

    if(NOT _hdr_v STREQUAL _rsrc_v OR NOT _hdr_STAGE STREQUAL _rsrc_STAGE)
        message(FATAL_ERROR
            "\n${MMV_NAME}: version defines disagree between the header and the PiPL.\n"
            "  header : ${_hdr_v}  (stage ${_hdr_STAGE})  ${MMV_HEADER}\n"
            "  PiPL   : ${_rsrc_v}  (stage ${_rsrc_STAGE})  ${MMV_PIPL}\n"
            "AE packs both into one integer and rejects the plugin at load when they\n"
            "differ, reporting only MAJOR.MINOR - so the two look identical in its\n"
            "error. Make all five defines match in BOTH files, then rebuild.\n")
    endif()

    set(MM_VERSION_${MMV_NAME} "${_hdr_v}" PARENT_SCOPE)
    set_property(GLOBAL APPEND PROPERTY MM_PLUGIN_VERSIONS "${MMV_NAME}=${_hdr_v}")
endfunction()

# ---------------------------------------------------------------------------
# mm_report_versions()
#
# Prints every registered plugin's version at the end of a suite configure.
# One place to see what a full build is about to deploy into AE.
# ---------------------------------------------------------------------------
function(mm_report_versions)
    get_property(_versions GLOBAL PROPERTY MM_PLUGIN_VERSIONS)
    if(NOT _versions)
        return()
    endif()
    message(STATUS "")
    message(STATUS "MM VFX Tools — plugin versions (header and PiPL agree):")
    foreach(entry ${_versions})
        string(REPLACE "=" ";" pair "${entry}")
        list(GET pair 0 _name)
        list(GET pair 1 _ver)
        string(LENGTH "${_name}" _len)
        math(EXPR _pad "18 - ${_len}")
        set(_spaces "")
        foreach(i RANGE 1 ${_pad})
            string(APPEND _spaces " ")
        endforeach()
        message(STATUS "    ${_name}${_spaces}${_ver}")
    endforeach()
    message(STATUS "")
endfunction()
