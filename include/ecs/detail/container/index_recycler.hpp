// ecs/detail/container/index_recycler.hpp
//
// A lock-free recycler for dense-array indices: hands out uint32_t slot indices,
// reusing freed ones before minting brand-new ones, with no lock on the hot
// path. It is NOT an intrusive free list (no linked list threaded through the
// slots); it is a side stack of recycled indices plus a high-water counter for
// fresh ones. Built for the "reserve during a wave, recycle at the barrier"
// pattern -- so its methods split into two phases with DIFFERENT thread-safety:
//
//   allocate()  -- LOCK-FREE and callable concurrently while systems run. It
//                  claims a recycled index (a CAS on a cursor into the free
//                  stack) or, when none remain, mints the next brand-new index
//                  (a single fetch_add). The free stack is only read here and is
//                  frozen (never mutated) during this phase, so concurrent reads
//                  are safe.
//   free()      -- SINGLE-THREADED. Returns an index for reuse. Call only when
//   compact()      no allocate() can run concurrently (e.g. the schedule's
//                  flush barrier, world-quiescent).
//
// The rest-state invariant is `cursor == free.size()`: free() re-publishes it,
// and compact() restores it after a wave by dropping the tail that allocate()
// claimed (those indices are now live, recorded elsewhere). The cursor and the
// high-water mark are kept on separate cache lines: concurrent allocators hammer
// both.
//
// It is deliberately NOT a general-purpose concurrent free list -- free() is not
// safe against a concurrent allocate(). It owns index lifecycle only; any
// per-slot payload (an entity's generation, say) lives with the caller.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ecs::detail {

class IndexRecycler {
public:
    using index_type = std::uint32_t;

    // Claim an index. Lock-free; safe to call concurrently. Recycles a freed
    // index (most-recently-freed first) or mints the next brand-new one.
    [[nodiscard]]
    index_type allocate() {
        auto n = cursor_.load(std::memory_order_acquire);
        while (n > 0) {
            if (cursor_.compare_exchange_weak(n,
                                              n - 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire))
                return free_[n - 1];
        }
        return high_.fetch_add(1, std::memory_order_relaxed);
    }

    // Return an index for reuse. Single-threaded (barrier only).
    void free(index_type const index) {
        free_.push_back(index);
        cursor_.store(free_.size(), std::memory_order_release);
    }

    // Drop the indices allocate() claimed during the wave, restoring the
    // `cursor == free.size()` rest state. Single-threaded (barrier only), before
    // the wave's free()s run.
    void compact() { free_.resize(cursor_.load(std::memory_order_acquire)); }

private:
    std::vector<index_type> free_;             // recycled indices; frozen while allocating
    std::atomic<index_type> high_ {0};         // next brand-new index
    // Padded onto its own cache line: concurrent allocators hammer the cursor
    // and the high-water mark, so keep them apart.
    alignas(128) std::atomic<std::size_t> cursor_ {0}; // claimable free entries
};

} // namespace ecs::detail
