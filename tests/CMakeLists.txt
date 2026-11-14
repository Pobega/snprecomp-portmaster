cmake_minimum_required(VERSION 3.20)
project(test_gles3_renderer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

add_executable(test_gles3_renderer
    test_gles3_renderer.cpp
    ../src/gles3/gl_loader.cpp
    ../src/gles3/tmem_texture_cache.cpp
    ../src/gles3/combiner_shader_cache.cpp
    ../src/gles3/fbo_manager.cpp
    ../src/gles3/rsp_state.cpp
    ../src/gles3/gles3_renderer_context.cpp
    ../src/game/widescreen.cpp
)

target_include_directories(test_gles3_renderer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/gles3
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/N64ModernRuntime/N64Recomp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/N64ModernRuntime/librecomp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/N64ModernRuntime/ultramodern/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/N64ModernRuntime/ultramodern/include/ultramodern
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/N64ModernRuntime/thirdparty
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(test_gles3_renderer PRIVATE
    ${SDL2_LIBRARIES}
    dl
)

add_executable(test_snp_patch
    test_snp_patch.cpp
)

target_include_directories(test_snp_patch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../tools
)

file(GLOB RCHEEVOS_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/rcheevos/src/api/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/rcheevos/src/client/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/rcheevos/src/hash/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/rcheevos/src/runtime/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/rcheevos/src/util/*.c
)

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/miniz_export.h
    "#pragma once\n#define MINIZ_EXPORT\n#define MINIZ_NO_EXPORT\n"
)

file(GLOB MINIZ_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/N64ModernRuntime/thirdparty/miniz/*.c
)

add_executable(test_ra_client
    test_ra_client.cpp
    ../src/gles3/gl_loader.cpp
    ../src/handheld/handheld_overlay.cpp
    ../src/handheld/ra_credentials.cpp
    ../src/handheld/ra_offline_cache.cpp
    ../src/handheld/png_decode.cpp
    ../src/handheld/ra_client.cpp
    ../src/handheld/handheld_config.cpp
    ../src/handheld/rom_loader.cpp
    ${MINIZ_TEST_SOURCES}
    ${RCHEEVOS_TEST_SOURCES}
)

target_include_directories(test_ra_client PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/handheld
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/gles3
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/rcheevos/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/rcheevos/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/N64ModernRuntime/thirdparty/miniz
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/N64ModernRuntime/thirdparty
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/N64ModernRuntime/thirdparty/concurrentqueue
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/N64ModernRuntime/thirdparty/sse2neon
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/N64ModernRuntime/N64Recomp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/N64ModernRuntime/N64Recomp/lib/tomlplusplus/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/N64ModernRuntime/librecomp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/N64ModernRuntime/ultramodern/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../lib/N64ModernRuntime/ultramodern/include/ultramodern
    ${SDL2_INCLUDE_DIRS}
)

target_compile_definitions(test_ra_client PRIVATE
    MINIZ_STATIC_DEFINE
    RC_CLIENT_SUPPORTS_HASH
    SNP_BACKEND_GLES3
)

target_link_libraries(test_ra_client PRIVATE
    ${SDL2_LIBRARIES}
    dl
    pthread
)


