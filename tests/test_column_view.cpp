// ColumnView<Cs...> (read every row of named components), chunk::row_begin()
// (where a slice sits in its archetype), and the grain{n} registration option
// (work-item granularity). The three together are what a GATHER-shaped kernel
// needs: a Query hands a body its own rows, and these let it read outside them,
// locate itself, and be split across lanes when the rows are few but heavy.

#include "check.hpp"
#include "setup.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <set>
#include <span>
#include <thread>
#include <vector>

using namespace ecs;

namespace {

struct Val {
    int v;
};
struct Other {
    int v;
};
struct Unrelated {
    int v;
};
struct TagA {};
struct TagB {};

// One component value: the row index where the type holds one, {} for a tag.
template <class C>
C make_one(int const i) {
    if constexpr (requires { C {i}; })
        return C {i};
    else
        return C {};
}

// Spawn n entities carrying one of each Cs through a one-shot system.
template <class... Cs>
void spawn_n(World& world, int const n) {
    Schedule init;
    init.add_serial(
        "spawn",
        [n](Commands& cmd) {
            for (int i = 0; i < n; ++i)
                cmd.spawn(make_one<Cs>(i)...);
        },
        times {1});
    init.run(world);
}

// --- reach ------------------------------------------------------------------
// The point of the parameter: a body sees its own slice through the Query and
// EVERY row through the ColumnView.
void column_view_reads_every_row() {
    World world;
    spawn_n<Val>(world, 5000); // > kMinItemRows, so the system is sliced

    std::atomic<int> items {0};
    std::atomic<int> items_seeing_all {0};
    std::atomic<std::size_t> query_rows {0};

    Schedule sched;
    sched.add_parallel("gather", [&](Query<Val const> q, ColumnView<Val> all) {
        std::size_t seen = 0;
        all.for_each_chunk([&](std::span<Entity const> e, chunk<Val const>) {
            seen += e.size();
        });
        q.for_each_chunk([&](std::span<Entity const> e, chunk<Val const>) {
            ++items;
            query_rows += e.size();
            if (seen == 5000)
                ++items_seeing_all;
        });
    });

    WorkerPool pool {4};
    sched.run(world, pool);

    CHECK(items.load() > 1);                        // actually sliced
    CHECK(items_seeing_all.load() == items.load()); // every item saw all 5000
    CHECK(query_rows.load() == 5000);               // slices tile the archetype
}

// --- access declaration -----------------------------------------------------
// The reason to prefer it over WorldView: it declares a read on exactly its
// components, so it does not serialize against unrelated writers.
void column_view_declares_narrow_reads() {
    {
        Schedule sched;
        sched.add_parallel("gather", [](Query<Other> q, ColumnView<Val>) { (void)q; });
        sched.add_parallel("unrelated", [](Query<Unrelated> q) { (void)q; });
        // Reads Val + writes Other vs writes Unrelated: disjoint, one wave.
        CHECK(sched.level_count() == 1);
    }
    {
        Schedule sched;
        sched.add_parallel("gather", [](Query<Other> q, WorldView) { (void)q; });
        sched.add_parallel("unrelated", [](Query<Unrelated> q) { (void)q; });
        // WorldView reads EVERYTHING, so the unrelated writer is serialized
        // behind it -- the cost ColumnView exists to avoid.
        CHECK(sched.level_count() == 2);
    }
}

// It is still a read: a writer of the viewed component is ordered against it.
void column_view_orders_against_its_writers() {
    Schedule sched;
    sched.add_parallel("write-val", [](Query<Val> q) { (void)q; });
    sched.add_parallel("read-val", [](Query<Other> q, ColumnView<Val>) { (void)q; });
    CHECK(sched.level_count() == 2);
}

// Listing a tag narrows the view to the archetypes that carry it -- how a
// per-level pass reads exactly the level below, rather than every row of the
// component and an argument about why that is safe.
void column_view_narrows_by_tag() {
    World world;
    spawn_n<Val, TagA>(world, 10);
    spawn_n<Val, TagB>(world, 7);

    std::atomic<std::size_t> b_rows {0};
    std::atomic<std::size_t> all_rows {0};

    Schedule sched;
    sched.add_parallel("narrow",
                       [&](Query<Other> q, ColumnView<Val, TagB> b, ColumnView<Val> all) {
                           (void)q;
                           b.for_each_chunk([&](std::span<Entity const> e,
                                                chunk<Val const>,
                                                chunk<TagB const>) {
                               b_rows += e.size();
                           });
                           all.for_each_chunk([&](std::span<Entity const> e,
                                                  chunk<Val const>) {
                               all_rows += e.size();
                           });
                       });
    // One row for the query to slice, so the body runs exactly once.
    spawn_n<Other>(world, 1);

    WorkerPool pool {2};
    sched.run(world, pool);

    CHECK(b_rows.load() == 7);
    CHECK(all_rows.load() == 17);
}

void column_view_works_in_a_serial_system() {
    World world;
    spawn_n<Val>(world, 12);

    std::size_t rows = 0;
    Schedule sched;
    sched.add_serial("serial-gather", [&](ColumnView<Val> all) {
        all.for_each_chunk([&](std::span<Entity const> e, chunk<Val const>) {
            rows += e.size();
        });
        rows += all.count();
    });
    sched.run(world);
    CHECK(rows == 24); // 12 from the walk + 12 from count()
}

// --- chunk::row_begin() -----------------------------------------------------
// Every item knows where its slice sits, the offsets tile the archetype, and
// they index the same columns a ColumnView hands back.
void chunk_row_begin_locates_the_slice() {
    World world;
    spawn_n<Val>(world, 3000);

    std::atomic<std::size_t> offset_sum {0};
    std::atomic<int> mismatches {0};
    std::atomic<int> items {0};

    Schedule sched;
    sched.add_parallel("locate", [&](Query<Val const> q, ColumnView<Val> all) {
        std::span<int const> whole;
        all.for_each_chunk([&](std::span<Entity const>, chunk<Val const> c) {
            whole = c.column<0>();
        });
        q.for_each_chunk([&](std::span<Entity const>, chunk<Val const> c) {
            ++items;
            offset_sum += c.row_begin();
            auto const mine = c.column<0>();
            for (std::size_t i = 0; i < mine.size(); ++i)
                if (mine[i] != whole[c.row_begin() + i])
                    ++mismatches;
        });
    });

    WorkerPool pool {4};
    sched.run(world, pool);

    CHECK(mismatches.load() == 0);
    CHECK(items.load() == 3); // 3000 rows / 1024-row floor
    // Offsets are 0, 1024, 2048 -- distinct starts, not a repeated 0.
    CHECK(offset_sum.load() == 0 + 1024 + 2048);
}

// A whole-column chunk starts at row 0, whichever way it was obtained.
void row_begin_of_an_adhoc_chunk_is_zero() {
    World world;
    spawn_n<Val>(world, 4);
    std::size_t begins = 1;
    world.for_each_chunk<Val>([&](std::span<Entity const>, chunk<Val const> c) {
        begins = c.row_begin();
    });
    CHECK(begins == 0);
}

// --- grain{n} ---------------------------------------------------------------
// The body runs once per work item, so counting invocations counts items.
int count_items(World& world, std::size_t const g) {
    std::atomic<int> items {0};
    Schedule sched;
    auto body = [&](Query<Val const> q) {
        q.for_each_chunk([&](std::span<Entity const>, chunk<Val const>) { ++items; });
    };
    if (g == 0)
        sched.add_parallel("count", body);
    else
        sched.add_parallel("count", body, grain {g});
    WorkerPool pool {2};
    sched.run(world, pool);
    return items.load();
}

void grain_raises_the_item_count_of_a_small_system() {
    World world;
    spawn_n<Val>(world, 300); // below the default floor: one item, one lane
    CHECK(count_items(world, 0) == 1);
    CHECK(count_items(world, 64) == 5);  // ceil(300 / 64)
    CHECK(count_items(world, 100) == 3); // ceil(300 / 100)
}

// It sets the item size outright, so it can also make a LARGE system finer
// than the executor's item target would -- what an unevenly weighted kernel
// needs, since its longest item is what every lane waits on.
void grain_sets_rows_per_item_on_a_large_system() {
    World world;
    spawn_n<Val>(world, 100000);
    CHECK(count_items(world, 0) == 65);    // default: max(1024, 100000/64) rows
    CHECK(count_items(world, 256) == 391); // ceil(100000 / 256)
    CHECK(count_items(world, 2000) == 50); // coarser than the default, too
}

// grain is a parallel-system concept: a serial system stays one opaque item.
void grain_does_not_split_a_serial_system() {
    World world;
    spawn_n<Val>(world, 5000);
    std::atomic<int> items {0};
    Schedule sched;
    sched.add_serial(
        "serial",
        [&](Query<Val const> q) {
            q.for_each_chunk([&](std::span<Entity const>, chunk<Val const>) { ++items; });
        },
        grain {8});
    WorkerPool pool {2};
    sched.run(world, pool);
    CHECK(items.load() == 1);
}

// --- Exec -------------------------------------------------------------------
// Exec fans an arbitrary index space onto the schedule's own lanes from inside
// a system. The safety argument is that it declares its system exclusive, so
// the system is alone in its wave and a one-item wave runs inline on the
// caller with the pool idle -- the tests below pin down both halves.
void exec_fans_work_onto_the_pool() {
    World world;
    spawn_n<Val>(world, 8);

    std::atomic<int> visited {0};
    std::mutex m;
    std::set<std::thread::id> threads;

    Schedule sched;
    sched.add_serial("fanout", [&](Exec exec) {
        // The claiming lane can drain 256 trivial indices before the others even
        // wake, so "more than one lane ran" is a race unless the body gives them
        // a chance. Hold each index until a second thread shows up -- against ONE
        // shared deadline, so a genuinely single-lane run costs that wait once
        // rather than 256 times, and fails the check below instead of hanging.
        auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        exec.for_each_index(256, [&](std::size_t) {
            ++visited;
            {
                auto lk = std::lock_guard(m);
                threads.insert(std::this_thread::get_id());
            }
            while (std::chrono::steady_clock::now() < deadline) {
                auto lk = std::lock_guard(m);
                if (threads.size() > 1)
                    break;
            }
        });
    });
    WorkerPool pool {4};
    sched.run(world, pool);

    CHECK(visited.load() == 256); // every index ran, exactly once
    CHECK(threads.size() > 1);    // ...and on more than one lane
}

// The dispatch is a real one, not a silent fallback: it must survive being
// nested inside the schedule's own run, which is what the exclusivity buys.
// A system that merely ran serially would still pass the count check above,
// so also assert the pool the body sees is the one the schedule was given.
void exec_reports_the_running_pool() {
    World world;
    spawn_n<Val>(world, 4);
    unsigned seen = 0;
    Schedule sched;
    sched.add_serial("lanes", [&](Exec exec) { seen = exec.lanes(); });
    WorkerPool pool {3};
    sched.run(world, pool);
    CHECK(seen == 3);
}

// Exclusivity is the mechanism, so it must actually be declared: an Exec system
// gets a wave to itself even among systems it shares no data with.
void exec_system_runs_alone_in_its_wave() {
    Schedule sched;
    sched.add_parallel("a", [](Query<Val> q) { (void)q; });
    sched.add_serial("fanout", [](Exec) {});
    sched.add_parallel("b", [](Query<Other> q) { (void)q; });
    // a and b touch different components and would share a wave; the Exec
    // system between them conflicts with both, so three waves.
    CHECK(sched.level_count() == 3);
}

// It composes with the ordinary parameters -- the fan-out is additive.
void exec_composes_with_other_parameters() {
    World world;
    spawn_n<Val>(world, 64);
    world.emplace_resource<int>(0);

    Schedule sched;
    sched.add_serial("scale", [](Query<Val> q, ResMut<int> res, Exec exec) {
        std::vector<int> rows;
        q.for_each([&](auto& v) { rows.push_back(v.v); });
        std::atomic<int> sum {0};
        exec.for_each_index(rows.size(), [&](std::size_t i) { sum += rows[i]; });
        *res = sum.load();
    });
    WorkerPool pool {4};
    sched.run(world, pool);
    CHECK(world.resource<int>() == 64 * 63 / 2); // 0 + 1 + ... + 63
}

} // namespace

int main() {
    RUN_SUITE(column_view_reads_every_row);
    RUN_SUITE(column_view_declares_narrow_reads);
    RUN_SUITE(column_view_orders_against_its_writers);
    RUN_SUITE(column_view_narrows_by_tag);
    RUN_SUITE(column_view_works_in_a_serial_system);
    RUN_SUITE(chunk_row_begin_locates_the_slice);
    RUN_SUITE(row_begin_of_an_adhoc_chunk_is_zero);
    RUN_SUITE(grain_raises_the_item_count_of_a_small_system);
    RUN_SUITE(grain_sets_rows_per_item_on_a_large_system);
    RUN_SUITE(grain_does_not_split_a_serial_system);
    RUN_SUITE(exec_fans_work_onto_the_pool);
    RUN_SUITE(exec_reports_the_running_pool);
    RUN_SUITE(exec_system_runs_alone_in_its_wave);
    RUN_SUITE(exec_composes_with_other_parameters);
    return REPORT();
}
