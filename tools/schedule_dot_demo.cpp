// tools/schedule_dot_demo.cpp -- render a sample schedule's wave DAG.
// Emits standalone SVG by default (no Graphviz needed); pass --dot for DOT.
//   ./schedule-dot          > schedule.svg
//   ./schedule-dot --dot | dot -Tsvg > schedule.svg
#include "ecs/ecs.hpp"
#include "ecs/schedule_dot.hpp"

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
    sched.add_once(
        "spawn_world", [](Commands&) {}, phase<-1> {});

    // gameplay (phase 0)
    sched.add("apply_gravity", [](Query<Velocity, Mass const>, Res<Gravity>) {});
    sched.add("integrate", [](Query<Position, Velocity const>) {});
    sched.add("collide", [](Query<Position, Velocity>) {});
    sched.add("damage", [](Query<Health>, ResMut<Score>) {}); // independent branch
    sched.add("render", [](WorldView) {});                    // reads-all -> after writers

    // teardown (phase 1)
    sched.add(
        "reap_dead", [](Commands&) {}, phase<1> {});
    sched.add(
        "debug_overlay", [](World&) {}, phase<1> {}); // exclusive -> runs alone

    viz::NameTable nt;
    nt.component<Position>("Position")
        .component<Velocity>("Velocity")
        .component<Mass>("Mass")
        .component<Health>("Health")
        .resource<Gravity>("Gravity")
        .resource<Score>("Score");

    bool const dot = argc > 1 && std::string_view(argv[1]) == "--dot";
    std::cout << (dot ? sched.to_dot(nt) : sched.to_svg(nt));
}
