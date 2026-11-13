# TheSuperHackers @build A prebuilt FFmpeg package for RTS_BUILD_OPTION_FFMPEG, without vcpkg.
#
# RTS_FFMPEG_DIR names a "shared" FFmpeg package as FFmpeg's Windows builds ship it: include/, lib/ with the MSVC
# import libraries (avformat.lib, ...) and bin/ with the DLLs. When it is set, this file gives the variables that
# vcpkg's find_package(FFMPEG) gives (FFMPEG_FOUND, FFMPEG_INCLUDE_DIRS, FFMPEG_LIBRARY_DIRS, FFMPEG_LIBRARIES), and
# rts_copy_ffmpeg_runtime(<target>) copies the DLLs the game needs next to the target's executable after each
# build. When it is empty, vcpkg provides FFmpeg as before and rts_copy_ffmpeg_runtime does nothing.

set(FFMPEG_RUNTIME_DLLS "")

if(RTS_BUILD_OPTION_FFMPEG AND RTS_FFMPEG_DIR)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "RTS_FFMPEG_DIR: the prebuilt FFmpeg package is for 64-bit builds; the 32-bit build keeps Bink (leave RTS_BUILD_OPTION_FFMPEG off) or uses vcpkg's FFmpeg (leave RTS_FFMPEG_DIR empty)")
    endif()

    set(_rts_ffmpeg_components avformat avcodec swscale swresample avutil)
    if(NOT EXISTS "${RTS_FFMPEG_DIR}/include/libavcodec/avcodec.h")
        message(FATAL_ERROR "RTS_FFMPEG_DIR: ${RTS_FFMPEG_DIR}/include/libavcodec/avcodec.h not found")
    endif()

    set(FFMPEG_INCLUDE_DIRS "${RTS_FFMPEG_DIR}/include")
    set(FFMPEG_LIBRARY_DIRS "${RTS_FFMPEG_DIR}/lib")
    set(FFMPEG_LIBRARIES "")
    foreach(_c IN LISTS _rts_ffmpeg_components)
        if(NOT EXISTS "${RTS_FFMPEG_DIR}/lib/${_c}.lib")
            message(FATAL_ERROR "RTS_FFMPEG_DIR: ${RTS_FFMPEG_DIR}/lib/${_c}.lib not found")
        endif()
        list(APPEND FFMPEG_LIBRARIES "${RTS_FFMPEG_DIR}/lib/${_c}.lib")
        file(GLOB _dll "${RTS_FFMPEG_DIR}/bin/${_c}-*.dll")
        list(LENGTH _dll _count)
        if(NOT _count EQUAL 1)
            message(FATAL_ERROR "RTS_FFMPEG_DIR: expected one ${RTS_FFMPEG_DIR}/bin/${_c}-*.dll, found ${_count}")
        endif()
        list(APPEND FFMPEG_RUNTIME_DLLS ${_dll})
    endforeach()
    set(FFMPEG_FOUND TRUE)
    message(STATUS "FFmpeg: prebuilt package ${RTS_FFMPEG_DIR}")
endif()

function(rts_copy_ffmpeg_runtime target)
    if(FFMPEG_RUNTIME_DLLS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:${target}>"
            COMMENT "Copying the FFmpeg DLLs next to ${target}"
            VERBATIM
        )
    endif()
endfunction()
