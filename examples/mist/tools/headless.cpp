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
//
// The schedule registers its own sort-rows maintenance hook (every 64 ticks,
// see simulation.cpp), so the bitwise checks below also prove determinism
// composes with row sorting.
#include "ecs/ecs.hpp"
#include "mist.hpp"
#include "simulation.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

    Schedule s;
    build_mist_schedule(s, MistInput {&g_wstate.cursor, &g_wstate.flags}, count);

    WorkerPool pool {lanes};
    s.run(w, pool); // tick 1: scatter + first sim tick
    auto const t0 = std::chrono::steady_clock::now();
    for (int t = 1; t < ticks; ++t)
        s.run(w, pool);
    auto const t1 = std::chrono::steady_clock::now();

    RunResult r;
    r.levels = s.level_count();
    r.ms_per_tick =
        std::chrono::duration<double, std::milli>(t1 - t0).count() / (ticks - 1);
    query<Position const, Velocity const>(w).for_each_serial([&](auto& p, auto& v) {
        r.state.insert(r.state.end(), {p.x, p.y, p.z, v.x, v.y, v.z});
    });
    channel.consume();
    r.frame = channel.front();
    return r;
}

} // namespace

int main() {
    int const ticks = env_int("ECS_TICKS", 240); // 8 s of flight at 30 Hz
    int const count = env_int("ECS_PARTICLES", 40'000);

    auto const serial   = run(1, ticks, count);
    auto const parallel = run(4, ticks, count);
    auto const again    = run(4, ticks, count);

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
