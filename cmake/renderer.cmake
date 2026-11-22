# TheSuperHackers @build O138 The renderer the game is built with.
#   RTS_RENDERER=dx12    Direct3D 12 (Backend/DX12), the default.
#   RTS_RENDERER=vulkan  Vulkan (Backend/Vulkan): the same renderer interface implemented with Vulkan, for macOS
#                        (through MoltenVK) and for comparing the two renderers on Windows. Needs the Vulkan SDK
#                        (VULKAN_SDK) for the headers and the DirectX Shader Compiler (dxcompiler), which is copied
#                        next to the executable. Vulkan itself is loaded at run time.
set(RTS_RENDERER "dx12" CACHE STRING "Renderer: dx12 or vulkan")
set_property(CACHE RTS_RENDERER PROPERTY STRINGS dx12 vulkan)
set(RTS_RENDERER_RUNTIME_FILES "")

if(RTS_RENDERER STREQUAL "vulkan")
    # Only the headers are needed (Vulkan is loaded at run time); the Vulkan SDK sets VULKAN_SDK.
    find_path(RTS_VULKAN_INCLUDE_DIR vulkan/vulkan.h HINTS "$ENV{VULKAN_SDK}/Include" "$ENV{VULKAN_SDK}/include")
    if(NOT RTS_VULKAN_INCLUDE_DIR)
        message(FATAL_ERROR "RTS_RENDERER=vulkan: vulkan/vulkan.h not found; install the Vulkan SDK or set VULKAN_SDK")
    endif()
    set(Vulkan_INCLUDE_DIRS "${RTS_VULKAN_INCLUDE_DIR}")
    # Every target sees the same definition: it changes the layout of the renderer's resource classes.
    add_compile_definitions(RTS_RENDERER_VULKAN=1)
    get_filename_component(_rts_vulkan_sdk "${Vulkan_INCLUDE_DIRS}/.." ABSOLUTE)
    if(WIN32)
        set(_rts_dxc "${_rts_vulkan_sdk}/Bin/dxcompiler.dll")
        if(NOT EXISTS "${_rts_dxc}")
            message(FATAL_ERROR "RTS_RENDERER=vulkan: ${_rts_dxc} not found (install the Vulkan SDK with the DirectX Shader Compiler)")
        endif()
        list(APPEND RTS_RENDERER_RUNTIME_FILES "${_rts_dxc}")
    endif()
    # TheSuperHackers @build O141 The renderer's own fixed shaders (Backend/Vulkan/*.hlsl) are compiled to SPIR-V when
    # the build is configured, into headers under the build folder; the generated fixed-function shaders are compiled at
    # run time instead (see vkinternal.h).
    find_program(RTS_DXC_EXECUTABLE dxc HINTS "${_rts_vulkan_sdk}/Bin" "${_rts_vulkan_sdk}/bin" "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
    if(NOT RTS_DXC_EXECUTABLE)
        message(FATAL_ERROR "RTS_RENDERER=vulkan: dxc (the DirectX Shader Compiler of the Vulkan SDK) not found")
    endif()
    set(RTS_VULKAN_SHADER_DIR "${CMAKE_BINARY_DIR}/generated/vkshaders")
    file(MAKE_DIRECTORY "${RTS_VULKAN_SHADER_DIR}")
    set(_rts_vk_hlsl "${CMAKE_SOURCE_DIR}/Core/Libraries/Source/WWVegas/WW3D2/Backend/Vulkan/vkgamma.hlsl")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_rts_vk_hlsl}")
    foreach(_stage IN ITEMS "vs;VSMain;VkGammaVS;vkgamma_vs.h" "ps;PSMain;VkGammaPS;vkgamma_ps.h")
        list(GET _stage 0 _profile)
        list(GET _stage 1 _entry)
        list(GET _stage 2 _name)
        list(GET _stage 3 _header)
        execute_process(
            COMMAND "${RTS_DXC_EXECUTABLE}" -spirv -fspv-target-env=vulkan1.1 -T ${_profile}_6_0 -E ${_entry}
                -Fh "${RTS_VULKAN_SHADER_DIR}/${_header}" -Vn ${_name} "${_rts_vk_hlsl}"
            RESULT_VARIABLE _rts_dxc_result
            OUTPUT_VARIABLE _rts_dxc_output
            ERROR_VARIABLE _rts_dxc_output
        )
        if(NOT _rts_dxc_result EQUAL 0)
            message(FATAL_ERROR "RTS_RENDERER=vulkan: compiling ${_rts_vk_hlsl} (${_entry}) failed:\n${_rts_dxc_output}")
        endif()
    endforeach()
    message(STATUS "Renderer: Vulkan (headers ${Vulkan_INCLUDE_DIRS}, shaders compiled with ${RTS_DXC_EXECUTABLE})")
elseif(NOT RTS_RENDERER STREQUAL "dx12")
    message(FATAL_ERROR "RTS_RENDERER must be dx12 or vulkan, not ${RTS_RENDERER}")
endif()

# Copies the files the renderer needs at run time next to the target's executable after each build.
function(rts_copy_renderer_runtime target)
    if(RTS_RENDERER_RUNTIME_FILES)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RTS_RENDERER_RUNTIME_FILES} "$<TARGET_FILE_DIR:${target}>"
            COMMENT "Copying the renderer runtime next to ${target}"
            VERBATIM
        )
    endif()
endfunction()
