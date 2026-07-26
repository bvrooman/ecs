// Test helpers: run one system once, inline. This is the production idiom
// (a times=1 system + an inline schedule run) wrapped for brevity in tests -- there is
// no World::run_once anymore.
//
//   setup(w, fn)        structural setup: spawn/add/remove via Commands
//   run_system(w, fn)   apply a real system -- e.g. a Query that MUTATES
//                       component values (ad-hoc World iteration is read-only,
//                       so writes go through a system's declared-write Query).
#pragma once

#include <ecs/ecs.hpp>

template <class Fn>
void run_system(ecs::World& world, Fn fn) {
    ecs::Schedule s;
    s.add_serial("system", std::move(fn), ecs::times {1});
    s.run(world); // inline: no thread pool needed
}

template <class Fn>
void setup(ecs::World& world, Fn fn) {
    run_system(world, std::move(fn));
}
