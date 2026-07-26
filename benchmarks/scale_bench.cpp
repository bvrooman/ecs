// benchmarks/scale_bench.cpp
//
// How the ECS schedule scales with population size AND pool width. Runs the
// demo's per-entity "steady state" systems (gravity/age/integrate/extract,
// registered as kernels so their rows slice across lanes -- no emitter/reaper
// churn, so N is fixed) over a pre-spawned population, as a lanes x N matrix.
// Each row is one N swept across the lane set; speedup is vs the serial
// (smallest-lane) baseline, so the turnover -- where extra lanes stop paying
// and dispatch/cache overhead wins -- shows in the row (the sweep reaches hw,
// which counts logical cores, so oversubscription past the physical-core
// count is part of the picture rather than a hidden single-point default).
//
//   scale_bench            (ECS_LANES="1,2,4,8" overrides the lane set)
#include <ecs/ecs.hpp>

#include "bench.hpp"
#include "particles.hpp"
#include <algorithm>
#include <cstdio>
#include <span>
#include <vector>

using namespace ecs;

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
        q.for_each_chunk([a](std::span<Entity const>, chunk<Velocity> v) {
            for (auto& vy : v.column<1>())
                vy += a * cfg::kDt;
        });
    });
    s.add_kernel("age", [](Query<Age> q) {
        q.for_each_chunk([](std::span<Entity const>, chunk<Age> a) {
            for (auto& t : a.column<0>())
                t += cfg::kDt;
        });
    });
    s.add_kernel("integrate", [](Query<Position, Velocity const> q) {
        q.for_each_chunk([](std::span<Entity const>,
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
                     q.for_each_chunk([&](std::span<Entity const>,
                                          chunk<Position const> p,
                                          chunk<Color const> c,
                                          chunk<Age const>) {
                         auto px = p.column<0>();
                         auto py = p.column<1>();
                         auto cr = c.column<0>();
                         auto cg = c.column<1>();
                         auto cb = c.column<2>();
                         for (std::size_t i = 0; i < px.size(); ++i)
                             out[i] =
                                 GpuParticle {px[i], py[i], cr[i], cg[i], cb[i], 1.0f};
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

// One (N, lanes) cell: mean us/tick for the steady schedule on a lanes-wide
// pool. World rebuilt per cell so cache state doesn't carry across lane counts.
static double cell_us(int n, unsigned lanes, int warm, int iters) {
    World w;
    setup(w);
    spawn_n(w, n);
    Schedule s;
    build_steady(s);
    WorkerPool pool {lanes};
    return mean_us(warm, iters, [&] { s.run(w, pool); });
}

int main() {
    bench::set_suite("scale");
    auto const lanes = bench::lane_set(); // ECS_LANES; default 1,2,4,...,hw

    // A lanes x N matrix: speedup is vs the serial (smallest-lane) baseline in
    // each N row, so the turnover -- where extra lanes stop helping and the
    // dispatch/cache overhead wins -- reads straight off the row instead of
    // being hidden behind a single (often past-peak) hw column.
    std::printf("scale_bench: steady per-tick cost, lanes x N. speedup vs x%u; "
                "* = best lane in the row\n%12s",
                lanes.front(),
                "particles");
    for (unsigned L : lanes)
        std::printf(" |  x%-2u us  spd", L);
    std::printf("\n");

    for (int n : {1000, 5000, 20000, 100000, 300000, 1000000}) {
        int const iters = std::max(60, std::min(3000, 8'000'000 / n));
        int const warm  = std::max(20, iters / 5);
        auto const nn = std::size_t(n), it = std::size_t(iters);

        std::vector<double> us(lanes.size());
        for (std::size_t i = 0; i < lanes.size(); ++i)
            us[i] = cell_us(n, lanes[i], warm, iters);
        double const base      = us.front();
        std::size_t const best = std::min_element(us.begin(), us.end()) - us.begin();

        std::printf("%12d", n);
        for (std::size_t i = 0; i < lanes.size(); ++i) {
            std::printf(" | %6.1f %4.2f%c", us[i], base / us[i], i == best ? '*' : ' ');
            bench::emit("steady_state",
                        "us_per_tick",
                        us[i],
                        "us",
                        "lower",
                        nn,
                        lanes[i],
                        it);
            bench::emit("steady_state",
                        "pool_speedup",
                        base / us[i],
                        "x",
                        "higher",
                        nn,
                        lanes[i],
                        it);
        }
        std::printf("\n");
    }
    return 0;
}
