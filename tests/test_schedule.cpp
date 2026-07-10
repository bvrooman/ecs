// std::execution scheduler tests. Access is derived from system parameters.
#include "check.hpp"
#include "ecs/event/observer.hpp" // ecs::event::overloaded
#include "setup.hpp"
#include <atomic>
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
    sched.add("physics", [](Query<Position>) {});      // writes Position
    sched.add("damage", [](Query<Health>) {});         // writes Health
    sched.add("render", [](Query<Position const>) {}); // reads Position
    // physics & damage are independent -> level 0; render reads Position that
    // physics writes -> level 1.
    CHECK(sched.level_count() == 2);
    CHECK(sched.systems()[0].level == 0); // physics
    CHECK(sched.systems()[1].level == 0); // damage
    CHECK(sched.systems()[2].level == 1); // render
}

static void parallel_systems_are_independent_levels() {
    Schedule sched;
    // Three systems with disjoint writes -> all level 0, fully parallel.
    sched.add("a", [](Query<Position>) {});
    sched.add("b", [](Query<Velocity>) {});
    sched.add("c", [](Query<Health>) {});
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
    sched.add("physics", [](Query<Position, Velocity const> q) {
        q.for_each_serial([](auto& p, auto& v) {
            p.x += v.dx;
            p.y += v.dy;
        });
    });
    // writes Health
    sched.add("damage",
              [](Query<Health> q) { q.for_each_serial([](auto& h) { h.hp -= 1; }); });
    // no ECS access -- just a side effect
    sched.add("render", [&] { render_calls.fetch_add(1, std::memory_order_relaxed); });

    WorkerPool pool {4};
    sched.run(w, pool);
    sched.run(w, pool);

    Entity first {0, 0};
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
        sched.add("observe_a", [](WorldView) {});
        sched.add("observe_b", [](WorldView) {});
        CHECK(sched.level_count() == 1); // two readers -> one wave
    }
    {
        Schedule sched;
        sched.add("mutate", [](Query<Position>) {}); // writes Position
        sched.add("observe", [](WorldView) {});      // reads everything
        CHECK(sched.level_count() == 2);
        // reader serialized after writer
    }
    {
        Schedule sched;
        sched.add("observe", [](WorldView) {}); // reads everything
        // Raw World& is not a system parameter; a genuinely unanalyzable
        // system declares itself exclusive via add_dynamic.
        SystemAccess excl;
        excl.exclusive = true;
        sched.add_dynamic("exclusive", excl, [](World&, Commands&, WorkerPool&) {});
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
    sched.add("spawn", [](Commands& cmd) {
        for (int i = 0; i < 3; ++i)
            cmd.spawn(Position {2, 2});
    });
    // Read-only ad-hoc access via WorldView, serialized after spawn by phase.
    sched.add(
        "observe",
        [&](WorldView view) {
            observed.store(int(view.size()), std::memory_order_relaxed);
        },
        phase<1> {});

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
    sched.add("boom", [](Commands&) { throw std::runtime_error("boom"); });

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
    bad.add("spawner", [](Commands& cmd) {
        cmd.spawn(Position {1, 1}); // recorded before the throw...
    });
    bad.add("boom", [](Query<const Position>) {
        throw std::runtime_error("boom");
    });

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
    ok.add_once("noop", [](Commands&) {});
    ok.run(w);
    CHECK(w.size() == 0);
}

// The observer hook is a general, always-available core feature -- an observer is
// any callable void(ScheduleEvent const&). This one counts the boundaries it is
// notified of and, on SystemBegin, appends its tag to a shared log so notification
// order across observers is observable.
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
                       [this](SystemBegin const&) {
                           ++systems;
                           if (log)
                               log->push_back(tag);
                       },
                       [](auto const&) {}, // TickEnd / WaveEnd / SystemEnd ignored
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
    sched.add("physics", [](Query<Position> q) { q.for_each_serial([](auto&) {}); });
    sched.add("render", [](Query<Position const>) {}); // reads Position -> wave 1

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
    // For each SystemBegin, A (registered first) is notified before B.
    CHECK((order == std::vector<char> {'A', 'B', 'A', 'B'}));

    // Removing one observer stops only its notifications.
    CHECK(sched.events().remove(ta));
    CHECK(sched.events().observer_count() == 1);
    order.clear();
    sched.run(w, pool);
    CHECK((a.ticks == 1 && b.ticks == 2)); // A frozen, B advanced
    CHECK((order == std::vector<char> {'B', 'B'}));
}

int main() {
    RUN_SUITE(leveling_respects_conflicts);
    RUN_SUITE(parallel_systems_are_independent_levels);
    RUN_SUITE(run_on_thread_pool_executes_all_systems);
    RUN_SUITE(worldview_reads_all_parallel_but_after_writers);
    RUN_SUITE(worldview_sees_writers_flush_in_prior_wave);
    RUN_SUITE(system_exception_propagates);
    RUN_SUITE(aborted_run_discards_recorded_commands);
    RUN_SUITE(multiple_observers_notified_in_order);
    return REPORT();
}