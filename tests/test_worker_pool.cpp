// WorkerPool tests: correctness of the data-parallel dispatch -- exact,
// exactly-once coverage of [0, count) however the lanes claim it, dispatch
// across idle gaps (the fixed-timestep pattern), the inline paths that skip
// the job slot, and exception propagation from a lane back to the caller.
//
// NOTE: CHECK is not thread-safe -- it bumps plain int globals -- so it must
// not be called from inside a dispatched body, where lanes run it
// concurrently. Record the condition in an atomic and CHECK after the join.
#include <ecs/parallel/worker_pool.hpp>

#include "check.hpp"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace ecs;

// Every index is handed to the body exactly once across all lanes. Claiming is
// dynamic, so *which* lane gets an index is not fixed -- the coverage is.
static void every_index_runs_exactly_once() {
    for (unsigned lanes : {1u, 2u, 4u}) {
        WorkerPool pool {lanes};
        std::size_t const n = 10'000;
        std::vector<int> hits(n, 0);
        pool.for_each_index(n, [&](std::size_t i) {
            hits[i] += 1; // distinct i per claim -> no data race on any element
        });
        CHECK(std::all_of(hits.begin(), hits.end(), [](int h) { return h == 1; }));
    }
}

// Same guarantee for blocks: [begin, end) ranges tile [0, count) exactly, with
// the last block short when grain does not divide count.
static void blocks_tile_the_range_exactly() {
    for (std::size_t grain : {1uz, 7uz, 256uz, 4096uz}) {
        WorkerPool pool {4};
        std::size_t const n = 10'000; // deliberately not a multiple of any grain
        std::vector<int> hits(n, 0);
        auto blocks    = std::atomic<int> {0};
        auto malformed = std::atomic<bool> {false};
        pool.for_each_block(n, grain, [&](std::size_t b, std::size_t e) {
            // Not CHECK: that writes a shared non-atomic counter (see the note
            // at the top of this file). Flag it here, assert after the join.
            if (!(b < e && e <= n && e - b <= grain))
                malformed.store(true, std::memory_order_relaxed);
            blocks.fetch_add(1, std::memory_order_relaxed);
            for (auto i = b; i < e; ++i)
                hits[i] += 1;
        });
        CHECK(!malformed.load());
        CHECK(std::all_of(hits.begin(), hits.end(), [](int h) { return h == 1; }));
        CHECK(blocks.load() == int((n + grain - 1) / grain));
    }
}

// A grain at or above the count means one lane would claim everything, so the
// body runs inline on the caller as a single block -- the job slot is untouched.
static void grain_at_or_above_count_runs_inline() {
    WorkerPool pool {4};
    auto calls = std::atomic<int> {0};
    auto whole = std::atomic<bool> {false};
    pool.for_each_block(100, 100, [&](std::size_t b, std::size_t e) {
        calls.fetch_add(1, std::memory_order_relaxed);
        whole.store(b == 0 && e == 100, std::memory_order_relaxed);
    });
    CHECK(calls.load() == 1);
    CHECK(whole.load());
}

// An empty range never calls the body and never dispatches.
static void empty_range_is_a_noop() {
    WorkerPool pool {4};
    bool called = false;
    pool.for_each_index(0, [&](std::size_t) { called = true; });
    pool.for_each_block(0, 8, [&](std::size_t, std::size_t) { called = true; });
    CHECK(!called);
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
        pool.for_each_block(n, 64, [&](std::size_t b, std::size_t e) {
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
    pool.for_each_index(1000, [&](std::size_t) { sum += 1; }); // no atomic needed
    CHECK(sum == 1000);
}

// for_each_lane is the raw fan-out: the kernel runs once per lane, with each
// lane's own index, whatever the pool's width.
static void for_each_lane_runs_once_per_lane() {
    for (unsigned lanes : {1u, 2u, 4u}) {
        WorkerPool pool {lanes};
        std::vector<int> seen(lanes, 0);
        std::atomic<int> calls {0};
        auto out_of_range = std::atomic<bool> {false};
        pool.for_each_lane([&](unsigned lane) {
            if (lane >= lanes) { // not CHECK: shared non-atomic counter
                out_of_range.store(true, std::memory_order_relaxed);
                return;
            }
            seen[lane] += 1; // distinct lane per invocation -> no race
            calls.fetch_add(1, std::memory_order_relaxed);
        });
        CHECK(!out_of_range.load());
        CHECK(calls.load() == int(lanes));
        CHECK(std::all_of(seen.begin(), seen.end(), [](int c) { return c == 1; }));
    }
}

// An exception on any lane propagates out to the caller (after every lane has
// finished, so the body stays alive for the whole dispatch).
static void exception_propagates() {
    WorkerPool pool {4};
    bool caught = false;
    try {
        pool.for_each_index(100'000, [](std::size_t i) {
            if (i == 0)
                throw std::runtime_error("boom");
        });
    } catch (std::runtime_error const&) {
        caught = true;
    }
    CHECK(caught);
}

// The same, straight out of the raw fan-out.
static void for_each_lane_propagates_exceptions() {
    WorkerPool pool {4};
    bool caught = false;
    try {
        pool.for_each_lane([](unsigned) { throw std::runtime_error("boom"); });
    } catch (std::runtime_error const&) {
        caught = true;
    }
    CHECK(caught);
}

// One job slot: a dispatch nested inside another throws rather than corrupting
// the in-flight job. It surfaces at the outer join like any kernel exception.
static void nested_dispatch_throws() {
    WorkerPool pool {4};
    bool caught = false;
    try {
        pool.for_each_index(1000, [&](std::size_t) {
            pool.for_each_index(10, [](std::size_t) {});
        });
    } catch (std::logic_error const&) {
        caught = true;
    }
    CHECK(caught);
}

int main() {
    RUN_SUITE(every_index_runs_exactly_once);
    RUN_SUITE(blocks_tile_the_range_exactly);
    RUN_SUITE(grain_at_or_above_count_runs_inline);
    RUN_SUITE(empty_range_is_a_noop);
    RUN_SUITE(dispatch_after_idle_gap);
    RUN_SUITE(single_lane_runs_inline);
    RUN_SUITE(for_each_lane_runs_once_per_lane);
    RUN_SUITE(exception_propagates);
    RUN_SUITE(for_each_lane_propagates_exceptions);
    RUN_SUITE(nested_dispatch_throws);
    return REPORT();
}
