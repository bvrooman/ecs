// ecs/schedule/params/extract.hpp
//
// Extract<T>: gather-shaped output (snapshots, render buffers) for kernel
// systems, where output size == matched rows. The vector-like T resource is
// pre-sized once before the dispatch and each item binds a span over its
// DISJOINT slice at an executor-computed offset -- one write per element, no
// fold at all.
//
// Declares a resource WRITE on T (same leveling story as Reduce: physical
// writes are disjoint; the pre-sizing happens in the single-threaded prepare
// hook).

#pragma once

#include <ecs/schedule/params/protocol.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>
#include <utility>

namespace ecs {

// A vector-like reduction/extraction target: contiguous, resizable.
template <class T>
concept ExtractTarget = requires(T& t, std::size_t n) {
    t.resize(n);
    t.data();
    { t.size() } -> std::convertible_to<std::size_t>;
};

// Gather-shaped output for a kernel system: a writable span over THIS ITEM's
// disjoint slice of the pre-sized T resource. Element i corresponds to the
// item's row i (the sliced Query walks the same rows in the same order), so
// the idiomatic body pairs them by index:
//
//   q.for_each_chunk([&](std::span<Entity>, chunk<const Position> p) {
//       auto x = p.column<0>();
//       for (std::size_t i = 0; i < x.size(); ++i)
//           out[i] = x[i];
//   });
template <ExtractTarget T>
class Extract {
    using Elem = std::remove_reference_t<decltype(*std::declval<T&>().data())>;

public:
    [[nodiscard]]
    std::span<Elem> span() const noexcept {
        return span_;
    }
    [[nodiscard]]
    Elem& operator[](std::size_t i) const noexcept {
        return span_[i];
    }
    [[nodiscard]]
    std::size_t size() const noexcept {
        return span_.size();
    }

private:
    template <class P>
    friend struct detail::kernel_param;
    explicit Extract(std::span<Elem> s) noexcept
        : span_(s) {}
    std::span<Elem> span_;
};

namespace detail {

    template <class T>
    struct kernel_param<Extract<T>> {
        static constexpr bool allowed = true;
        struct state {
            std::vector<std::size_t> offsets; // per-ordinal global row offset
        };

        static void declare(SystemAccess& a) { a.res_writes.push_back(resource_id<T>); }

        static void prepare(state& s,
                            World& w,
                            std::span<std::uint32_t const> rows,
                            KernelWaveContext const&) {
            s.offsets.resize(rows.size());
            std::size_t total = 0;
            for (std::size_t i = 0; i < rows.size(); ++i) {
                s.offsets[i] = total;
                total += rows[i];
            }
            // Pre-size ONCE, before the dispatch: item spans stay stable for
            // the whole wave (vector capacity is retained across ticks, so the
            // steady state allocates nothing).
            w.resource<T>().resize(total);
        }

        static void finish(state&, World&) {} // writes were direct + disjoint

        static Extract<T> bind(state& s, World& w, Commands&, WorkItem const& item) {
            auto& target = w.resource<T>();
            auto b       = item.begin;
            auto e       = item.end;
            auto ordinal = item.ordinal;
            return Extract<T>(std::span(target.data() + s.offsets[ordinal], e - b));
        }
    };

} // namespace detail
} // namespace ecs
