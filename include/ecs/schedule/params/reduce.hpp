// ecs/schedule/params/reduce.hpp
//
// Reduce<T, Op>: fold-shaped state (sums, bounds, grids) for kernel systems.
// Each item gets a private T partial; at the barrier the partials fold into
// the T RESOURCE with Op in ordinal order. The target is rebuilt every run
// (reset at prepare), like the serial rebuild-every-tick reductions it
// replaces.
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
#include <vector>

namespace ecs {

// Fold-shaped per-item state for a kernel system. `sum->field` / `*sum`
// reaches THIS ITEM's private partial of T -- no sharing, no atomics. At the
// wave barrier the partials are folded into the T resource, in item-ordinal
// (serial-walk) order, with Op: any stateless callable
//   void operator()(T& into, T& partial) const
// (partial is mutable so Op may move out of it). Precondition, checked at
// bind: the world owns a T resource.
template <class T, class Op>
class Reduce {
public:
    [[nodiscard]]
    T& operator*() const noexcept {
        return *partial_;
    }
    T* operator->() const noexcept { return partial_; }

private:
    template <class P>
    friend struct detail::kernel_param;
    explicit Reduce(T& partial) noexcept
        : partial_(&partial) {}
    T* partial_;
};

namespace detail {

    template <class P>
    inline constexpr bool is_reduce_v = false;
    template <class T, class Op>
    inline constexpr bool is_reduce_v<Reduce<T, Op>> = true;

    template <class T, class Op>
    struct kernel_param<Reduce<T, Op>> {
        static constexpr bool allowed = true;
        // Slots are cache-line padded: items accumulate into their slot
        // throughout iteration, and adjacent small partials would otherwise
        // false-share across lanes. ~kTargetItemsPerKernel slots per system,
        // so the padding costs a few KB.
        struct alignas(128) Slot {
            T value {};
        };
        struct state {
            std::vector<Slot> slots;
            std::size_t active = 0;
        };

        static void declare(SystemAccess& a) { a.res_writes.push_back(resource_id<T>); }

        static void prepare(state& s,
                            World& w,
                            std::span<std::uint32_t const> rows,
                            KernelWaveContext const&) {
            s.active = rows.size();
            if (s.slots.size() < s.active)
                s.slots.resize(s.active);
            for (std::size_t i = 0; i < s.active; ++i)
                reset_value(s.slots[i].value);
            // The target holds THIS run's reduction: rebuilt every run, like
            // the serial rebuild-every-tick systems Reduce replaces.
            reset_value(w.resource<T>());
        }

        static void finish(state& s, World& w) {
            auto& target = w.resource<T>();
            Op op {};
            for (std::size_t i = 0; i < s.active; ++i)
                op(target, s.slots[i].value);
        }

        static Reduce<T, Op> bind(state& s,
                                  World&,
                                  Commands&,
                                  std::uint32_t,
                                  std::size_t,
                                  std::size_t,
                                  std::uint32_t ordinal) {
            return Reduce<T, Op>(s.slots[ordinal].value);
        }
    };

} // namespace detail
} // namespace ecs
