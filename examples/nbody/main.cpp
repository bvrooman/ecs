// examples/nbody/main.cpp
//
// The measurement harness around the naive O(n^2) simulation: run the schedule
// at each lane count, time it, and check that the run is worth trusting as a
// reference -- energy conserved, momentum conserved, and bitwise-identical
// state at every lane count. Exits non-zero if any of those fail; a reference
// that is silently wrong is worse than no reference.
//
// One library limit this surfaces rather than hides -- which is half of why the
// demo exists (see simulation.cpp). The executor slices a parallel system into
// items of grain = max(kMinItemRows, rows / 64), with kMinItemRows = 1024
// (wave.hpp). That floor is sized for cheap per-row work, but here per-row work
// is O(n) -- so below 1024 * lanes bodies there are fewer items than lanes and
// the spare lanes idle. Measured on 4 cores: 1024 bodies -> 1 item -> 1.00x at
// any lane count; 2048 -> 2 items -> 1.88x; 8192 -> 8 items -> 3.77x.
//
// That finding is what produced grain{n}, the per-system slice-size hint, so
// the opt-out now exists. This demo deliberately does not take it: the numbers
// above are the DEFAULT executor's behavior on a compute-heavy kernel, and
// tuning them away here would delete the measurement. The item count is
// printed, and called out when it binds, so a flat speedup reads as
// grain-limited rather than as the executor failing to scale.
//
//   ./build/examples/nbody                       # 4096 bodies, 50 ticks
//   ECS_BODIES=8192 ECS_TICKS=200 ./build/examples/nbody
//   ECS_LANES=1,2,4 ./build/examples/nbody       # lane sweep
//   ECS_BENCH_OUT=/tmp/b.csv ./build/examples/nbody
//
// Build Release for meaningful timing: cmake -DCMAKE_BUILD_TYPE=Release

#include <ecs/ecs.hpp>

#include "bench.hpp"
#include "nbody.hpp"
#include "simulation.hpp"
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <utility>
#include <vector>

using namespace ecs;

namespace {

int env_int(char const* key, int const dflt) { return int(bench::env_long(key, dflt)); }

struct RunResult {
    std::vector<float> state; // every body's position + velocity, serial order
    double ms_per_tick = 0;
    Diag diag {};
    std::size_t levels = 0;
};

RunResult run(unsigned const lanes, int const ticks, int const n) {
    World world;
    // Load, then loop: seed at load so every tick the schedule runs is a steady
    // tick, and prewarm can size the per-item state against a populated world.
    seed_bodies(world, n);

    Schedule s;
    build_nbody_schedule(s);

    WorkerPool pool {lanes};
    s.prewarm(world);
    for (int i = 0, wu = env_int("ECS_WARMUP", 0); i < wu; ++i)
        s.run(world, pool);

    auto const t0 = std::chrono::steady_clock::now();
    for (int t = 0; t < ticks; ++t)
        s.run(world, pool);
    auto const t1 = std::chrono::steady_clock::now();

    RunResult r;
    r.levels      = s.level_count();
    r.ms_per_tick = std::chrono::duration<double, std::milli>(t1 - t0).count() / ticks;
    r.diag        = world.resource<Diag>();
    world.for_each<Position const, Velocity const>([&](auto& p, auto& v) {
        r.state.insert(r.state.end(), {p.x, p.y, p.z, v.x, v.y, v.z});
    });
    return r;
}

} // namespace

int main() {
    bench::set_suite("nbody");
    int const n     = env_int("ECS_BODIES", 4096);
    int const ticks = env_int("ECS_TICKS", 50);

    std::printf("naive O(n^2) n-body: %d bodies, %d ticks, dt=%.3f, eps=%.3f\n",
                n,
                ticks,
                double(cfg::kDt),
                double(cfg::kSoft));
    std::printf("%lld interactions per tick\n", (long long)n * n);

    auto const lanes = bench::lane_set();

    // How many work items the executor will cut `forces` into, by the same rule
    // WavePlan::build_parallel uses -- see the header comment for why it matters
    // more here than for a typical kernel.
    constexpr std::size_t kMinItemRows = 1024; // WavePlan::kMinItemRows
    constexpr std::size_t kTargetItems = 64;   // WavePlan::kTargetItemsPerSystem
    std::size_t const grain = std::max(kMinItemRows, std::size_t(n) / kTargetItems);
    std::size_t const items = (std::size_t(n) + grain - 1) / grain;
    std::printf("work items for `forces`: %zu (grain %zu rows)\n", items, grain);
    if (items < lanes.back())
        std::printf("NOTE: %zu items < %u lanes -- speedup is capped at %zux by the "
                    "grain floor, not by the kernel. Use ECS_BODIES >= %zu to "
                    "saturate %u lanes.\n",
                    items,
                    lanes.back(),
                    items,
                    kMinItemRows * lanes.back(),
                    lanes.back());
    std::printf("\n");

    std::vector<RunResult> results;
    results.reserve(lanes.size());
    for (unsigned const l : lanes) {
        auto r = run(l, ticks, n);
        std::printf("%2u lane%s: %8.3f ms/tick  %6.1f Minteractions/s  "
                    "energy drift %.3e  |P| %.3e\n",
                    l,
                    l == 1 ? " " : "s",
                    r.ms_per_tick,
                    double(n) * double(n) / (r.ms_per_tick * 1e3),
                    r.diag.max_drift,
                    r.diag.p);
        bench::emit("naive",
                    "ms_per_tick",
                    r.ms_per_tick,
                    "ms",
                    "lower",
                    std::size_t(n),
                    l,
                    std::size_t(ticks));
        bench::emit("naive",
                    "energy_drift",
                    r.diag.max_drift,
                    "rel",
                    "lower",
                    std::size_t(n),
                    l,
                    std::size_t(ticks));
        results.push_back(std::move(r));
    }

    auto const& base = results.front();
    std::printf("\nschedule: %zu waves (gather -> forces -> integrate -> diagnostics)\n",
                base.levels);
    std::printf("energy: E0 = %.6f, E = %.6f, max |dE/E0| = %.3e over %llu ticks\n",
                base.diag.e0,
                base.diag.e,
                base.diag.max_drift,
                (unsigned long long)base.diag.ticks);
    std::printf("momentum: |P0| = %.3e, |P| = %.3e, max drift %.3e\n",
                base.diag.p0,
                base.diag.p,
                base.diag.max_p_drift);

    // Lane-count invariance: the library's headline determinism claim. Every
    // body's sum walks the same array in the same order regardless of how the
    // rows were sliced, so the result must be bitwise identical -- not close.
    bool all_identical = true;
    for (std::size_t i = 1; i < results.size(); ++i) {
        bool const same = results[i].state == base.state;
        all_identical   = all_identical && same;
        std::printf("state %u-lane == %u-lane (bitwise): %s\n",
                    lanes[i],
                    lanes.front(),
                    same ? "YES" : "NO");
    }
    if (results.size() > 1) {
        double const speedup = base.ms_per_tick / results.back().ms_per_tick;
        std::printf("speedup %u -> %u lanes: %.2fx\n",
                    lanes.front(),
                    lanes.back(),
                    speedup);
        bench::emit("naive",
                    "speedup",
                    speedup,
                    "x",
                    "higher",
                    std::size_t(n),
                    lanes.back(),
                    std::size_t(ticks));
    }

    bool ok = true;
    if (!all_identical) {
        std::printf("FAIL: results differ across lane counts\n");
        ok = false;
    }
    if (!(base.diag.max_drift < 1e-3)) {
        std::printf("FAIL: energy drift %.3e exceeds 1e-3\n", base.diag.max_drift);
        ok = false;
    }
    // Momentum is conserved exactly by a pairwise-symmetric force, so this
    // checks float rounding, not physics -- it catches an asymmetric or
    // double-counted interaction, which is precisely the bug a tree code is
    // prone to.
    if (!(base.diag.max_p_drift < 1e-5)) {
        std::printf("FAIL: momentum drift %.3e exceeds 1e-5\n", base.diag.max_p_drift);
        ok = false;
    }
    std::printf("%s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
