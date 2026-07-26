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

int main() {
    RUN_SUITE(partition_is_exact_and_disjoint);
    RUN_SUITE(dispatch_after_idle_gap);
    RUN_SUITE(single_lane_runs_inline);
    RUN_SUITE(exception_propagates);
    return REPORT();
}
