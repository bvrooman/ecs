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
// matters is getting all lanes running *now*. (Parking idle lanes on a condition
// variable was tried and measured markedly *worse* here: the per-tick wakeup
// latency, and its jitter, dwarfs the idle-core cost it saves -- a 120 Hz loop
// turned a ~620 us tick with a ~700 us p99 into ~1100 us with a ~6 ms p99.)
//
// So idle lanes spin -- but only for a bounded budget (`spin_before_park`
// relax-iterations, roughly a few hundred microseconds by default). A lane that
// exhausts it parks on the generation counter with std::atomic::wait -- a futex,
// far cheaper to signal than a condition variable, and the dispatcher pays the
// wake syscall only when a lane actually parked (a waiter count guards it). The
// result keeps the measured in-frame dispatch latency -- consecutive dispatches
// within a tick land well inside the budget -- while lanes no longer burn their
// cores across the idle gap between ticks, while the sim is paused, or for the
// lifetime of a pool that stopped ticking. Pass `WorkerPool::spin_forever` to
// restore pure spinning. Under Emscripten lanes always park quickly:
// cpu_relax() compiles to nothing on wasm and idle Web Workers burning 100% of
// a core is exactly what a browser must not do (std::atomic::wait lowers to
// Atomics.wait, which is legal on worker threads; lane 0 -- possibly the
// browser main thread, where Atomics.wait throws -- only ever spins on the
// join, which is bounded by kernel runtime, not idle time).
// Size the pool to leave a performance core for each other hot
// thread (e.g. a render/main thread) rather than oversubscribing.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
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
    // Idle lanes spin for this many cpu_relax() iterations before parking on a
    // futex (see the header comment). spin_forever restores pure spinning.
    static constexpr std::size_t spin_forever = ~std::size_t {0};
#if defined(__EMSCRIPTEN__)
    static constexpr std::size_t default_spin = 1u << 8; // park fast on the web
#else
    static constexpr std::size_t default_spin = 1u << 15; // ~a few hundred us
#endif

    explicit WorkerPool(unsigned lanes, std::size_t spin_before_park = default_spin)
        : lanes_(lanes < 1 ? 1u : lanes)
        , spin_before_park_(spin_before_park) {
        for (unsigned w = 1; w < lanes_; ++w)
            threads_.emplace_back([this, w] { worker_main(w); });
    }

    ~WorkerPool() {
        stop_.store(true, std::memory_order_release);
        gen_.fetch_add(1, std::memory_order_seq_cst); // wake spinning workers
        gen_.notify_all();                            // and parked ones
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
        if (lanes_ == 1 || count < kMinParallel) {
            kernel(std::size_t {0}, count); // serial: exception propagates directly
            return;
        }
        // Re-entrancy guard: there is ONE job slot. A nested dispatch -- a
        // kernel that itself constructs a Query on this pool, or a second
        // thread sharing it -- would overwrite count_/fn_/ctx_ while lanes are
        // mid-flight on the outer job (corruption or deadlock). Run such a
        // dispatch serially on the calling thread instead: still correct, and
        // the outer dispatch keeps its lanes.
        if (dispatching_.exchange(true, std::memory_order_acquire)) {
            kernel(std::size_t {0}, count);
            return;
        }
        count_ = count;
        fn_    = +[](void* ctx, std::size_t b, std::size_t e) {
            (*static_cast<std::remove_reference_t<Kernel>*>(ctx))(b, e);
        };
        ctx_ = static_cast<void*>(&kernel);
        has_err_.store(false, std::memory_order_relaxed);
        remaining_.store(lanes_, std::memory_order_relaxed);
        // seq_cst pairs with the seq_cst waiter bookkeeping in worker_main: if
        // a lane decided to park before this bump became visible to it, its
        // waiter increment is visible to the notify check below (Dekker-style),
        // so the wake cannot be missed.
        gen_.fetch_add(1, std::memory_order_seq_cst); // publish the job
        if (waiters_.load(std::memory_order_seq_cst) > 0)
            gen_.notify_all(); // wake parked lanes (skipped while they spin)
        run_lane(0);           // the caller is lane 0
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
            std::size_t spins = 0;
            while (gen_.load(std::memory_order_acquire) == last) {
                if (stop_.load(std::memory_order_acquire))
                    return;
                if (spins < spin_before_park_) {
                    ++spins;
                    cpu_relax();
                    continue;
                }
                // Budget exhausted: park on gen_. The waiter count is bumped
                // BEFORE the final seq_cst recheck; the dispatcher bumps gen_
                // (seq_cst) BEFORE reading the count -- so either this lane
                // sees the new generation here and skips the wait, or its
                // increment is visible to the dispatcher, which then notifies.
                waiters_.fetch_add(1, std::memory_order_seq_cst);
                if (gen_.load(std::memory_order_seq_cst) == last &&
                    !stop_.load(std::memory_order_acquire))
                    gen_.wait(last, std::memory_order_acquire);
                waiters_.fetch_sub(1, std::memory_order_relaxed);
                spins = 0;
            }
            last = gen_.load(std::memory_order_acquire);
            if (stop_.load(std::memory_order_acquire))
                return;
            run_lane(lane);
        }
    }

    unsigned lanes_;
    std::size_t spin_before_park_;
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
    alignas(kNoShare) std::atomic<unsigned> waiters_ {0};   // lanes parked on gen_
    std::atomic<bool> stop_ {false};
    std::atomic<bool> dispatching_ {false}; // re-entrancy guard (one job slot)

    // First exception thrown by any lane this dispatch (written once, by the
    // lane that wins has_err_; read by the caller after the join).
    std::atomic<bool> has_err_ {false};
    std::exception_ptr err_;
};

} // namespace ecs::parallel

namespace ecs {
// WorkerPool is part of the developer-facing API (Schedule::run(World&,
// WorkerPool&)), so it is reachable directly as ecs::WorkerPool even though it
// is defined in ecs::parallel alongside the rest of the data-parallel runtime.
using parallel::WorkerPool;
} // namespace ecs
