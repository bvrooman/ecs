// ecs/parallel/worker_pool.hpp
//
// A persistent fork-join worker pool for *data-parallel* system execution --
// the runtime behind Schedule::run(World&, WorkerPool&).
//
// It is built once and reused every tick. `lanes` execution lanes (the calling
// thread is lane 0; lanes-1 resident OS threads are lanes 1..N) stay alive and
// spin-wait on a generation counter, so a dispatch never creates, wakes, or
// sleeps a thread -- the source of the tail latency a general work-stealing pool
// (woken per wave) suffers. for_each() hands a range out to the lanes in
// dynamically claimed pieces and blocks until all finish; for_each_index() and
// for_each_block() are the same over an index space. All three cover their
// range exactly once, so a body touching only its own element cannot race.
// Beneath them is a raw per-lane fan-out that makes no such promise; it is
// private, reachable only through detail::PoolAccess. The body is referenced,
// never copied or type-erased onto the heap, so a dispatch allocates nothing.
// On macOS workers request the performance-core QoS.
//
// Claiming is dynamic rather than a fixed partition because the wave executor
// -- the pool's one in-tree consumer -- runs ragged work items whose costs
// differ by design. Claiming costs one relaxed fetch_add per block and lets a
// lane that drew cheap blocks keep pulling; a static split would leave it idle
// on a slice it finished early. Pass grain = count / lanes() to recover a
// static partition for a uniform workload.
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
// dispatch on the same pool -- e.g. a query iterated from inside another
// query's kernel -- throws std::logic_error rather than corrupting the
// in-flight dispatch. Run inner iteration serially (for_each / an
// ad-hoc 1-lane query) or restructure the system.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <ranges>
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

namespace detail {
    struct PoolAccess;
} // namespace detail

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

    // The three dispatches below are one mechanism in layers, most-used first:
    // for_each -> for_each_index -> for_each_block, each implemented by the
    // next, and all three finally by the private for_each_lane. They share the
    // semantics the file comment describes -- dynamic claiming, exactly-once
    // coverage, blocking until every lane rejoins, no allocation -- and the
    // exception and re-entrancy contract documented on for_each_lane.

    // Run body(element) once per element of `range`, each element claimed by
    // whichever lane is free. The dispatch most callers want: say what the work
    // *is* rather than counting it and indexing back in.
    //
    //   pool.for_each(items, run_one);   // not for_each_index(items.size(), ...)
    //
    // Random access and sized because claiming hands a lane an arbitrary index,
    // which has to be O(1); a list would have to be walked. `range` must outlive
    // the call, which it does -- the dispatch blocks until every lane rejoins.
    // The element reference follows the range's constness, so a body taking T&
    // can mutate in place.
    template <class R, class Body>
        requires std::ranges::random_access_range<R> && std::ranges::sized_range<R>
    void for_each(R&& range, Body&& body) {
        auto const first = std::ranges::begin(range);
        for_each_index(std::ranges::size(range),
                       [&](std::size_t i) { body(first[std::ptrdiff_t(i)]); });
    }

    // The same, over an index space rather than a container: body(i) once per
    // index in [0, count). Reach for it when there is nothing to iterate --
    // indices into several parallel arrays, or a computed range.
    //
    // This is for_each_block at grain 1, one element per claim. The wave
    // executor's shape -- a handful of coarse, uneven work items, where
    // per-item claiming is the point and the one atomic per item is noise
    // against the item's own cost. A lone item runs inline on the caller, so
    // the common single-system tick never pays a dispatch.
    template <class Body>
    void for_each_index(std::size_t count, Body&& body) {
        for_each_block(count, 1, [&body](std::size_t b, std::size_t e) {
            for (auto i = b; i < e; ++i)
                body(i);
        });
    }

    // Claim [0, count) in `grain`-sized blocks: body(begin, end) once per
    // claimed block, every index landing in exactly one call. Use it when the
    // body is cheap per element and wants a run to amortise over, or when it
    // needs a contiguous span.
    //
    // Dynamic rather than a fixed partition: the wave executor's work items are
    // ragged (build() sorts them longest-first precisely because their costs
    // differ), so a lane that draws short blocks keeps pulling instead of
    // idling on an equal static slice it finished early. For a *uniform*
    // workload pass grain = count / lanes() to recover a static partition at
    // the cost of one atomic per lane.
    //
    // `grain` doubles as the serial threshold: at count <= grain a single lane
    // would claim everything anyway, so the body runs inline on the caller and
    // the job slot is never touched. A 1-lane pool is always inline.
    template <class Body>
    void for_each_block(std::size_t count, std::size_t grain, Body&& body) {
        if (count == 0)
            return;
        if (lanes_ == 1 || count <= grain) {
            body(std::size_t {0}, count); // serial: exception propagates directly
            return;
        }
        auto next = std::atomic<std::size_t> {0};
        for_each_lane([&](unsigned) {
            // Claim, then test: the lane that draws b >= count is done. Relaxed
            // is enough -- the fetch_add only has to hand out distinct b's, and
            // the join is what orders the bodies' writes against the caller.
            std::size_t b = 0;
            while ((b = next.fetch_add(grain, std::memory_order_relaxed)) < count)
                body(b, std::min(b + grain, count));
        });
    }

private:
    // Run kernel(lane) once on every lane (the caller is lane 0) and block
    // until all finish; a 1-lane pool calls it inline. The raw fan-out the
    // three public dispatches are built on.
    //
    // PRIVATE ON PURPOSE. Every dispatch above guarantees exactly-once coverage
    // of a range, so a body that touches only its own element cannot race. This
    // one guarantees nothing: it hands out a lane index and leaves disjointness
    // entirely to the caller. Overlap and you get a data race; leave a gap and
    // the work silently never runs. Neither fails to compile, and both can pass
    // on a 1-lane pool, where the kernel runs exactly once on the caller.
    //
    // Genuine per-lane work -- pinning a slice to a lane, per-lane setup such
    // as the wasm spike's eval-a-kernel-into-each-Web-Worker -- reaches it via
    // detail::PoolAccess, which has to be named explicitly. That is the point:
    // it should be a deliberate act, not the fourth entry in a menu.
    //
    // Allocation-free: the kernel is referenced via a static trampoline, never
    // stored or type-erased onto the heap. That holds for the public dispatches
    // too, since they all route through here.
    //
    // Exceptions: if the kernel throws on any lane, every lane still runs to
    // completion (so the referenced kernel stays alive for the whole dispatch),
    // then the first exception is rethrown on the calling thread -- a throwing
    // system propagates out of Schedule::run as it would inline, with no
    // std::terminate from a worker and no use-after-unwind.
    friend struct detail::PoolAccess;

    template <class Kernel>
    void for_each_lane(Kernel&& kernel) {
        if (lanes_ == 1) {
            kernel(0u); // serial: exception propagates directly
            return;
        }
        // Re-entrancy guard: there is ONE job slot. A nested dispatch -- a
        // kernel that itself iterates a pool-bound Query, or a second thread
        // sharing the pool -- would overwrite fn_/ctx_ while lanes are
        // mid-flight on the outer job (corruption or deadlock). Disallowed:
        // thrown from inside an outer kernel, this surfaces at the outer
        // dispatch's join like any kernel exception.
        if (dispatching_.exchange(true, std::memory_order_acquire))
            throw std::logic_error(
                "WorkerPool::for_each_lane: nested dispatch on a pool that is "
                "already mid-dispatch (a Query iterated inside another query's "
                "kernel?); use for_each for inner iteration");
        fn_ = +[](void* ctx, unsigned lane) {
            (*static_cast<std::remove_reference_t<Kernel>*>(ctx))(lane);
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

    // Padding so the two atomics every lane hammers do not share a line with
    // each other or the job slot (128 covers x86's 64B lines and the 128B
    // spatial prefetcher / Apple Silicon).
    static constexpr std::size_t kNoShare = 128;

    void run_lane(unsigned lane) {
        try {
            fn_(ctx_, lane);
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
    void (*fn_)(void*, unsigned) = nullptr;
    void* ctx_                   = nullptr;

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

namespace detail {
    // The way to the pool's raw per-lane fan-out, for the cases that genuinely
    // need a lane pinned rather than a claimed slice. Deliberately awkward: you
    // have to name detail::PoolAccess, which is a decision, whereas an extra
    // pool.for_each_* in the completion list is a shrug. Read WorkerPool's
    // for_each_lane comment before using it -- disjointness becomes yours, and
    // getting it wrong races or silently drops work without failing to compile.
    struct PoolAccess {
        template <class Kernel>
        static void for_each_lane(WorkerPool& pool, Kernel&& kernel) {
            pool.for_each_lane(std::forward<Kernel>(kernel));
        }
    };
} // namespace detail

// A process-wide 1-lane WorkerPool. One lane spawns no threads and its
// dispatches just call the body on the calling thread -- they never touch
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
