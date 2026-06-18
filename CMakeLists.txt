cmake_minimum_required(VERSION 3.24)
project(ecs LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# ECS_USE_P2996: build the reflection facade against real C++26 static
# reflection (P2996 std::meta). Requires an experimental toolchain such as the
# clang-p2996 fork. When OFF (default) the portable structured-bindings backend
# is used so the project builds on stock Clang/GCC.
option(ECS_USE_P2996 "Use real C++26 P2996 static reflection" OFF)
option(ECS_BUILD_TESTS "Build the test suite" ON)
option(ECS_BUILD_EXAMPLES "Build examples/benchmark" ON)

if(ECS_USE_P2996)
  set(CMAKE_CXX_STANDARD 26)
else()
  set(CMAKE_CXX_STANDARD 23)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Header-only ECS core
# ---------------------------------------------------------------------------
add_library(ecs INTERFACE)
add_library(ecs::ecs ALIAS ecs)
target_include_directories(ecs INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(ecs INTERFACE cxx_std_23)
if(ECS_USE_P2996)
  target_compile_definitions(ecs INTERFACE ECS_USE_P2996=1)
  # The clang-p2996 fork enables reflection via -freflection / -std=c++26.
  target_compile_options(ecs INTERFACE -freflection)
endif()

# ---------------------------------------------------------------------------
# std::execution backend (NVIDIA stdexec, the P2300 reference implementation)
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
  stdexec
  GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
  GIT_TAG        189aac04dffde8796700f98a0df4b8ebf971197c
  GIT_SHALLOW    OFF)
set(STDEXEC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(STDEXEC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(stdexec)

target_link_libraries(ecs INTERFACE STDEXEC::stdexec)

# ---------------------------------------------------------------------------
# Tests & examples
# ---------------------------------------------------------------------------
if(ECS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(ECS_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
