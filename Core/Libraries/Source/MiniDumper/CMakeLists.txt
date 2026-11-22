set(MINIDUMPER_SRC
    "Include/MiniDumper/MiniDumper.h"
    "MiniDumper.cpp"
)

# TheSuperHackers @build O150 (headless port) The crash dump writer uses dbghelp (Windows); macOS and Linux get its header.
if(NOT WIN32)
    add_library(core_minidumper INTERFACE)
    target_include_directories(core_minidumper INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Include)
    return()
endif()

add_library(core_minidumper STATIC)

target_sources(core_minidumper PRIVATE ${MINIDUMPER_SRC})

target_include_directories(core_minidumper PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Include
)

target_link_libraries(core_minidumper PUBLIC
    core_libraries # Gives force included headers. Link early.
    deps_dbghelp
    stlport
)

target_link_libraries(core_minidumper PRIVATE
    core_config
    deps_utility
    resources
)
