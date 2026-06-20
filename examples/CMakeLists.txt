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
  target_link_libraries(particles PRIVATE ecs::ecs glfw OpenGL::GL)
  # Bake the shaders' absolute path so the app loads them regardless of cwd.
  target_compile_definitions(particles PRIVATE
    PARTICLES_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/particles/shaders")
  target_compile_options(particles PRIVATE $<$<CONFIG:Release>:-O2>)
else()
  message(STATUS "examples: skipping 'particles' (needs APPLE + GLFW; "
                 "install with `brew install glfw`)")
endif()
