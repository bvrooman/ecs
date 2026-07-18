// ecs/detail/freelist.hpp
//
// A lock-free LIFO free list over a dense index space [0, minted). It hands out
// uint32_t slot indices and recycles freed ones, and is agnostic of whatever
// the caller stores at each index -- it manages index lifecycle only. The free
// chain is a linked list, but NOT an intrusive one: the "next" links live in
// the free list's own `next_` array (`next_[i]` is the slot freed just before
// `i`), not threaded through the caller's payload. So it never touches that
// payload, and any per-slot data (an entity's generation, say) stays with the
// caller.
//
// Built for the "reserve during a wave, recycle at the barrier" pattern, so its
// methods split into two phases with DIFFERENT thread-safety:
//
//   allocate()   -- LOCK-FREE and callable concurrently while systems run. It
//                   pops the free chain (a Treiber-stack CAS on the head) or,
//                   when the chain is empty, mints the next brand-new index (a
//                   single fetch_add). The `next_` links are only read here and
//                   are frozen (never mutated) during this phase, so concurrent
//                   reads are safe.
//   deallocate() -- SINGLE-THREADED. Returns an index for reuse by pushing it
//                   onto the chain (growing link storage to cover it). Call only
//                   when no allocate() can run concurrently (e.g. the schedule's
//                   flush barrier, world-quiescent).
//
// It is ABA-free WITHOUT a tagged head because pushes (deallocate) happen only
// at the barrier: during a wave the chain is pop-only, so the head walks
// monotonically down a frozen chain and no index is re-linked until the next
// barrier. That same phase split is why deallocate() need not be safe against a
// concurrent allocate() -- it deliberately is not a general-purpose concurrent
// free list. Unlike an array-backed stack, a popped node is unlinked in place,
// so there is nothing to reconcile after a wave (no compaction step).

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace ecs::detail {

class Freelist {
public:
    using index_type = std::uint32_t;
    // Chain terminator: the "no successor" / empty-chain sentinel. The index
    // space is [0, minted), so the top value can never be a live index.
    static constexpr index_type kNil = ~index_type {0};

    // Claim an index. Lock-free; safe to call concurrently. Recycles the
    // most-recently-freed index (LIFO) or mints the next brand-new one.
    [[nodiscard]]
    index_type allocate() {
        auto head = head_.load(std::memory_order_acquire);
        while (head != kNil) {
            // The link is frozen during the allocate phase (only barrier-time
            // deallocate writes next_), so this read is race-free.
            auto const next = next_[head];
            if (head_.compare_exchange_weak(head,
                                            next,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire))
                return head;
        }
        return minted_.fetch_add(1, std::memory_order_relaxed);
    }

    // Return an index for reuse. Single-threaded (barrier only). Pushes it onto
    // the free chain, growing link storage to cover it if needed.
    void deallocate(index_type const idx) {
        if (idx >= next_.size())
            next_.resize(idx + 1, kNil);
        next_[idx] = head_.load(std::memory_order_relaxed);
        head_.store(idx, std::memory_order_release);
    }

private:
    std::vector<index_type> next_;         // chain links; frozen while allocating
    std::atomic<index_type> minted_ {0};   // next brand-new index
    // Padded onto its own cache line: concurrent allocators hammer the chain
    // head and the mint counter, so keep them apart.
    alignas(128) std::atomic<index_type> head_ {kNil}; // top of the free chain
};

} // namespace ecs::detail
