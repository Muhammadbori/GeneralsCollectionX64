if(WIN32)
    add_executable(z_generals WIN32)
else()
    add_executable(z_generals) # O158 headless (UnixMain.cpp)
endif()

# Use a binary name that doesn't conflict with original game.
if("${CMAKE_SYSTEM}" MATCHES "Windows")
    set_target_properties(z_generals PROPERTIES OUTPUT_NAME "generalszh${RTS_BUILD_OUTPUT_SUFFIX}")
else()
    set_target_properties(z_generals PROPERTIES OUTPUT_NAME "generalszh${RTS_BUILD_OUTPUT_SUFFIX}")
endif()

target_link_libraries(z_generals PRIVATE
    z_gameenginedevice
    zi_always
)
if(WIN32)
    target_link_libraries(z_generals PRIVATE comctl32 imm32 vfw32 winmm)
elseif(NOT APPLE)
    # O162 The engine and the device libraries reference each other; GNU ld scans a static library once, so they are
    # linked repeatedly (Apple's linker and MSVC resolve this by themselves).
    # The cycle is declared, so CMake repeats the two libraries on the link line.
    target_link_libraries(z_gameengine PUBLIC z_gameenginedevice)
    set_property(TARGET z_gameenginedevice APPEND PROPERTY LINK_INTERFACE_MULTIPLICITY 3)
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
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/BuildVersion.h
"#pragma once

#define VERSION_MAJOR 1
#define VERSION_MINOR 4
#define VERSION_BUILDNUM 601
"
)

if(MSVC)
    target_link_options(z_generals PRIVATE "/NODEFAULTLIB:libci.lib")
endif()

target_include_directories(z_generals PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

if(WIN32)
    target_sources(z_generals PRIVATE
        WinMain.cpp
        WinMain.h
    )
else()
    target_sources(z_generals PRIVATE UnixMain.cpp)
endif()

# RC files optional for MinGW builds
# Icon: LoadIcon() handles missing resource gracefully (uses default system icon)
# Manifest: DPI awareness metadata (nice to have but not essential)
if(MSVC)
    # VS2005 and later adds default manifest, we need to turn it off to prevent conflict with custom manifest
    if(NOT IS_VS6_BUILD)
        target_link_options(z_generals PRIVATE
            "/MANIFEST:NO"
        )
    endif()

    target_sources(z_generals PRIVATE
        RTS.RC
    )
endif()

# Strip debug symbols to separate file for MinGW Release builds
# This creates generalszh${RTS_BUILD_OUTPUT_SUFFIX}.exe.debug (similar to MSVC .pdb files)
if(MINGW AND COMMAND add_debug_strip_target)
    add_debug_strip_target(z_generals)
endif()

# The FFmpeg DLLs next to the executable when a prebuilt FFmpeg package is used (cmake/ffmpeg-prebuilt.cmake).
rts_copy_ffmpeg_runtime(z_generals)
rts_copy_renderer_runtime(z_generals)
