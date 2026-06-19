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
// A command is a type-erased `void(World&)` closure (move_only_function, so
// move-only captured component values are fine).

#pragma once

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

  // Record a deferred operation. Lock-free shard lookup; the only lock is the
  // calling thread's own (normally uncontended) shard mutex.
  template <class F>
  void record(F&& f) {
    Shard& shard = local_shard();
    std::lock_guard lock(shard.mutex);
    shard.commands.emplace_back(std::forward<F>(f));
  }

  // Replay all recorded commands, then clear. Must be called when no systems
  // are executing. Drains shards in creation order; within a shard, in record
  // order.
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
};

} // namespace ecs
