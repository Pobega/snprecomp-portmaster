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

