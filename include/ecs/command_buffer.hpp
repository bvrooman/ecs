// ecs/command_buffer.hpp
//
// Deferred structural changes (see also world.hpp for the typed helpers).
//
// Adding/removing components, spawning and destroying entities all move data
// between archetype tables, which would invalidate any iteration in flight and
// race with other systems running on the schedule's thread pool. So during the
// parallel phase systems do not mutate structure directly -- they *record*
// their intent here, and the recorded commands are replayed single-threaded at
// a sync point (the Schedule flushes at every level barrier; see schedule.hpp).
//
// Recording is sharded per worker thread: each thread appends to its own shard
// with no cross-thread contention on the hot path (shard lookup is a lock-free
// atomic load; the per-shard mutex is uncontended unless more threads exist
// than shards). apply() drains every shard single-threaded. Commands within one
// thread's shard replay in record order; ordering *across* shards is
// unspecified (matches per-system command semantics in other ECS designs).
//
// A command is a type-erased `void(World&)` closure (move_only_function, so
// move-only captured component values are fine). The typed helpers are declared
// here and defined in world.hpp where World is a complete type.

#pragma once

#include "entity.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace ecs {

namespace detail {
// A stable, process-wide index for the calling thread (0, 1, 2, ... assigned on
// first use). Used to shard the command buffer without per-call synchronization.
inline int this_thread_index() noexcept {
  static std::atomic<int> counter{0};
  thread_local int idx = counter.fetch_add(1, std::memory_order_relaxed);
  return idx;
}
} // namespace detail

class World; // defined in world.hpp

class CommandBuffer {
public:
  using Command = std::move_only_function<void(World&)>;

  // Record an arbitrary deferred operation. Lock-free shard lookup; the only
  // lock is the calling thread's own (normally uncontended) shard mutex.
  template <class F>
  void record(F&& f) {
    Shard& shard = local_shard();
    std::lock_guard lock(shard.mutex);
    shard.commands.emplace_back(std::forward<F>(f));
  }

  // Typed conveniences (definitions in world.hpp). Each is a no-op at apply
  // time if its target entity is no longer alive, so order-independent teardown
  // (e.g. two systems both destroying the same entity) is safe.
  void destroy(Entity e);
  template <class C>
  void add(Entity e, C value);
  template <class C>
  void remove(Entity e);
  // Reserves an entity handle immediately (thread-safe) and returns it, so the
  // caller can record further edits on it this frame; the entity's storage and
  // components are created when the buffer is applied.
  template <class... Cs>
  Entity spawn(Cs... comps);

  // Replay all recorded commands, then clear. Must be called when no systems
  // are executing (no recording happens concurrently). Drains shards in
  // creation order; within a shard, in record order.
  void apply(World& world) {
    std::lock_guard lock(create_mutex_);
    for (auto& shard : shards_) {
      for (auto& cmd : shard->commands) cmd(world);
      shard->commands.clear();
    }
  }

  std::size_t size() {
    std::lock_guard lock(create_mutex_);
    std::size_t n = 0;
    for (auto& shard : shards_) n += shard->commands.size();
    return n;
  }
  bool empty() { return size() == 0; }

private:
  friend class World; // sets world_ and reaches reserve()/materialize()

  static constexpr int kShards = 64;

  struct Shard {
    std::mutex mutex;
    std::vector<Command> commands;
  };

  Shard& local_shard() {
    const int i = detail::this_thread_index() % kShards;
    if (Shard* s = slots_[i].load(std::memory_order_acquire)) return *s;
    // First touch of this slot: create the shard under the structural lock.
    std::lock_guard lock(create_mutex_);
    if (Shard* s = slots_[i].load(std::memory_order_relaxed)) return *s;
    shards_.push_back(std::make_unique<Shard>());
    Shard* s = shards_.back().get();
    slots_[i].store(s, std::memory_order_release);
    return *s;
  }

  std::array<std::atomic<Shard*>, kShards> slots_{}; // lock-free shard lookup
  std::vector<std::unique_ptr<Shard>> shards_;       // ownership + apply order
  std::mutex create_mutex_;                          // shard creation + apply
  World* world_ = nullptr;                           // owning world (ctor-set)
};

} // namespace ecs
