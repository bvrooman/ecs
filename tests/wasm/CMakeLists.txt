# ---------------------------------------------------------------------------
# WebAssembly smoke tests for the core. Added only under an Emscripten
# toolchain (see the root CMakeLists guard), so a native build never descends
# here. They keep the core portable to wasm32 -- 32-bit size_t, Emscripten's
# libc++, the portable reflection backend, wasm threads -- while the JS
# bindings and browser demos are parked (docs/PARKED.md). Both run under node
# and exit non-zero when a check fails:
#   node build-wasm/tests/wasm/ecs_smoke.js
#   node build-wasm/tests/wasm/ecs_smoke_mt.js
# ---------------------------------------------------------------------------

# Wasm SIMD: the library's pitch is SoA laid out for vectorizable iteration,
# so build the smokes the way a real wasm consumer would.
add_compile_options(-msimd128 -mrelaxed-simd -Wall -Wextra)

# Single-threaded: no -pthread, so no SharedArrayBuffer. Schedule::run(World&)
# uses a 1-lane WorkerPool that spawns no thread. ALLOW_MEMORY_GROWTH for the
# 50k-entity population; EXIT_RUNTIME so node returns after main().
add_executable(ecs_smoke smoke.cpp)
target_link_libraries(ecs_smoke PRIVATE ecs::ecs)
target_link_options(ecs_smoke PRIVATE
        "-sENVIRONMENT=node"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sEXIT_RUNTIME=1")
set_target_properties(ecs_smoke PROPERTIES SUFFIX ".js")

# Threads: -pthread on both compile and link (it sets the shared-memory ABI for
# the whole program). The root CMakeLists deliberately does not link
# Threads::Threads under Emscripten, so this target opts in itself.
# PTHREAD_POOL_SIZE pre-spawns the workers the 4-lane WorkerPool needs (a
# thread created on demand would wait on the event loop the blocked main
# thread never returns to); fixed memory, since growth with threads is slow.
add_executable(ecs_smoke_mt smoke_mt.cpp)
target_link_libraries(ecs_smoke_mt PRIVATE ecs::ecs)
target_compile_options(ecs_smoke_mt PRIVATE -pthread)
target_link_options(ecs_smoke_mt PRIVATE
        -pthread
        "-sENVIRONMENT=node,worker"
        "-sPTHREAD_POOL_SIZE=4"
        "-sINITIAL_MEMORY=134217728"
        "-sEXIT_RUNTIME=1")
set_target_properties(ecs_smoke_mt PROPERTIES SUFFIX ".js")
