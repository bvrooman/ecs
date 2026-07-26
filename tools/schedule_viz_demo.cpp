// tools/schedule_viz_demo.cpp -- render a sample schedule's wave DAG.
// Emits standalone SVG by default (no Graphviz needed); pass --dot for DOT.
//   ./schedule-viz          > schedule.svg
//   ./schedule-viz --dot | dot -Tsvg > schedule.svg
#include <ecs/ecs.hpp>
#include "schedule_viz.hpp"
#include <iostream>
#include <string_view>

struct Position {
    float x, y;
};
struct Velocity {
    float x, y;
};
struct Mass {
    float kg;
};
struct Health {
    int hp;
};
struct Gravity {
    float g;
};
struct Score {
    int v;
};

int main(int argc, char** argv) {
    using namespace ecs;
    Schedule sched;

    // startup: spawn the world (Commands = untracked side channel) -- phase<-1>
    sched.add_once("spawn_world", [](Commands&) {}, phase<-1> {});

    // gameplay (phase 0)
    sched.add("apply_gravity", [](Query<Velocity, Mass const>, Res<Gravity>) {});
    sched.add("integrate", [](Query<Position, Velocity const>) {});
    sched.add("collide", [](Query<Position, Velocity>) {});
    sched.add("damage", [](Query<Health>, ResMut<Score>) {}); // independent branch
    sched.add("render", [](WorldView) {}); // reads-all -> after writers

    // teardown (phase 1)
    sched.add("reap_dead", [](Commands&) {}, phase<1> {});
    // A declared-exclusive system (raw World& is not a system parameter; a
    // genuinely unanalyzable system says so explicitly via add_dynamic).
    ecs::SystemAccess excl;
    excl.exclusive = true;
    sched.add_dynamic("debug_overlay",
                      excl,
                      [](ecs::World&, ecs::Commands&, ecs::detail::WorkItem const&) {},
                      /*phase=*/1); // exclusive -> runs alone

    // Component/resource names come from reflection at build time (this target
    // is compiled with ECS_REFLECT_NAMES=1) -- no manual name table. Pass a
    // viz::NameTable only to override or alias specific ids.
    bool const dot = argc > 1 && std::string_view(argv[1]) == "--dot";
    std::cout << (dot ? viz::to_dot(sched) : viz::to_svg(sched));
}
