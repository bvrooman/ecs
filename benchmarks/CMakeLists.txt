add_executable(soa_bench soa_bench.cpp)
target_link_libraries(soa_bench PRIVATE ecs::ecs)
# Benchmarks are only meaningful optimized; -march=native lets the SoA columns
# vectorize. Applied unconditionally so `soa_bench` reflects real layout costs
# regardless of CMAKE_BUILD_TYPE.
target_compile_options(soa_bench PRIVATE -O3 -DNDEBUG -march=native)
