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

# gather_bench: how for_each's per-row gather/scatter (vs for_each_chunk) scales with component
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

# churn_bench: structural costs -- spawn/despawn/add/remove throughput and an
# emitter/reaper steady state with the allocation counter.
ecs_add_benchmark(churn_bench churn_bench.cpp)

# primitives_bench: the kernel parallel primitives (Reduce/Extract/Collect/
# Bin/Events) against hand-rolled imperative baselines at 1 and hw lanes.
ecs_add_benchmark(primitives_bench primitives_bench.cpp)

# sort_bench: World::sort_rows mechanics -- early exit, full counting sort,
# and the min_disorder skip under realistic churn drift.
ecs_add_benchmark(sort_bench sort_bench.cpp)

# access_bench: per-handle access (get/alive) and query matching across a
# fragmented archetype set.
ecs_add_benchmark(access_bench access_bench.cpp)
