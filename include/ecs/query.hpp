// ecs/query.hpp
//
// A Query selects every archetype whose signature contains all of the
// requested component types and iterates the matching entities.
//
// Two access styles:
//
//   q.each([](Entity e, Position& p, Velocity& v){ ... });
//       Ergonomic, per-entity. Components are gathered from their per-field
//       columns into locals, passed by reference, then scattered back -- so
//       mutation works regardless of the underlying SoA layout.
//
//   q.for_each_chunk([](std::span<Entity> ents,
//                       soa_storage<Position>& pos,
//                       soa_storage<Velocity>& vel){ ... });
//       The SoA fast path: hands you whole archetype columns so you can pull
//       contiguous per-field spans (pos.column<0>() ...) and run tight,
//       vectorizer-friendly loops with no per-element gather/scatter.

#pragma once

#include "world.hpp"

#include <span>
#include <tuple>

namespace ecs {

template <class... Cs>
class Query {
public:
  explicit Query(World& world) : world_(world) {}

  template <class F>
  void each(F&& fn) {
    for (auto& arch_ptr : world_.archetypes()) {
      Archetype& arch = *arch_ptr;
      if (!matches(arch)) continue;
      auto stores = std::tie(arch.template column<Cs>().store...);
      const std::size_t n = arch.size();
      for (std::size_t row = 0; row < n; ++row)
        invoke_row(fn, arch.entities[row], row, stores,
                   std::index_sequence_for<Cs...>{});
    }
  }

  template <class F>
  void for_each_chunk(F&& fn) {
    for (auto& arch_ptr : world_.archetypes()) {
      Archetype& arch = *arch_ptr;
      if (!matches(arch)) continue;
      fn(std::span<Entity>(arch.entities),
         arch.template column<Cs>().store...);
    }
  }

  std::size_t count() const {
    std::size_t n = 0;
    for (auto& arch_ptr : world_.archetypes())
      if (matches(*arch_ptr)) n += arch_ptr->size();
    return n;
  }

private:
  static bool matches(const Archetype& arch) {
    return (arch.has(component_id<Cs>) && ...);
  }

  // Gather each requested component into a local, call fn with references,
  // then scatter the (possibly modified) locals back.
  template <class F, class Stores, std::size_t... I>
  static void invoke_row(F& fn, Entity e, std::size_t row, Stores& stores,
                         std::index_sequence<I...>) {
    std::tuple<Cs...> locals{std::get<I>(stores).gather(row)...};
    fn(e, std::get<I>(locals)...);
    ((std::get<I>(stores).set(row, std::get<I>(locals))), ...);
  }

  World& world_;
};

template <class... Cs>
Query<Cs...> query(World& world) {
  return Query<Cs...>(world);
}

} // namespace ecs
