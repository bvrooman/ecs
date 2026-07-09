// tests/test_profiler.cpp -- the two schedule timing observers in tools/
// (ScheduleReport, ScheduleTrace). These observer tools always compile (no build
// flag); this TU links ecs::profiler for their headers. The core observer
// *mechanism* (events().add / notification order / multiplicity) is tested
// separately in test_schedule.cpp.

#include "check.hpp"
#include "schedule_report.hpp"
#include "schedule_trace.hpp"
#include "setup.hpp"
#include "sink.hpp"

#include <functional> // std::ref
#include <string>
#include <string_view>
#include <vector>

using namespace ecs;

struct Position {
    float x, y;
};
struct Velocity {
    float dx, dy;
};

// Two systems -> two waves (integrate writes Position, render reads it).
static Schedule make_sched() {
    Schedule sched;
    sched.add("integrate", [](Query<Position, Velocity const> q) {
        q.for_each_serial([](auto& p, auto& v) {
            p.x += v.dx;
            p.y += v.dy;
        });
    });
    sched.add("render", [](Query<Position const>) {});
    return sched;
}

static void populate(World& w) {
    setup(w, [](Commands& c) {
        for (int i = 0; i < 100; ++i)
            c.spawn(Position {}, Velocity {1, 1});
    });
}

// ScheduleReport emits a "sim tick" summary, then each wave with its systems
// grouped beneath it in execution order.
static void report_summarizes_systems() {
    World w;
    populate(w);
    Schedule sched = make_sched(); // integrate -> wave 0, render -> wave 1

    std::vector<std::string> out;
    diag::ScheduleReport report([&](std::string_view s) { out.emplace_back(s); });
    sched.events().add(std::ref(report));

    WorkerPool pool {1};
    for (int t = 0; t < 5; ++t)
        sched.run(w, pool);
    report.flush(); // 2s cadence never elapses in a test -> force it

    auto const idx = [&](std::string_view needle) -> int {
        for (int i = 0; i < static_cast<int>(out.size()); ++i)
            if (out[i].find(needle) != std::string::npos)
                return i;
        return -1;
    };
    int const i_tick = idx("sim tick");
    int const i_w0 = idx("wave 0"), i_int = idx("integrate");
    int const i_w1 = idx("wave 1"), i_ren = idx("render");
    CHECK(i_tick == 0); // tick summary first
    CHECK((i_w0 >= 0 && i_int >= 0 && i_w1 >= 0 && i_ren >= 0));
    // Systems grouped under their wave, in execution order: integrate under
    // wave 0 (before wave 1), render under wave 1.
    CHECK((i_w0 < i_int && i_int < i_w1 && i_w1 < i_ren));
    // A system line is indented deeper (4 spaces) than its wave line (2 spaces).
    CHECK(out[i_int].rfind("    ", 0) == 0);
    CHECK(out[i_w0].rfind("    ", 0) != 0);
}

// ScheduleTrace emits a CSV header plus one row per system per tick.
static void trace_writes_one_row_per_system_per_tick() {
    World w;
    populate(w);
    Schedule sched = make_sched();

    std::vector<std::string> rows;
    diag::ScheduleTrace trace([&](std::string_view s) { rows.emplace_back(s); });
    sched.events().add(std::ref(trace));

    WorkerPool pool {1};
    constexpr int kTicks = 5;
    for (int t = 0; t < kTicks; ++t)
        sched.run(w, pool);
    trace.flush();

    CHECK(!rows.empty());
    CHECK(rows.front() == "tick,wave,system,us"); // header on construction
    CHECK(rows.size() == 1 + std::size_t(kTicks) * 2);
    CHECK((rows[1].find(",integrate,") != std::string::npos ||
           rows[1].find(",render,") != std::string::npos));
}

// Both observers can drive the same schedule at once (report + trace).
static void report_and_trace_compose() {
    World w;
    populate(w);
    Schedule sched = make_sched();

    std::vector<std::string> report_out, trace_out;
    diag::ScheduleReport report([&](std::string_view s) { report_out.emplace_back(s); });
    diag::ScheduleTrace trace([&](std::string_view s) { trace_out.emplace_back(s); });
    sched.events().add(std::ref(report));
    sched.events().add(std::ref(trace));
    CHECK(sched.events().observer_count() == 2);

    WorkerPool pool {1};
    for (int t = 0; t < 3; ++t)
        sched.run(w, pool);
    report.flush();
    trace.flush();

    bool tick_line = false;
    for (auto const& l : report_out)
        tick_line |= l.find("sim tick") != std::string::npos;
    CHECK(tick_line);                                // report produced output
    CHECK(trace_out.size() == 1 + std::size_t(3) * 2); // trace produced rows
}

int main() {
    RUN_SUITE(report_summarizes_systems);
    RUN_SUITE(trace_writes_one_row_per_system_per_tick);
    RUN_SUITE(report_and_trace_compose);
    return REPORT();
}
