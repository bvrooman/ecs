// ecs/world.hpp
//
// Umbrella for the world: entity lifecycle, archetype transitions, and the
// resource registry. The pieces live in world/ (mirroring schedule/):
//
//   world/world.hpp     the World class -- entity table, archetype tables,
//                       resources, and the immediate mutation primitives
//   world/commands.hpp  Commands -- the deferred mutation API systems see
//   world/res.hpp       Res<T> / ResMut<T> -- typed resource parameters
//
// Include this header; the split is an implementation layout, not an API.

#pragma once

#include <ecs/world/commands.hpp>
#include <ecs/world/res.hpp>
#include <ecs/world/view.hpp>
#include <ecs/world/world.hpp>
