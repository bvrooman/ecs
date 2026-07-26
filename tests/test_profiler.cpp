// tests/test_profiler.cpp -- the two schedule timing observers in tools/
// (ScheduleReport, ScheduleTrace). These observer tools always compile (no build
// flag); this TU links ecs::profiler for their headers. The core observer
// *mechanism* (events().add / notification order / multiplicity) is tested
// separately in test_schedule.cpp.

#include <ecs/diag/schedule_report.hpp>
#include <ecs/diag/schedule_trace.hpp>
#include <ecs/diag/sink.hpp>

#include "check.hpp"
#include "setup.hpp"
#include <algorithm>
#include <cstdint>
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
    sched.add_serial("integrate", [](Query<Position, Velocity const> q) {
        q.for_each([](auto& p, auto& v) {
            p.x += v.dx;
            p.y += v.dy;
        });
    });
    sched.add_serial("render", [](Query<Position const>) {});
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
    CHECK(rows.front() == "tick,wave,system,busy_us,prepare_us,finish_us,items,"
                          "wave_us,flush_us,tick_us,cmd_us,build_us,sort_us");
    CHECK(rows.size() == 1 + std::size_t(kTicks) * 2);
    CHECK((rows[1].find(",integrate,") != std::string::npos ||
           rows[1].find(",render,") != std::string::npos));
    // Every measurement column is numeric: the row parses into 13 fields with
    // no gaps (CSV sinks feed tools/schedule_report directly).
    CHECK(std::count(rows[1].begin(), rows[1].end(), ',') == 12);
    // Lone-wave systems: busy time is the item's real duration, so the wave's
    // wall time (wave_us) must be >= its one system's busy time. Field 3 is
    // busy_us, field 7 is wave_us.
    {
        std::vector<std::string> f;
        std::size_t pos = 0;
        for (std::size_t c = rows[1].find(',');;
             pos = c + 1, c = rows[1].find(',', pos)) {
            f.emplace_back(rows[1].substr(pos, c - pos));
            if (c == std::string::npos)
                break;
        }
        CHECK(f.size() == 13);
        CHECK(std::stod(f[3]) > 0.0);              // busy was measured
        CHECK(std::stod(f[7]) >= std::stod(f[3])); // wave wall >= lone busy
        CHECK(std::stod(f[9]) >= std::stod(f[7])); // tick >= wave
        CHECK(std::stod(f[6]) >= 1.0);             // items
        CHECK(std::stod(f[10]) == 0.0);            // no commands -> no cmd flush
        // build_us (flatten) and sort_us are DISJOINT build phases, both inside
        // the wave, so the wave wall covers their sum.
        CHECK(std::stod(f[7]) >= std::stod(f[11]) + std::stod(f[12]));
    }
}

// A fanned wave (two non-conflicting systems flattened into one dispatch) has
// no per-system wall interval -- but SystemWork carries each system's REAL
// measured busy time, which is the regression this event exists to fix (the
// old balanced Begin/End pairs reported ~0 for every fanned system).
static void fanned_wave_reports_real_busy_time() {
    World w;
    setup(w, [](Commands& c) {
        for (int i = 0; i < 30'000; ++i)
            c.spawn(Position {float(i), 0}, Velocity {1, 1});
    });
    Schedule sched;
    // Disjoint access -> one wave, two systems, flattened items.
    sched.add_parallel("move", [](Query<Position> q) {
        q.for_each([](auto& p) { p.x += 0.5f; });
    });
    sched.add_parallel("drag", [](Query<Velocity> q) {
        q.for_each([](auto& v) { v.dx *= 0.999f; });
    });
    CHECK(sched.level_count() == 1);

    std::vector<std::string> rows;
    diag::ScheduleTrace trace([&](std::string_view s) { rows.emplace_back(s); });
    sched.events().add(std::ref(trace));
    WorkerPool pool {4};
    for (int t = 0; t < 3; ++t)
        sched.run(w, pool);
    trace.flush();

    CHECK(rows.size() == 1 + 3 * 2); // header + 2 systems x 3 ticks
    auto busy_of = [&](std::string_view name) {
        double total = 0;
        for (auto const& r : rows) {
            auto const key = "," + std::string(name) + ",";
            if (auto const at = r.find(key); at != std::string::npos) {
                auto const rest = r.substr(at + key.size());
                total += std::stod(rest.substr(0, rest.find(',')));
            }
        }
        return total;
    };
    CHECK(busy_of("move") > 0.0); // real measurements, not balanced-pair zeros
    CHECK(busy_of("drag") > 0.0);
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
    CHECK(tick_line);                                  // report produced output
    CHECK(trace_out.size() == 1 + std::size_t(3) * 2); // trace produced rows
}

// A per-N-tick system (Schedule `every`) runs only on the ticks it is due, so
// it contributes trace rows on those ticks alone -- like a one-shot phase, its
// wave exists for a subset of ticks. The observer sees it through the ordinary
// SystemWork path, no maintenance special case.
static void trace_records_every_cadence_system() {
    World w;
    populate(w);
    Schedule sched = make_sched(); // integrate (wave 0), render (wave 1)
    int ran        = 0;
    // Own phase so it is alone in its wave; that wave is present only every 3rd
    // tick and skipped (no rows) otherwise.
    sched.add_serial(
        "beat",
        [&](Query<Position const>) { ++ran; },
        phase<1> {},
        /*every=*/3);

    std::vector<std::string> rows;
    diag::ScheduleTrace trace([&](std::string_view s) { rows.emplace_back(s); });
    sched.events().add(std::ref(trace));

    WorkerPool pool {1};
    constexpr int kTicks = 9;
    for (int t = 0; t < kTicks; ++t)
        sched.run(w, pool);
    trace.flush();

    auto field = [](std::string const& r, std::size_t idx) {
        std::size_t pos = 0;
        for (std::size_t k = 0; k < idx; ++k)
            pos = r.find(',', pos) + 1;
        return r.substr(pos, r.find(',', pos) - pos);
    };
    // tick_ counts from 1 inside run(); every==3 fires on tick_ 3,6,9, i.e.
    // trace ticks 2,5,8 (0-based). "beat" rows appear only there.
    int beat_rows = 0;
    for (std::size_t i = 1; i < rows.size(); ++i)
        if (field(rows[i], 2) == "beat") {
            ++beat_rows;
            int const t = std::stoi(field(rows[i], 0));
            CHECK((t + 1) % 3 == 0); // trace ticks 2, 5, 8
        }
    CHECK(beat_rows == 3);
    CHECK(ran == 3); // body ran on exactly the 3 due ticks
}

// A wave's command flush is attributed per system: two command-recording
// systems share one wave (both take only Commands&, so disjoint access), and
// the one that records far more commands is charged far more flush -- the case
// a wave-level flush number cannot separate.
static void flush_attributed_per_system() {
    World w;
    Schedule s;
    s.add_serial("cheap", [](Commands& c) { c.spawn(Position {1, 0}); });
    s.add_serial("heavy", [](Commands& c) {
        for (int i = 0; i < 20'000; ++i)
            c.spawn(Position {float(i), 0});
    });
    CHECK(s.level_count() == 1); // no access conflict -> one shared wave

    double heavy_flush = 0, cheap_flush = 0;
    s.events().add([&](ScheduleEvent const& e) {
        if (auto const* sw = std::get_if<sched_event::SystemWork>(&e)) {
            if (sw->name == "heavy")
                heavy_flush += sw->flush_us;
            else if (sw->name == "cheap")
                cheap_flush += sw->flush_us;
        }
    });
    WorkerPool pool {4};
    s.run(w, pool);

    CHECK(w.size() == 20'001);        // both systems' spawns applied
    CHECK(heavy_flush > 0.0);         // heavy's commands were timed
    CHECK(heavy_flush > cheap_flush); // and charged more than the cheap system
}

int main() {
    RUN_SUITE(report_summarizes_systems);
    RUN_SUITE(flush_attributed_per_system);
    RUN_SUITE(trace_writes_one_row_per_system_per_tick);
    RUN_SUITE(fanned_wave_reports_real_busy_time);
    RUN_SUITE(report_and_trace_compose);
    RUN_SUITE(trace_records_every_cadence_system);
    return REPORT();
}
