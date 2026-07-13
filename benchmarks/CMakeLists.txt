# Benchmarks are only meaningful optimized; -march=native lets the SoA columns
# vectorize. Applied unconditionally (not via CMAKE_BUILD_TYPE) so every suite
# reflects real layout costs regardless of how the tree was configured.
function(ecs_add_benchmark name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE ecs::ecs)
  target_compile_options(${name} PRIVATE -O3 -DNDEBUG -march=native)
endfunction()

# soa_bench: SoA storage vs an AoS baseline + the two query iteration paths.
ecs_add_benchmark(soa_bench soa_bench.cpp)

# gather_bench: how for_each_parallel's per-row gather/scatter scales with component
# complexity (wide/sparse, non-trivial fields) vs for_each_chunk. Most illustrative
# on the portable backend (-DECS_USE_P2996=OFF) -- see the file header.
ecs_add_benchmark(gather_bench gather_bench.cpp)

# schedule_bench: per-tick throughput + jitter of the particle demo's actual
# schedule (reuses examples/particles/simulation.cpp; no GL/window).
ecs_add_benchmark(schedule_bench
  schedule_bench.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../examples/particles/simulation.cpp)
target_include_directories(schedule_bench
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../examples/particles)

# scale_bench: per-tick cost vs population size (cache scaling + where the pool
# starts to pay off). Self-contained steady-state systems.
ecs_add_benchmark(scale_bench scale_bench.cpp)
target_include_directories(scale_bench
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../examples/particles)
