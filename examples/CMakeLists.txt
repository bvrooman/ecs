add_executable(nbody nbody.cpp)
target_link_libraries(nbody PRIVATE ecs::ecs)
# A light optimization level so the SoA timing is meaningful.
target_compile_options(nbody PRIVATE $<$<CONFIG:Release>:-O2>)

# ---------------------------------------------------------------------------
# particles: a minimal OpenGL particle effect driven by the ECS.
# Needs GLFW (brew install glfw) and the macOS OpenGL framework, so it is built
# only when both are available -- other platforms/CI just skip it.
# ---------------------------------------------------------------------------
find_package(glfw3 QUIET)
if(APPLE AND glfw3_FOUND)
  find_package(OpenGL REQUIRED)
  add_executable(particles
    particles/main.cpp
    particles/simulation.cpp
    particles/gl_util.cpp)
  target_include_directories(particles PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/particles)
  target_link_libraries(particles PRIVATE ecs::ecs ecs::diag glfw OpenGL::GL)
  # Bake the shaders' absolute path so the app loads them regardless of cwd.
  target_compile_definitions(particles PRIVATE
    PARTICLES_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/particles/shaders")
  target_compile_options(particles PRIVATE $<$<CONFIG:Release>:-O2>)
  # Dev tooling profile: link the visualizer and dump the schedule to SVG on
  # startup (names come from reflection, enabled globally by ECS_DEV_TOOLING).
  if(ECS_DEV_TOOLING)
    target_link_libraries(particles PRIVATE ecs::schedule_viz)
    target_compile_definitions(particles PRIVATE PARTICLES_DEV_TOOLING=1)
  endif()

  # -------------------------------------------------------------------------
  # mist: a second OpenGL demo -- a 3D starling murmuration. Tiny black "birds"
  # fly by Reynolds boids (cohesion + alignment + separation + wander over a
  # spatial grid) at a constant cruise, led around by a wandering goal that
  # sweeps the flock over the close perspective camera, with a local cursor lure
  # that draws nearby birds toward the pointer. Reuses the particle example's
  # gl_util.cpp (GL/text helpers, identical) via its include dir; own shaders.
  # -------------------------------------------------------------------------
  add_executable(mist
    mist/main.cpp
    mist/simulation.cpp
    particles/gl_util.cpp)
  target_include_directories(mist PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/mist
    ${CMAKE_CURRENT_SOURCE_DIR}/particles)
  # Links ecs::profiler for the ScheduleReport/ScheduleTrace observer headers;
  # they always compile, so main.cpp gates them purely at runtime (ECS_STATS /
  # ECS_TRACE) with no build flag.
  target_link_libraries(mist PRIVATE ecs::ecs ecs::diag ecs::profiler glfw OpenGL::GL)
  target_compile_definitions(mist PRIVATE
    MIST_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/mist/shaders")
  # -O3 (vs the -O2 the other demos use): lets the SoA per-bird kernels
  # (integrate, and any elementwise chunk loop) auto-vectorize to NEON.
  target_compile_options(mist PRIVATE $<$<CONFIG:Release>:-O3>)
else()
  message(STATUS "examples: skipping 'particles'/'mist' (needs APPLE + GLFW; "
                 "install with `brew install glfw`)")
endif()
