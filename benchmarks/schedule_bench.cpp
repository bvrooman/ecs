// benchmarks/schedule_bench.cpp
//
// Sweeps the WorkerPool executor's lane count on the particle demo's real
// schedule (x1 = serial: one lane, no worker threads). It reports the full
// per-tick timing distribution (mean/p50/p99/max -- jitter, not best-case) and
// allocations per tick (a global new/delete counter), so throughput scaling,
// tail predictability, and the zero-allocation goal show up together.
//
//   schedule_bench [measured_ticks]      (default 5000)
#include "bench.hpp"
#include "ecs/ecs.hpp"
#include "particles.hpp"
#include "simulation.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

// --- global allocation counter -------------------------------------------
static std::atomic<long> g_allocs {0};
void* operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc {};
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace ecs;
using clk = std::chrono::steady_clock;

struct Stats {
    double mean = 0, p50 = 0, p99 = 0, mx = 0;
    static Stats of(std::vector<double> v) {
        std::sort(v.begin(), v.end());
        Stats s;
        s.mx       = v.back();
        double sum = 0;
        for (double x : v)
            sum += x;
        s.mean   = sum / v.size();
        auto pct = [&](double p) {
            return v[std::min(v.size() - 1, std::size_t(p * v.size()))];
        };
        s.p50 = pct(0.50);
        s.p99 = pct(0.99);
        return s;
    }
};

static void setup(World& w) {
    w.emplace_resource<Gravity>(cfg::kGravity);
    w.emplace_resource<Rng>(Rng {std::mt19937 {12345u}}); // fixed seed: reproducible
    w.emplace_resource<TripleBuffer<RenderSnapshot>>();
    w.emplace_resource<Clock>(Clock {0.0f});
    w.emplace_resource<Emitter>(
        Emitter {cfg::kEmitPerTick, cfg::kOriginX, cfg::kOriginY});
}

static void
report(char const* executor, unsigned lanes, Stats s, double allocs_per_tick) {
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

// The machine-readable mirror of report(): the tick distribution + allocation
// rate as (metric, value) rows for the ECS_BENCH_OUT sink.
static void emit_stats(char const* name, unsigned lanes, Stats const& s,
                       double allocs_per_tick, std::size_t entities,
                       std::size_t ticks) {
    bench::emit(name, "mean_us_per_tick", s.mean, "us", "lower", entities, lanes, ticks);
    bench::emit(name, "p50_us_per_tick", s.p50, "us", "lower", entities, lanes, ticks);
    bench::emit(name, "p99_us_per_tick", s.p99, "us", "lower", entities, lanes, ticks);
    bench::emit(name, "max_us_per_tick", s.mx, "us", "lower", entities, lanes, ticks);
    bench::emit(name, "allocs_per_tick", allocs_per_tick, "1", "lower", entities,
                lanes, ticks);
}

// Warm to steady state, then time `n` ticks, returning the distribution and the
// allocations counted during the measured window.
template <class RunOne>
static std::pair<Stats, double> measure(int warm, int n, RunOne&& run_one) {
    for (int i = 0; i < warm; ++i)
        run_one();
    std::vector<double> us;
    us.reserve(n);
    long const a0 = g_allocs.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        auto t0 = clk::now();
        run_one();
        us.push_back(std::chrono::duration<double, std::micro>(clk::now() - t0).count());
    }
    double const allocs = double(g_allocs.load(std::memory_order_relaxed) - a0) / n;
    return {Stats::of(std::move(us)), allocs};
}

// --- imperative vs kernel systems (the work-item executor) -----------------
// One conflict-free wave: 8 tiny disjoint-write systems + 1 heavy system over
// the same entities. Registered imperatively, every system is one opaque work
// item -- the heavy one cannot be split, so it is the wave's straggler.
// Registered as kernel systems, the heavy system's rows slice across lanes and
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

// add() and add_kernel() take the SAME system callable -- the registration
// word is the only difference between the opaque and the sliceable form.
template <int I>
void add_small(ecs::Schedule& s, bool kernel) {
    auto body = [](ecs::Query<Small<I>> q) {
        q.for_each_chunk([](std::span<ecs::Entity>, ecs::chunk<Small<I>> c) {
            for (auto& v : c.template column<0>())
                v = v * 1.0001f + 0.5f;
        });
    };
    if (kernel)
        s.add_kernel("small", body);
    else
        s.add("small", body);
}

inline void build(ecs::Schedule& s, bool kernel) {
    [&]<int... I>(std::integer_sequence<int, I...>) {
        (add_small<I>(s, kernel), ...);
    }(std::make_integer_sequence<int, 8> {});
    auto body = [](ecs::Query<Heavy> q) {
        q.for_each_chunk([](std::span<ecs::Entity>, ecs::chunk<Heavy> c) {
            for (auto& v : c.column<0>())
                v = heavy_step(v);
        });
    };
    if (kernel)
        s.add_kernel("heavy", body);
    else
        s.add("heavy", body);
}

inline void populate(ecs::World& w, std::size_t n) {
    ecs::Schedule init;
    init.add_once("seed", [n](ecs::Commands& cmd) {
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
    });
    init.run(w);
}
} // namespace shape

int main(int argc, char** argv) {
    bench::set_suite("schedule");
    int const measured = argc > 1 ? std::atoi(argv[1]) : 5000;
    int const warm     = 800;
    unsigned const hw  = std::max(2u, std::thread::hardware_concurrency());

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
    for (unsigned t : {1u, 2u, 4u, 8u, hw}) {
        World w;
        setup(w);
        Schedule s;
        build_particle_schedule(s);
        WorkerPool pool {t};
        auto [st, al] = measure(warm, measured, [&] { s.run(w, pool); });
        report("workerpool", t, st, al);
        emit_stats("particles", t, st, al, particles, std::size_t(measured));
    }

    // Imperative vs kernel registration of the same 9-system wave (see shape::).
    std::printf("\nimperative vs kernel systems: 8 small + 1 heavy, one wave, "
                "40k entities\n");
    int const shape_ticks = std::max(200, measured / 10);
    for (bool kernel : {false, true}) {
        for (unsigned t : {1u, hw}) {
            World w;
            shape::populate(w, 40'000);
            Schedule s;
            shape::build(s, kernel);
            WorkerPool pool {t};
            auto [st, al] =
                measure(warm / 4, shape_ticks, [&] { s.run(w, pool); });
            report(kernel ? "kernel" : "imperative", t, st, al);
            emit_stats(kernel ? "shape_kernel" : "shape_imperative", t, st, al,
                       40'000, std::size_t(shape_ticks));
        }
    }
    return 0;
}
