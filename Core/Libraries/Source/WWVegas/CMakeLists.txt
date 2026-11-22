# Interface libraries to set common defines and includes, avoid duplication later
add_library(core_wwcommon INTERFACE)

target_link_libraries(core_wwcommon INTERFACE
    # TheSuperHackers @build DirectInput (the keyboard) from the Windows SDK, not from the Direct3D 8 SDK.
    dinput8
    dxguid
    deps_dbghelp
    deps_miles
    deps_usp10
    stlport
)

target_include_directories(core_wwcommon INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(RTS_BUILD_CORE_ONLY OR RTS_BUILD_ENGINE_ONLY)
    add_subdirectory(WWDebug)
    add_subdirectory(WWLib)
    add_subdirectory(WWMath)
else()
    add_subdirectory(WW3D2)
    add_subdirectory(WWAudio)
    add_subdirectory(WWDebug)
    add_subdirectory(WWDownload)
    add_subdirectory(WWLib)
    add_subdirectory(WWMath)
    add_subdirectory(WWStub)
endif()

# Helpful interface to bundle the ww modules together.
add_library(core_wwvegas INTERFACE)

target_include_directories(core_wwvegas INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(core_wwvegas INTERFACE
    # core_ww3d2
    # core_wwaudio
    core_wwdebug
    # core_wwdownload
    core_wwlib
    core_wwmath
)
