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
        struct state {
            slot_array<T> parts;
        };

        static void declare(SystemAccess&) {} // private state: no access

        static void prepare(state& s,
                            World&,
                            std::span<std::uint32_t const> rows,
                            KernelWaveContext const&) {
            s.parts.prepare(rows.size());
        }

        static void finish(state&, World&) {}

        static Scratch<T> bind(
            state& s, World&, Commands&, WorkItem const&, std::uint32_t ordinal) {
            return Scratch<T>(s.parts[ordinal]);
        }
    };

} // namespace detail
} // namespace ecs
