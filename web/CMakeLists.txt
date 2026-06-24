# ---------------------------------------------------------------------------
# WebAssembly targets. Added only under an Emscripten toolchain (see the root
# CMakeLists guard), so a native build never descends here.
# ---------------------------------------------------------------------------

# The browser demos live under web/examples/. Their static assets (HTML/CSS/JS in
# examples/public/) are copied wholesale to the build output dir so it is directly
# serveable; the GL demos embed the shared GLSL from examples/shaders/ into the
# wasm (--embed-file), read at runtime via examples/gl_util.hpp. Headless smoke
# tests live under web/test/.
add_custom_target(web_public ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_CURRENT_SOURCE_DIR}/examples/public ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Copying static web assets to the serve dir")
set(WEB_SHADERS_EMBED "SHELL:--embed-file ${CMAKE_CURRENT_SOURCE_DIR}/examples/shaders@/shaders")

# Phase 0 smoke test: proves the ECS core compiles and runs under Emscripten with
# the portable reflection backend, single-threaded. Emits a Node-runnable loader.
#   run: node build-wasm/web/ecs_smoke.js
add_executable(ecs_smoke test/smoke.cpp)
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
# drawing (examples/particles/main_web.cpp) are web-specific.
# ---------------------------------------------------------------------------
set(PARTICLES_NATIVE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../examples/particles)
add_executable(particles_web
        examples/particles/main_web.cpp
        ${PARTICLES_NATIVE_DIR}/simulation.cpp)
target_include_directories(particles_web PRIVATE ${PARTICLES_NATIVE_DIR})
target_link_libraries(particles_web PRIVATE ecs::ecs)

# GLFW's Emscripten port + a forced WebGL2 (GLES 3.0) context. Single-threaded,
# so no -pthread; ALLOW_MEMORY_GROWTH for the growing particle buffers. No
# EXIT_RUNTIME -- this is a main-loop app that never returns from main().
target_link_options(particles_web PRIVATE
        "-lembind" # JS-defined systems over the native components (Phase C)
        ${WEB_SHADERS_EMBED}
        "-sENVIRONMENT=web"
        "-sUSE_GLFW=3"
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
        "-sFULL_ES3=1"
        "-sALLOW_MEMORY_GROWTH=1"
        "-O2")
set_target_properties(particles_web PROPERTIES OUTPUT_NAME particles SUFFIX ".js")

# ---------------------------------------------------------------------------
# Phase 3: experimental multi-threaded build. The SAME sources, compiled with
# -pthread, so the real WorkerPool (parallel/worker_pool.hpp) spins up resident
# Web Worker lanes over SharedArrayBuffer and the data-parallel field systems run
# across them. Needs cross-origin isolation (COOP/COEP) to load -- see
# web/coi-server.py / `build-wasm.sh serve`. The single-threaded build above
# stays the host-anywhere default.
# ---------------------------------------------------------------------------
add_executable(particles_web_mt
        examples/particles/main_web.cpp
        ${PARTICLES_NATIVE_DIR}/simulation.cpp)
target_include_directories(particles_web_mt PRIVATE ${PARTICLES_NATIVE_DIR})
target_link_libraries(particles_web_mt PRIVATE ecs::ecs)

# -pthread must be on both compile and link (it sets the shared-memory ABI for
# every TU). PTHREAD_POOL_SIZE pre-warms enough workers that pool (re)creation on
# the main thread never has to block on the event loop. Fixed memory (no growth)
# sidesteps growable-SharedArrayBuffer entirely.
target_compile_options(particles_web_mt PRIVATE -pthread)
target_link_options(particles_web_mt PRIVATE
        -pthread
        "-lembind" # JS-defined systems over the native components (Phase C)
        ${WEB_SHADERS_EMBED}
        "-sENVIRONMENT=web,worker"
        "-sUSE_GLFW=3"
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
        "-sFULL_ES3=1"
        "-sPTHREAD_POOL_SIZE=8"
        "-sINITIAL_MEMORY=134217728"
        "-O2")
set_target_properties(particles_web_mt PROPERTIES OUTPUT_NAME particles-mt SUFFIX ".js")

# ---------------------------------------------------------------------------
# Phase D: a *fully JavaScript-defined* particle fountain. The C++ is only the
# ECS runtime (dynamic/bindings.cpp -> the DynamicWorld embind class) plus a
# WebGL draw host (examples/particles_js/host.cpp owns the context + the draw).
# Components, entities and systems are all defined in examples/public/index-js.html.
# ---------------------------------------------------------------------------
add_executable(particles_js
        dynamic/bindings.cpp
        examples/particles_js/host.cpp)
target_link_libraries(particles_js PRIVATE ecs::ecs)
target_link_options(particles_js PRIVATE
        "-lembind"
        ${WEB_SHADERS_EMBED}
        "-sENVIRONMENT=web"
        "-sUSE_GLFW=3"
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
        "-sFULL_ES3=1"
        "-sALLOW_MEMORY_GROWTH=1"
        "-O2")
set_target_properties(particles_js PROPERTIES OUTPUT_NAME particles-js SUFFIX ".js")

# Multi-threaded fold-in: the same JS-defined fountain, but its hot data systems
# use defineParallelSystem -- gravity/turbulence/integrate fan out across Web
# Worker lanes. Needs cross-origin isolation (COOP/COEP) to load; serve via
# web/coi-server.py. Fixed memory keeps the shared HEAPF32.buffer stable.
add_executable(particles_js_mt
        dynamic/bindings.cpp
        examples/particles_js/host.cpp)
target_link_libraries(particles_js_mt PRIVATE ecs::ecs)
target_compile_options(particles_js_mt PRIVATE -pthread)
target_link_options(particles_js_mt PRIVATE
        -pthread
        "-lembind"
        ${WEB_SHADERS_EMBED}
        "-sENVIRONMENT=web,worker"
        "-sUSE_GLFW=3"
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
        "-sFULL_ES3=1"
        "-sPTHREAD_POOL_SIZE=8"
        "-sINITIAL_MEMORY=134217728"
        "-O2")
set_target_properties(particles_js_mt PROPERTIES OUTPUT_NAME particles-js-mt SUFFIX ".js")

# ---------------------------------------------------------------------------
# Stretch spike: parallel JS kernels (worker-eval'd). A -pthread node target that
# dispatches an eval'd JS kernel across WorkerPool lanes over shared memory --
# proving JS systems can run off the main thread. Fixed memory (no growth) keeps
# the shared HEAPF32.buffer stable. Run: node build-wasm/web/ecs_parallel_smoke.js
# ---------------------------------------------------------------------------
add_executable(ecs_parallel_smoke test/parallel_smoke.cpp)
target_link_libraries(ecs_parallel_smoke PRIVATE ecs::ecs)
target_compile_options(ecs_parallel_smoke PRIVATE -pthread)
target_link_options(ecs_parallel_smoke PRIVATE
        -pthread
        "-sENVIRONMENT=node,worker"
        "-sPTHREAD_POOL_SIZE=4"
        "-sINITIAL_MEMORY=67108864"
        "-sEXIT_RUNTIME=1"
        "-O2")
set_target_properties(ecs_parallel_smoke PROPERTIES OUTPUT_NAME ecs_parallel_smoke SUFFIX ".js")

# ---------------------------------------------------------------------------
# Phase A: dynamic (JS-defined) components. An embind module exposing the runtime
# component path (ecs::dynamic) to JavaScript as an ES module, plus a Node smoke
# test. Headless, no GL. Verify with:
#   node build-wasm/web/smoke_dynamic.mjs
# ---------------------------------------------------------------------------
add_executable(ecs_dynamic dynamic/bindings.cpp)
target_link_libraries(ecs_dynamic PRIVATE ecs::ecs)
target_link_options(ecs_dynamic PRIVATE
        "-lembind"
        "-sENVIRONMENT=node"
        "-sMODULARIZE=1"
        "-sEXPORT_ES6=1"
        "-sALLOW_MEMORY_GROWTH=1"
        "-O2")
set_target_properties(ecs_dynamic PROPERTIES OUTPUT_NAME ecs_dynamic SUFFIX ".mjs")

# Drop the JS smoke test next to the generated module so its relative import resolves.
add_custom_command(TARGET ecs_dynamic POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
                ${CMAKE_CURRENT_SOURCE_DIR}/test/smoke_dynamic.mjs
                $<TARGET_FILE_DIR:ecs_dynamic>/smoke_dynamic.mjs
        COMMENT "Copying dynamic-components node smoke test")

# Multi-threaded build of the same module: -pthread so DynamicWorld's resident
# WorkerPool fans defineParallelSystem out across Web Worker lanes. Same sources,
# verified by smoke_parallel.mjs.  node build-wasm/web/smoke_parallel.mjs
add_executable(ecs_dynamic_mt dynamic/bindings.cpp)
target_link_libraries(ecs_dynamic_mt PRIVATE ecs::ecs)
target_compile_options(ecs_dynamic_mt PRIVATE -pthread)
target_link_options(ecs_dynamic_mt PRIVATE
        -pthread
        "-lembind"
        "-sENVIRONMENT=node,worker"
        "-sMODULARIZE=1"
        "-sEXPORT_ES6=1"
        "-sPTHREAD_POOL_SIZE=4"
        "-sINITIAL_MEMORY=134217728"
        "-O2")
set_target_properties(ecs_dynamic_mt PROPERTIES OUTPUT_NAME ecs_dynamic_mt SUFFIX ".mjs")

add_custom_command(TARGET ecs_dynamic_mt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
                ${CMAKE_CURRENT_SOURCE_DIR}/test/smoke_parallel.mjs
                $<TARGET_FILE_DIR:ecs_dynamic_mt>/smoke_parallel.mjs
        COMMENT "Copying parallel-system node smoke test")
