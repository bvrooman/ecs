// ecs/schedule/params/scratch.hpp
//
// Scratch<T>: per-item temporary workspace for kernel systems. No declared
// access, no barrier merge -- pure scratch, for the temp vector / stack /
// candidate list a body would otherwise allocate per row.

#pragma once

#include "protocol.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ecs {

// Per-item temporary WORKSPACE for a kernel system: `tmp->...` / `*tmp`
// reaches this item's private T, cleared (capacity retained, when T offers
// clear()) before every run. No declared access, no barrier merge -- it is
// pure scratch, for the temp vector / stack / candidate list a body would
// otherwise allocate per row. Contents do not survive the run.
template <class T>
class Scratch {
public:
    [[nodiscard]]
    T& operator*() const noexcept {
        return *scratch_;
    }
    T* operator->() const noexcept { return scratch_; }

private:
    template <class P>
    friend struct detail::kernel_param;
    explicit Scratch(T& scratch) noexcept
        : scratch_(&scratch) {}
    T* scratch_;
};

namespace detail {

    template <class T>
    struct kernel_param<Scratch<T>> {
        static constexpr bool allowed = true;
        // Padded for the same reason as Reduce slots: scratch is written
        // throughout iteration, and adjacent items run on different lanes.
        struct alignas(128) Slot {
            T value {};
        };
        struct state {
            std::vector<Slot> slots;
        };

        static void declare(SystemAccess&) {} // private state: no access

        static void prepare(state& s,
                            World&,
                            std::span<std::uint32_t const> rows,
                            KernelWaveContext const&) {
            if (s.slots.size() < rows.size())
                s.slots.resize(rows.size());
            for (std::size_t i = 0; i < rows.size(); ++i)
                reset_value(s.slots[i].value);
        }

        static void finish(state&, World&) {}

        static Scratch<T> bind(state& s,
                               World&,
                               Commands&,
                               std::uint32_t,
                               std::size_t,
                               std::size_t,
                               std::uint32_t ordinal) {
            return Scratch<T>(s.slots[ordinal].value);
        }
    };

} // namespace detail
} // namespace ecs
