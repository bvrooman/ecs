// ecs/schedule/params/fold.hpp
//
// Fold<T, Scope>: a reduction target that declares WHEN its accumulator
// resets. Scope is a property of the target, not of any one system folding
// into it, so every folder agrees by construction.
//
//   Fold<Energy, fold::tick>      one tick's total, across every wave
//   Fold<Stats, fold::ticks(60)>  a rolling 60-tick window
//   Fold<Totals, fold::lifetime>  never reset; a monotonic counter
//
// A bare target (Reduce<FSum, Add>) is fold::wave: the accumulator is rebuilt
// at every wave, so folders that share a wave compose and folders in
// different waves do not. That is the default because it is the smallest
// window -- widening one is a decision the call site should make.

#pragma once

#include <ecs/schedule/params/parallel.hpp> // detail::reset_value

#include <cstdint>

namespace ecs {

namespace fold {

    // The reset boundary. `n` is the window length in ticks for Window; the
    // other kinds ignore it.
    struct Scope {
        enum Kind : unsigned char { Wave, Tick, Window, Lifetime };
        Kind kind       = Wave;
        std::uint64_t n = 0;
    };

    inline constexpr Scope wave {Scope::Wave, 0};
    inline constexpr Scope tick {Scope::Tick, 0};
    inline constexpr Scope lifetime {Scope::Lifetime, 0};
    constexpr Scope ticks(std::uint64_t const n) { return {Scope::Window, n}; }

} // namespace fold

// A reduction target carrying its own reset scope. Reach the value through
// value(); the primitives fold into it and the executor opens its windows.
template <class T, fold::Scope S>
class Fold {
    static_assert(S.kind != fold::Scope::Window || S.n > 0, "fold::ticks(n) needs n > 0");

public:
    [[nodiscard]]
    T& value() noexcept {
        return v_;
    }
    [[nodiscard]]
    T const& value() const noexcept {
        return v_;
    }

    // Open the window this tick belongs to, resetting the accumulator if it is
    // a new one. Idempotent within a window, so every folder may call it.
    // Driven by the fold parameters' prepare hooks; call it directly only when
    // no such parameter touches the target.
    void begin_window(std::uint64_t const tick) {
        if constexpr (S.kind == fold::Scope::Lifetime) {
            (void)tick; // never reset
        } else if constexpr (S.kind == fold::Scope::Wave) {
            detail::reset_value(v_); // every wave; no window to track
        } else {
            auto const w = S.kind == fold::Scope::Tick ? tick : tick / S.n;
            if (w != win_) {
                detail::reset_value(v_);
                win_ = w;
            }
        }
    }

private:
    T v_ {};
    std::uint64_t win_ = ~std::uint64_t {0}; // no window opened yet
};

namespace detail {

    // Uniform access to a fold target, wrapped or bare. A bare target is
    // fold::wave, which keeps every existing Reduce/Collect working unchanged.
    template <class Target>
    struct fold_target {
        using value_type                   = Target;
        static constexpr fold::Scope scope = fold::wave;
        static Target& value(Target& t) noexcept { return t; }
        static void begin_window(Target& t, std::uint64_t) { reset_value(t); }
    };

    template <class T, fold::Scope S>
    struct fold_target<Fold<T, S>> {
        using value_type                   = T;
        static constexpr fold::Scope scope = S;
        static T& value(Fold<T, S>& f) noexcept { return f.value(); }
        static void begin_window(Fold<T, S>& f, std::uint64_t const tick) {
            f.begin_window(tick);
        }
    };

    template <class Target>
    using fold_value_t = typename fold_target<Target>::value_type;

} // namespace detail
} // namespace ecs
