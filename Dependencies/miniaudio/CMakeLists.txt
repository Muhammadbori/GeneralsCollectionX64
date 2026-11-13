# miniaudio 0.11.26, vendored unchanged (see README.md). Compiled as C on its own, without the game's forced
# includes.
add_library(deps_miniaudio STATIC)

target_sources(deps_miniaudio PRIVATE
    miniaudio.c
    miniaudio.h
)

target_include_directories(deps_miniaudio PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
