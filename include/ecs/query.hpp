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
//                       chunk<Position> pos, chunk<Velocity const> vel){ ... });
//       The SoA fast path: hands each lane a `chunk` per component, already
//       scoped to that lane's slice of the archetype's rows. chunk::column<I>()
//       returns that field's contiguous std::span for this lane (span<const F>
//       for a read-only component), so you run a tight, vectorizer-friendly loop
//       -- `for (auto& x : pos.column<0>())` -- with no per-element
//       gather/scatter and no begin/end bookkeeping. The executor splits each
//       archetype's rows across the WorkerPool's lanes; because a chunk exposes
//       only its own slice, writing through it is data-parallel and race-free.

#pragma once

#include "parallel/worker_pool.hpp"
#include "reflection/field_view.hpp"
#include "world.hpp"
#include <algorithm>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ecs {

// chunk<C>: a lane's view of component C over the row range the executor handed
// it. for_each_chunk gives each lane one chunk per component, already scoped to
// that lane's rows. Access this lane's slice of a field either by name -- pos.x,
// pos.y (from the reflected member names, span<const F> when C is const) -- or by
// index, column<I>(); both denote the same std::span. The kernel iterates with
// `for (auto& v : pos.x)` (or column<I>()[i], 0-based within the chunk) and never
// handles a begin/end; because a chunk exposes only its own slice, writing through
// a mutable one cannot touch another lane's rows, which is what keeps the
// deterministic split race-free. Trivially copyable (a storage pointer + the
// range + the field spans), so it is passed by value like a span.
//
// The named accessors are the field spans of a generated field_view<C> base and
// exist only on the P2996 reflection backend (it alone recovers member names); on
// the portable backend chunk carries no base and column<I>() is the only path.
template <class C>
class chunk
#if ECS_USE_P2996
    : public field_view<C> // named field spans: pos.x, pos.y, ... (populated below)
#endif
{
    using bare    = std::remove_const_t<C>;
    using store_t = soa_storage<bare>;
    using storage = std::conditional_t<std::is_const_v<C>, store_t const, store_t>;

#if ECS_USE_P2996
    static constexpr std::size_t field_count = reflect::field_count_v<bare>;
    static_assert(
        !detail::field_name_collides_with_chunk<bare>(),
        "ecs::chunk: a component field is named size/empty/column, which shadows "
        "chunk's own members; rename the field or read it via column<I>().");
#endif

public:
    chunk(storage& store, std::size_t begin, std::size_t end) noexcept
#if ECS_USE_P2996
        : chunk(store, begin, end, std::make_index_sequence<field_count> {}) {}
#else
        : store_(store)
        , begin_(begin)
        , end_(end) {}
#endif

    // This lane's contiguous slice of field I (span<const F> if C is const). Same
    // span the named accessor denotes; the portable, always-available form.
    template <std::size_t I>
    [[nodiscard]]
    auto column() const noexcept {
        return store_.template column<I>().subspan(begin_, end_ - begin_);
    }

    [[nodiscard]]
    std::size_t size() const noexcept {
        return end_ - begin_;
    }
    [[nodiscard]]
    bool empty() const noexcept {
        return begin_ == end_;
    }

private:
#if ECS_USE_P2996
    // Populate the field_view<C> base: each named span is this lane's [begin, end)
    // slice of the matching field column, so pos.x aliases column<0>() and so on.
    template <std::size_t... I>
    chunk(storage& store,
          std::size_t begin,
          std::size_t end,
          std::index_sequence<I...>) noexcept
        : field_view<C> {store.template column<I>().subspan(begin, end - begin)...}
        , store_(store)
        , begin_(begin)
        , end_(end) {}
#endif

    storage& store_;
    std::size_t begin_, end_;
};

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
    // `pool` is the data-parallel WorkerPool the executor binds (null for ad-hoc
    // queries, which then iterate serially).
    explicit Query(World& world, WorkerPool* pool = nullptr)
        : world_(world)
        , pool_(pool) {}

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

    // The SoA fast path. Splits each matching archetype's rows across the bound
    // WorkerPool's lanes and calls the kernel once per (archetype x lane) with
    // that lane's entities and a `chunk` per component, each already scoped to
    // the lane's row slice (const for read-only components). A chunk exposes only
    // its own rows, so the deterministic partition is race-free with no locking.
    // A 1-lane pool (or an ad-hoc query with no pool) runs the whole archetype in
    // one call, so the same kernel serves serial and parallel.
    //
    //   q.for_each_chunk([](std::span<Entity>,
    //                       chunk<Position> pos, chunk<Velocity const> vel) {
    //       auto px = pos.column<0>(); auto vx = vel.column<0>();
    //       for (std::size_t i = 0; i < px.size(); ++i) px[i] += vx[i]; // vectorizable
    //   });
    template <class F>
    void for_each_chunk(F&& fn) {
        auto const& archs = world_.archetypes();
        for (auto const ai : world_.matching_archetypes(required())) {
            auto& arch      = *archs[ai];
            auto const ents = std::span(arch.entities);
            auto const n    = arch.size();
            auto run        = [&](std::size_t b, std::size_t e) {
                fn(ents.subspan(b, e - b), chunk_arg<Cs>(arch, b, e)...);
            };
            if (pool_)
                pool_->parallel_for(n, run);
            else
                run(0, n);
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
    // Wrap this archetype's column for C in a chunk scoped to [b, e). The chunk's
    // constness follows C, so a read-only component yields spans of const.
    template <class C>
    static chunk<C> chunk_arg(Archetype& arch, std::size_t b, std::size_t e) {
        return chunk<C>(arch.column<bare<C>>().store, b, e);
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
    WorkerPool* pool_ = nullptr; // data-parallel lanes for for_each_chunk
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
