// ecs/worker_pool.hpp
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
// turned a ~620 us tick with a ~700 us p99 into ~1100 us with a ~6 ms p99. So
// idle lanes spin.) Size the pool to leave a performance core for each other hot
// thread (e.g. a render/main thread) rather than oversubscribing.

#pragma once

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

namespace ecs {

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
        if (lanes_ == 1 || count < kMinParallel) {
            kernel(std::size_t {0}, count); // serial: exception propagates directly
            return;
        }
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
        if (has_err_.load(std::memory_order_acquire)) {
            auto e = std::exchange(err_, nullptr);
            std::rethrow_exception(e);
        }
    }

private:
    static constexpr std::size_t kMinParallel = 256; // below this, just run serial

    void run_lane(unsigned lane) {
        std::size_t const b = count_ * lane / lanes_;
        std::size_t const e = count_ * (lane + 1) / lanes_;
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

    std::atomic<unsigned> gen_ {0};       // bumped to release a job
    std::atomic<unsigned> remaining_ {0}; // lanes still working
    std::atomic<bool> stop_ {false};

    // First exception thrown by any lane this dispatch (written once, by the
    // lane that wins has_err_; read by the caller after the join).
    std::atomic<bool> has_err_ {false};
    std::exception_ptr err_;
};

} // namespace ecs
