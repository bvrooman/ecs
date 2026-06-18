// ecs/schedule.hpp
//
// std::execution (P2300) compatible system scheduler.
//
// A *system* is a callable `void(World&)` together with the set of components
// it reads and writes. The schedule analyses those access sets to find systems
// that conflict (one writes what another reads or writes) and assigns each
// system to a wavefront *level*: every system runs after all earlier-registered
// systems it conflicts with, and systems in the same level are guaranteed
// conflict-free, so they execute concurrently.
//
// Execution is driven entirely through senders/receivers: each system is
// `starts_on(scheduler, then(just(), run))`, spawned into an `exec::async_scope`,
// and each level is separated by a `sync_wait(scope.on_empty())` barrier. Any
// P2300 scheduler works -- `exec::static_thread_pool`, a GPU scheduler, an
// Asio-backed scheduler, etc. -- so the engine plugs into whatever async
// runtime the application already uses.

#pragma once

#include "entity.hpp"
#include "world.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include <exec/async_scope.hpp>
#include <stdexec/execution.hpp>

namespace ecs {

// Declarative access lists, e.g. reads<Position>, writes<Position, Velocity>.
template <class... Cs>
struct reads {
  static std::vector<ComponentId> ids() { return {component_id<Cs>...}; }
};
template <class... Cs>
struct writes {
  static std::vector<ComponentId> ids() { return {component_id<Cs>...}; }
};

class Schedule {
public:
  struct System {
    std::string name;
    std::vector<ComponentId> reads;
    std::vector<ComponentId> writes;
    std::function<void(World&)> run;
    std::size_t level = 0;
  };

  // Register a system. Access sets default to empty (a system declaring no
  // writes never conflicts and is maximally parallel).
  template <class Reads = reads<>, class Writes = writes<>, class Fn>
  Schedule& add(std::string name, Reads, Writes, Fn&& fn) {
    systems_.push_back(System{std::move(name), Reads::ids(), Writes::ids(),
                              std::forward<Fn>(fn), 0});
    dirty_ = true;
    return *this;
  }

  std::size_t size() const noexcept { return systems_.size(); }
  std::size_t level_count() {
    rebuild();
    return levels_.size();
  }
  const std::vector<System>& systems() const { return systems_; }

  // Run every system on `scheduler`, respecting conflicts, blocking until all
  // have completed. Independent systems within a level run concurrently.
  template <class Scheduler>
  void run(World& world, Scheduler scheduler) {
    rebuild();
    exec::async_scope scope;
    for (const auto& level : levels_) {
      for (std::size_t idx : level) {
        System& sys = systems_[idx];
        scope.spawn(stdexec::starts_on(
            scheduler, stdexec::then(stdexec::just(), [&sys, &world] {
              sys.run(world);
            })));
      }
      // Barrier: wait for this wavefront to drain before starting the next.
      stdexec::sync_wait(scope.on_empty());
    }
  }

private:
  // Two systems conflict if one's write set intersects the other's read or
  // write set.
  static bool intersects(const std::vector<ComponentId>& a,
                         const std::vector<ComponentId>& b) {
    for (ComponentId x : a)
      if (std::find(b.begin(), b.end(), x) != b.end()) return true;
    return false;
  }
  static bool conflict(const System& a, const System& b) {
    return intersects(a.writes, b.reads) || intersects(a.writes, b.writes) ||
           intersects(b.writes, a.reads);
  }

  void rebuild() {
    if (!dirty_) return;
    // Level of system i = 1 + max level over earlier conflicting systems.
    for (std::size_t i = 0; i < systems_.size(); ++i) {
      std::size_t lvl = 0;
      for (std::size_t j = 0; j < i; ++j)
        if (conflict(systems_[i], systems_[j]))
          lvl = std::max(lvl, systems_[j].level + 1);
      systems_[i].level = lvl;
    }
    levels_.clear();
    for (std::size_t i = 0; i < systems_.size(); ++i) {
      if (systems_[i].level >= levels_.size())
        levels_.resize(systems_[i].level + 1);
      levels_[systems_[i].level].push_back(i);
    }
    dirty_ = false;
  }

  std::vector<System> systems_;
  std::vector<std::vector<std::size_t>> levels_;
  bool dirty_ = true;
};

} // namespace ecs
