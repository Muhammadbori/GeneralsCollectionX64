# Build tools

# Build useful tool binaries.
if(RTS_BUILD_GENERALS_TOOLS)
    add_subdirectory(ImagePacker)
    add_subdirectory(MapCacheBuilder)
    # Not ported to Direct3D 12 yet, see RTS_BUILD_DX8_TOOLS.
    if(RTS_BUILD_DX8_TOOLS)
        add_subdirectory(GUIEdit)
        add_subdirectory(W3DView)
        add_subdirectory(WorldBuilder)
    endif()
endif()

# Build less useful tool/test binaries.
if(RTS_BUILD_GENERALS_EXTRAS)
    add_subdirectory(Autorun)
    add_subdirectory(Launcher)
    add_subdirectory(PATCHGET)
endif()
