# ---------------------------------------------------------------------------
# schedule-viz: render a Schedule's wave DAG. Emits standalone SVG by default
# (no dependencies); pass --dot for Graphviz DOT (pipe to `dot -Tsvg`).
#   ./schedule-viz            > schedule.svg
#   ./schedule-viz --dot | dot -Tsvg > schedule.svg
# ---------------------------------------------------------------------------
add_executable(schedule-viz schedule_viz_demo.cpp)
target_link_libraries(schedule-viz PRIVATE ecs::ecs)
target_compile_options(schedule-viz PRIVATE $<$<CONFIG:Release>:-O2>)
