// ecs/schedule/params/exec.hpp
//
// Exec: run an arbitrary index space across the schedule's lanes, from inside a
// system.
//
// Every other route to the pool is row-shaped. A parallel system is sliced by
// the rows its Query matches, which is the right model for anything that maps
// over entities -- and no model at all for the irregular stages a real
// simulation is full of: building a tree, walking one, partitioning a broad
// phase, filling a CSR structure. Those have work to spread but no rows to
// slice, so they end up in an add_serial system, which is ONE work item on ONE
// lane however many lanes exist. In the FMM example that shape (the tree build)
// is 1.5% of a 4-lane tick and ~half of a 64-lane one.
//
//   sched.add_serial("build-tree", [](ResMut<Tree> t, Exec exec) {
//       exec.for_each_index(t->level_nodes.size(), [&](std::size_t i) {
//           subdivide(*t, t->level_nodes[i]);   // whatever shape the work is
//       });
//   });
//
// The system may still take its usual parameters; Exec only adds the fan-out.
//
// WHY THIS IS SAFE, and what it costs. A dispatch is not re-entrant -- one job
// slot -- so a system running as one item of a fanned wave must not dispatch,
// and the pool throws if it tries. Exec therefore declares its system
// EXCLUSIVE: an exclusive system conflicts with every other, so it is alone in
// its wave, its wave holds exactly one item, and a one-item wave is run inline
// on the calling thread (WorkerPool::for_each_index returns before it touches
// the job slot). The pool is idle at that moment, so the body's dispatch is a
// fresh one, not a nested one.
//
// The cost is that exclusivity: a system taking Exec runs alone, overlapping
// with nothing else in its phase. That is the right trade for a stage that owns
// the tick anyway, and the wrong one for a system that merely wants a bit of
// parallelism -- if the work IS row-shaped, use add_parallel and let the
// executor slice it, which overlaps with the rest of the wave.

#pragma once

#include <ecs/parallel/worker_pool.hpp>
#include <ecs/schedule/params/serial.hpp>

#include <cassert>
#include <cstddef>
#include <span>
#include <utility>

namespace ecs {

// The fan-out handle. Bound to the pool the schedule is running on, so the work
// lands on the lanes the caller already sized -- not on a second pool that
// would oversubscribe the cores.
class Exec {
public:
    // Run body(i) once per index in [0, count), each claimed by whichever lane
    // is free, and block until all finish. A 1-lane pool (or a single index)
    // runs inline. An exception from any index is rethrown here, once.
    template <class Body>
    void for_each_index(std::size_t const count, Body&& body) const {
        pool_->for_each_index(count, std::forward<Body>(body));
    }

    // Run body(element) once per element of a random-access range.
    template <class R, class Body>
    void for_each(R&& range, Body&& body) const {
        pool_->for_each(std::forward<R>(range), std::forward<Body>(body));
    }

    // Lanes available, for sizing the index space (a good split is a few times
    // this, so claiming can balance uneven work).
    [[nodiscard]]
    unsigned lanes() const noexcept {
        return pool_->lanes();
    }

private:
    template <class P>
    friend struct detail::serial_param;
    explicit Exec(parallel::WorkerPool& pool) noexcept
        : pool_(&pool) {}
    parallel::WorkerPool* pool_;
};

namespace detail {

    template <>
    struct serial_param<Exec> {
        static constexpr bool allowed = true;
        using state                   = no_state;

        // Exclusive: conflicts with every other system, which is what puts this
        // system alone in its wave and therefore inline on the caller. See the
        // header comment -- the whole safety argument rests on this line.
        static void declare(SystemAccess& a) { a.exclusive = true; }

        static void prepare(state&,
                            World&,
                            std::span<std::uint32_t const>,
                            WaveContext const&) {}
        static void finish(state&, World&, parallel::WorkerPool&) {}

        static Exec bind(state&, World&, Commands&, WorkItem const& item) {
            // Set by the executor for every item it runs. Null would mean the
            // item reached a body some other way, which would make dispatching
            // unsafe -- the pool's own re-entrancy guard is the backstop.
            assert(item.pool && "Exec: work item carries no pool");
            return Exec(*item.pool);
        }
    };

} // namespace detail
} // namespace ecs
