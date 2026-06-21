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
option(ECS_BUILD_EXAMPLES "Build examples" ON)
option(ECS_BUILD_BENCHMARKS "Build the microbenchmarks" ON)

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
# Threads -- the WorkerPool (include/ecs/worker_pool.hpp) runs the schedule on
# resident std::thread workers. No external dependencies otherwise.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
target_link_libraries(ecs INTERFACE Threads::Threads)

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

if(ECS_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
