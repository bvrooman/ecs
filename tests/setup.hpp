// Test helper: populate/mutate a world via a one-shot setup system run inline.
// This is the production idiom (add_once + an inline schedule run) wrapped for
// brevity in tests -- there is no World::run_once anymore.
#pragma once

#include "ecs/ecs.hpp"

template <class Fn>
inline void setup(ecs::World& world, Fn fn) {
  ecs::Schedule s;
  s.add_once("setup", std::move(fn));
  s.run(world); // inline: no thread pool needed
}
