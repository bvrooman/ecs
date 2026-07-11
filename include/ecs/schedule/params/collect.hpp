// ecs/schedule/params/collect.hpp
//
// Collect<T>: filtered gather (kill lists, visibility sets, target lists) for
// kernel systems -- the Extract shape when output size is NOT known up front.
// Each item appends into a private T partial; at the barrier the partials are
// concatenated into the T RESOURCE in ordinal order, so the result is exactly
// what a serial filtered walk would have produced. The target is rebuilt
// every run, like Reduce.

#pragma once

#include "protocol.hpp"
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <vector>

namespace ecs {

// A collect target: clearable, and appendable from another instance of
// itself (vectors, deques, strings...).
template <class T>
concept CollectTarget = requires(T& into, T& part) {
    into.clear();
    into.insert(into.end(),
                std::make_move_iterator(part.begin()),
                std::make_move_iterator(part.end()));
};

// Filtered-gather output for a kernel system. `out->push_back(x)` / `*out`
// reaches THIS ITEM's private partial of T -- no sharing, no atomics, no
// pre-counting. At the wave barrier the partials are moved-appended into the
// T resource in item-ordinal (serial-walk) order: the collected sequence is
// identical to a serial `for_each_serial` filter at any lane count.
// Precondition, checked at prepare: the world owns a T resource.
template <CollectTarget T>
class Collect {
public:
    [[nodiscard]]
    T& operator*() const noexcept {
        return *partial_;
    }
    T* operator->() const noexcept { return partial_; }

private:
    template <class P>
    friend struct detail::kernel_param;
    explicit Collect(T& partial) noexcept
        : partial_(&partial) {}
    T* partial_;
};

namespace detail {

    template <class P>
    inline constexpr bool is_collect_v = false;
    template <class T>
    inline constexpr bool is_collect_v<Collect<T>> = true;

    template <class T>
    struct kernel_param<Collect<T>> {
        static constexpr bool allowed = true;
        // Padded like Reduce slots: items append into their slot throughout
        // iteration, and adjacent partials would otherwise false-share.
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
            // The target holds THIS run's gather: rebuilt every run.
            reset_value(w.resource<T>());
        }

        static void finish(state& s, World& w) {
            auto& target = w.resource<T>();
            for (std::size_t i = 0; i < s.active; ++i) {
                auto& part = s.slots[i].value;
                target.insert(target.end(),
                              std::make_move_iterator(part.begin()),
                              std::make_move_iterator(part.end()));
            }
        }

        static Collect<T> bind(state& s,
                               World&,
                               Commands&,
                               std::uint32_t,
                               std::size_t,
                               std::size_t,
                               std::uint32_t ordinal) {
            return Collect<T>(s.slots[ordinal].value);
        }
    };

} // namespace detail
} // namespace ecs
