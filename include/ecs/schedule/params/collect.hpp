// ecs/schedule/params/collect.hpp
//
// Collect<T>: filtered gather (kill lists, visibility sets, target lists) for
// parallel systems -- the Extract shape when output size is NOT known up front.
// Each item appends into a private T partial; at the barrier the partials are
// concatenated into the T RESOURCE in ordinal order, so the result is exactly
// what a serial filtered walk would have produced. Like Reduce, it declares a
// FOLD and rebuilds the target at the target's scope: two systems collecting
// into one target concatenate in finish-hook order, sharing a wave by default
// and spanning waves at a wider scope (fold.hpp).

#pragma once

#include <ecs/schedule/params/fold.hpp>
#include <ecs/schedule/params/parallel.hpp>
#include <ecs/schedule/params/protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>

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

// Filtered-gather output for a parallel system. `out->push_back(x)` / `*out`
// reaches THIS ITEM's private partial of T -- no sharing, no atomics, no
// pre-counting. At the wave barrier the partials are moved-appended into the
// T resource in item-ordinal (serial-walk) order: the collected sequence is
// identical to a serial `for_each` filter at any lane count.
// Precondition, checked at prepare: the world owns a T resource.
template <class T>
class Collect {
    // T may be the sequence itself or a Fold<Seq, Scope> around it.
    using Seq = detail::fold_value_t<T>;
    static_assert(CollectTarget<Seq>,
                  "Collect's target must be a clearable, insertable sequence");

public:
    [[nodiscard]]
    Seq& operator*() const noexcept {
        return *partial_;
    }
    Seq* operator->() const noexcept { return partial_; }

private:
    template <class P>
    friend struct detail::parallel_param;
    explicit Collect(Seq& partial) noexcept
        : partial_(&partial) {}
    Seq* partial_;
};

namespace detail {

    template <class T>
    struct parallel_param<Collect<T>> {
        static constexpr bool allowed = true;
        using Seq                     = fold_value_t<T>;
        struct state {
            slot_array<Seq> parts;
        };

        static void declare(SystemAccess& a) { a.res_folds.push_back(resource_id<T>); }

        static void prepare(state& s,
                            World& w,
                            std::span<std::uint32_t const> rows,
                            WaveContext const& ctx) {
            s.parts.prepare(rows.size());
            // Rebuilt at the target's declared scope; idempotent within one.
            fold_target<T>::begin_window(w.resource<T>(), ctx.tick);
        }

        static void finish(state& s, World& w) {
            auto& target = fold_target<T>::value(w.resource<T>());
            for (std::size_t i = 0; i < s.parts.active; ++i) {
                auto& part = s.parts[i];
                target.insert(target.end(),
                              std::make_move_iterator(part.begin()),
                              std::make_move_iterator(part.end()));
            }
        }

        static Collect<T> bind(state& s, World&, Commands&, WorkItem const& item) {
            auto ordinal = item.ordinal;
            return Collect<T>(s.parts[ordinal]);
        }
    };

} // namespace detail
} // namespace ecs
