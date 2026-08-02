// std::execution scheduler tests. Access is derived from system parameters.
#include <ecs/event/observer.hpp> // ecs::event::overloaded

#include "check.hpp"
#include "setup.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string_view>
#include <variant>
#include <vector>

using namespace ecs;

struct Position {
    float x, y;
};

struct Velocity {
    float dx, dy;
};

struct Health {
    int hp;
};

static void leveling_respects_conflicts() {
    Schedule sched;
    auto a = sched.add_serial("physics", [](Query<Position>) {});      // writes Position
    auto b = sched.add_serial("damage", [](Query<Health>) {});         // writes Health
    auto c = sched.add_serial("render", [](Query<Position const>) {}); // reads Position
    // physics & damage are independent -> level 0; render reads Position that
    // physics writes -> level 1.
    CHECK(sched.level_count() == 2);
    CHECK(sched.systems()[a].level == 0); // physics
    CHECK(sched.systems()[b].level == 0); // damage
    CHECK(sched.systems()[c].level == 1); // render
}

static void parallel_systems_are_independent_levels() {
    Schedule sched;
    // Three systems with disjoint writes -> all level 0, fully parallel.
    sched.add_serial("a", [](Query<Position>) {});
    sched.add_serial("b", [](Query<Velocity>) {});
    sched.add_serial("c", [](Query<Health>) {});
    CHECK(sched.level_count() == 1);
}

static void run_on_thread_pool_executes_all_systems() {
    World w;
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 1000; ++i)
            cmd.spawn(Position {0, 0}, Velocity {1, 2}, Health {100});
    });

    std::atomic<int> render_calls {0};
    Schedule sched;
    // writes Position, reads Velocity
    sched.add_serial("physics", [](Query<Position, Velocity const> q) {
        q.for_each([](auto& p, auto& v) {
            p.x += v.dx;
            p.y += v.dy;
        });
    });
    // writes Health
    sched.add_serial("damage",
                     [](Query<Health> q) { q.for_each([](auto& h) { h.hp -= 1; }); });
    // no ECS access -- just a side effect
    sched.add_serial("render",
                     [&] { render_calls.fetch_add(1, std::memory_order_relaxed); });

    WorkerPool pool {4};
    sched.run(w, pool);
    sched.run(w, pool);

    Entity first = Entity::from_raw(0, 0);
    CHECK(w.get<Position>(first).x == 2.f); // physics ran twice
    CHECK(w.get<Health>(first).hp == 98);   // damage ran twice
    CHECK(render_calls.load() == 2);        // render ran each tick
}

// A WorldView reads everything but writes nothing: read-only systems sharing a
// WorldView run in the same wave, but a WorldView reader is serialized after
// any writer (it could observe a half-applied write otherwise). A raw World&,
// by contrast, is exclusive and conflicts with everyone.
static void worldview_reads_all_parallel_but_after_writers() {
    {
        Schedule sched;
        sched.add_serial("observe_a", [](WorldView) {});
        sched.add_serial("observe_b", [](WorldView) {});
        CHECK(sched.level_count() == 1); // two readers -> one wave
    }
    {
        Schedule sched;
        sched.add_serial("mutate", [](Query<Position>) {}); // writes Position
        sched.add_serial("observe", [](WorldView) {});      // reads everything
        CHECK(sched.level_count() == 2);
        // reader serialized after writer
    }
    {
        Schedule sched;
        sched.add_serial("observe", [](WorldView) {}); // reads everything
        // Raw World& is not a system parameter; a genuinely unanalyzable
        // system declares itself exclusive via add_dynamic_serial.
        SystemAccess excl;
        excl.exclusive = true;
        sched.add_dynamic_serial("exclusive",
                                 excl,
                                 [](World&, Commands&, detail::WorkItem const&) {});
        CHECK(sched.level_count() == 2); // exclusive conflicts with all
    }
}

static void worldview_sees_writers_flush_in_prior_wave() {
    World w;
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 5; ++i)
            cmd.spawn(Position {1, 1});
    });

    std::atomic<int> observed {-1};
    Schedule sched;
    // Spawns three more, flushed at the wave barrier.
    sched.add_serial("spawn", [](Commands& cmd) {
        for (int i = 0; i < 3; ++i)
            cmd.spawn(Position {2, 2});
    });
    // Read-only ad-hoc access via WorldView, serialized after spawn by phase.
    sched.add_serial(
        "observe",
        [&](WorldView view) {
            observed.store(int(view.size()), std::memory_order_relaxed);
        },
        phase {1});

    WorkerPool pool {4};
    sched.run(w, pool);
    CHECK(w.size() == 8);
    CHECK(observed.load() == 8); // saw the spawn wave's flush
}

// A system that throws on the thread-pool path propagates out of run() (caught
// and rethrown at the barrier) rather than calling std::terminate.
static void system_exception_propagates() {
    World w;
    Schedule sched;
    sched.add_serial("boom", [](Commands&) { throw std::runtime_error("boom"); });

    WorkerPool pool {2};
    bool caught = false;
    try {
        sched.run(w, pool);
    } catch (std::runtime_error const& e) {
        caught = (std::string_view(e.what()) == "boom");
    }
    CHECK(caught);
}

// A run that throws mid-wave must DISCARD the edits recorded before the throw:
// they must not leak into a later run's first flush (which could even belong
// to a different schedule sharing the world).
static void aborted_run_discards_recorded_commands() {
    World w;
    Schedule bad;
    bad.add_serial("spawner", [](Commands& cmd) {
        cmd.spawn(Position {1, 1}); // recorded before the throw...
    });
    bad.add_serial("boom",
                   [](Query<Position const>) { throw std::runtime_error("boom"); });

    bool threw = false;
    try {
        bad.run(w);
    } catch (std::runtime_error const&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(w.size() == 0); // nothing flushed by the failed run

    // A subsequent clean run on the same world must not replay the discarded
    // spawn.
    Schedule ok;
    ok.add_serial("noop", [](Commands&) {}, times {1});
    ok.run(w);
    CHECK(w.size() == 0);
}

// Registration options are an order-independent pack: each add_* form accepts
// phase/every/times in any order, and omitting one uses its default. A parallel
// system takes them too -- it can be one-shot like a serial one.
static void registration_options_are_order_independent() {
    World w;
    setup(w, [](Commands& cmd) {
        for (int i = 0; i < 4; ++i)
            cmd.spawn(Position {0, 0}, Velocity {1, 0});
    });
    WorkerPool pool {2};

    { // the two orderings must produce the same schedule shape
        Schedule a, b;
        a.add_serial("x", [](Commands&) {}, phase {-1}, every {2});
        b.add_serial("x", [](Commands&) {}, every {2}, phase {-1});
        CHECK(a.level_count() == b.level_count());
        CHECK(a.size() == b.size());
    }
    { // times on a PARALLEL system -- previously impossible
        Schedule s;
        std::atomic<int> ran {0};
        s.add_parallel(
            "bump",
            [&](Query<Position> q) {
                q.for_each([](auto& p) { p.x += 1.f; });
                ran.fetch_add(1, std::memory_order_relaxed);
            },
            times {1});
        for (int i = 0; i < 5; ++i)
            s.run(w, pool);
        CHECK(ran.load() == 1);
        CHECK(s.size() == 0); // retired after its single run
    }
    { // phase alone still orders, with every/times left at their defaults
        Schedule s;
        std::atomic<int> seen {-1};
        s.add_serial(
            "late",
            [&](Query<Position const> q) { seen.store(int(q.count())); },
            phase {1});
        s.add_serial(
            "early",
            [](Commands& cmd) { cmd.spawn(Position {9, 9}); },
            phase {-1});
        CHECK(s.level_count() == 2);
        s.run(w, pool);
        CHECK(seen.load() == 5); // 4 seeded + 1 spawned by the earlier phase
    }
}

// A tick emits exactly one WaveBegin per wave plan, and their ids are the plans'
// positions -- 0..n-1, ascending, in the order the waves run. Two things that
// are NOT that position and have each been shipped in its place: the wavefront
// level (two phases each contribute a level 0, so it repeats within a tick) and
// the order the waves were discovered while grouping (build_wave_plans sorts by
// {phase, level} afterwards, which permutes it). Consumers index per-wave state
// by the id and print in id order, so a repeat pools two waves into one bucket
// and a permutation reports them out of execution order.
//
// The registration order below is load-bearing: `startup` is registered LAST so
// that discovery order (a, b, startup) differs from execution order (startup,
// a, b). Register it first and the two coincide, and the test stops
// discriminating -- which is exactly how the discovery-order bug passed CI.
static void wave_ordinals_are_plan_positions() {
    World w;
    Schedule sched;
    sched.add_serial("a", [](Query<Position> q) { q.for_each([](auto&) {}); });
    sched.add_serial("b", [](Query<Position> q) { q.for_each([](auto&) {}); });
    sched.add_serial("startup", [](Commands&) {}, phase {-1}); // phase -1, level 0
    CHECK(sched.level_count() == 3);                           // (-1,0), (0,0), (0,1)

    std::vector<WaveId> begins;
    std::vector<std::string> order;
    std::size_t announced = 0;
    sched.events().add([&](ScheduleEvent const& e) {
        if (auto const* tb = std::get_if<sched_event::TickBegin>(&e))
            announced = tb->n_waves;
        else if (auto const* wb = std::get_if<sched_event::WaveBegin>(&e))
            begins.push_back(wb->ordinal);
        else if (auto const* sw = std::get_if<sched_event::SystemWork>(&e))
            order.emplace_back(sw->name);
    });
    sched.run(w);

    CHECK(announced == 3);
    CHECK(begins.size() == 3);         // one per plan
    CHECK(order.front() == "startup"); // the earlier phase really does run first
    for (auto id = WaveId {}; auto const got : begins)
        CHECK(got == id++); // 0,1,2 -- positions, not levels, not discovery order
}

// The observer hook is a general, always-available core feature -- an observer is
// any callable void(ScheduleEvent const&). This one counts the boundaries it is
// notified of and, on each SystemWork (the per-system boundary), appends its tag
// to a shared log so notification order across observers is observable.
struct TallyObserver {
    char tag;
    std::vector<char>* log;
    int ticks = 0, waves = 0, systems = 0;
    std::size_t last_n_waves = 0;
    TallyObserver(char t, std::vector<char>* l)
        : tag(t)
        , log(l) {}
    void operator()(ScheduleEvent const& e) {
        using namespace sched_event;
        std::visit(event::overloaded {
                       [this](TickBegin const& ev) {
                           ++ticks;
                           last_n_waves = ev.n_waves;
                       },
                       [this](WaveBegin const&) { ++waves; },
                       [this](SystemWork const&) {
                           ++systems;
                           if (log)
                               log->push_back(tag);
                       },
                       [](auto const&) {}, // TickEnd / WaveEnd ignored
                   },
                   e);
    }
};

// A Schedule can drive several observers; each is notified of every boundary, in
// registration order, and remove() detaches just one.
static void multiple_observers_notified_in_order() {
    World w;
    setup(w, [](Commands& c) {
        for (int i = 0; i < 8; ++i)
            c.spawn(Position {});
    });
    Schedule sched;
    sched.add_serial("physics", [](Query<Position> q) { q.for_each([](auto&) {}); });
    sched.add_serial("render", [](Query<Position const>) {}); // reads Position -> wave 1

    std::vector<char> order;
    TallyObserver a {'A', &order}, b {'B', &order};
    auto const ta = sched.events().add(std::ref(a));
    sched.events().add(std::ref(b));
    CHECK(sched.events().observer_count() == 2);

    WorkerPool pool {1};
    sched.run(w, pool);

    // Both observers saw the whole tick: 1 tick, 2 waves, 2 systems.
    CHECK((a.ticks == 1 && b.ticks == 1));
    CHECK((a.waves == 2 && b.waves == 2));
    CHECK((a.systems == 2 && b.systems == 2));
    CHECK((a.last_n_waves == 2 && b.last_n_waves == 2));
    // For each SystemWork, A (registered first) is notified before B.
    CHECK((order == std::vector<char> {'A', 'B', 'A', 'B'}));

    // Removing one observer stops only its notifications.
    CHECK(sched.events().remove(ta));
    CHECK(sched.events().observer_count() == 1);
    order.clear();
    sched.run(w, pool);
    CHECK((a.ticks == 1 && b.ticks == 2)); // A frozen, B advanced
    CHECK((order == std::vector<char> {'B', 'B'}));
}

// --- grain{n}: the parallel slice-size hint ---------------------------------
// The executor's default grain is derived from the row count alone, which
// assumes rows cost about the same. grain{n} is how a kernel whose per-row work
// is far from typical says otherwise. Item counts are observable directly on
// the SystemWork event, so these assert the slicing rather than infer it from
// timing.

// Run one trivial parallel system over `rows` entities at `grain_rows` (0 =
// executor default) and report how many work items it was cut into.
static std::uint32_t items_for(std::size_t const rows,
                               std::size_t const grain_rows,
                               unsigned const lanes) {
    World w;
    setup(w, [rows](Commands& cmd) {
        for (std::size_t i = 0; i < rows; ++i)
            cmd.spawn(Position {float(i), 0.0f});
    });
    Schedule sched;
    // Passing grain{0} here is deliberate: it exercises that the neutral value
    // takes exactly the same path as omitting the option.
    sched.add_parallel(
        "k",
        [](Query<Position> q) { q.for_each([](auto&) {}); },
        grain {grain_rows});
    std::uint32_t items = 0;
    sched.events().add([&](ScheduleEvent const& e) {
        if (auto const* sw = std::get_if<sched_event::SystemWork>(&e))
            items = sw->items;
    });
    WorkerPool pool {lanes};
    sched.run(w, pool);
    return items;
}

static void grain_sets_the_slice_size() {
    // Default: max(kMinItemRows = 1024, rows / 64) -> 1024 rows per item.
    CHECK(items_for(4096, 0, 1) == 4);
    // grain{n} overrides it outright, in both directions.
    CHECK(items_for(4096, 64, 1) == 64);
    CHECK(items_for(4096, 128, 1) == 32);
    CHECK(items_for(4096, 4096, 1) == 1);
    // A grain that does not divide the row count leaves a short final item.
    CHECK(items_for(4096, 1000, 1) == 5); // 4x1000 + one of 96
    // The case the option exists for: at or below the default floor every lane
    // but one would otherwise have nothing to claim.
    CHECK(items_for(1024, 0, 1) == 1);   // floor -> a single item, whatever the lanes
    CHECK(items_for(1024, 16, 1) == 64); // grain{16} -> 64 items
    // Items are a property of the schedule, not the pool: the same counts at
    // any lane count, which is what keeps barrier folds reproducible.
    CHECK(items_for(4096, 64, 4) == 64);
    CHECK(items_for(4096, 0, 4) == 4);
}

// A fine grain must still cover every row exactly once -- the slicing is what
// guarantees disjointness, so an off-by-one here would double-apply or skip.
static void grain_slices_cover_every_row_once() {
    constexpr std::size_t kRows = 5000;
    World w;
    setup(w, [](Commands& cmd) {
        for (std::size_t i = 0; i < kRows; ++i)
            cmd.spawn(Position {0.0f, 0.0f}, Velocity {1.0f, 2.0f});
    });
    Schedule sched;
    // grain{7}: not a divisor of 5000, and far below the default floor.
    sched.add_parallel(
        "bump",
        [](Query<Position, Velocity const> q) {
            q.for_each([](auto& p, auto& v) {
                p.x += v.dx;
                p.y += v.dy;
            });
        },
        grain {7});
    WorkerPool pool {4};
    sched.run(w, pool);
    sched.run(w, pool);

    std::size_t seen = 0;
    bool all_twice   = true;
    w.for_each<Position const>([&](auto& p) {
        ++seen;
        all_twice = all_twice && p.x == 2.0f && p.y == 4.0f; // exactly two ticks
    });
    CHECK(seen == kRows);
    CHECK(all_twice);
}

// Reduce partials are per work item, so grain determines how many there are and
// therefore the barrier fold's grouping. What must not vary is the result
// across LANE counts at a fixed grain -- that is the reproducibility guarantee,
// and it holds because grain never reads the lane count.
struct GrainSum {
    double v = 0;
};
struct AddGrainSum {
    void operator()(GrainSum& a, GrainSum& b) const { a.v += b.v; }
};

static double sum_with(std::size_t const grain_rows, unsigned const lanes) {
    World w;
    w.emplace_resource<GrainSum>();
    setup(w, [](Commands& cmd) {
        for (std::size_t i = 0; i < 5000; ++i)
            cmd.spawn(Position {float(i) * 0.1f, 0.0f});
    });
    Schedule sched;
    sched.add_parallel(
        "sum",
        [](Query<Position const> q, Reduce<GrainSum, AddGrainSum> s) {
            q.for_each([&](auto& p) { s->v += double(p.x); });
        },
        grain {grain_rows});
    WorkerPool pool {lanes};
    sched.run(w, pool);
    return w.resource<GrainSum>().v;
}

// add_dynamic_parallel is a template with no native caller -- only the wasm
// bindings instantiate it -- so a signature error in it compiles clean through
// a full native build and the whole test suite, and surfaces only in the
// Emscripten CI leg. That is not a safety net worth relying on: instantiate it
// here, and check grain reaches the dynamic path while we are at it.
static void dynamic_parallel_is_instantiated_and_honours_grain() {
    World w;
    setup(w, [](Commands& cmd) {
        for (int i = 0; i < 4096; ++i)
            cmd.spawn(Position {float(i), 0.0f});
    });

    SystemAccess access;
    access.writes.push_back(component_id<Position>);

    // Written from several lanes, so they are atomics and the CHECKs come
    // after the run (check.hpp's counters are not thread-safe).
    std::atomic<int> calls {0};
    std::atomic<std::uint32_t> rows {0};

    Schedule sched;
    sched.add_dynamic_parallel(
        "dyn",
        access,
        Signature {component_id<Position>},
        [&](World&, Commands&, detail::WorkItem const& item) {
            ++calls;
            for (auto const& u : item.units)
                rows += u.end - u.begin;
        },
        grain {64});

    WorkerPool pool {4};
    sched.run(w, pool);
    CHECK(calls.load() == 64);  // 4096 rows / grain 64
    CHECK(rows.load() == 4096); // every row covered exactly once
}

static void grain_folds_are_lane_invariant() {
    CHECK(sum_with(0, 1) == sum_with(0, 4));   // executor default
    CHECK(sum_with(64, 1) == sum_with(64, 4)); // finer than the default floor
    CHECK(sum_with(7, 1) == sum_with(7, 4));   // finer still, uneven
}

int main() {
    RUN_SUITE(leveling_respects_conflicts);
    RUN_SUITE(parallel_systems_are_independent_levels);
    RUN_SUITE(run_on_thread_pool_executes_all_systems);
    RUN_SUITE(worldview_reads_all_parallel_but_after_writers);
    RUN_SUITE(worldview_sees_writers_flush_in_prior_wave);
    RUN_SUITE(system_exception_propagates);
    RUN_SUITE(aborted_run_discards_recorded_commands);
    RUN_SUITE(registration_options_are_order_independent);
    RUN_SUITE(wave_ordinals_are_plan_positions);
    RUN_SUITE(multiple_observers_notified_in_order);
    RUN_SUITE(grain_sets_the_slice_size);
    RUN_SUITE(grain_slices_cover_every_row_once);
    RUN_SUITE(dynamic_parallel_is_instantiated_and_honours_grain);
    RUN_SUITE(grain_folds_are_lane_invariant);
    return REPORT();
}