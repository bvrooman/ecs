// ecs/query.hpp
//
// A Query selects every archetype whose signature contains all of the
// requested component types and iterates the matching entities.
//
// A component may be written `const` in the query type to mark it read-only --
// `query<const Position, Velocity>` reads Position and read-writes Velocity.
// Read-only components are passed by const reference and are NOT written back,
// which avoids the cost (and the undeclared-write hazard) of scattering a
// component the system only reads. When a Query is used as a system parameter,
// the scheduler derives that system's read/write access directly from these
// const/non-const marks, so the declared access cannot drift from actual use.
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
//       read-only components) so you can pull contiguous per-field spans and
//       run tight, vectorizer-friendly loops with no per-element
//       gather/scatter.

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
    // stable after first use, so this is computed on the first query of this
    // type.
    static Signature const& required() {
        static auto const s = [] {
            auto v = Signature {component_id<bare<Cs>>...};
            std::ranges::sort(v);
            return v;
        }();
        return s;
    }

public:
    explicit Query(World& world)
        : world_(world) {}

    template <class F>
    void each(F&& fn) {
        auto const& archs = world_.archetypes();
        for (auto const ai : world_.matching_archetypes(required())) {
            auto& arch   = *archs[ai];
            auto stores  = std::tie(arch.column<bare<Cs>>().store...);
            auto const n = arch.size();
            for (auto row = 0; row < n; ++row)
                invoke_row(fn,
                           arch.entities[row],
                           row,
                           stores,
                           std::index_sequence_for<Cs...> {});
        }
    }

    template <class F>
    void for_each_chunk(F&& fn) {
        auto const& archs = world_.archetypes();
        for (auto const ai : world_.matching_archetypes(required())) {
            auto& arch = *archs[ai];
            fn(std::span(arch.entities), chunk_arg<Cs>(arch)...);
        }
    }

    [[nodiscard]]
    std::size_t count() const {
        std::size_t n = 0;
        for (auto const ai : world_.matching_archetypes(required()))
            n += world_.archetypes()[ai]->size();
        return n;
    }

private:
    // Read-only components get a const storage reference (whose column<I>()
    // yields spans of const), mutable ones a non-const reference.
    template <class C>
    static auto& chunk_arg(Archetype& arch) {
        auto& store = arch.column<bare<C>>().store;
        if constexpr (std::is_const_v<C>)
            return std::as_const(store);
        else
            return store;
    }

    // Gather each component into a local, call fn with references whose
    // constness matches the query type, then scatter back only the mutable
    // components.
    template <class F, class Stores, std::size_t... I>
    static void invoke_row(
        F& fn, Entity e, std::size_t row, Stores& stores, std::index_sequence<I...>) {
        auto locals = std::tuple<bare<Cs>...> {std::get<I>(stores).gather(row)...};
        fn(e, static_cast<Cs&>(std::get<I>(locals))...);
        (write_back<Cs>(std::get<I>(stores), row, std::get<I>(locals)), ...);
    }

    template <class C, class Store, class Local>
    static void write_back(Store& store, std::size_t row, Local const& local) {
        if constexpr (!std::is_const_v<C>)
            store.set(row, local);
    }

    World& world_;
};

template <class... Cs>
Query<Cs...> query(World& world) {
    return Query<Cs...>(world);
}

// A read-only view of the world for systems that need ad-hoc reads (size, get,
// has, alive, queries) beyond what Query/Res express. Everything it exposes is
// read-only -- its query() forces every component to const, so nothing can be
// written through it. As a system parameter it therefore declares "reads
// everything": it runs concurrently with other readers but is serialized
// against any writer (unlike a raw World&, which is fully exclusive).
class WorldView {
public:
    explicit WorldView(World& world)
        : world_(&world) {}

    [[nodiscard]]
    auto size() const {
        return world_->size();
    }
    [[nodiscard]]
    auto alive(Entity const e) const {
        return world_->alive(e);
    }
    template <class C>
    [[nodiscard]]
    auto has(Entity const e) const {
        return world_->has<C>(e);
    }
    template <class C>
    auto get(Entity const e) const {
        return world_->get<C>(e);
    }
    // Read-only query: every component is iterated by const reference.
    template <class... Cs>
    auto query() const {
        return Query<std::remove_const_t<Cs> const...>(*world_);
    }

private:
    World* world_;
};

} // namespace ecs
