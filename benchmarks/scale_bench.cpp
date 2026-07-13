// benchmarks/scale_bench.cpp
//
// How the ECS schedule scales with population size, and where the thread pool
// starts to pay off. Runs the demo's per-entity "steady state" systems
// (gravity/age/integrate/extract -- no emitter/reaper churn, so N is fixed) over
// a pre-spawned population, sweeping N. Reports mean per-tick time and, more
// tellingly, ns/entity: a flat ns/entity means cache-friendly linear scaling; a
// rise at large N means the working set spilled out of cache.
//
//   scale_bench
#include "bench.hpp"
#include "ecs/ecs.hpp"
#include "particles.hpp"
#include <chrono>
#include <cstdio>
#include <span>
#include <thread>
#include <vector>

using namespace ecs;
using clk = std::chrono::steady_clock;

// The per-entity hot path only: a fixed population, no spawning/reaping.
// Registered as KERNELS (add_kernel), so the executor slices each system's
// rows across lanes -- which is what makes "where the pool pays off" a
// meaningful question. An imperative add() is one opaque work item over all
// rows: the pool cannot split it, so it would only overlap the two
// disjoint-write systems (gravity/age) in one wave and pay a fork-join
// barrier + cross-core cache traffic on every wave -- pure overhead, slower
// at every N. The systems still fall into the same conflict waves
// ({gravity,age} -> integrate -> extract); the difference is that each
// system's rows now fan out. Below the executor's slice threshold (small N)
// a kernel stays a single item, so the small-N rows honestly show ~no
// speedup rather than a fabricated one.
static void build_steady(Schedule& s) {
    s.add_kernel("gravity", [](Query<Velocity> q, Res<Gravity> g) {
        float const a = g->accel;
        q.for_each_chunk([a](std::span<Entity>, chunk<Velocity> v) {
            for (auto& vy : v.column<1>())
                vy += a * cfg::kDt;
        });
    });
    s.add_kernel("age", [](Query<Age> q) {
        q.for_each_chunk([](std::span<Entity>, chunk<Age> a) {
            for (auto& t : a.column<0>())
                t += cfg::kDt;
        });
    });
    s.add_kernel("integrate", [](Query<Position, Velocity const> q) {
        q.for_each_chunk([](std::span<Entity>,
                            chunk<Position> p,
                            chunk<Velocity const> v) {
            auto px = p.column<0>();
            auto py = p.column<1>();
            auto vx = v.column<0>();
            auto vy = v.column<1>();
            for (std::size_t i = 0; i < px.size(); ++i) {
                px[i] += vx[i] * cfg::kDt;
                py[i] += vy[i] * cfg::kDt;
            }
        });
    });
    // Gather a draw-ready snapshot. The Extract primitive pre-sizes the target
    // once at the barrier and each item writes its disjoint span, so the
    // gather parallelizes too (vs the old serial push_back into a TripleBuffer,
    // which forced this system serial and capped the whole sweep's speedup).
    s.add_kernel("extract",
                 [](Query<Position const, Color const, Age const> q,
                    Extract<RenderSnapshot> out) {
                     q.for_each_chunk([&](std::span<Entity>,
                                          chunk<Position const> p,
                                          chunk<Color const> c,
                                          chunk<Age const>) {
                         auto px = p.column<0>();
                         auto py = p.column<1>();
                         auto cr = c.column<0>();
                         auto cg = c.column<1>();
                         auto cb = c.column<2>();
                         for (std::size_t i = 0; i < px.size(); ++i)
                             out[i] = GpuParticle {px[i], py[i],
                                                   cr[i], cg[i], cb[i], 1.0f};
                     });
                 });
}

static void setup(World& w) {
    w.emplace_resource<Gravity>(cfg::kGravity);
    w.emplace_resource<Rng>(Rng {std::mt19937 {1u}});
    w.emplace_resource<RenderSnapshot>(); // Extract target (was a TripleBuffer)
}

static void spawn_n(World& w, int n) {
    Schedule init;
    init.add_once("spawn", [n](Commands& cmd) {
        for (int i = 0; i < n; ++i)
            cmd.spawn(Position {0, 0},
                      Velocity {0.1f, 1.0f},
                      Color {1, 0.7f, 0.3f},
                      Age {0.0f, 1e9f}); // effectively immortal for the run
    });
    init.run(w);
}

template <class RunOne>
static double mean_us(int warm, int n, RunOne&& run_one) {
    return bench::measure_ticks(warm, n, run_one).mean;
}

int main() {
    bench::set_suite("scale");
    unsigned const hw = std::max(2u, std::thread::hardware_concurrency());
    std::printf("scale_bench: per-tick cost of the 4 steady-state systems vs N\n");
    std::printf("%10s | %12s %10s | %12s %10s | %s\n",
                "particles",
                "inline us",
                "ns/ent",
                "pool us",
                "ns/ent",
                "pool speedup");

    for (int n : {1000, 5000, 20000, 100000, 300000, 1000000}) {
        int const iters = std::max(60, std::min(3000, 8'000'000 / n));
        int const warm  = std::max(20, iters / 5);

        double in_us, pl_us;
        {
            World w;
            setup(w);
            spawn_n(w, n);
            Schedule s;
            build_steady(s);
            WorkerPool serial {1};
            in_us = mean_us(warm, iters, [&] { s.run(w, serial); });
        }
        {
            World w;
            setup(w);
            spawn_n(w, n);
            Schedule s;
            build_steady(s);
            // Size to the actual machine: a hardcoded 8 on a 4-core host would
            // silently measure oversubscription, not pool speedup.
            WorkerPool pool {std::max(2u, hw)};
            pl_us = mean_us(warm, iters, [&] { s.run(w, pool); });
        }
        std::printf("%10d | %12.2f %10.2f | %12.2f %10.2f | %5.2fx\n",
                    n,
                    in_us,
                    in_us * 1000.0 / n,
                    pl_us,
                    pl_us * 1000.0 / n,
                    in_us / pl_us);
        auto const nn = std::size_t(n), it = std::size_t(iters);
        bench::emit("steady_state", "us_per_tick", in_us, "us", "lower", nn, 1, it);
        bench::emit("steady_state", "us_per_tick", pl_us, "us", "lower", nn, hw, it);
        bench::emit("steady_state", "pool_speedup", in_us / pl_us, "x", "higher",
                    nn, hw, it);
    }
    return 0;
}
