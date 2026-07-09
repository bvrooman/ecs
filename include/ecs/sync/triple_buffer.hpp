// ecs/sync/triple_buffer.hpp
//
// Generic lock-free single-producer / single-consumer triple buffer.
//
// This is the mechanism behind "double-buffered" snapshot handoff: a producer
// extracts a snapshot of whatever world state a downstream consumer needs into
// a back buffer and publish()es it; the consumer, running on its own thread at
// its own rate, consume()s the most recently published snapshot. The library is
// deliberately agnostic about what the snapshot *is* -- T is any user type (a
// vector of draw items, audio emitters, a struct of arrays, ...). Nothing here
// knows about rendering, audio, or any particular subsystem.
//
// Why three buffers rather than two: with three slots the producer and consumer
// always own distinct buffers, so a consumer reading an older snapshot never
// sees one the producer is concurrently overwriting -- no tearing and no
// blocking on either side. The price is one extra buffer's worth of memory.
//
// Constraints: exactly one producer thread and one consumer thread at a time.
// The roles may move between threads across frames, but a role must not run
// concurrently with itself (e.g. don't publish() from two threads at once).
// "Latest value wins": if the producer publishes several times between
// consume()s, the consumer jumps to the newest and intermediate snapshots are
// skipped.

#pragma once

#include <array>
#include <atomic>

namespace ecs::sync {

template <class T>
class TripleBuffer {
public:
    TripleBuffer() = default;
    // Seed all three slots with an initial value (requires T copyable).
    explicit TripleBuffer(T const& init)
        : buffers_ {init, init, init} {}

    TripleBuffer(TripleBuffer const&)            = delete;
    TripleBuffer& operator=(TripleBuffer const&) = delete;

    // --- producer side ----------------------------------------------------
    // The buffer to fill; valid until the next publish().
    T& back() noexcept { return buffers_[write_]; }

    // Make the current back buffer the latest published snapshot and take a
    // fresh back buffer to write next. The release pairs with consume()'s
    // acquire so the consumer sees the fully written snapshot.
    void publish() noexcept {
        unsigned const prev =
            shared_.exchange(write_ | kDirty, std::memory_order_acq_rel);
        write_ = prev & kIndexMask;
    }

    // --- consumer side ----------------------------------------------------
    // Advance the front buffer to the newest published snapshot if there is
    // one. Returns true if front() changed. The dirty flag lives in the same
    // atomic as the index, so the check and the swap are one indivisible
    // operation -- the consumer can never grab a stale buffer or swap without
    // fresh data.
    bool consume() noexcept {
        if ((shared_.load(std::memory_order_acquire) & kDirty) == 0)
            return false;
        unsigned const prev = shared_.exchange(read_, std::memory_order_acq_rel);
        read_               = prev & kIndexMask;
        return true;
    }

    // The most recently consumed snapshot (stable until the next consume()).
    T const& front() const noexcept { return buffers_[read_]; }

    // Whether a newer snapshot is available to consume().
    [[nodiscard]]
    auto has_fresh() const noexcept {
        return (shared_.load(std::memory_order_acquire) & kDirty) != 0;
    }

private:
    static constexpr unsigned kIndexMask = 0x3; // low bits hold a 0..2 index
    static constexpr unsigned kDirty     = 0x4; // "new snapshot published" bit

    // Invariant: {write_, read_, shared_ & kIndexMask} is always a permutation
    // of {0,1,2}. Every transition is an exchange that swaps a thread-local
    // index with the shared one, so the three indices stay distinct -- producer
    // and consumer never touch the same buffer.
    std::array<T, 3> buffers_ {};
    unsigned write_ = 0; // producer-owned index
    unsigned read_  = 1; // consumer-owned index
    // Own cache line: both threads RMW this word every publish/consume; when T
    // is small it would otherwise share a line with the buffers themselves.
    alignas(128) std::atomic<unsigned> shared_ {2}; // published index + dirty bit
};

} // namespace ecs::sync

namespace ecs {
// Snapshot-handoff primitives are developer-facing (typically installed as a
// resource, e.g. emplace_resource<TripleBuffer<Snapshot>>()), so they are
// reachable directly as ecs::TripleBuffer though they live in ecs::sync.
using sync::TripleBuffer;
} // namespace ecs
