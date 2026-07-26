// tools/schedule_profiler_demo.cpp -- profile a sample schedule with two
// observers.
//
// Runs a small ECS schedule for a few thousand ticks with BOTH timing observers
// registered on the schedule at once:
//   * ScheduleReport -> stdout : rolling per-system/wave/tick distribution
//   * ScheduleTrace  -> profiler_trace.csv : one CSV row per system per tick
// It demonstrates that a Schedule can drive multiple observers, and that the
// report and trace are independent observers you can attach separately.
//
//   ./schedule-profiler
//   head profiler_trace.csv
#include <ecs/diag/schedule_report.hpp>
#include <ecs/diag/schedule_trace.hpp>
#include <ecs/diag/sink.hpp>
#include <ecs/ecs.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional> // std::ref
#include <iostream>

struct Position {
    float x, y;
};
struct Velocity {
    float dx, dy;
};
struct Health {
    int hp;
};

int main() {
    using namespace ecs;

    // Populate the world via the production setup idiom: a one-shot system run
    // inline (no thread pool needed).
    World world;
    {
        Schedule setup;
        setup.add_serial(
            "spawn",
            [](Commands& cmd) {
                for (int i = 0; i < 50'000; ++i)
                    cmd.spawn(Position {0, 0}, Velocity {1, 1}, Health {100});
            },
            times {1});
        setup.run(world);
    }

    Schedule sched;
    // Cheap: advance positions (writes Position, reads Velocity).
    sched.add_serial("integrate", [](Query<Position, Velocity const> q) {
        q.for_each([](auto& p, auto& v) {
            p.x += v.dx;
            p.y += v.dy;
        });
    });
    // Expensive: a curl-ish turbulence pass (writes Velocity, reads Position).
    // It conflicts with integrate, so it lands in a later wave -- and the sin/cos
    // makes it visibly the hottest system in the report.
    sched.add_serial("turbulence", [](Query<Position const, Velocity> q) {
        q.for_each([](auto& p, auto& v) {
            v.dx += 0.01f * std::sin(p.y);
            v.dy += 0.01f * std::cos(p.x);
        });
    });
    // Cheap independent branch (writes Health) -- shares wave 0 with integrate.
    sched.add_serial("damage", [](Query<Health> q) {
        q.for_each([](auto& h) {
            if (--h.hp <= 0)
                h.hp = 100;
        });
    });
    // Reads everything -> runs after the writers, in its own final wave.
    sched.add_serial("render", [](WorldView) {});

    // Two observers on one schedule: a distribution report and a CSV trace. Each
    // is a stateful visitor, registered on the schedule's event emitter by
    // reference (std::ref); they are notified in registration order.
    std::ofstream csv("profiler_trace.csv");
    diag::ScheduleReport report(diag::to_stream(std::cout));
    diag::ScheduleTrace trace(diag::to_stream(csv));
    sched.events().add(std::ref(report));
    sched.events().add(std::ref(trace));

    WorkerPool pool {1}; // serial: profiling measures per-system wall time
    constexpr int kTicks = 4000;
    for (int t = 0; t < kTicks; ++t)
        sched.run(world, pool);

    // Final flush of both regardless of the ~2s cadence.
    report.flush();
    trace.flush();

    std::printf("\nran %d ticks with %zu observers; wrote per-tick trace to "
                "profiler_trace.csv\n",
                kTicks,
                sched.events().observer_count());
}
