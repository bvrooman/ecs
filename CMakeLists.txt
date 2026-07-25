cmake_minimum_required(VERSION 3.24)
project(ecs LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# ECS_USE_P2996: build the reflection facade against C++26 static reflection
# (P2996 std::meta). Requires a toolchain with P2996 support such as GCC 16
# (g++-16, which ships libstdc++). When OFF the portable structured-bindings
# backend is used so the project builds on stock Clang/GCC.
option(ECS_USE_P2996 "Use C++26 P2996 static reflection" ON)
option(ECS_BUILD_TESTS "Build the test suite" ON)
option(ECS_BUILD_EXAMPLES "Build examples" ON)
option(ECS_BUILD_BENCHMARKS "Build the microbenchmarks" ON)
option(ECS_BUILD_TOOLS "Build the tools" ON)
option(ECS_REFLECT_NAMES "Register component/resource type names via reflection (tooling/diagnostics)" OFF)
option(ECS_DEV_TOOLING "Development profile" OFF)
# Semicolon- or comma-separated sanitizer list applied to everything built in
# this tree (tests are the intended target), e.g. "address;undefined" or
# "thread". Empty = off. The library itself is header-only, so consumers are
# unaffected either way.
set(ECS_SANITIZE "" CACHE STRING "Sanitizers to build with (e.g. address;undefined or thread)")

if (ECS_SANITIZE)
    string(REPLACE ";" "," ECS_SANITIZE_FLAG "${ECS_SANITIZE}")
    add_compile_options(-fsanitize=${ECS_SANITIZE_FLAG} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${ECS_SANITIZE_FLAG})
endif ()

# Emscripten's Clang has no P2996 static reflection (-freflection / <meta>), so
# force the portable structured-bindings backend there regardless of the cached
# value -- the WASM build works without the caller having to remember the flag.
if (EMSCRIPTEN AND ECS_USE_P2996)
    message(STATUS "ecs: Emscripten toolchain -- forcing ECS_USE_P2996=OFF (portable reflection)")
    set(ECS_USE_P2996 OFF CACHE BOOL "Use C++26 P2996 static reflection" FORCE)
endif ()

if (ECS_USE_P2996)
    set(CMAKE_CXX_STANDARD 26)
else ()
    set(CMAKE_CXX_STANDARD 23)
endif ()
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
if (ECS_USE_P2996)
    target_compile_definitions(ecs INTERFACE ECS_USE_P2996=1)
    # GCC 16 enables P2996 reflection via -freflection (with -std=c++26).
    target_compile_options(ecs INTERFACE -freflection)
endif ()

# Opt-in: record component/resource type names (via reflection) into a registry
# so tooling/diagnostics can resolve a numeric id to a readable name. Off by
# default -- zero cost (no registry, no string storage) when unused. The dev
# tooling profile turns it on automatically.
if (ECS_REFLECT_NAMES)
    target_compile_definitions(ecs INTERFACE ECS_REFLECT_NAMES=1)
endif ()

if (ECS_DEV_TOOLING)
    target_compile_definitions(ecs INTERFACE ECS_REFLECT_NAMES=1)
endif ()

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
# Header-only timing diagnostics (tools/stats.hpp -> ecs::diag::TickStats):
# periodic per-tick timing distributions (mean/p50/p99/max). No engine
# dependency; like the visualizer it lives in tools/, not the core 'ecs' headers,
# so examples (native + web) link this to log their stats.
# ---------------------------------------------------------------------------
add_library(diag INTERFACE)
add_library(ecs::diag ALIAS diag)
target_include_directories(diag INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/tools)

# ---------------------------------------------------------------------------
# Header-only schedule profiler: two ScheduleObserver timing tools in tools/ --
# ecs::diag::ScheduleReport (schedule_report.hpp, rolling per-system/wave/tick
# distributions) and ecs::diag::ScheduleTrace (schedule_trace.hpp, per-tick CSV
# trace), both feeding a configurable ecs::diag::Sink (sink.hpp). They build on
# the core observer hook (Schedule::add_observer), which is always available;
# the tools carry no compile gate of their own -- an app links this target and
# decides when to construct/attach them at runtime (e.g. the mist demo does so on
# the ECS_STATS / ECS_TRACE env vars). Like the visualizer they live in tools/.
# ---------------------------------------------------------------------------
add_library(profiler INTERFACE)
add_library(ecs::profiler ALIAS profiler)
target_include_directories(profiler INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/tools)
target_link_libraries(profiler INTERFACE ecs::ecs)

# ---------------------------------------------------------------------------
# Threads -- the WorkerPool (include/ecs/parallel/worker_pool.hpp) runs the schedule on
# resident std::thread workers. No external dependencies otherwise.
#
# Under an Emscripten (WASM) toolchain we build single-threaded: a 1-lane
# WorkerPool spawns no std::thread, so the schedule runs inline on the sole wasm
# thread. Linking Threads::Threads there would inject -pthread and force a
# SharedArrayBuffer build (needs cross-origin isolation), so skip it.
# ---------------------------------------------------------------------------
if (NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    target_link_libraries(ecs INTERFACE Threads::Threads)
endif ()

# ---------------------------------------------------------------------------
# Tests & examples
# ---------------------------------------------------------------------------
if (ECS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

if (ECS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()

if (ECS_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif ()

if (ECS_BUILD_TOOLS)
    add_subdirectory(tools)
endif ()

# WebAssembly targets (Phase 0 smoke test; later the browser demo). Only under an
# Emscripten toolchain -- native builds skip this entirely.
if (EMSCRIPTEN)
    add_subdirectory(web)
endif ()
