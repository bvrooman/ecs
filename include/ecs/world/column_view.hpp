// ecs/world/column_view.hpp
//
// ColumnView<Cs...>: read every row of {Cs...}, declaring a read on exactly
// those components.
//
// A system's Query is bound to ONE work item, so it shows the body only that
// item's row slice -- which is what makes a parallel write race-free by
// construction. An algorithm whose kernel is a GATHER (an n-body tree pass
// reading other nodes' expansions, a solver reading its neighbours' cells,
// anything indexing a spatial structure) needs to read rows OUTSIDE that
// slice. WorldView already permits it, but at the cost of declaring
// "reads everything", which serializes the system against every writer in its
// phase -- including writers it has nothing to do with.
//
// ColumnView is that same access with an honest declaration:
//
//   sched.add_parallel("m2l", [](Query<Local, Node const> q,   // writes own rows
//                                ColumnView<Multipole> src) {  // reads all rows
//       src.for_each_chunk([&](std::span<Entity const>, chunk<Multipole const> m) {
//           auto const col = m.column<0>();      // EVERY row, contiguous
//           ...
//       });
//   });
//
// Two properties follow, and both matter for the gather case:
//
//   * Conflict analysis stays precise. The system above conflicts with writers
//     of Multipole and Local -- not with an unrelated system writing Velocity,
//     which a WorldView would have serialized it against.
//   * You can name the archetypes you mean. The view matches every archetype
//     CONTAINING {Cs...}, so listing a tag component narrows it: with one
//     archetype per tree level, ColumnView<Multipole, Level<3>> reads exactly
//     level 3 -- provably disjoint from a query writing level 2, rather than
//     disjoint only by the author's argument.
//
// It is strictly read-only (every component is observed as chunk<C const>), so
// it never needs a write declaration; the system's own writes still go through
// its Query, one item's rows at a time. What the library CANNOT check is that a
// body's reads and its own writes do not overlap when both touch the same
// component: items write disjoint rows, but a ColumnView can read any row,
// including one a sibling item is writing. Keep the read set disjoint from the
// write set (different components, or different archetypes as above).

#pragma once

#include <ecs/world/world.hpp>

#include <cstddef>
#include <type_traits>

namespace ecs {

template <class... Cs>
class ColumnView {
    // Duplicates would hand a kernel two chunks over the same column. Harmless
    // here (both are const) but always a mistake, and Query rejects it too.
    static_assert(detail::are_distinct_v<std::remove_const_t<Cs>...>,
                  "ColumnView<Cs...>: duplicate component type (ignoring const)");

public:
    explicit ColumnView(World const& world)
        : world_(&world) {}

    // Once per matching archetype, with that archetype's FULL columns --
    // the same chunk type a Query hands out, spanning [0, archetype size)
    // instead of one item's slice.
    template <class F>
    void for_each_chunk(F&& fn) const {
        world_->for_each_chunk<std::remove_const_t<Cs>...>(static_cast<F&&>(fn));
    }
    // Row-shaped iteration over the same rows. Prefer for_each_chunk for the
    // gather case: this reassembles each component per row on the portable
    // reflection backend, which a random-access kernel pays for nothing.
    template <class F>
    void for_each(F&& fn) const {
        world_->for_each<std::remove_const_t<Cs>...>(static_cast<F&&>(fn));
    }
    [[nodiscard]]
    std::size_t count() const {
        return world_->count<std::remove_const_t<Cs>...>();
    }

private:
    World const* world_;
};

} // namespace ecs
