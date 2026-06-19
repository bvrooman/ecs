// ecs/snapshot_channel.hpp
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

#include <atomic>
#include <memory>
#include <utility>

namespace ecs {

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
    void publish() {
        latest_.store(std::shared_ptr<T const>(writing_), std::memory_order_release);
        writing_ = std::make_shared<T>();
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
    std::shared_ptr<T> writing_;                   // producer-owned, being filled
    std::atomic<std::shared_ptr<T const>> latest_; // last published (immutable)
};

} // namespace ecs
