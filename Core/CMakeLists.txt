# Contains base types, functions, macros and pragmas for all game build targets
add_subdirectory(Libraries)

# i stands for Interface
add_library(corei_gameengine_include INTERFACE)
add_library(corei_libraries_source_wwvegas INTERFACE)
add_library(corei_main INTERFACE)
add_library(corei_always INTERFACE)
add_library(corei_always_no_pch INTERFACE) # Use this for Shared Libs with MFC AFX

target_include_directories(corei_gameengine_include INTERFACE "GameEngine/Include")
target_include_directories(corei_libraries_source_wwvegas INTERFACE "Libraries/Source/WWVegas")
target_include_directories(corei_main INTERFACE "Main")

target_link_libraries(corei_always INTERFACE
    core_libraries # Gives force included headers. Link early.
    core_config
    deps_utility
    resources
)
target_link_libraries(corei_always_no_pch INTERFACE
    core_libraries # Gives force included headers. Link early.
    core_config
    deps_utility
    resources
)

# Set where the build results will end up
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

if(RTS_BUILD_CORE_ONLY)
    return()
endif()

# Base GameEngine library
add_subdirectory(GameEngine)

# O150 (RTS_BUILD_ENGINE_ONLY) see the top level CMakeLists.txt.
if(RTS_BUILD_ENGINE_ONLY)
    return()
endif()

# Platform specific GameEngine code
add_subdirectory(GameEngineDevice)

# O158 (RTS_BUILD_HEADLESS) the headless games only.
if(NOT RTS_BUILD_HEADLESS)
    add_subdirectory(Tools)
endif()
