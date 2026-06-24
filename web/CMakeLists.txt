# ---------------------------------------------------------------------------
# WebAssembly targets. Added only under an Emscripten toolchain (see the root
# CMakeLists guard), so a native build never descends here.
# ---------------------------------------------------------------------------

# Phase 0 smoke test: proves the ECS core compiles and runs under Emscripten with
# the portable reflection backend, single-threaded. Emits a Node-runnable loader.
#   run: node build-wasm/web/ecs_smoke.js
add_executable(ecs_smoke smoke.cpp)
target_link_libraries(ecs_smoke PRIVATE ecs::ecs)

# Single-threaded: no -pthread, so no SharedArrayBuffer / cross-origin isolation.
# ALLOW_MEMORY_GROWTH so the 50k-entity population can grow the heap; EXIT_RUNTIME
# so `node` returns after main() instead of keeping the runtime alive.
target_link_options(ecs_smoke PRIVATE
        "-sENVIRONMENT=node"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sEXIT_RUNTIME=1")

# The Emscripten toolchain already names executables *.js; be explicit so the
# output path is stable regardless of toolchain defaults.
set_target_properties(ecs_smoke PROPERTIES SUFFIX ".js")

# ---------------------------------------------------------------------------
# Phase 1 browser demo: the ECS particle fountain rendered to a WebGL2 canvas.
# The ECS sim is reused verbatim from the native example -- only windowing and
# drawing (particles/main_web.cpp) are web-specific.
# ---------------------------------------------------------------------------
set(PARTICLES_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../examples/particles)
add_executable(particles_web
        particles/main_web.cpp
        ${PARTICLES_NATIVE_DIR}/simulation.cpp)
target_include_directories(particles_web PRIVATE ${PARTICLES_NATIVE_DIR})
target_link_libraries(particles_web PRIVATE ecs::ecs)

# GLFW's Emscripten port + a forced WebGL2 (GLES 3.0) context. Single-threaded,
# so no -pthread; ALLOW_MEMORY_GROWTH for the growing particle buffers. No
# EXIT_RUNTIME -- this is a main-loop app that never returns from main().
target_link_options(particles_web PRIVATE
        "-sENVIRONMENT=web"
        "-sUSE_GLFW=3"
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
        "-sFULL_ES3=1"
        "-sALLOW_MEMORY_GROWTH=1"
        "-O2")
set_target_properties(particles_web PROPERTIES OUTPUT_NAME particles SUFFIX ".js")

# Drop the static HTML shell next to the generated particles.js so the build
# output directory is directly serveable.
add_custom_command(TARGET particles_web POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
                ${CMAKE_CURRENT_SOURCE_DIR}/particles/index.html
                $<TARGET_FILE_DIR:particles_web>/index.html
        COMMENT "Copying particle demo HTML shell")
