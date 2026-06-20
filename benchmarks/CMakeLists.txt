add_executable(soa_bench soa_bench.cpp)
target_link_libraries(soa_bench PRIVATE ecs::ecs)
# Benchmarks are only meaningful optimized; -march=native lets the SoA columns
# vectorize. Applied unconditionally so `soa_bench` reflects real layout costs
# regardless of CMAKE_BUILD_TYPE.
target_compile_options(soa_bench PRIVATE -O3 -DNDEBUG -march=native)

# schedule_bench: per-tick throughput + jitter of the particle demo's actual
# schedule (reuses examples/particles/simulation.cpp; no GL/window).
add_executable(schedule_bench
  schedule_bench.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../examples/particles/simulation.cpp)
target_include_directories(schedule_bench
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../examples/particles)
target_link_libraries(schedule_bench PRIVATE ecs::ecs)
target_compile_options(schedule_bench PRIVATE -O3 -DNDEBUG -march=native)

# scale_bench: per-tick cost vs population size (cache scaling + where the pool
# starts to pay off). Self-contained steady-state systems.
add_executable(scale_bench scale_bench.cpp)
target_include_directories(scale_bench
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../examples/particles)
target_link_libraries(scale_bench PRIVATE ecs::ecs)
target_compile_options(scale_bench PRIVATE -O3 -DNDEBUG -march=native)
