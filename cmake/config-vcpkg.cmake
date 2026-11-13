# Contains options for VCPKG features

option(RTS_BUILD_OPTION_FFMPEG "Enable FFmpeg support" OFF)
# TheSuperHackers @build A prebuilt FFmpeg package (bin, include, lib) instead of vcpkg's; see cmake/ffmpeg-prebuilt.cmake.
set(RTS_FFMPEG_DIR "" CACHE PATH "A prebuilt 64-bit FFmpeg package to use with RTS_BUILD_OPTION_FFMPEG instead of vcpkg's; empty uses vcpkg")
if(RTS_BUILD_OPTION_FFMPEG AND NOT RTS_FFMPEG_DIR)
    list(APPEND VCPKG_MANIFEST_FEATURES "ffmpeg")
endif()
