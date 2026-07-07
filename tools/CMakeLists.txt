# ---------------------------------------------------------------------------
# schedule-viz: render a Schedule's wave DAG. Emits standalone SVG by default
# (no dependencies); pass --dot for Graphviz DOT (pipe to `dot -Tsvg`).
#   ./schedule-viz            > schedule.svg
#   ./schedule-viz --dot | dot -Tsvg > schedule.svg
# ---------------------------------------------------------------------------
add_executable(schedule-viz schedule_viz_demo.cpp)
target_link_libraries(schedule-viz PRIVATE ecs::ecs)
# Resolve component/resource names via reflection so labels read "Position" etc.
target_compile_definitions(schedule-viz PRIVATE ECS_REFLECT_NAMES=1)
target_compile_options(schedule-viz PRIVATE $<$<CONFIG:Release>:-O2>)

# ---------------------------------------------------------------------------
# schedule-profiler: run a sample schedule under the ScheduleReport + ScheduleTrace
# observers, printing per-system/wave/tick timing distributions and writing a
# per-tick CSV trace. The observer tools are always available (no compile gate).
#   ./schedule-profiler
# ---------------------------------------------------------------------------
add_executable(schedule-profiler schedule_profiler_demo.cpp)
target_link_libraries(schedule-profiler PRIVATE ecs::ecs ecs::profiler)
target_compile_options(schedule-profiler PRIVATE $<$<CONFIG:Release>:-O2>)
