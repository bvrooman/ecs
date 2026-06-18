// ecs/command_buffer.hpp
//
// Deferred structural changes.
//
// Adding/removing components, spawning and destroying entities all move data
// between archetype tables, which would invalidate any iteration in flight and
// race with other systems running on the schedule's thread pool. So during the
// parallel phase systems do not mutate structure directly -- they *record*
// their intent into a CommandBuffer, and the recorded commands are replayed
// single-threaded at a sync point (the Schedule flushes the buffer at every
// level barrier; see schedule.hpp).
//
// The buffer is internally synchronized, so many systems running concurrently
// may record into the same buffer without declaring a conflict -- recording is
// a thread-safe side channel, not tracked component/resource state.
//
// A command is type-erased as a `void(World&)` closure (move_only_function, so
// move-only captured component values are fine). The typed helpers below build
// those closures; they are declared here and defined in world.hpp where World
// is a complete type.

#pragma once

#include "entity.hpp"

#include <cstddef>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace ecs {

class World; // defined in world.hpp

class CommandBuffer {
public:
  using Command = std::move_only_function<void(World&)>;

  // Record an arbitrary deferred operation. Thread-safe.
  template <class F>
  void record(F&& f) {
    std::lock_guard lock(mutex_);
    commands_.emplace_back(std::forward<F>(f));
  }

  // Typed conveniences (definitions in world.hpp). Each is a no-op at apply
  // time if its target entity is no longer alive, so order-independent teardown
  // (e.g. two systems both destroying the same entity) is safe.
  void destroy(Entity e);
  template <class C>
  void add(Entity e, C value);
  template <class C>
  void remove(Entity e);
  template <class... Cs>
  void spawn(Cs... comps);

  // Replay all recorded commands in record order, then clear. Must be called
  // when no systems are executing (no recording happens concurrently).
  void apply(World& world) {
    std::vector<Command> pending;
    {
      std::lock_guard lock(mutex_);
      pending.swap(commands_);
    }
    for (auto& cmd : pending) cmd(world);
  }

  std::size_t size() {
    std::lock_guard lock(mutex_);
    return commands_.size();
  }
  bool empty() { return size() == 0; }

private:
  std::vector<Command> commands_;
  std::mutex mutex_;
};

} // namespace ecs
