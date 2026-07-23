// ecs/parallel/worker_pool.hpp
//
// A persistent fork-join worker pool for *data-parallel* system execution -- the
// runtime behind Schedule::run(World&, WorkerPool&) and Query::for_each_chunk.
//
// It is built once and reused every tick. `lanes` execution lanes (the calling
// thread is lane 0; lanes-1 resident OS threads are lanes 1..N) stay alive and
// spin-wait on a generation counter, so a dispatch never creates, wakes, or
// sleeps a thread -- the source of the tail latency a general work-stealing pool
// (woken per wave) suffers. parallel_for() splits [0, count) into `lanes`
// contiguous, equal slices (deterministic load balancing -- no work stealing, no
// random victim selection, so the per-call cost is predictable) and runs the
// kernel on every lane concurrently, blocking until all finish. The kernel is
// referenced, never copied or type-erased onto the heap, so a dispatch allocates
// nothing. On macOS workers request the performance-core QoS.
//
// Trade-off: resident workers spin while idle -- lowest dispatch latency, but
// they keep their cores busy. That is the right trade for a latency-sensitive,
// fixed-timestep loop, where a dispatch lands every tick and the cost that
// matters is getting all lanes running *now*. Parking idle lanes has been
// tried and measured markedly *worse* twice: on a condition variable (a 120 Hz
// loop turned a ~620 us tick with a ~700 us p99 into ~1100 us with a ~6 ms
// p99) and again on a bounded-spin-then-futex (std::atomic::wait) hybrid,
// where any budget that actually parks between 60-120 Hz ticks multiplied
// dispatch p99 by 5-100x on a contended host. So idle lanes spin,
// unconditionally. Size the pool to leave a performance core for each other
// hot thread (e.g. a render/main thread) rather than oversubscribing, and
// destroy (or simply stop dispatching to and shrink) pools whose work is gone
// -- a paused app that must not burn idle cores should own that decision
// explicitly rather than have the pool guess at it.
//
// A dispatch is NOT re-entrant: there is one job slot, so a nested
// parallel_for on the same pool -- e.g. a query iterated from inside another
// query's kernel -- throws std::logic_error rather than corrupting the
// in-flight dispatch. Run inner iteration serially (for_each / an
// ad-hoc 1-lane query) or restructure the system.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

namespace ecs::parallel {

// CPU "relax" hint for spin loops (lets the core de-prioritize the spinning
// hyperthread / save power without yielding the OS time slice).
inline void cpu_relax() noexcept {
#if defined(__aarch64__) || defined(__arm64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__)
    asm volatile("pause" ::: "memory");
#endif
}

class WorkerPool {
public:
    explicit WorkerPool(unsigned lanes)
        : lanes_(lanes < 1 ? 1u : lanes) {
        for (unsigned w = 1; w < lanes_; ++w)
            threads_.emplace_back([this, w] { worker_main(w); });
    }

    ~WorkerPool() {
        stop_.store(true, std::memory_order_release);
        gen_.fetch_add(1, std::memory_order_release); // wake spinning workers
        for (auto& t : threads_)
            t.join();
    }

    WorkerPool(WorkerPool const&)            = delete;
    WorkerPool& operator=(WorkerPool const&) = delete;

    [[nodiscard]]
    unsigned lanes() const noexcept {
        return lanes_;
    }

    // Run kernel(begin, end) over a deterministic, equal partition of [0, count)
    // across all lanes; blocks until every lane finishes. Allocation-free: the
    // kernel is referenced via a static trampoline, not stored. Small ranges run
    // serially on the caller (dispatch is not worth it).
    //
    // Exceptions: if a kernel invocation on any lane throws, every lane still
    // runs to completion (so the referenced kernel stays alive for the whole
    // dispatch), then the first exception is rethrown on the calling thread --
    // so a throwing system propagates out of Schedule::run as it would inline,
    // with no std::terminate from a worker and no use-after-unwind.
    template <class Kernel>
    void parallel_for(std::size_t count, Kernel&& kernel) {
        parallel_for(count, kMinParallel, std::forward<Kernel>(kernel));
    }

    // As above, with a caller-chosen serial threshold: dispatch whenever
    // count >= min_parallel. The schedule executor uses this to fan a handful
    // of coarse tasks (whole systems) across lanes, where the default 256-row
    // threshold would always run serial.
    template <class Kernel>
    void parallel_for(std::size_t count, std::size_t min_parallel, Kernel&& kernel) {
        if (lanes_ == 1 || count < min_parallel) {
            kernel(std::size_t {0}, count); // serial: exception propagates directly
            return;
        }
        // Re-entrancy guard: there is ONE job slot. A nested dispatch -- a
        // kernel that itself iterates a pool-bound Query, or a second thread
        // sharing the pool -- would overwrite count_/fn_/ctx_ while lanes are
        // mid-flight on the outer job (corruption or deadlock). Disallowed:
        // thrown from inside an outer kernel, this surfaces at the outer
        // dispatch's join like any kernel exception.
        if (dispatching_.exchange(true, std::memory_order_acquire))
            throw std::logic_error(
                "WorkerPool::parallel_for: nested dispatch on a pool that is "
                "already mid-dispatch (a Query iterated inside another query's "
                "kernel?); use for_each for inner iteration");
        count_ = count;
        fn_    = +[](void* ctx, std::size_t b, std::size_t e) {
            (*static_cast<std::remove_reference_t<Kernel>*>(ctx))(b, e);
        };
        ctx_ = static_cast<void*>(&kernel);
        has_err_.store(false, std::memory_order_relaxed);
        remaining_.store(lanes_, std::memory_order_relaxed);
        gen_.fetch_add(1, std::memory_order_release); // publish the job
        run_lane(0);                                  // the caller is lane 0
        while (remaining_.load(std::memory_order_acquire) != 0)
            cpu_relax();
        dispatching_.store(false, std::memory_order_release);
        if (has_err_.load(std::memory_order_acquire)) {
            auto e = std::exchange(err_, nullptr);
            std::rethrow_exception(e);
        }
    }

private:
    static constexpr std::size_t kMinParallel = 256; // below this, just run serial
    // Padding so the two atomics every lane hammers do not share a line with
    // each other or the job slot (128 covers x86's 64B lines and the 128B
    // spatial prefetcher / Apple Silicon).
    static constexpr std::size_t kNoShare = 128;

    void run_lane(unsigned lane) {
        // Equal contiguous slices without the count*(lane+1) overflow hazard.
        std::size_t const q = count_ / lanes_;
        std::size_t const r = count_ % lanes_;
        std::size_t const b = q * lane + std::min<std::size_t>(lane, r);
        std::size_t const e = b + q + (lane < r ? 1 : 0);
        try {
            fn_(ctx_, b, e);
        } catch (...) {
            // Keep the first exception; let other lanes finish (the kernel must
            // stay alive until the join), then the caller rethrows.
            if (!has_err_.exchange(true, std::memory_order_acq_rel))
                err_ = std::current_exception();
        }
        remaining_.fetch_sub(1, std::memory_order_acq_rel);
    }

    void worker_main(unsigned lane) {
#if defined(__APPLE__)
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
        unsigned last = 0;
        for (;;) {
            while (gen_.load(std::memory_order_acquire) == last) {
                if (stop_.load(std::memory_order_acquire))
                    return;
                cpu_relax();
            }
            last = gen_.load(std::memory_order_acquire);
            if (stop_.load(std::memory_order_acquire))
                return;
            run_lane(lane);
        }
    }

    unsigned lanes_;
    std::vector<std::thread> threads_;

    // Job slot: written by the caller before it bumps gen_, read by each lane
    // after it observes the new gen_ (release/acquire handshake).
    std::size_t count_                           = 0;
    void (*fn_)(void*, std::size_t, std::size_t) = nullptr;
    void* ctx_                                   = nullptr;

    // gen_ and remaining_ each get their own cache line: every lane spins on
    // gen_ and every lane's fetch_sub invalidates remaining_ -- sharing a line
    // (with each other or the job slot) would cross-thrash them.
    alignas(kNoShare) std::atomic<unsigned> gen_ {0};       // bumped to release a job
    alignas(kNoShare) std::atomic<unsigned> remaining_ {0}; // lanes still working
    std::atomic<bool> stop_ {false};
    std::atomic<bool> dispatching_ {false}; // re-entrancy guard (one job slot)

    // First exception thrown by any lane this dispatch (written once, by the
    // lane that wins has_err_; read by the caller after the join).
    std::atomic<bool> has_err_ {false};
    std::exception_ptr err_;
};

// A process-wide 1-lane WorkerPool. One lane spawns no threads and its
// parallel_for just calls the kernel on the calling thread -- it never touches
// the job slot -- so it is free and safe to share across threads: at 1 lane
// there is no per-dispatch state. Ad-hoc queries (query()/WorldView), the
// schedule's serial run(world), and imperative work items inside a multi-item
// wave all bind this pool.
inline WorkerPool& serial_pool() {
    static WorkerPool pool {1};
    return pool;
}

} // namespace ecs::parallel

namespace ecs {
// WorkerPool is part of the developer-facing API (Schedule::run(World&,
// WorkerPool&)), so it is reachable directly as ecs::WorkerPool even though it
// is defined in ecs::parallel alongside the rest of the data-parallel runtime.
using parallel::WorkerPool;
} // namespace ecs
