// ecs/schedule/params/random.hpp
//
// Random: per-item deterministic random stream (PCG32) for parallel systems,
// with streams derived from (RandomSeed resource, schedule tick, system id,
// item ordinal) -- counter-based, no shared state, independent of lane count
// and iteration timing.

#pragma once

#include <ecs/schedule/params/protocol.hpp>

#include <cstdint>
#include <span>

namespace ecs {

// Optional resource seeding every Random parameter in the world's schedules.
// Absent, a fixed default seed is used (runs are then reproducible across
// processes by default).
struct RandomSeed {
    std::uint64_t value = 0x9E37'79B9'7F4A'7C15ull;
};

// Per-item deterministic random stream (PCG32) for a parallel system. The
// stream is derived from (RandomSeed resource, schedule tick, system id, item
// ordinal) -- counter-based, no shared state -- so results are independent of
// lane count and iteration timing: the same world, seed, and tick produce the
// SAME numbers on 1 lane and N, run after run. (A serial ResMut<Rng> system
// can't promise that: its draw order is its iteration order.) Draws stream
// per item; a fresh tick or ordinal is a fresh, decorrelated stream.
class Random {
public:
    // Uniform bits / [0, 1) canonical floats.
    std::uint32_t u32() noexcept {
        std::uint64_t const old = state_;
        state_                  = old * 6364136223846793005ull + inc_;
        auto const xorshifted   = std::uint32_t(((old >> 18u) ^ old) >> 27u);
        auto const rot          = std::uint32_t(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }
    std::uint64_t u64() noexcept { return (std::uint64_t(u32()) << 32) | u32(); }
    float f32() noexcept { return float(u32() >> 8) * 0x1.0p-24f; }
    double f64() noexcept { return double(u64() >> 11) * 0x1.0p-53; }
    // Uniform integer in [0, n) (n > 0). Cheap bounded form (negligible
    // modulo bias for game-sized n).
    std::uint32_t below(std::uint32_t n) noexcept { return u32() % n; }

private:
    template <class P>
    friend struct detail::parallel_param;
    Random(std::uint64_t seed, std::uint64_t seq) noexcept
        : inc_((seq << 1u) | 1u) {
        u32();
        state_ += seed;
        u32();
    }
    std::uint64_t state_ = 0;
    std::uint64_t inc_;
};

namespace detail {

    // SplitMix64 finalizer: decorrelates the (seed, tick, system, ordinal)
    // coordinates into stream seeds.
    inline constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
        x += 0x9E37'79B9'7F4A'7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58'476D'1CE4'E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D0'49BB'1331'11EBull;
        return x ^ (x >> 31);
    }

    template <>
    struct parallel_param<Random> {
        static constexpr bool allowed = true;
        struct state {
            std::uint64_t stream_base = 0;
        };

        // Declares a READ of the RandomSeed resource: a system that (re)seeds
        // the world serializes ahead of every Random user, and Random users
        // still run concurrently with each other.
        static void declare(SystemAccess& a) {
            a.res_reads.push_back(resource_id<RandomSeed>);
        }

        static void prepare(state& s,
                            World& w,
                            std::span<std::uint32_t const>,
                            WaveContext const& ctx) {
            auto const* seed = w.try_resource<RandomSeed>();
            auto const base  = seed ? seed->value : RandomSeed {}.value;
            s.stream_base    = mix64(base) ^ mix64(ctx.tick) ^ mix64(ctx.system.value);
        }

        static void finish(state&, World&, parallel::WorkerPool&) {}

        static Random bind(state& s, World&, Commands&, WorkItem const& item) {
            auto ordinal = item.ordinal;
            return Random(mix64(s.stream_base ^ ordinal), ordinal);
        }
    };

} // namespace detail
} // namespace ecs
