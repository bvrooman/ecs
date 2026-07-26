// ecs/command_buffer.hpp
//
// The deferral mechanism behind the Commands mutation API.
//
// Structural changes (spawn/destroy/add/remove) and value writes (set) move
// data between archetype tables or into columns, which would invalidate
// iteration in flight and race with systems on the schedule's thread pool. So
// rather than mutate directly, a system records each edit here through its
// Commands object; the recorded commands are replayed single-threaded at a sync
// point -- the Schedule flushes (calls apply()) at every level barrier, when no
// systems are running. Mutation is reachable only via Commands, so there is no
// non-deferred path to guard against.
//
// Recording is sharded per worker thread: each thread appends to its own shard
// with no cross-thread contention on the hot path (shard lookup is a lock-free
// atomic load; the per-shard mutex is uncontended unless more threads exist
// than shards). apply() drains every shard single-threaded. Commands within one
// thread's shard replay in record order; ordering *across* shards is
// unspecified.
//
// A command is a type-erased `void(World&)` callable. Each shard stores its
// callables in a monotonic arena (CommandStore) -- a chain of fixed blocks the
// callable is constructed into in place and never relocated, so a move-only or
// non-trivially-relocatable capture (e.g. spawning a component that owns memory)
// is safe. apply() rewinds the arena instead of freeing it, so after warmup a
// tick records and flushes its commands without touching the heap at all (the
// "zero allocation per tick" property): no per-command closure allocation, which
// is where a std::move_only_function with a large capture (a multi-component
// spawn) would otherwise hit malloc on every call.

#pragma once

#include <ecs/system_id.hpp> // SystemId (the recording-source tag)
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <new>

namespace ecs {
namespace detail {
    // A stable, process-wide index for the calling thread (0, 1, 2, ...
    // assigned on first use). Used to shard the command buffer without per-call
    // synchronization.
    inline int this_thread_index() noexcept {
        static std::atomic counter {0};
        thread_local int idx = counter.fetch_add(1, std::memory_order_relaxed);
        return idx;
    }

    // The system whose commands the calling thread is currently recording --
    // set by the executor around each work item and around each single-threaded
    // barrier finish hook, read by CommandStore::record so each command carries
    // its recording system. That lets apply() attribute the flush time per
    // system (see Schedule's per-system flush reporting). A default SystemId{}
    // (id 0) means "unattributed" (setup outside a run, etc.).
    inline SystemId& recording_source() noexcept {
        thread_local SystemId src {};
        return src;
    }
} // namespace detail

// Per-system command-apply time accumulated by a flush; keys are whatever
// recording_source() held when each command was recorded.
using FlushAttrib = std::unordered_map<SystemId, double>;

class World; // defined in world.hpp

// A monotonic arena of type-erased `void(World&)` commands, kept in record
// order. record() constructs the callable in place in a block and never moves
// it; apply() invokes then destroys each in order and rewinds the arena (blocks
// retained), so a steady-state tick allocates nothing. Single-threaded: a
// CommandBuffer shard owns one and serializes access with its mutex.
class CommandStore {
public:
    CommandStore() = default;
    // A store may die holding never-applied commands (e.g. a kernel item's
    // private store after an aborted run): destroy their captures rather
    // than leak them.
    ~CommandStore() { discard(); }
    CommandStore(CommandStore const&)            = delete;
    CommandStore& operator=(CommandStore const&) = delete;

    template <class F>
    void record(F&& f) {
        using Fn = std::decay_t<F>;
        // Components stored in spawn/add/set closures align to <= 16; block
        // bases (operator new[]) meet that, so aligning the offset suffices.
        static_assert(alignof(Fn) <= alignof(std::max_align_t),
                      "command capture is over-aligned for the arena");
        // Ensure capacity before constructing the callable: if push_back had to
        // grow afterwards and threw, the constructed capture would leak.
        if (cmds_.size() == cmds_.capacity())
            cmds_.reserve(cmds_.empty() ? 16 : cmds_.capacity() * 2);
        void* obj = allocate(sizeof(Fn), alignof(Fn));
        ::new (obj) Fn(std::forward<F>(f));
        cmds_.push_back(Cmd {
            obj,
            [](void* o, World& w) { (*static_cast<Fn*>(o))(w); },
            [](void* o) noexcept { static_cast<Fn*>(o)->~Fn(); },
            detail::recording_source(), // the system recording this command
        });
    }

    void apply(World& world, FlushAttrib* attrib = nullptr) {
        // Invoke-and-destroy in ONE pass. If a command throws, the remaining
        // (uninvoked) commands are destroyed and the store rewound on unwind --
        // already-applied commands can never replay on the next flush (which
        // would duplicate spawns of the same reserved handle, double-destroy,
        // and replay adds).
        std::size_t i = 0;
        try {
            if (attrib == nullptr) {
                for (; i < cmds_.size(); ++i) {
                    auto const& c = cmds_[i];
                    c.invoke(c.obj, world);
                    c.destroy(c.obj);
                }
            } else if (!cmds_.empty()) {
                // Attribute apply time per recording source. Commands from one
                // work item are contiguous (same source), so time each run
                // rather than each command -- a handful of clock reads, not one
                // per command.
                using clock = std::chrono::steady_clock;
                auto batch  = clock::now();
                auto src    = cmds_[0].source;
                for (; i < cmds_.size(); ++i) {
                    auto const& c = cmds_[i];
                    if (c.source != src) {
                        auto const now = clock::now();
                        (*attrib)[src] +=
                            std::chrono::duration<double, std::micro>(now - batch)
                                .count();
                        batch = now;
                        src   = c.source;
                    }
                    c.invoke(c.obj, world);
                    c.destroy(c.obj);
                }
                (*attrib)[src] +=
                    std::chrono::duration<double, std::micro>(clock::now() - batch)
                        .count();
            }
        } catch (...) {
            for (; i < cmds_.size(); ++i)
                cmds_[i].destroy(cmds_[i].obj);
            rewind();
            throw;
        }
        rewind();
    }

    // Destroy every pending command WITHOUT invoking it and rewind. Used when a
    // run aborts (a system threw): the aborted wave's edits must not leak into
    // the next run's first flush.
    void discard() noexcept {
        for (auto const& c : cmds_)
            c.destroy(c.obj);
        rewind();
    }

    [[nodiscard]]
    std::size_t size() const noexcept {
        return cmds_.size();
    }

private:
    // Type-erased handle to one recorded command living in the arena.
    struct Cmd {
        void* obj;
        void (*invoke)(void*, World&);
        void (*destroy)(void*) noexcept;
        SystemId source; // the system that recorded it (flush attribution)
    };
    struct Block {
        std::unique_ptr<std::byte[]> data;
        std::size_t cap;
    };
    static constexpr std::size_t kBlock = 64 * 1024;

    // Forget the recorded commands and rewind the block chain; blocks and the
    // cmds_ capacity are retained for reuse.
    void rewind() noexcept {
        cmds_.clear();
        cur_ = 0;
        off_ = 0;
    }

    static std::size_t align_up(std::size_t n, std::size_t a) noexcept {
        return (n + a - 1) & ~(a - 1);
    }

    // Bump-allocate sz bytes from the current block, advancing to the next
    // retained block or appending a fresh one as needed. Returned storage stays
    // put until apply() rewinds, so constructed commands are never relocated.
    void* allocate(std::size_t sz, std::size_t al) {
        for (;;) {
            if (cur_ < blocks_.size()) {
                std::size_t const base = align_up(off_, al);
                if (base + sz <= blocks_[cur_].cap) {
                    off_ = base + sz;
                    return blocks_[cur_].data.get() + base;
                }
                if (cur_ + 1 < blocks_.size()) { // try the next retained block
                    ++cur_;
                    off_ = 0;
                    continue;
                }
            }
            std::size_t const cap = std::max(kBlock, align_up(sz, al));
            blocks_.push_back(Block {std::make_unique<std::byte[]>(cap), cap});
            cur_ = blocks_.size() - 1;
            off_ = 0;
        }
    }

    std::vector<Block> blocks_;
    std::size_t cur_ = 0; // index of the block being filled
    std::size_t off_ = 0; // bytes used in that block
    std::vector<Cmd> cmds_;
};

class CommandBuffer {
public:
    // Record a deferred operation. Lock-free shard lookup; the only lock is the
    // calling thread's own (normally uncontended) shard mutex.
    template <class F>
    void record(F&& f) {
        // Recording from inside a flushing command would invalidate the store
        // being drained (and self-deadlock on the shard mutex) -- catch it in
        // debug before the deadlock does.
        assert(!applying_.load(std::memory_order_relaxed) &&
               "CommandBuffer::record called during apply() (from a command?)");
        auto& shard = local_shard();
        std::lock_guard lock(shard.mutex);
        shard.store.record(std::forward<F>(f));
    }

    // Replay all recorded commands, then clear. Must be called when no systems
    // are executing. Drains shards in creation order; within a shard, in record
    // order. Takes each shard's (uncontended) mutex so a stray recording thread
    // is excluded rather than racing the drain.
    void apply(World& world, FlushAttrib* attrib = nullptr) {
        auto lock = std::lock_guard(create_mutex_);
        applying_.store(true, std::memory_order_relaxed);
        try {
            for (auto const& shard : shards_) {
                std::lock_guard shard_lock(shard->mutex);
                shard->store.apply(world, attrib);
            }
        } catch (...) {
            // The throwing shard already cleaned itself up; discard the
            // not-yet-drained shards too so no command from this failed flush
            // replays on a later apply.
            for (auto const& shard : shards_) {
                std::lock_guard shard_lock(shard->mutex);
                shard->store.discard();
            }
            applying_.store(false, std::memory_order_relaxed);
            throw;
        }
        applying_.store(false, std::memory_order_relaxed);
    }

    // Drop every pending command without applying it (see CommandStore::discard).
    void discard() noexcept {
        auto lock = std::lock_guard(create_mutex_);
        for (auto const& shard : shards_) {
            std::lock_guard shard_lock(shard->mutex);
            shard->store.discard();
        }
    }

    auto size() {
        auto lock = std::lock_guard(create_mutex_);
        auto n    = 0ul;
        for (auto const& shard : shards_)
            n += shard->store.size();
        return n;
    }

    bool empty() { return size() == 0; }

private:
    static constexpr int kShards = 64;

    struct Shard {
        std::mutex mutex;
        CommandStore store;
    };

    Shard& local_shard() {
        int const i = detail::this_thread_index() % kShards;
        if (Shard* s = slots_[i].load(std::memory_order_acquire))
            return *s;
        // First touch of this slot: create the shard under the structural lock.
        std::lock_guard lock(create_mutex_);
        if (Shard* s = slots_[i].load(std::memory_order_relaxed))
            return *s;
        shards_.push_back(std::make_unique<Shard>());
        Shard* s = shards_.back().get();
        slots_[i].store(s, std::memory_order_release);
        return *s;
    }

    std::array<std::atomic<Shard*>, kShards> slots_ {};
    // lock-free shard lookup
    std::vector<std::unique_ptr<Shard>> shards_; // ownership + apply order
    std::mutex create_mutex_;                    // shard creation + apply
    std::atomic<bool> applying_ {false};         // debug: no record during apply
};

} // namespace ecs
