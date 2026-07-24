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
//   q.for_each([](auto& p, auto& v){ p.x += v.x; }); // + optional Entity
//       ROW-shaped iteration: each component passed by reference (a write-through
//       proxy under P2996, a gathered local on the portable backend), accessed as
//       p.x, plus an optional leading Entity.
//
//   q.for_each_chunk([](std::span<Entity> ents,
//                       chunk<Position> pos, chunk<Velocity const> vel){ ... });
//       COLUMN-shaped (SoA) iteration: a `chunk` per component whose column<I>()
//       is that field's contiguous std::span (span<const F> for a read-only
//       component), so you run a tight, vectorizer-friendly loop -- `for (auto& x
//       : pos.column<0>())` -- touching only the fields you use with NO per-row
//       gather/scatter. Prefer it for a wide or sparsely-touched component on the
//       portable backend, where for_each reassembles the whole struct.
//
// A Query never dispatches: both forms iterate on the calling thread. Parallelism
// is the executor's job -- an add_kernel system slices its rows into work items
// across the pool's lanes, each binding a Query restricted to its slice.

#pragma once

#include "chunk.hpp"
#include "detail/for_each_row.hpp"
#include "schedule/work_item.hpp"
#include "world.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace ecs {

namespace detail {
    template <class P>
    struct system_param; // schedule/system.hpp; befriended for slicing
}

template <class... Cs>
class Query {
    template <class C>
    using bare = std::remove_const_t<C>; // component type minus the read-only mark

    template <class P>
    friend struct detail::system_param;

    // Duplicate component types (Query<A, A> or Query<A, const A>) would hand a
    // kernel two chunks aliasing the same column -- one possibly mutable --
    // exactly the aliasing the scheduler's analysis promises cannot happen.
    static_assert(detail::are_distinct_v<std::remove_const_t<Cs>...>,
                  "Query<Cs...>: duplicate component type (ignoring const)");

    // The sorted set of required component ids, built once. Component ids are
    // stable after first use, so this is computed on the first query of this
    // type.
    static Signature const& required() {
        static Signature const s {component_id<bare<Cs>>...}; // ctor sorts + dedups
        return s;
    }

    // A Query restricted to rows [b, e) of ONE matching archetype -- how a
    // kernel system's work item binds its Query parameter (the executor owns
    // the slicing; see schedule/executor.hpp). Every iteration method then
    // walks just that slice. Constructed only by detail::query_param_traits
    // (befriended above).
    Query(World& world, detail::WorkItem const& item)
        : world_(world)
        , item_(item) {}

public:
    // A Query is a pure iterator: it never dispatches. Parallelism is the
    // executor's job -- an add_kernel system's rows are sliced into work items
    // that each bind a restricted Query -- so a Query holds no WorkerPool.
    // explicit Query(World& world)
    //     : world_(world) {}

    // for_each: per-element iteration. Hands the kernel each entity's
    // components (a write-through proxy per component under P2996, a gathered
    // value on the portable backend, accessed as p.x), plus an optional leading
    // Entity if the kernel declares one. Row-shaped access; pair with
    // for_each_chunk when you want the raw SoA columns instead.
    //
    //   q.for_each([&](auto& p){ out.push_back({p.x, p.y}); });
    template <class F>
    void for_each(F&& fn) {
        for (auto const& [archetype, begin, end] : item_.units) {
            auto& arch    = *world_.archetypes()[archetype];
            auto entities = std::span(arch.entities).subspan(begin, end - begin);
            detail::for_each_row(fn, entities, chunk_arg<Cs>(arch, begin, end)...);
        }
    }

    // for_each_chunk: the SoA access shape. Calls the kernel once per matching
    // archetype (or once for a kernel-item's slice) with that archetype's
    // entities and a `chunk` per component -- each `chunk` exposing its columns
    // as contiguous spans, so you run a tight vectorizer-friendly loop over just
    // the fields you touch with NO per-row gather/scatter. Prefer it over
    // for_each for a wide or sparsely-touched component on the portable
    // backend (where for_each reassembles the whole struct per row). Like
    // for_each it never dispatches: parallelism comes only from an
    // add_kernel system's row slicing.
    //
    //   q.for_each_chunk([](std::span<Entity>,
    //                       chunk<Position> pos, chunk<Velocity const> vel) {
    //       auto px = pos.column<0>(); auto vx = vel.column<0>();
    //       for (std::size_t i = 0; i < px.size(); ++i) px[i] += vx[i]; // vectorizable
    //   });
    template <class F>
    void for_each_chunk(F&& fn) {
        for (auto const& [archetype, begin, end] : item_.units) {
            auto& arch    = *world_.archetypes()[archetype];
            auto entities = std::span(arch.entities).subspan(begin, end - begin);
            fn(entities, chunk_arg<Cs>(arch, begin, end)...);
        }
    }

    [[nodiscard]]
    std::size_t count() const {
        auto b = item_.begin;
        auto e = item_.end;
        return e - b;
    }

private:
    // Wrap this archetype's column for C in a chunk scoped to [b, e). The chunk's
    // constness follows C, so a read-only component yields spans of const.
    template <class C>
    static chunk<C> chunk_arg(Archetype& arch, std::size_t b, std::size_t e) {
        return chunk<C>(arch.column<bare<C>>().store, b, e);
    }

    World& world_;
    detail::WorkItem const& item_;
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
