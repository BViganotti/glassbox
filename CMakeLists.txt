cmake_minimum_required(VERSION 3.20)
project(glassbox VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(GBX_BUILD_TESTS "Build unit, fuzz and benchmark targets" ON)
option(GBX_WERROR "Treat warnings as errors" ON)
set(GBX_SANITIZER "" CACHE STRING "Sanitizer to enable: address, thread or undefined")

add_library(gbx_flags INTERFACE)
target_compile_options(gbx_flags INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wundef
  -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference)
if(GBX_WERROR)
  target_compile_options(gbx_flags INTERFACE -Werror)
endif()
if(GBX_SANITIZER)
  target_compile_options(gbx_flags INTERFACE -fsanitize=${GBX_SANITIZER} -fno-omit-frame-pointer -g)
  target_link_options(gbx_flags INTERFACE -fsanitize=${GBX_SANITIZER})
endif()

# ---------------------------------------------------------------------------
# Embedded UI assets (generated at configure time; no external tools).
# ---------------------------------------------------------------------------
set(GBX_ASSET_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/ui/index.html
  ${CMAKE_CURRENT_SOURCE_DIR}/ui/app.js
  ${CMAKE_CURRENT_SOURCE_DIR}/ui/style.css)
set(GBX_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(GBX_ASSETS_CPP ${GBX_GENERATED_DIR}/embedded_assets.cpp)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GBX_ASSET_SOURCES})
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedAssets.cmake)
gbx_embed_assets("${GBX_ASSETS_CPP}" ${GBX_ASSET_SOURCES})

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
file(GLOB GBX_TRACE_SOURCES CONFIGURE_DEPENDS src/trace/*.cpp)
file(GLOB GBX_RECORDER_SOURCES CONFIGURE_DEPENDS src/recorder/*.cpp)
file(GLOB GBX_ALLOC_SOURCES CONFIGURE_DEPENDS src/alloc/*.cpp)
file(GLOB GBX_SAMPLE_SOURCES CONFIGURE_DEPENDS src/sample/*.cpp)
file(GLOB GBX_ANALYSIS_SOURCES CONFIGURE_DEPENDS src/analysis/*.cpp)
file(GLOB GBX_QUERY_SOURCES CONFIGURE_DEPENDS src/query/*.cpp)
file(GLOB GBX_UI_SOURCES CONFIGURE_DEPENDS src/ui/*.cpp)
file(GLOB GBX_DEMO_SOURCES CONFIGURE_DEPENDS src/demo/*.cpp)

add_library(glassbox STATIC
  ${GBX_TRACE_SOURCES}
  ${GBX_RECORDER_SOURCES}
  ${GBX_ALLOC_SOURCES}
  ${GBX_SAMPLE_SOURCES}
  ${GBX_ANALYSIS_SOURCES}
  ${GBX_QUERY_SOURCES}
  ${GBX_UI_SOURCES}
  ${GBX_DEMO_SOURCES}
  ${GBX_ASSETS_CPP})
add_library(glassbox::glassbox ALIAS glassbox)
target_include_directories(glassbox PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${GBX_GENERATED_DIR})
target_link_libraries(glassbox PUBLIC gbx_flags)
target_compile_definitions(glassbox PUBLIC
  GBX_VERSION="${PROJECT_VERSION}"
  _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE)
find_package(Threads REQUIRED)
target_link_libraries(glassbox PUBLIC Threads::Threads)

# Opt-in global new/delete interception. Never linked into the main binary or
# sanitizer builds; consumers must opt in explicitly.
add_library(glassbox_hooks STATIC src/alloc/hooks_impl.cpp)
target_link_libraries(glassbox_hooks PUBLIC glassbox gbx_flags)

# ---------------------------------------------------------------------------
# Launcher / workbench executable
# ---------------------------------------------------------------------------
add_executable(glassbox_launcher src/app/main.cpp)
set_target_properties(glassbox_launcher PROPERTIES OUTPUT_NAME glassbox)
target_link_libraries(glassbox_launcher PRIVATE glassbox)

add_executable(glassbox_demo examples/glassbox_demo/main.cpp)
target_link_libraries(glassbox_demo PRIVATE glassbox)

add_executable(glassbox_basic examples/basic/main.cpp)
target_link_libraries(glassbox_basic PRIVATE glassbox)

# ---------------------------------------------------------------------------
# Tests, fuzz and benchmarks
# ---------------------------------------------------------------------------
if(GBX_BUILD_TESTS)
  include(CTest)
  add_subdirectory(tests)
  add_subdirectory(fuzz)
  add_subdirectory(bench)
endif()
