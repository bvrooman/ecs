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
option(ECS_BUILD_TOOLS "Build the tools (schedule-viz)" ON)
option(ECS_REFLECT_NAMES "Register component/resource type names via reflection (tooling/diagnostics)" OFF)
option(ECS_DEV_TOOLING "Development profile: reflected names + dump example schedules to SVG" OFF)

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

# Opt-in: record component/resource type names (via reflection) into a registry
# so tooling/diagnostics can resolve a numeric id to a readable name. Off by
# default -- zero cost (no registry, no string storage) when unused. The dev
# tooling profile turns it on automatically.
if(ECS_REFLECT_NAMES OR ECS_DEV_TOOLING)
  target_compile_definitions(ecs INTERFACE ECS_REFLECT_NAMES=1)
endif()

# ---------------------------------------------------------------------------
# Header-only schedule visualizer (tools/schedule_viz*). Kept out of the core
# 'ecs' library so the engine carries no visualization code; examples and apps
# that want to dump a schedule's wave DAG link this instead.
# ---------------------------------------------------------------------------
add_library(schedule_viz INTERFACE)
add_library(ecs::schedule_viz ALIAS schedule_viz)
target_include_directories(schedule_viz INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/tools)
target_link_libraries(schedule_viz INTERFACE ecs::ecs)

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

if(ECS_BUILD_TOOLS)
  add_subdirectory(tools)
endif()
