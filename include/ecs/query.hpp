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
//   q.for_each_serial([](auto& p, auto& v){ p.x += v.x; }); // + optional Entity
//       SERIAL per-entity: never uses the pool. Each component is passed by
//       reference (a write-through proxy under P2996, a gathered local on the
//       portable backend), accessed as p.x. The path for kernels that touch shared
//       state (reductions, ordered gathers, command recording).
//
//   q.for_each_parallel([](auto& p, auto& v){ p.x += v.x; }); // + optional Entity
//       PARALLEL per-entity: same as for_each_serial but splits the rows across the
//       WorkerPool lanes -- for INDEPENDENT per-entity work only.
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

#include "chunk.hpp"
#include "detail/for_each_row.hpp"
#include "parallel/worker_pool.hpp"
#include "world.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace ecs {

namespace detail {
    template <class P>
    struct query_param_traits; // schedule/system.hpp; befriended for slicing
}

template <class... Cs>
class Query {
    template <class C>
    using bare = std::remove_const_t<C>; // component type minus the read-only mark

    template <class P>
    friend struct detail::query_param_traits;

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

    // The matching archetype list, memoized per (query type, thread) against
    // the world's archetype generation: the steady state is two relaxed-ish
    // loads and two compares -- no mutex, no signature hash -- while a new
    // matching archetype (created at flush; the generation only changes there)
    // or a different World forces one locked re-lookup. The world instance id
    // guards against a destroyed World's address being reused.
    [[nodiscard]]
    std::vector<ArchetypeId> const& matches() const {
        struct Cache {
            WorldId world                        = WorldId::none();
            std::uint64_t gen                    = ~std::uint64_t {0};
            std::vector<ArchetypeId> const* list = nullptr;
        };
        thread_local Cache cache;
        auto const gen = world_.archetype_generation();
        if (cache.world == world_.instance_id() && cache.gen == gen)
            return *cache.list;
        auto const& list = world_.matching_archetypes(required());
        cache            = {world_.instance_id(), gen, &list};
        return list;
    }

    // A Query restricted to rows [b, e) of ONE matching archetype -- how a
    // kernel system's work item binds its Query parameter (the executor owns
    // the slicing; see schedule/executor.hpp). Every iteration method then
    // walks just that slice, inline on the calling lane. Constructed only by
    // detail::query_param_traits (befriended above).
    Query(World& world,
          WorkerPool& pool,
          ArchetypeId slice_archetype,
          std::size_t slice_begin,
          std::size_t slice_end)
        : world_(world)
        , pool_(pool)
        , slice_arch_(slice_archetype)
        , slice_b_(slice_begin)
        , slice_e_(slice_end) {}

    static constexpr ArchetypeId kNoSlice {0xFFFF'FFFFu};

public:
    // `pool` is the data-parallel WorkerPool the executor binds. Ad-hoc queries
    // (query()/WorldView, outside a schedule) reference a shared 1-lane serial pool,
    // so pool_ is always valid -- no null check on the iteration path.
    explicit Query(World& world, WorkerPool& pool)
        : world_(world)
        , pool_(pool) {}

    // for_each_serial: SERIAL per-element iteration -- never uses the pool. Hands
    // the kernel each entity's components with the same ergonomics as
    // for_each_parallel (a write-through proxy per component under P2996, gathered
    // references on the portable backend, accessed as p.x; plus an optional leading
    // Entity if the kernel declares one). This is the path for a kernel that touches
    // SHARED state -- a reduction into a resource, an ordered gather into a snapshot,
    // command recording -- since for_each_parallel/for_each_chunk may split the rows
    // across WorkerPool lanes.
    //
    //   q.for_each_serial([&](auto& p){ out.push_back({p.x, p.y}); });
    template <class F>
    void for_each_serial(F&& fn) {
        if (slice_arch_ != kNoSlice) {
            auto& arch = *world_.archetypes()[slice_arch_];
            detail::for_each_row(
                fn,
                std::span(arch.entities).subspan(slice_b_, slice_e_ - slice_b_),
                chunk_arg<Cs>(arch, slice_b_, slice_e_)...);
            return;
        }
        auto const& archs = world_.archetypes();
        for (auto const ai : matches()) {
            auto& arch      = *archs[ai];
            auto const ents = std::span(arch.entities);
            detail::for_each_row(fn, ents, chunk_arg<Cs>(arch, 0, arch.size())...);
        }
    }

    // for_each_parallel: PARALLEL per-element iteration -- built on for_each_chunk,
    // so it splits an archetype's rows across the WorkerPool lanes. Use it for
    // INDEPENDENT per-entity work (each row touches only its own components); for a
    // kernel that writes shared state use for_each_serial instead. Same ergonomics
    // as for_each_serial: under P2996 a write-through row<C> proxy per component
    // (named fields p.x reference this entity's SoA slot in place, so it vectorizes
    // like a hand index loop), with an optional leading Entity if declared:
    //
    //   q.for_each_parallel([](auto& p, auto& v){ p.x += v.x * dt; });      // rows
    //   q.for_each_parallel([](Entity e, auto& p, auto& v){ ...use e... }); // +entity
    //
    // Both backends split rows across the pool's lanes (lanes own disjoint rows, so
    // it is race-free). Under P2996 each row is a zero-copy proxy -- it touches only
    // the fields the kernel uses, so it costs the same as for_each_chunk. The
    // portable backend (no field names) instead gathers the WHOLE component into a
    // local and scatters the mutable fields back, regardless of what the kernel
    // touches: cheap for a small component the kernel mostly uses, but it scales with
    // the field count and is severe for a WIDE component read/written sparsely or one
    // with a non-trivially-copyable field (e.g. std::string) -- there, on the
    // portable backend, prefer for_each_chunk. Quantified by benchmarks/gather_bench.
    template <class F>
    void for_each_parallel(F&& fn) {
        for_each_chunk([&fn](std::span<Entity> ents, chunk<Cs>... cs) {
            detail::for_each_row(fn, ents, cs...);
        });
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
        if (slice_arch_ != kNoSlice) {
            // Sliced (kernel-item) query: exactly this row range, inline on
            // the calling lane -- the executor already parallelized the items.
            auto& arch = *world_.archetypes()[slice_arch_];
            fn(std::span(arch.entities).subspan(slice_b_, slice_e_ - slice_b_),
               chunk_arg<Cs>(arch, slice_b_, slice_e_)...);
            return;
        }
        auto const& archs   = world_.archetypes();
        auto const& matched = matches();
        // ONE dispatch over the query's total row count, with each lane mapping
        // its global range back onto (archetype, row-range) segments -- rather
        // than a full fork-join barrier per archetype. This also makes the
        // pool's serial threshold apply to the query total, not per archetype
        // (a fragmented world used to run entirely serial even with millions of
        // total rows), and empty archetypes are skipped instead of invoking the
        // kernel with zero rows.
        std::size_t total = 0;
        for (auto const ai : matched)
            total += archs[ai]->size();
        if (total == 0)
            return;
        auto run = [&](std::size_t b, std::size_t e) {
            std::size_t base = 0;
            for (auto const ai : matched) {
                auto& arch          = *archs[ai];
                std::size_t const n = arch.size();
                if (base + n > b) {
                    std::size_t const lo = b > base ? b - base : 0;
                    std::size_t const hi = std::min(n, e - base);
                    if (lo < hi)
                        fn(std::span(arch.entities).subspan(lo, hi - lo),
                           chunk_arg<Cs>(arch, lo, hi)...);
                }
                base += n;
                if (base >= e)
                    break;
            }
        };
        pool_.parallel_for(total, run); // a 1-lane pool runs [0, total) inline
    }

    [[nodiscard]]
    std::size_t count() const {
        if (slice_arch_ != kNoSlice)
            return slice_e_ - slice_b_;
        std::size_t n = 0;
        for (auto const ai : matches())
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

    World& world_;
    WorkerPool& pool_; // data-parallel lanes; a shared 1-lane pool for ad-hoc queries
    // Slice restriction for kernel-item queries (kNoSlice = iterate all
    // matching archetypes, the normal mode).
    ArchetypeId slice_arch_ = kNoSlice;
    std::size_t slice_b_    = 0;
    std::size_t slice_e_    = 0;
};

template <class... Cs>
Query<Cs...> query(World& world) {
    return Query<Cs...>(world, parallel::serial_pool());
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
        return Query<std::remove_const_t<Cs> const...>(*world_, parallel::serial_pool());
    }

private:
    World* world_;
};

} // namespace ecs
