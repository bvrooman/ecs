# ecs_add_test(<name> [<source>]) -- <source> defaults to <name>.cpp.
function(ecs_add_test name)
    if (ARGC GREATER 1)
        set(source ${ARGV1})
    else ()
        set(source ${name}.cpp)
    endif ()
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE ecs::ecs)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND ${name})
    # The threaded tests' failure mode is a hang (a missed wake, a stuck join);
    # bound them so CI reports a failure instead of stalling.
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

ecs_add_test(test_strong_id)
ecs_add_test(test_signature)
ecs_add_test(test_small_vector)
ecs_add_test(test_index_recycler)
ecs_add_test(test_handle_vector)
ecs_add_test(test_sparse_handle_vector)
ecs_add_test(test_soa)
ecs_add_test(test_emitter)
ecs_add_test(test_world)
ecs_add_test(test_worker_pool)
ecs_add_test(test_schedule)
ecs_add_test(test_resource)
ecs_add_test(test_command_buffer)
ecs_add_test(test_triple_buffer)
ecs_add_test(test_snapshot_channel)
ecs_add_test(test_dynamic)
ecs_add_test(test_sort_rows)
# test_sort_rows again, with compiler extensions on (-std=gnu++23) -- what a
# consumer compiles the headers with by leaving CMake's CXX_EXTENSIONS default
# alone, where this tree builds strict. Up to GCC 15, libstdc++ counts __int128
# as std::integral only with extensions on, so on those compilers -- CI's among
# them -- only this build lets the suite's 128-bit key case fail: without
# sort_rows' width guard such a key reaches the counting sort here, and not in
# test_sort_rows. The definition makes the source assert that __int128 is
# std::integral in this build.
ecs_add_test(test_sort_rows_gnu test_sort_rows.cpp)
set_target_properties(test_sort_rows_gnu PROPERTIES CXX_EXTENSIONS ON)
target_compile_definitions(test_sort_rows_gnu PRIVATE ECS_TEST_EXPECT_INTEGRAL_INT128)
ecs_add_test(test_parallel_paths)
ecs_add_test(test_stats)
target_link_libraries(test_stats PRIVATE ecs::diag)
ecs_add_test(test_profiler)
target_link_libraries(test_profiler PRIVATE ecs::profiler) # schedule timing observers
