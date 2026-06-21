// benchmarks/schedule_bench.cpp
//
// Sweeps the WorkerPool executor's lane count on the particle demo's real
// schedule (x1 = serial: one lane, no worker threads). It reports the full
// per-tick timing distribution (mean/p50/p99/max -- jitter, not best-case) and
// allocations per tick (a global new/delete counter), so throughput scaling,
// tail predictability, and the zero-allocation goal show up together.
//
//   schedule_bench [measured_ticks]      (default 5000)
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

int main(int argc, char** argv) {
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
    }
    return 0;
}
