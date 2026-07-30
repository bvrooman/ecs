// WorkerPool tests: correctness of the data-parallel dispatch -- exact disjoint
// partition, dispatch across idle gaps (the fixed-timestep pattern), the 1-lane
// inline path, and exception propagation from a lane back to the caller.
#include <ecs/parallel/worker_pool.hpp>

#include "check.hpp"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace ecs;

// Every lane covers a disjoint, contiguous slice and together they cover
// [0, count) exactly once -- the property the data-parallel split relies on.
static void partition_is_exact_and_disjoint() {
    WorkerPool pool {4};
    std::size_t const n = 100'000;
    std::vector<int> hits(n, 0);
    pool.parallel_for(n, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i)
            hits[i] += 1; // disjoint ranges -> no data race on any element
    });
    CHECK(std::all_of(hits.begin(), hits.end(), [](int h) { return h == 1; }));
}

// Many dispatches separated by idle gaps -- the fixed-timestep usage pattern.
// Every dispatch must reach all the resident lanes and they must all rejoin the
// barrier; a missed lane would deadlock (test hangs) or undercount the total.
static void dispatch_after_idle_gap() {
    WorkerPool pool {4};
    std::atomic<long> total {0};
    std::size_t const n  = 10'000;
    constexpr int rounds = 50;
    for (int r = 0; r < rounds; ++r) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // idle between ticks
        pool.parallel_for(n, [&](std::size_t b, std::size_t e) {
            total.fetch_add(long(e - b), std::memory_order_relaxed);
        });
    }
    CHECK(total.load() == long(n) * rounds);
}

// A 1-lane pool spawns no workers and runs everything inline on the caller.
static void single_lane_runs_inline() {
    WorkerPool pool {1};
    CHECK(pool.lanes() == 1);
    long sum = 0;
    pool.parallel_for(1000, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i)
            sum += 1;
    });
    CHECK(sum == 1000);
}

// An exception on any lane propagates out of parallel_for on the caller (after
// every lane has finished, so the kernel stays alive for the whole dispatch).
static void exception_propagates() {
    WorkerPool pool {4};
    bool caught = false;
    try {
        pool.parallel_for(100'000, [](std::size_t b, std::size_t e) {
            if (b != e)
                throw std::runtime_error("boom");
        });
    } catch (std::runtime_error const&) {
        caught = true;
    }
    CHECK(caught);
}

// for_each_lane runs the kernel exactly once per lane -- no range, no
// partition. This is the wave executor's shape: every lane drains a shared
// atomic cursor, so the total claimed must be the item count exactly once.
static void for_each_lane_runs_once_per_lane() {
    for (unsigned lanes : {1u, 2u, 4u}) {
        WorkerPool pool {lanes};
        std::atomic<int> calls {0};
        pool.for_each_lane([&] { calls.fetch_add(1, std::memory_order_relaxed); });
        CHECK(calls.load() == int(lanes));
    }
}

// The cursor-drain pattern: each item claimed exactly once across all lanes,
// whatever the split -- the property Wave::run depends on.
static void for_each_lane_drains_a_cursor_exactly_once() {
    WorkerPool pool {4};
    std::size_t const n = 10'000;
    std::vector<int> hits(n, 0);
    auto next = std::atomic<std::size_t> {0};
    pool.for_each_lane([&] {
        for (auto i = next.fetch_add(1, std::memory_order_relaxed); i < n;
             i      = next.fetch_add(1, std::memory_order_relaxed))
            hits[i] += 1; // distinct i per claim -> no data race on any element
    });
    CHECK(std::all_of(hits.begin(), hits.end(), [](int h) { return h == 1; }));
}

// A throwing lane propagates out of for_each_lane on the caller, as it does
// out of parallel_for -- for_each_lane delegates, so it inherits the join.
static void for_each_lane_propagates_exceptions() {
    WorkerPool pool {4};
    bool caught = false;
    try {
        pool.for_each_lane([] { throw std::runtime_error("boom"); });
    } catch (std::runtime_error const&) {
        caught = true;
    }
    CHECK(caught);
}

int main() {
    RUN_SUITE(partition_is_exact_and_disjoint);
    RUN_SUITE(dispatch_after_idle_gap);
    RUN_SUITE(single_lane_runs_inline);
    RUN_SUITE(exception_propagates);
    RUN_SUITE(for_each_lane_runs_once_per_lane);
    RUN_SUITE(for_each_lane_drains_a_cursor_exactly_once);
    RUN_SUITE(for_each_lane_propagates_exceptions);
    return REPORT();
}
