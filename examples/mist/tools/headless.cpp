// examples/mist/tools/headless.cpp
//
// The murmuration without the renderer: run the mist schedule for a fixed
// number of ticks and verify the port's lane-count-invariance claim -- the
// same seed produces a BITWISE identical flock (every position, velocity, and
// published draw vertex) at 1 lane and 4 lanes, run after run -- then report
// tick timing. Builds on every platform (no GLFW), so it doubles as the
// demo's determinism check and a quick perf probe:
//
//   ./build/examples/mist-headless               # defaults: 40k birds, 240 ticks
//   ECS_PARTICLES=85000 ECS_TICKS=600 ./build/examples/mist-headless
//   ECS_METRICS=/tmp/m.csv ./build/examples/mist-headless  # + metrics CSV
//     (the CSV is written once per run(); the last of the three runs wins)
//   ECS_TRACE=/tmp/t.csv ./build/examples/mist-headless    # + schedule trace CSV
//     (per-system per-tick rows for tools/schedule_report; the deterministic
//      fixed-seed run makes two traces from the same config directly
//      comparable -- `schedule-report compare base.csv head.csv`. Written per
//      run(); the last run -- 4 lanes -- wins, like ECS_METRICS.)
//   ECS_BENCH_OUT=/tmp/b.csv ./build/examples/mist-headless # + results rows
//     (per-lane us/tick + speedup in the benchmarks/bench.hpp CSV schema)
//   ECS_PREWARM=0 ./build/examples/mist-headless             # skip prewarm
//     (A/B the cold vs warm first tick: by default the flock is seeded at load
//      and Schedule::prewarm pre-pays the kernels' first-tick state sizing, so
//      tick 0 is steady; =0 leaves it cold for comparison)
//
// The schedule registers its own sort-rows maintenance hook (every 64 ticks,
// see simulation.cpp), so the bitwise checks below also prove determinism
// composes with row sorting.
#include "bench.hpp"
#include "ecs/ecs.hpp"
#include "mist.hpp"
#include "schedule_trace.hpp"
#include "simulation.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <random>
#include <vector>

using namespace ecs;

namespace {

WindowState g_wstate; // cursor inactive: the goal wanders on its own

int env_int(char const* key, int const dflt) {
    if (char const* v = std::getenv(key))
        if (int n = std::atoi(v); n > 0)
            return n;
    return dflt;
}

struct RunResult {
    std::vector<float> state; // every bird's position + velocity, serial order
    RenderSnapshot frame;     // last published draw snapshot
    double ms_per_tick = 0;
    std::size_t levels = 0;
};

RunResult run(unsigned const lanes, int const ticks, int const count) {
    World w;
    w.emplace_resource<Rng>(Rng {std::mt19937 {12345u}});
    w.emplace_resource<Clock>(Clock {0.0f});
    w.emplace_resource<Cursor>(Cursor {});
    w.emplace_resource<Goal>(Goal {0.0f, 0.0f, 0.0f});
    w.emplace_resource<FlockGrid>();
    w.emplace_resource<FlockStats>();
    auto& channel = w.emplace_resource<TripleBuffer<RenderSnapshot>>();
    w.emplace_resource<SnapshotTarget>(SnapshotTarget {&channel});

    // Load-then-loop: seed the flock now (not via a scatter-on-tick-0 system),
    // so the per-tick schedule below holds only steady work and every traced
    // tick is a real sim tick. This is what lets prewarm pre-pay the kernels'
    // first-tick allocation -- the birds already exist when it sizes the state.
    seed_flock(w, count);

    Schedule s;
    build_mist_schedule(s, MistInput {&g_wstate.cursor, &g_wstate.flags});

    // Optional schedule trace, truncated per run() so the last run wins (the
    // 4-lane rerun) -- same convention as ECS_METRICS above.
    std::ofstream trace_file;
    std::optional<diag::ScheduleTrace> trace;
    std::optional<event::Emitter<ScheduleEvent>::Subscription> trace_sub;
    if (char const* path = std::getenv("ECS_TRACE")) {
        trace_file.open(path);
        trace.emplace(diag::to_stream(trace_file));
        trace_sub.emplace(s.events().subscribe(std::ref(*trace)));
    }

    WorkerPool pool {lanes};
    // Pre-pay the kernels' one-time first-tick state sizing (ECS_PREWARM=0 to
    // skip it, for an A/B of the cold vs warm first tick).
    char const* pw = std::getenv("ECS_PREWARM");
    if (!pw || std::atoi(pw) != 0)
        s.prewarm(w);
    auto const t0 = std::chrono::steady_clock::now();
    for (int t = 0; t < ticks; ++t) // every tick is a real sim tick now
        s.run(w, pool);
    auto const t1 = std::chrono::steady_clock::now();
    if (trace)
        trace->flush(); // drain the buffered tail before trace_file closes

    RunResult r;
    r.levels = s.level_count();
    r.ms_per_tick =
        std::chrono::duration<double, std::milli>(t1 - t0).count() / ticks;
    query<Position const, Velocity const>(w).for_each_serial([&](auto& p, auto& v) {
        r.state.insert(r.state.end(), {p.x, p.y, p.z, v.x, v.y, v.z});
    });
    channel.consume();
    r.frame = channel.front();
    return r;
}

} // namespace

int main() {
    bench::set_suite("mist");
    int const ticks = env_int("ECS_TICKS", 240); // 8 s of flight at 30 Hz
    int const count = env_int("ECS_PARTICLES", 40'000);

    auto const serial   = run(1, ticks, count);
    auto const parallel = run(4, ticks, count);
    auto const again    = run(4, ticks, count);

    auto const n = std::size_t(count), t = std::size_t(ticks - 1);
    bench::emit("flock", "us_per_tick", serial.ms_per_tick * 1e3, "us", "lower",
                n, 1, t);
    bench::emit("flock", "us_per_tick", parallel.ms_per_tick * 1e3, "us",
                "lower", n, 4, t);
    bench::emit("flock", "pool_speedup",
                serial.ms_per_tick / parallel.ms_per_tick, "x", "higher", n, 4, t);

    std::printf("mist-headless: %d birds, %d ticks, %zu schedule levels\n",
                count,
                ticks,
                serial.levels);
    std::printf("1-lane: %.2f ms/tick   4-lane: %.2f ms/tick   speedup: %.2fx\n",
                serial.ms_per_tick,
                parallel.ms_per_tick,
                serial.ms_per_tick / parallel.ms_per_tick);

    bool const state_ok = serial.state == parallel.state && !serial.state.empty();
    bool const rerun_ok = parallel.state == again.state;
    bool frame_ok = serial.frame.size() == parallel.frame.size() &&
                    serial.frame.size() == std::size_t(count);
    for (std::size_t i = 0; frame_ok && i < serial.frame.size(); ++i)
        frame_ok = serial.frame[i].x == parallel.frame[i].x &&
                   serial.frame[i].y == parallel.frame[i].y &&
                   serial.frame[i].z == parallel.frame[i].z;

    std::printf("state  1-lane == 4-lane (bitwise): %s\n", state_ok ? "YES" : "NO");
    std::printf("state  4-lane run-to-run:          %s\n", rerun_ok ? "YES" : "NO");
    std::printf("frame  1-lane == 4-lane (bitwise): %s\n", frame_ok ? "YES" : "NO");
    return (state_ok && rerun_ok && frame_ok) ? 0 : 1;
}
