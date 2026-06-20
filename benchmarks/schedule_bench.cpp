// benchmarks/schedule_bench.cpp
//
// Benchmarks the particle demo's *actual* schedule (examples/particles) to
// answer: what is the ECS schedule throughput, how consistent is it tick-to-
// tick (jitter), and does the std::execution thread pool help or hurt at the
// demo's workload size? It runs the real systems (gravity/age/integrate/reaper/
// extract + emitter) over the demo's steady-state population and reports the
// full per-tick timing distribution -- mean, stddev and tail percentiles, which
// is what matters for stutter, not the best-case kernel time.
//
//   schedule_bench [measured_ticks]      (default 5000)
#include "ecs/ecs.hpp"
#include "particles.hpp"
#include "simulation.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exec/static_thread_pool.hpp>
#include <thread>
#include <vector>

using namespace ecs;
using clk = std::chrono::steady_clock;

static double us_of(clk::duration d) {
    return std::chrono::duration<double, std::micro>(d).count();
}

// Distribution of per-tick times (microseconds).
struct Stats {
    double mean = 0, sd = 0, mn = 0, p50 = 0, p90 = 0, p99 = 0, mx = 0;
    static Stats of(std::vector<double> v) {
        std::sort(v.begin(), v.end());
        Stats s;
        s.mn       = v.front();
        s.mx       = v.back();
        double sum = 0;
        for (double x : v)
            sum += x;
        s.mean    = sum / v.size();
        double va = 0;
        for (double x : v)
            va += (x - s.mean) * (x - s.mean);
        s.sd     = std::sqrt(va / v.size());
        auto pct = [&](double p) {
            return v[std::min(v.size() - 1, std::size_t(p * v.size()))];
        };
        s.p50 = pct(0.50);
        s.p90 = pct(0.90);
        s.p99 = pct(0.99);
        return s;
    }
    void print(char const* label) const {
        std::printf("  %-12s  mean %7.2f  sd %7.2f | min %7.2f  p50 %7.2f  p90 %7.2f "
                    " p99 %8.2f  max %9.2f us | %6.0f ticks/s\n",
                    label,
                    mean,
                    sd,
                    mn,
                    p50,
                    p90,
                    p99,
                    mx,
                    1e6 / mean);
    }
};

static void setup(World& w) {
    w.emplace_resource<Gravity>(cfg::kGravity);
    w.emplace_resource<Rng>(Rng {std::mt19937 {12345u}}); // fixed seed: reproducible
    w.emplace_resource<TripleBuffer<RenderSnapshot>>();
    w.emplace_resource<Clock>(Clock {0.0f});
}

// Sample `n` per-tick times after `warm` warm-up ticks, calling run_one() each
// tick. Returns the distribution plus the final live-particle count.
template <class RunOne>
static std::pair<Stats, std::size_t>
sample(World& world, int warm, int n, RunOne&& run_one) {
    for (int i = 0; i < warm; ++i)
        run_one();
    std::vector<double> us;
    us.reserve(n);
    for (int i = 0; i < n; ++i) {
        auto t0 = clk::now();
        run_one();
        us.push_back(us_of(clk::now() - t0));
    }
    return {Stats::of(std::move(us)), world.size()};
}

int main(int argc, char** argv) {
    int const measured = argc > 1 ? std::atoi(argv[1]) : 5000;
    int const warm     = 800; // ~steady state (lifespan ~2.3s * 120 Hz ~ 276 ticks)
    unsigned const hw  = std::max(2u, std::thread::hardware_concurrency());

    std::printf("schedule_bench: %d measured ticks/config, %u hw threads, "
                "emit %d/tick\n\n",
                measured,
                hw,
                cfg::kEmitPerTick);

    std::size_t particles = 0;

    // --- inline (no pool): systems run sequentially on this thread ---------
    {
        World world;
        setup(world);
        Schedule sched;
        build_particle_schedule(sched);
        std::printf("schedule: %zu systems across %zu waves\n",
                    sched.size(),
                    sched.level_count());
        auto [s, n] = sample(world, warm, measured, [&] { sched.run(world); });
        particles   = n;
        std::printf("population at steady state: %zu particles\n\n", n);
        std::printf("per-tick schedule.run() cost:\n");
        s.print("inline");
    }

    // --- thread pool, sweeping the worker count ----------------------------
    for (unsigned t : {1u, 2u, 4u, 6u, 8u, hw}) {
        World world;
        setup(world);
        Schedule sched;
        build_particle_schedule(sched);
        exec::static_thread_pool pool {t};
        auto scheduler = pool.get_scheduler();
        auto [s, n] = sample(world, warm, measured, [&] { sched.run(world, scheduler); });
        (void)n;
        char lbl[16];
        std::snprintf(lbl, sizeof(lbl), "pool x%u", t);
        s.print(lbl);
    }

    std::printf("\nthroughput is per *tick* (whole 6-system schedule over ~%zu "
                "particles).\n",
                particles);
    return 0;
}
