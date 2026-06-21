# ---------------------------------------------------------------------------
# schedule-dot: render a Schedule's wave DAG as Graphviz DOT on stdout.
# Pipe it through Graphviz to get an image:  ./schedule-dot | dot -Tsvg > s.svg
# ---------------------------------------------------------------------------
add_executable(schedule-dot schedule_dot_demo.cpp)
target_link_libraries(schedule-dot PRIVATE ecs::ecs)
target_compile_options(schedule-dot PRIVATE $<$<CONFIG:Release>:-O2>)
