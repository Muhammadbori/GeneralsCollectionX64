if(WIN32)
    add_executable(g_generals WIN32)
else()
    add_executable(g_generals) # O158 headless (UnixMain.cpp)
endif()

# Use a binary name that doesn't conflict with original game.
if("${CMAKE_SYSTEM}" MATCHES "Windows")
    set_target_properties(g_generals PROPERTIES OUTPUT_NAME "generalsv${RTS_BUILD_OUTPUT_SUFFIX}")
else()
    set_target_properties(g_generals PROPERTIES OUTPUT_NAME "generalsv${RTS_BUILD_OUTPUT_SUFFIX}")
endif()

target_link_libraries(g_generals PRIVATE
    g_gameenginedevice
    gi_always
)
if(WIN32)
    target_link_libraries(g_generals PRIVATE comctl32 imm32 vfw32 winmm)
elseif(NOT APPLE)
    # O162 The engine and the device libraries reference each other; GNU ld scans a static library once, so they are
    # linked repeatedly (Apple's linker and MSVC resolve this by themselves).
    # The cycle is declared, so CMake repeats the two libraries on the link line.
    target_link_libraries(g_gameengine PUBLIC g_gameenginedevice)
    set_property(TARGET g_gameenginedevice APPEND PROPERTY LINK_INTERFACE_MULTIPLICITY 3)
endif()

# TODO Originally referred to build host and user, replace with git info perhaps?
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/GeneratedVersion.h
"#pragma once

#define VERSION_LOCALBUILDNUM 0
#define VERSION_BUILDUSER \"\"
#define VERSION_BUILDLOC \"\"
"
)

# Based on original binary values for these variables.
if (IS_VS6_BUILD)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/BuildVersion.h
"#pragma once

#define VERSION_MAJOR 1
#define VERSION_MINOR 7
#define VERSION_BUILDNUM 601
"
)
else()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/BuildVersion.h
"#pragma once

#define VERSION_MAJOR 1
#define VERSION_MINOR 8
#define VERSION_BUILDNUM 601
"
)
endif()

if(MSVC)
    target_link_options(g_generals PRIVATE "/NODEFAULTLIB:libci.lib")
endif()

target_include_directories(g_generals PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_BINARY_DIR}/Code/Libraries
)

if(WIN32)
    target_sources(g_generals PRIVATE
        WinMain.cpp
        WinMain.h
    )
else()
    target_sources(g_generals PRIVATE UnixMain.cpp)
endif()

# RC files are optional for MinGW builds
# Icon: LoadIcon() handles missing resource gracefully (uses default system icon)
# Manifest: DPI awareness metadata (nice to have but not essential)
# TYPELIB: Broken (0 bytes) and causes windres memory exhaustion errors
if(MSVC)
    # VS2005 and later adds default manifest, we need to turn it off to prevent conflict with custom manifest
    if(NOT IS_VS6_BUILD)
        target_link_options(g_generals PRIVATE
            "/MANIFEST:NO"
        )
    endif()

    target_sources(g_generals PRIVATE
        RTS.RC
    )
endif()

# Strip debug symbols to separate file for MinGW Release builds
# This creates generalsv${RTS_BUILD_OUTPUT_SUFFIX}.exe.debug (similar to MSVC .pdb files)
if(MINGW AND COMMAND add_debug_strip_target)
    add_debug_strip_target(g_generals)
endif()

# The FFmpeg DLLs next to the executable when a prebuilt FFmpeg package is used (cmake/ffmpeg-prebuilt.cmake).
rts_copy_ffmpeg_runtime(g_generals)
rts_copy_renderer_runtime(g_generals)
