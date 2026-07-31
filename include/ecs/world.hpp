// ecs/world.hpp
//
// Umbrella for the world: entity lifecycle, archetype transitions, and the
// resource registry. The pieces live in world/ (mirroring schedule/):
//
//   world/world.hpp     the World class -- entity table, archetype tables,
//                       resources, and the immediate mutation primitives
//   world/commands.hpp  Commands -- the deferred mutation API systems see
//   world/res.hpp       Res<T> / ResMut<T> -- typed resource parameters
//   world/view.hpp      WorldView -- the read-only ad-hoc read parameter
//   world/column_view.hpp  ColumnView<Cs...> -- read every row of named
//                       components (the gather parameter), declaring a read on
//                       exactly those
//
// Include this header; the split is an implementation layout, not an API.

#pragma once

#include <ecs/world/column_view.hpp>
#include <ecs/world/commands.hpp>
#include <ecs/world/res.hpp>
#include <ecs/world/view.hpp>
#include <ecs/world/world.hpp>
