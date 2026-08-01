cmake_minimum_required(VERSION 3.24)
project(ecs VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Toolchain requirements
# ---------------------------------------------------------------------------
# The library uses C++23 deducing this (P0847) throughout -- soa.hpp,
# archetype.hpp, schedule.hpp -- so an older compiler configures cleanly and
# only then dies inside a template instantiation whose diagnostics name neither
# the feature nor the fix (Ubuntu 24.04's default g++ 13 is the common way to
# land here). Fail up front with the requirement instead. Only the two
# compilers CI covers are checked; anything else is left alone rather than
# guessed at.
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
    message(FATAL_ERROR
            "ecs requires GCC >= 14 (found ${CMAKE_CXX_COMPILER_VERSION}): the "
            "library uses C++23 deducing this (P0847), which GCC gained in 14. "
            "Build with g++-14 or newer, or with Clang >= 18.")
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18)
    message(FATAL_ERROR
            "ecs requires Clang >= 18 (found ${CMAKE_CXX_COMPILER_VERSION}): the "
            "library uses C++23 deducing this (P0847), which Clang gained in 18. "
            "Build with clang++-18 or newer, or with GCC >= 14.")
endif ()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# ECS_USE_P2996: build the reflection facade against C++26 static reflection
# (P2996 std::meta). Requires a toolchain with P2996 support such as GCC 16
# (g++-16, which ships libstdc++). OFF by default: the portable
# structured-bindings backend builds on stock Clang/GCC, which is what the
# README quickstart, every CI leg, and the sanitizer/wasm presets use. Turn it
# ON explicitly -- or via the dev-config/release-config presets, which pin
# g++-16 -- to get real member names out of std::meta.
option(ECS_USE_P2996 "Use C++26 P2996 static reflection" OFF)
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

# ECS_USE_P2996=ON is only meaningful if the toolchain can actually compile the
# backend. Check it HERE, while CMAKE_CXX_STANDARD is still unset -- otherwise
# the first find_package() that runs a compile check (Threads, below) fails
# with "the current compiler does not support CXX26", which names neither this
# option nor the flag that would fix it. The probe mirrors what
# reflection/reflect_p2996.hpp actually uses, so a toolchain carrying an older
# or partial reflection implementation is rejected too -- not just one with no
# <meta> at all.
if (ECS_USE_P2996)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-std=c++26 -freflection")
    check_cxx_source_compiles("
        #include <cstddef>
        #include <meta>
        struct Probe { int a; };
        consteval std::size_t fields() {
            return std::meta::nonstatic_data_members_of(
                       ^^Probe, std::meta::access_context::current()).size();
        }
        static_assert(fields() == 1);
        int main() { return 0; }
    " ECS_HAVE_P2996)
    unset(CMAKE_REQUIRED_FLAGS)
    if (NOT ECS_HAVE_P2996)
        message(FATAL_ERROR
                "ECS_USE_P2996=ON, but ${CMAKE_CXX_COMPILER_ID} "
                "${CMAKE_CXX_COMPILER_VERSION} cannot compile the P2996 "
                "reflection backend (-std=c++26 -freflection with <meta>). "
                "Build with a toolchain that supports it (e.g. GCC 16 / "
                "g++-16), or configure with -DECS_USE_P2996=OFF to use the "
                "portable reflection backend.")
    endif ()
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
# Header-only schedule visualizer (tools/include/ecs/viz/, namespace ecs::viz). Kept out of the core
# 'ecs' library so the engine carries no visualization code; examples and apps
# that want to dump a schedule's wave DAG link this instead.
# ---------------------------------------------------------------------------
add_library(schedule_viz INTERFACE)
add_library(ecs::schedule_viz ALIAS schedule_viz)
target_include_directories(schedule_viz INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tools/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(schedule_viz INTERFACE ecs::ecs)

# ---------------------------------------------------------------------------
# Header-only timing diagnostics (ecs/diag/stats.hpp -> ecs::diag::TickStats):
# periodic per-tick timing distributions (mean/p50/p99/max). No engine
# dependency; like the visualizer it lives in tools/, not the core 'ecs' headers,
# so examples (native + web) link this to log their stats.
# ---------------------------------------------------------------------------
add_library(diag INTERFACE)
add_library(ecs::diag ALIAS diag)
target_include_directories(diag INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tools/include>
        $<INSTALL_INTERFACE:include>)

# ---------------------------------------------------------------------------
# Header-only schedule profiler: two ScheduleObserver timing tools in tools/ --
# ecs::diag::ScheduleReport (ecs/diag/schedule_report.hpp, rolling per-system/wave/tick
# distributions) and ecs::diag::ScheduleTrace (schedule_trace.hpp, per-tick CSV
# trace), both feeding a configurable ecs::diag::Sink (ecs/diag/sink.hpp). They build on
# the core observer hook (Schedule::add_observer), which is always available;
# the tools carry no compile gate of their own -- an app links this target and
# decides when to construct/attach them at runtime (e.g. the mist demo does so on
# the ECS_STATS / ECS_TRACE env vars). Like the visualizer they live in tools/.
# ---------------------------------------------------------------------------
add_library(profiler INTERFACE)
add_library(ecs::profiler ALIAS profiler)
target_include_directories(profiler INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tools/include>
        $<INSTALL_INTERFACE:include>)
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

# ---------------------------------------------------------------------------
# Install / export
#
# All four targets are INTERFACE (header-only), so installing is just headers
# plus the usage requirements CMake records for them. Two header roots ship:
# include/ (the library) and tools/include/ (ecs/diag/, ecs/viz/), both landing
# under the same ecs/ prefix so an installed consumer spells every header the
# same way an in-tree one does.
#
# Skipped when ecs is pulled in with add_subdirectory/FetchContent -- a
# subproject has no business adding install rules to its parent.
# ---------------------------------------------------------------------------
if (PROJECT_IS_TOP_LEVEL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS ecs schedule_viz diag profiler EXPORT ecsTargets)
    install(DIRECTORY include/ tools/include/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            FILES_MATCHING PATTERN "*.hpp")

    install(EXPORT ecsTargets
            FILE ecsTargets.cmake
            NAMESPACE ecs::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ecs)

    # Header-only and compiler-version-independent, so any consumer version
    # works with any newer library version.
    write_basic_package_version_file(
            "${CMAKE_CURRENT_BINARY_DIR}/ecsConfigVersion.cmake"
            VERSION ${PROJECT_VERSION}
            COMPATIBILITY SameMajorVersion
            ARCH_INDEPENDENT)

    configure_package_config_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ecsConfig.cmake.in"
            "${CMAKE_CURRENT_BINARY_DIR}/ecsConfig.cmake"
            INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ecs)

    install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/ecsConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/ecsConfigVersion.cmake"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ecs)
endif ()
