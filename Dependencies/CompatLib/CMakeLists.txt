# TheSuperHackers @build O140 CompatLib: the Win32 subset the headless game uses on macOS and Linux (windows.h and a
# few other Windows headers, implemented with POSIX). Adapted from the CompatLib of GeneralsX
# (https://github.com/fbraz3/GeneralsX, GPL-3.0-or-later, Felipe Braz and contributors). Not built on Windows.
add_library(deps_compatlib STATIC
    Include/windows_compat.h
    Include/windows.h
    Source/windows_compat.cpp
    Include/windows_compat_o154.h
    Include/winsock_compat.h
    Source/windows_compat_o154.cpp
)
target_include_directories(deps_compatlib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Include")
if (APPLE)
    target_include_directories(deps_compatlib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Include/macos")
endif()
target_include_directories(deps_compatlib PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../Utility")
find_package(Threads REQUIRED)
target_link_libraries(deps_compatlib PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# Everything that uses the Utility headers sees <windows.h> from here.
target_link_libraries(deps_utility INTERFACE deps_compatlib)
