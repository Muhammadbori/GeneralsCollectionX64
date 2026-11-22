# Interface libraries to set common defines and includes, avoid duplication later
add_library(z_wwcommon INTERFACE)

target_link_libraries(z_wwcommon INTERFACE
    core_wwcommon
)

target_include_directories(z_wwcommon INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# O150 (RTS_BUILD_ENGINE_ONLY) the game engine sees these headers; their libraries are not built.
if(NOT RTS_BUILD_ENGINE_ONLY)
    add_subdirectory(WWAudio)
    add_subdirectory(WW3D2)
    add_subdirectory(WWDownload)
endif()

# Helpful interface to bundle the ww modules together.
add_library(z_wwvegas INTERFACE)

target_include_directories(z_wwvegas INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(RTS_BUILD_ENGINE_ONLY)
    target_link_libraries(z_wwvegas INTERFACE core_wwvegas)
else()
target_link_libraries(z_wwvegas INTERFACE
    core_wwvegas
    z_ww3d2
    z_wwdownload
)
endif()
