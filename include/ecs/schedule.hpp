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
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <exec/async_scope.hpp>
#include <stdexec/execution.hpp>

namespace ecs {

// What a declarative tag means.
enum class AccessKind {
  ComponentRead,
  ComponentWrite,
  ResourceRead,
  ResourceWrite,
  PhaseTag
};

// Declarative access lists. Components: reads<Position>, writes<Velocity>.
// Resources: reads_res<Clock>, writes_res<RenderQueue>.
template <class... Cs>
struct reads {
  static constexpr AccessKind kind = AccessKind::ComponentRead;
  static std::vector<std::uint32_t> ids() { return {component_id<Cs>...}; }
};
template <class... Cs>
struct writes {
  static constexpr AccessKind kind = AccessKind::ComponentWrite;
  static std::vector<std::uint32_t> ids() { return {component_id<Cs>...}; }
};
template <class... Rs>
struct reads_res {
  static constexpr AccessKind kind = AccessKind::ResourceRead;
  static std::vector<std::uint32_t> ids() { return {resource_id<Rs>...}; }
};
template <class... Rs>
struct writes_res {
  static constexpr AccessKind kind = AccessKind::ResourceWrite;
  static std::vector<std::uint32_t> ids() { return {resource_id<Rs>...}; }
};

// Ordering tag: systems run in ascending phase order, with a barrier between
// phases; within a phase they are leveled by conflicts as usual. Default phase
// is 0, so e.g. phase<-1> runs before normal systems (startup), phase<1> after.
template <int N>
struct phase {
  static constexpr AccessKind kind = AccessKind::PhaseTag;
  static constexpr int value = N;
};

// Stable handle to a registered system, returned by add()/add_once().
using SystemId = std::uint64_t;

class Schedule {
public:
  struct System {
    SystemId id = 0;
    std::string name;
    std::vector<std::uint32_t> reads;       // component ids
    std::vector<std::uint32_t> writes;      // component ids
    std::vector<std::uint32_t> res_reads;   // resource ids
    std::vector<std::uint32_t> res_writes;  // resource ids
    std::function<void(World&, Commands&)> run;
    int phase = 0;                          // coarse ordering across barriers
    std::size_t level = 0;                  // intra-phase conflict wavefront
    bool once = false;                      // removed after it next runs
  };

  // Register a system: its callable followed by any number of access tags, in
  // any order. Returns a handle that remove() can later unschedule. A system
  // declaring no writes never conflicts and so is maximally parallel. Example:
  //   sched.add("render", extract_fn,
  //             reads<Transform, Mesh>{}, writes_res<RenderQueue>{});
  template <class Fn, class... Access>
  SystemId add(std::string name, Fn&& fn, Access... access) {
    return emplace(std::move(name), std::forward<Fn>(fn), /*once=*/false,
                   access...);
  }

  // Register a one-shot system: it runs on the next run() and is then removed.
  // The natural way to do setup -- spawn the initial world, then stop running.
  template <class Fn, class... Access>
  SystemId add_once(std::string name, Fn&& fn, Access... access) {
    return emplace(std::move(name), std::forward<Fn>(fn), /*once=*/true,
                   access...);
  }

  // Unschedule a system by handle. Returns true if it was present.
  bool remove(SystemId id) {
    const auto n = std::erase_if(systems_,
                                 [id](const System& s) { return s.id == id; });
    if (n) dirty_ = true;
    return n != 0;
  }

  std::size_t size() const noexcept { return systems_.size(); }
  // Number of sequential waves (barriers) a run() will execute: ascending
  // (phase, conflict-level) groups. With all systems in the default phase this
  // is just the conflict-level count.
  std::size_t level_count() {
    rebuild();
    return waves_.size();
  }
  const std::vector<System>& systems() const { return systems_; }

  // Run every system on `scheduler`, respecting conflicts, blocking until all
  // have completed. Systems in the same wave run concurrently; each gets a
  // `Commands` to record mutations, flushed at every wave barrier. One-shot
  // (add_once) systems are removed afterwards.
  template <class Scheduler>
  void run(World& world, Scheduler scheduler) {
    rebuild();
    Commands cmds{world};
    exec::async_scope scope;
    for (const auto& wave : waves_) {
      for (std::size_t idx : wave) {
        auto& sys = systems_[idx];
        scope.spawn(stdexec::starts_on(
            scheduler, stdexec::then(stdexec::just(), [&sys, &world, &cmds] {
              sys.run(world, cmds);
            })));
      }
      stdexec::sync_wait(scope.on_empty()); // barrier
      world.apply_commands();               // make this wave's edits visible
    }
    prune_once();
  }

  // Run inline on the calling thread (no scheduler/thread pool). Systems within
  // a wave run sequentially. The way to do setup: add_once a setup system, then
  // schedule.run(world).
  void run(World& world) {
    rebuild();
    Commands cmds{world};
    for (const auto& wave : waves_) {
      for (std::size_t idx : wave) systems_[idx].run(world, cmds);
      world.apply_commands();
    }
    prune_once();
  }

private:
  void prune_once() {
    if (std::erase_if(systems_, [](const System& s) { return s.once; }))
      dirty_ = true;
  }

  using IdList = std::vector<std::uint32_t>;

  template <class Fn, class... Access>
  SystemId emplace(std::string name, Fn&& fn, bool once, Access... access) {
    System sys;
    sys.id = ++next_id_;
    sys.name = std::move(name);
    sys.run = std::forward<Fn>(fn);
    sys.once = once;
    (apply_access(sys, access), ...);
    systems_.push_back(std::move(sys));
    dirty_ = true;
    return sys.id;
  }

  // Route one access tag into the matching set on the system.
  template <class A>
  static void apply_access(System& sys, A) {
    auto append = [](IdList& dst, IdList src) {
      dst.insert(dst.end(), src.begin(), src.end());
    };
    if constexpr (A::kind == AccessKind::ComponentRead)
      append(sys.reads, A::ids());
    else if constexpr (A::kind == AccessKind::ComponentWrite)
      append(sys.writes, A::ids());
    else if constexpr (A::kind == AccessKind::ResourceRead)
      append(sys.res_reads, A::ids());
    else if constexpr (A::kind == AccessKind::ResourceWrite)
      append(sys.res_writes, A::ids());
    else if constexpr (A::kind == AccessKind::PhaseTag)
      sys.phase = A::value;
  }

  static bool intersects(const IdList& a, const IdList& b) {
    for (std::uint32_t x : a)
      if (std::find(b.begin(), b.end(), x) != b.end()) return true;
    return false;
  }

  // A write/read or write/write overlap on the same id space is a conflict.
  static bool conflicts_on(const IdList& aw, const IdList& ar, const IdList& bw,
                           const IdList& br) {
    return intersects(aw, br) || intersects(aw, bw) || intersects(bw, ar);
  }

  // Two systems conflict if they conflict on components OR on resources.
  static bool conflict(const System& a, const System& b) {
    return conflicts_on(a.writes, a.reads, b.writes, b.reads) ||
           conflicts_on(a.res_writes, a.res_reads, b.res_writes, b.res_reads);
  }

  void rebuild() {
    if (!dirty_) return;
    // Intra-phase level: 1 + max level over earlier conflicting systems in the
    // *same* phase. Cross-phase ordering is handled by the phase barrier, so
    // conflicts only matter within a phase.
    for (std::size_t i = 0; i < systems_.size(); ++i) {
      std::size_t lvl = 0;
      for (std::size_t j = 0; j < i; ++j)
        if (systems_[j].phase == systems_[i].phase &&
            conflict(systems_[i], systems_[j]))
          lvl = std::max(lvl, systems_[j].level + 1);
      systems_[i].level = lvl;
    }
    // Waves run in ascending (phase, level) order with a barrier between each;
    // a std::map keys them in exactly that order.
    std::map<std::pair<int, std::size_t>, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < systems_.size(); ++i)
      groups[{systems_[i].phase, systems_[i].level}].push_back(i);
    waves_.clear();
    waves_.reserve(groups.size());
    for (auto& [key, idxs] : groups) waves_.push_back(std::move(idxs));
    dirty_ = false;
  }

  std::vector<System> systems_;
  std::vector<std::vector<std::size_t>> waves_; // ordered groups, barrier between
  SystemId next_id_ = 0;
  bool dirty_ = true;
};

} // namespace ecs
