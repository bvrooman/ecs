// ecs/query.hpp
//
// A Query selects every archetype whose signature contains all of the
// requested component types and iterates the matching entities.
//
// A component may be written `const` in the query type to mark it read-only --
// `query<const Position, Velocity>` reads Position and read-writes Velocity.
// Read-only components are passed by const reference and are NOT written back,
// which avoids the cost (and the undeclared-write hazard) of scattering a
// component the system only reads. (Note: the query type is independent of the
// system's reads<>/writes<> access tags; keep them consistent.)
//
//   q.each([](Entity e, const Position& p, Velocity& v){ ... });
//       Ergonomic, per-entity. Components are gathered from their per-field
//       columns into locals, passed by reference, and the mutable (non-const)
//       ones scattered back. Convenience path; prefer for_each_chunk when hot.
//
//   q.for_each_chunk([](std::span<Entity> ents,
//                       const soa_storage<Position>& pos,
//                       soa_storage<Velocity>& vel){ ... });
//       The SoA fast path: hands you whole archetype columns (const for
//       read-only components) so you can pull contiguous per-field spans and run
//       tight, vectorizer-friendly loops with no per-element gather/scatter.

#pragma once

#include "world.hpp"

#include <algorithm>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ecs {

template <class... Cs>
class Query {
  template <class C>
  using bare = std::remove_const_t<C>; // component type minus the read-only mark

  // The sorted set of required component ids, built once. Component ids are
  // stable after first use, so this is computed on the first query of this type.
  static const Signature& required() {
    static const Signature s = [] {
      Signature v{component_id<bare<Cs>>...};
      std::sort(v.begin(), v.end());
      return v;
    }();
    return s;
  }

public:
  explicit Query(World& world) : world_(world) {}

  template <class F>
  void each(F&& fn) {
    auto& archs = world_.archetypes();
    for (std::uint32_t ai : world_.matching_archetypes(required())) {
      auto& arch = *archs[ai];
      auto stores = std::tie(arch.template column<bare<Cs>>().store...);
      const auto n = arch.size();
      for (std::size_t row = 0; row < n; ++row)
        invoke_row(fn, arch.entities[row], row, stores,
                   std::index_sequence_for<Cs...>{});
    }
  }

  template <class F>
  void for_each_chunk(F&& fn) {
    auto& archs = world_.archetypes();
    for (std::uint32_t ai : world_.matching_archetypes(required())) {
      auto& arch = *archs[ai];
      fn(std::span<Entity>(arch.entities), chunk_arg<Cs>(arch)...);
    }
  }

  std::size_t count() const {
    std::size_t n = 0;
    for (std::uint32_t ai : world_.matching_archetypes(required()))
      n += world_.archetypes()[ai]->size();
    return n;
  }

private:
  // Read-only components get a const storage reference (whose column<I>() yields
  // spans of const), mutable ones a non-const reference.
  template <class C>
  static auto& chunk_arg(Archetype& arch) {
    auto& store = arch.template column<bare<C>>().store;
    if constexpr (std::is_const_v<C>)
      return std::as_const(store);
    else
      return store;
  }

  // Gather each component into a local, call fn with references whose constness
  // matches the query type, then scatter back only the mutable components.
  template <class F, class Stores, std::size_t... I>
  static void invoke_row(F& fn, Entity e, std::size_t row, Stores& stores,
                         std::index_sequence<I...>) {
    std::tuple<bare<Cs>...> locals{std::get<I>(stores).gather(row)...};
    fn(e, static_cast<Cs&>(std::get<I>(locals))...);
    (write_back<Cs>(std::get<I>(stores), row, std::get<I>(locals)), ...);
  }

  template <class C, class Store, class Local>
  static void write_back(Store& store, std::size_t row, const Local& local) {
    if constexpr (!std::is_const_v<C>) store.set(row, local);
  }

  World& world_;
};

template <class... Cs>
Query<Cs...> query(World& world) {
  return Query<Cs...>(world);
}

} // namespace ecs
