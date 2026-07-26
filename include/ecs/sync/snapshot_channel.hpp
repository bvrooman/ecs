// ecs/sync/snapshot_channel.hpp
//
// Multi-consumer snapshot fan-out.
//
// Where TripleBuffer<T> hands a snapshot to exactly one consumer (and is
// lock-free and allocation-free), SnapshotChannel<T> broadcasts the latest
// published snapshot to *any number* of independent consumer threads -- e.g. a
// renderer and an audio mixer both reading the same extracted world state, each
// at its own rate.
//
// The mechanism: the producer fills a buffer and publish()es it as an immutable
// std::shared_ptr<const T> into an atomic. Consumers load() that pointer and
// hold their own reference for as long as they read; because a published
// snapshot is never mutated, all consumers read it concurrently with no tearing
// and no blocking, and the buffer is freed automatically once the producer has
// moved on and every consumer has released it.
//
// Single producer (publish() is not safe to call concurrently with itself);
// any number of consumers. "Latest value wins": a consumer that polls slowly
// simply skips intermediate snapshots. This relies on std::atomic<shared_ptr>,
// which the standard library may implement with an internal lock pool rather
// than true lock-freedom; the trade vs TripleBuffer is allocation-per-publish
// and that overhead, in exchange for unlimited consumers.

#pragma once

#include <ecs/detail/compat/atomic_shared_ptr.hpp>

#include <atomic>
#include <memory>
#include <utility>
#include <vector>

// ThreadSanitizer does not model std::atomic_thread_fence synchronization, so
// the (formally correct, see acquire_buffer) fence-based buffer reclaim below
// reads as a false-positive race under TSAN. Skip pooling there.
#if defined(__SANITIZE_THREAD__)
#define ECS_SNAPSHOT_POOL 0
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define ECS_SNAPSHOT_POOL 0
#endif
#endif
#ifndef ECS_SNAPSHOT_POOL
#define ECS_SNAPSHOT_POOL 1
#endif

namespace ecs::sync {

template <class T>
class SnapshotChannel {
public:
    SnapshotChannel()
        : writing_(std::make_shared<T>()) {}
    explicit SnapshotChannel(T const& init)
        : writing_(std::make_shared<T>(init)) {}

    SnapshotChannel(SnapshotChannel const&)            = delete;
    SnapshotChannel& operator=(SnapshotChannel const&) = delete;

    // --- producer side ----------------------------------------------------
    // The buffer to fill; exclusively the producer's until publish().
    T& back() noexcept { return *writing_; }

    // Publish the current back buffer as the latest snapshot and take a fresh
    // buffer to fill next. Readers keep any snapshot they still hold alive.
    // Steady state allocates nothing: previously published buffers are retained
    // in a producer-owned pool and recycled once every consumer has released
    // them (use_count back to 1 -- only the pool's reference left).
    void publish() {
        latest_.store(std::shared_ptr<T const>(writing_), std::memory_order_release);
#if ECS_SNAPSHOT_POOL
        pool_.push_back(std::move(writing_));
        writing_ = acquire_buffer();
#else
        writing_ = std::make_shared<T>();
#endif
    }

    // --- consumer side (any number of threads) ----------------------------
    // The most recently published snapshot, or nullptr if nothing published
    // yet. The returned shared_ptr keeps that snapshot valid for as long as
    // held.
    std::shared_ptr<T const> latest() const {
        return latest_.load(std::memory_order_acquire);
    }

    // Per-consumer cursor that advances only when a newer snapshot is
    // available, so a consumer can cheaply tell "is there new data?" and avoid
    // reprocessing.
    class Reader {
    public:
        explicit Reader(SnapshotChannel const& channel)
            : channel_(&channel) {}

        // Advance to the newest snapshot; returns true if it changed.
        bool poll() {
            auto next = channel_->latest();
            if (next == current_)
                return false;
            current_ = std::move(next);
            return true;
        }

        [[nodiscard]]
        T const* get() const noexcept {
            return current_.get();
        }

        [[nodiscard]]
        std::shared_ptr<T const> const& snapshot() const noexcept {
            return current_;
        }

    private:
        SnapshotChannel const* channel_;
        std::shared_ptr<T const> current_;
    };

    Reader reader() const { return Reader(*this); }

private:
    // Reuse a pooled buffer nobody else references, or allocate a fresh one.
    // use_count() == 1 means the pool's own reference is the only one left: the
    // buffer is no longer the published latest and no consumer holds it, and
    // since new references can only be minted from latest_, none can appear
    // concurrently. Synchronization ([atomics.fences]p4): the last consumer's
    // shared_ptr release is a release RMW on the control-block counter; our
    // use_count() load reads the value it wrote and is sequenced before the
    // acquire fence, so the consumer's final reads happen-before our overwrite.
    std::shared_ptr<T> acquire_buffer() {
        for (auto& slot : pool_) {
            if (slot.use_count() == 1) {
                std::atomic_thread_fence(std::memory_order_acquire);
                auto buf = std::move(slot);
                slot     = std::move(pool_.back());
                pool_.pop_back();
                return buf;
            }
        }
        return std::make_shared<T>();
    }

    std::shared_ptr<T> writing_;                     // producer-owned, being filled
    std::vector<std::shared_ptr<T>> pool_;           // producer-owned recycle pool
    ecs::detail::atomic_shared_ptr<T const> latest_; // last published (immutable)
};

} // namespace ecs::sync

namespace ecs {
// See triple_buffer.hpp: the snapshot-handoff primitives are developer-facing,
// so SnapshotChannel is reachable directly as ecs::SnapshotChannel.
using sync::SnapshotChannel;
} // namespace ecs
