add_executable(nbody nbody.cpp)
target_link_libraries(nbody PRIVATE ecs::ecs)
# A light optimization level so the SoA timing is meaningful.
target_compile_options(nbody PRIVATE $<$<CONFIG:Release>:-O2>)
