// ecs/schedule/params/reduce.hpp
//
// Reduce<T, Op, Partial = T>: fold-shaped state (sums, bounds, grids) for
// kernel systems. Each item gets a private Partial; at the barrier the
// partials fold into the T RESOURCE with Op in ordinal order. The target is
// rebuilt every run (reset at prepare), like the serial rebuild-every-tick
// reductions it replaces.
//
// Partial defaults to T (a float sum folds float partials). Give it a
// different type when the target is large and dense but each item touches
// only a corner of it -- e.g. a spatial grid: Partial = a sparse map of
// touched cells, folded into the dense grid resource at the barrier, instead
// of one full grid copy per item.
//
// Declares a resource WRITE on T, so same-wave readers of T level after the
// system exactly as with ResMut -- but the physical writes are per-item-
// private during the dispatch, and the shared-target writes happen only in
// the single-threaded barrier hooks.

#pragma once

#include "protocol.hpp"
#include <cstddef>
#include <cstdint>
#include <span>

namespace ecs {

// Fold-shaped per-item state for a kernel system. `sum->field` / `*sum`
// reaches THIS ITEM's private Partial -- no sharing, no atomics. At the wave
// barrier the partials are folded into the T resource, in item-ordinal
// (serial-walk) order, with Op: any stateless callable
//   void operator()(T& into, Partial& partial) const
// (partial is mutable so Op may move out of it). Precondition, checked at
// bind: the world owns a T resource.
template <class T, class Op, class Partial = T>
class Reduce {
public:
    [[nodiscard]]
    Partial& operator*() const noexcept {
        return *partial_;
    }
    Partial* operator->() const noexcept { return partial_; }

private:
    template <class P>
    friend struct detail::kernel_param;
    explicit Reduce(Partial& partial) noexcept
        : partial_(&partial) {}
    Partial* partial_;
};

namespace detail {

    template <class T, class Op, class Partial>
    struct kernel_param<Reduce<T, Op, Partial>> {
        static constexpr bool allowed = true;
        struct state {
            slot_array<Partial> parts;
        };

        static void declare(SystemAccess& a) { a.res_writes.push_back(resource_id<T>); }

        static void prepare(state& s,
                            World& w,
                            std::span<std::uint32_t const> rows,
                            KernelWaveContext const&) {
            s.parts.prepare(rows.size());
            // The target holds THIS run's reduction: rebuilt every run, like
            // the serial rebuild-every-tick systems Reduce replaces.
            reset_value(w.resource<T>());
        }

        static void finish(state& s, World& w) {
            auto& target = w.resource<T>();
            Op op {};
            for (std::size_t i = 0; i < s.parts.active; ++i)
                op(target, s.parts[i]);
        }

        static Reduce<T, Op, Partial> bind(state& s,
                                           World&,
                                           Commands&,
                                           WorkItem const& item) {
            auto ordinal = item.ordinal;
            return Reduce<T, Op, Partial>(s.parts[ordinal]);
        }
    };

} // namespace detail
} // namespace ecs
