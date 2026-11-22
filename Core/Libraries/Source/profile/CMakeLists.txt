set(PROFILE_SRC
    "Include/profile/profile_doc.h"
    "Include/profile/profile_funclevel.h"
    "Include/profile/profile_highlevel.h"
    "Include/profile/profile_result.h"
    "Include/profile/profile.h"
    "internal_cmd.h"
    "internal_funclevel.h"
    "internal_highlevel.h"
    "internal_result.h"
    "internal.h"
    "profile_cmd.cpp"
    "profile_funclevel.cpp"
    "profile_highlevel.cpp"
    "profile_result.cpp"
    "profile.cpp"
)

# TheSuperHackers @build O146 (headless port) Built on Windows only for now, as the debug library it uses (see there);
# macOS and Linux get its headers.
if(NOT WIN32)
    add_library(core_profile_legacy INTERFACE)
    target_include_directories(core_profile_legacy INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Include)
    return()
endif()

add_library(core_profile_legacy STATIC)

target_sources(core_profile_legacy PRIVATE ${PROFILE_SRC})

target_include_directories(core_profile_legacy PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Include
)

target_precompile_headers(core_profile_legacy PRIVATE
    [["Precompiled/precompiled.h"]] # Must be first
    "Include/profile/profile.h"
    "internal.h"
    <windows.h>
)

target_link_libraries(core_profile_legacy PRIVATE
    depsi_always # Gives force included headers. Link early.
    core_config
    core_debug
    deps_utility
    stlport
)
