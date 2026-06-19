// ecs/schedule.hpp
//
// std::execution (P2300) compatible system scheduler.
//
// A *system* is a callable whose parameters declare what it touches:
//
//   void physics(Query<Position, const Velocity> q, ResMut<Gravity> g);
//
//     Query<Cs...>  -- iterate matching entities; a `const` component is a read,
//                      a non-const component is a write.
//     Res<T>        -- read resource T.        ResMut<T> -- write resource T.
//     Commands&     -- record deferred structural edits (no tracked access).
//
// The scheduler derives each system's read/write sets *from these parameter
// types* and constructs the parameters when the system runs -- so the declared
// access cannot drift from what the system actually does (the load-bearing
// assumption behind safe parallelism). It then finds systems that conflict (one
// writes what another reads or writes) and assigns each to a wavefront *level*:
// every system runs after all earlier conflicting systems, and same-level
// systems are conflict-free, so they execute concurrently. A `phase<N>` tag
// gives coarse ordering across barriers independent of conflicts.
//
// Execution is driven through senders/receivers: each system is
// `starts_on(scheduler, then(just(), run))`, spawned into an `exec::async_scope`,
// with a `sync_wait(scope.on_empty())` barrier between waves. Any P2300
// scheduler works; `run(world)` runs inline with no thread pool.

#pragma once

#include "query.hpp"
#include "world.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <exec/async_scope.hpp>
#include <stdexec/execution.hpp>

namespace ecs {

// Coarse ordering tag: systems run in ascending phase order with a barrier
// between phases; within a phase they are leveled by conflicts. Default 0, so
// phase<-1> is a startup phase, phase<1> a teardown/late phase.
template <int N>
struct phase {
  static constexpr int value = N;
};

using SystemId = std::uint64_t;

// A system's derived component/resource access (id sets). `exclusive` means the
// system has unanalyzable access (it took a raw World&) and so conflicts with
// every other system -- it runs alone in its wave.
struct SystemAccess {
  std::vector<std::uint32_t> reads, writes, res_reads, res_writes;
  bool exclusive = false;
};

namespace detail {

// --- extract a callable's parameter types --------------------------------
template <class F>
struct fn_traits : fn_traits<decltype(&F::operator())> {};
template <class C, class R, class... A>
struct fn_traits<R (C::*)(A...)> { using args = std::tuple<A...>; };
template <class C, class R, class... A>
struct fn_traits<R (C::*)(A...) const> { using args = std::tuple<A...>; };
template <class C, class R, class... A>
struct fn_traits<R (C::*)(A...) noexcept> { using args = std::tuple<A...>; };
template <class C, class R, class... A>
struct fn_traits<R (C::*)(A...) const noexcept> { using args = std::tuple<A...>; };
template <class R, class... A>
struct fn_traits<R (*)(A...)> { using args = std::tuple<A...>; };

// --- system parameter protocol: declare(access) + bind(world, commands) --
template <class P>
struct system_param; // unspecialized => using an unsupported parameter type

template <class C>
void declare_component(SystemAccess& a) {
  if constexpr (std::is_const_v<C>)
    a.reads.push_back(component_id<std::remove_const_t<C>>);
  else
    a.writes.push_back(component_id<C>);
}

template <class... Cs>
struct system_param<Query<Cs...>> {
  static void declare(SystemAccess& a) { (declare_component<Cs>(a), ...); }
  static Query<Cs...> bind(World& w, Commands&) { return Query<Cs...>(w); }
};
template <class T>
struct system_param<Res<T>> {
  static void declare(SystemAccess& a) { a.res_reads.push_back(resource_id<T>); }
  static Res<T> bind(World& w, Commands&) { return Res<T>(w); }
};
template <class T>
struct system_param<ResMut<T>> {
  static void declare(SystemAccess& a) { a.res_writes.push_back(resource_id<T>); }
  static ResMut<T> bind(World& w, Commands&) { return ResMut<T>(w); }
};
template <>
struct system_param<Commands&> {
  static void declare(SystemAccess&) {}            // side channel, no access
  static Commands& bind(World&, Commands& c) { return c; }
};
// Escape hatch: a raw World& grants ad-hoc read access (queries, size, get,
// has, alive). Its access cannot be analyzed, so the system is marked exclusive
// and runs alone -- safe, but not parallel. Prefer Query/Res for parallelism.
template <>
struct system_param<World&> {
  static void declare(SystemAccess& a) { a.exclusive = true; }
  static World& bind(World& w, Commands&) { return w; }
};

// phase tag detection
template <class T>
struct phase_value {
  static constexpr bool is = false;
  static constexpr int value = 0;
};
template <int N>
struct phase_value<phase<N>> {
  static constexpr bool is = true;
  static constexpr int value = N;
};

// Validate the trailing tags are phase<N> and return the (last) phase, default 0.
template <class... Tags>
constexpr int phase_of() {
  static_assert((phase_value<Tags>::is && ...),
                "the only allowed system tag is phase<N>; component/resource "
                "access is derived from the system's parameters");
  int p = 0;
  ((p = phase_value<Tags>::value), ...);
  return p;
}

} // namespace detail

class Schedule {
public:
  struct System {
    SystemId id = 0;
    std::string name;
    SystemAccess access;
    std::function<void(World&, Commands&)> run;
    int phase = 0;
    std::size_t level = 0;
    bool once = false;
  };

  // Register a system. Its access is derived from its parameter types; the only
  // accepted trailing tag is phase<N> (ordering). Returns a handle remove() can
  // unschedule. Example:
  //   sched.add("integrate", [](Query<Position, const Velocity> q){ ... });
  //   sched.add("startup",   setup_fn, phase<-1>{});
  template <class Fn, class... Tags>
  SystemId add(std::string name, Fn&& fn, Tags...) {
    return emplace(std::move(name), std::forward<Fn>(fn), /*once=*/false,
                   detail::phase_of<Tags...>());
  }

  // One-shot system: runs on the next run() and is then removed (e.g. setup).
  template <class Fn, class... Tags>
  SystemId add_once(std::string name, Fn&& fn, Tags...) {
    return emplace(std::move(name), std::forward<Fn>(fn), /*once=*/true,
                   detail::phase_of<Tags...>());
  }

  // Unschedule a system by handle. Returns true if it was present.
  bool remove(SystemId id) {
    const auto n = std::erase_if(systems_,
                                 [id](const System& s) { return s.id == id; });
    if (n) dirty_ = true;
    return n != 0;
  }

  std::size_t size() const noexcept { return systems_.size(); }
  // Number of sequential waves (barriers) a run() executes: ascending
  // (phase, conflict-level) groups.
  std::size_t level_count() {
    rebuild();
    return waves_.size();
  }
  const std::vector<System>& systems() const { return systems_; }

  // Run on `scheduler`, blocking until all systems complete. Systems in the
  // same wave run concurrently; each gets its parameters (built from the world
  // and a shared Commands), and recorded edits flush at every wave barrier.
  // One-shot systems are removed afterwards.
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

  // Run inline on the calling thread (no scheduler). Systems within a wave run
  // sequentially. The way to do setup: add_once a setup system, then run(world).
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

  template <class Args, std::size_t... I>
  static void declare_into(SystemAccess& a, std::index_sequence<I...>) {
    (detail::system_param<std::tuple_element_t<I, Args>>::declare(a), ...);
  }
  template <class Args, class Fn, std::size_t... I>
  static void invoke(Fn& fn, World& w, Commands& c, std::index_sequence<I...>) {
    fn(detail::system_param<std::tuple_element_t<I, Args>>::bind(w, c)...);
  }

  template <class Fn>
  SystemId emplace(std::string name, Fn&& fn, bool once, int phase) {
    using Args = typename detail::fn_traits<std::decay_t<Fn>>::args;
    constexpr auto N = std::tuple_size_v<Args>;
    System sys;
    sys.id = ++next_id_;
    sys.name = std::move(name);
    sys.phase = phase;
    sys.once = once;
    declare_into<Args>(sys.access, std::make_index_sequence<N>{});
    sys.run = [fn = std::forward<Fn>(fn)](World& w, Commands& c) mutable {
      invoke<Args>(fn, w, c, std::make_index_sequence<N>{});
    };
    systems_.push_back(std::move(sys));
    dirty_ = true;
    return sys.id;
  }

  using IdList = std::vector<std::uint32_t>;

  static bool intersects(const IdList& a, const IdList& b) {
    for (std::uint32_t x : a)
      if (std::find(b.begin(), b.end(), x) != b.end()) return true;
    return false;
  }
  static bool conflicts_on(const IdList& aw, const IdList& ar, const IdList& bw,
                           const IdList& br) {
    return intersects(aw, br) || intersects(aw, bw) || intersects(bw, ar);
  }
  static bool conflict(const System& a, const System& b) {
    if (a.access.exclusive || b.access.exclusive) return true;
    return conflicts_on(a.access.writes, a.access.reads, b.access.writes,
                        b.access.reads) ||
           conflicts_on(a.access.res_writes, a.access.res_reads,
                        b.access.res_writes, b.access.res_reads);
  }

  void rebuild() {
    if (!dirty_) return;
    // Intra-phase level: 1 + max level over earlier conflicting systems in the
    // same phase (cross-phase ordering is handled by the phase barrier).
    for (std::size_t i = 0; i < systems_.size(); ++i) {
      std::size_t lvl = 0;
      for (std::size_t j = 0; j < i; ++j)
        if (systems_[j].phase == systems_[i].phase &&
            conflict(systems_[i], systems_[j]))
          lvl = std::max(lvl, systems_[j].level + 1);
      systems_[i].level = lvl;
    }
    // Waves run in ascending (phase, level) order with a barrier between each.
    std::map<std::pair<int, std::size_t>, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < systems_.size(); ++i)
      groups[{systems_[i].phase, systems_[i].level}].push_back(i);
    waves_.clear();
    waves_.reserve(groups.size());
    for (auto& [key, idxs] : groups) waves_.push_back(std::move(idxs));
    dirty_ = false;
  }

  std::vector<System> systems_;
  std::vector<std::vector<std::size_t>> waves_;
  SystemId next_id_ = 0;
  bool dirty_ = true;
};

} // namespace ecs
