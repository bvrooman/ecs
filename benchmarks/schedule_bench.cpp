// benchmarks/schedule_bench.cpp
//
// Sweeps the WorkerPool executor's lane count on the particle demo's real
// schedule (x1 = serial: one lane, no worker threads). It reports the full
// per-tick timing distribution (mean/p50/p99/max -- jitter, not best-case) and
// allocations per tick (a global new/delete counter), so throughput scaling,
// tail predictability, and the zero-allocation goal show up together.
//
//   schedule_bench [measured_ticks]      (default 5000)
#include <ecs/ecs.hpp>

#include "bench.hpp"
#include "bench_alloc.hpp"
#include "particles.hpp"
#include "simulation.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace ecs;
using Stats = bench::Dist; // shared tick-distribution type (bench.hpp)

static void setup(World& w) {
    w.emplace_resource<Gravity>(cfg::kGravity);
    w.emplace_resource<Rng>(Rng {std::mt19937 {12345u}}); // fixed seed: reproducible
    w.emplace_resource<TripleBuffer<RenderSnapshot>>();
    w.emplace_resource<Clock>(Clock {0.0f});
    w.emplace_resource<Emitter>(Emitter {cfg::kEmitPerTick,
                                         cfg::kOriginX,
                                         cfg::kOriginY});
}

static void report(char const* executor,
                   unsigned lanes,
                   Stats s,
                   double allocs_per_tick) {
    char lbl[24];
    if (lanes)
        std::snprintf(lbl, sizeof(lbl), "%s x%u", executor, lanes);
    else
        std::snprintf(lbl, sizeof(lbl), "%s", executor);
    std::printf("  %-14s mean %7.1f  p50 %7.1f  p99 %8.1f  max %9.1f us | %6.0f "
                "ticks/s | %5.1f alloc/tick\n",
                lbl,
                s.mean,
                s.p50,
                s.p99,
                s.mx,
                1e6 / s.mean,
                allocs_per_tick);
}

// The machine-readable mirror of report(): the shared distribution rows plus
// the allocation rate.
static void emit_stats(char const* name,
                       unsigned lanes,
                       Stats const& s,
                       double allocs_per_tick,
                       std::size_t entities,
                       std::size_t ticks) {
    bench::emit_dist(name, lanes, s, entities, ticks);
    bench::emit(name,
                "allocs_per_tick",
                allocs_per_tick,
                "1",
                "lower",
                entities,
                lanes,
                ticks);
}

// Warm to steady state, then time `n` ticks, returning the distribution and the
// allocations counted during the measured window.
template <class RunOne>
static std::pair<Stats, double> measure(int warm, int n, RunOne&& run_one) {
    bench::warmup(warm, run_one); // untimed AND uncounted: warmup churn must
                                  // not inflate the steady-state allocs/tick
    Stats st;
    double const allocs =
        bench::count_allocs(n, [&] { st = bench::measure_ticks(0, n, run_one); });
    return {st, allocs};
}

// --- serial vs parallel systems (the work-item executor) -----------------
// One conflict-free wave: 8 tiny disjoint-write systems + 1 heavy system over
// the same entities. Registered imperatively, every system is one opaque work
// item -- the heavy one cannot be split, so it is the wave's straggler.
// Registered as parallel systems, the heavy system's rows slice across lanes and
// overlap the tiny systems' items in the same single dispatch.
namespace shape {
template <int>
struct Small {
    float v;
};
struct Heavy {
    float v;
};

inline float heavy_step(float v) {
    // Compute-bound (a dependent sqrt chain), so the heavy system dominates
    // the wave -- the shape where slicing it matters.
    for (int k = 0; k < 32; ++k)
        v = std::sqrt(v * v + 1.0f);
    return v;
}

// add() and add_parallel() take the SAME system callable -- the registration
// word is the only difference between the opaque and the sliceable form.
template <int I>
void add_small(ecs::Schedule& s, bool parallel) {
    auto body = [](ecs::Query<Small<I>> q) {
        q.for_each_chunk([](std::span<ecs::Entity const>, ecs::chunk<Small<I>> c) {
            for (auto& v : c.template column<0>())
                v = v * 1.0001f + 0.5f;
        });
    };
    if (parallel)
        s.add_parallel("small", body);
    else
        s.add_serial("small", body);
}

inline void build(ecs::Schedule& s, bool parallel) {
    [&]<int... I>(std::integer_sequence<int, I...>) {
        (add_small<I>(s, parallel), ...);
    }(std::make_integer_sequence<int, 8> {});
    auto body = [](ecs::Query<Heavy> q) {
        q.for_each_chunk([](std::span<ecs::Entity const>, ecs::chunk<Heavy> c) {
            for (auto& v : c.column<0>())
                v = heavy_step(v);
        });
    };
    if (parallel)
        s.add_parallel("heavy", body);
    else
        s.add_serial("heavy", body);
}

inline void populate(ecs::World& w, std::size_t n) {
    ecs::Schedule init;
    init.add_serial(
        "seed",
        [n](ecs::Commands& cmd) {
            for (std::size_t i = 0; i < n; ++i)
                cmd.spawn(Small<0> {1},
                          Small<1> {1},
                          Small<2> {1},
                          Small<3> {1},
                          Small<4> {1},
                          Small<5> {1},
                          Small<6> {1},
                          Small<7> {1},
                          Heavy {1});
        },
        {},
        /*every=*/1,
        /*times=*/1);
    init.run(w);
}
} // namespace shape

int main(int argc, char** argv) {
    bench::set_suite("schedule");
    int const measured =
        argc > 1 ? std::atoi(argv[1]) : int(bench::env_long("ECS_TICKS", 5000));
    int const warm    = 800;
    unsigned const hw = std::max(2u, std::thread::hardware_concurrency());

    std::size_t particles = 0;
    {
        World world;
        setup(world);
        Schedule sched;
        build_particle_schedule(sched);
        for (int i = 0; i < warm; ++i)
            sched.run(world);
        particles = world.size();
        std::printf("schedule_bench: %d ticks/config, %u hw threads, emit %d/tick\n"
                    "schedule: %zu systems / %zu waves, %zu particles steady\n\n",
                    measured,
                    hw,
                    cfg::kEmitPerTick,
                    sched.size(),
                    sched.level_count(),
                    particles);
    }

    // WorkerPool lane sweep (x1 = serial: one lane, no worker threads).
    // ECS_LANES overrides; the default reaches hw so the throughput turnover
    // is visible (see bench::lane_set).
    auto const lanes = bench::lane_set();
    for (unsigned t : lanes) {
        World w;
        setup(w);
        Schedule s;
        build_particle_schedule(s);
        WorkerPool pool {t};
        auto [st, al] = measure(warm, measured, [&] { s.run(w, pool); });
        report("workerpool", t, st, al);
        emit_stats("particles", t, st, al, particles, std::size_t(measured));
    }

    // The SAME 9-system wave (8 disjoint-write "small" systems + 1 compute-
    // bound "heavy" one) registered two ways -- all add() vs all add_parallel()
    // (shape::build) -- each swept across the lane set. This is serial vs
    // parallel *registration*, not serial-at-different-lane-counts: the two
    // families share identical bodies and differ only in the registration
    // word. The lane axis is the point. Imperative makes each system one
    // opaque work item, so the heavy system is an unsliceable straggler and
    // the wave plateaus at ~heavy-time no matter how many lanes; parallel
    // registration slices the heavy system's rows too, so it keeps scaling.
    // Reading the two blocks down the lanes shows serial flattening (and,
    // past the physical-core count, its tail blowing up) while parallel keeps
    // dropping.
    std::printf("\nimperative vs parallel REGISTRATION of one 9-system wave "
                "(8 small + 1 heavy straggler), 40k entities, swept:\n");
    int const shape_ticks = std::max(200, measured / 10);
    for (bool parallel : {false, true}) {
        for (unsigned t : lanes) {
            World w;
            shape::populate(w, 40'000);
            Schedule s;
            shape::build(s, parallel);
            WorkerPool pool {t};
            auto [st, al] = measure(warm / 4, shape_ticks, [&] { s.run(w, pool); });
            report(parallel ? "parallel" : "serial", t, st, al);
            emit_stats(parallel ? "shape_parallel" : "shape_serial",
                       t,
                       st,
                       al,
                       40'000,
                       std::size_t(shape_ticks));
        }
    }
    return 0;
}
