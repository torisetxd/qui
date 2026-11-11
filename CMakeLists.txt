cmake_minimum_required(VERSION 3.25)
project(qui LANGUAGES CXX)

option(QUI_BUILD_SYS "Build qui_sys (Linux install, portals, screen and mic capture) when its dependencies exist" ON)

find_package(PkgConfig REQUIRED)
if(WIN32 OR ANDROID)
  pkg_check_modules(QUI_DEPS REQUIRED IMPORTED_TARGET sdl3 freetype2 harfbuzz)
  set(QUI_FONT_SOURCE src/system_fonts_scan.cpp)
else()
  pkg_check_modules(QUI_DEPS REQUIRED IMPORTED_TARGET sdl3 freetype2 harfbuzz fontconfig)
  set(QUI_FONT_SOURCE src/system_fonts_fontconfig.cpp)
endif()

add_library(qui STATIC src/app.cpp src/cpu.cpp src/drive.cpp src/engine.cpp src/font_manager.cpp src/gl.cpp src/input.cpp src/svg.cpp ${QUI_FONT_SOURCE} src/text_shaper.cpp)
target_compile_features(qui PUBLIC cxx_std_20)
target_include_directories(qui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(qui PRIVATE PkgConfig::QUI_DEPS)
add_library(qui::qui ALIAS qui)
# Apps fill Style with designated initializers; clang can accept them out of
# declaration order, which gcc never does.
target_compile_options(qui INTERFACE $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-reorder-init-list>)

# Offscreen runs print the qui commit they were built from, so a stale
# checkout or build cache shows up in the first line of output.
find_package(Git QUIET)
if(GIT_FOUND)
  set(QUI_BUILD_ID_DIR ${CMAKE_CURRENT_BINARY_DIR}/build_id)
  add_custom_target(qui_build_id
    COMMAND ${CMAKE_COMMAND} -DGIT=${GIT_EXECUTABLE} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${QUI_BUILD_ID_DIR}/qui_build_id.h -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_id.cmake
    BYPRODUCTS ${QUI_BUILD_ID_DIR}/qui_build_id.h
    VERBATIM)
  add_dependencies(qui qui_build_id)
  target_include_directories(qui PRIVATE ${QUI_BUILD_ID_DIR})
  target_compile_definitions(qui PRIVATE QUI_HAS_BUILD_ID)
endif()

if(QUI_BUILD_SYS AND NOT WIN32 AND NOT ANDROID)
  pkg_check_modules(QUI_SYS_DEPS IMPORTED_TARGET gio-2.0 gio-unix-2.0 libpipewire-0.3)
endif()
if(QUI_SYS_DEPS_FOUND)
  add_library(qui_sys STATIC sys/capture.cpp sys/desktop.cpp sys/portal.cpp)
  target_compile_features(qui_sys PUBLIC cxx_std_20)
  target_include_directories(qui_sys PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sys)
  target_link_libraries(qui_sys PRIVATE PkgConfig::QUI_SYS_DEPS)
  add_library(qui::sys ALIAS qui_sys)
endif()

if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
