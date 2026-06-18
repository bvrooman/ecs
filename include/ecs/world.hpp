// ecs/world.hpp
//
// The World owns all entities and archetypes and drives the dynamic-archetype
// transitions: adding or removing a component moves an entity's data from its
// current archetype table into the one whose signature matches the new set.

#pragma once

#include "archetype.hpp"
#include "command_buffer.hpp"
#include "entity.hpp"
#include "resource.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ecs {

namespace detail {
struct SigHash {
  std::size_t operator()(const Signature& s) const noexcept {
    return hash_signature(s);
  }
};
} // namespace detail

class World {
public:
  World() {
    // Archetype 0 is the empty archetype (entities with no components).
    empty_archetype_ = make_archetype(Signature{}, [](Archetype&) {});
  }

  World(const World&) = delete;
  World& operator=(const World&) = delete;

  // --- mutation (auto-deferring) ----------------------------------------
  // --- mutation (command-buffer-only) -----------------------------------
  // spawn/destroy/add/remove/set never mutate directly: they record into the
  // command buffer and take effect when it is flushed -- at each Schedule level
  // barrier, or when a World::run_once(fn) returns. They must therefore be
  // called inside a run context (a system, or the run_once callback); doing so
  // outside one is a programming error (asserted in debug builds). Because every
  // structural edit is deferred, mutating mid-iteration is always safe.
  // spawn() returns a usable handle immediately (alive after the next flush).
  template <class... Cs>
  Entity spawn(Cs... comps) {
    assert(executing_ && "spawn() must be called inside Schedule::run or "
                         "World::run_once");
    const Entity e = reserve();
    commands_.record([e, ... comps = std::move(comps)](World& w) mutable {
      w.materialize(e);
      (w.add_now<Cs>(e, std::move(comps)), ...);
    });
    return e;
  }

  void destroy(Entity e) {
    assert(executing_ && "destroy() must be called inside a run context");
    commands_.record([e](World& w) { w.destroy_now(e); });
  }

  template <class C>
  void add(Entity e, C value) {
    assert(executing_ && "add() must be called inside a run context");
    commands_.record([e, value = std::move(value)](World& w) mutable {
      if (w.alive(e)) w.add_now<C>(e, std::move(value));
    });
  }

  template <class C>
  void remove(Entity e) {
    assert(executing_ && "remove() must be called inside a run context");
    commands_.record([e](World& w) {
      if (w.alive(e)) w.remove_now<C>(e);
    });
  }

  template <class C>
  void set(Entity e, C value) {
    assert(executing_ && "set() must be called inside a run context");
    commands_.record([e, value = std::move(value)](World& w) mutable {
      if (w.alive(e)) w.set_now<C>(e, std::move(value));
    });
  }

  // Run fn once inside a fresh run context, then flush the recorded mutations
  // single-threaded -- the way to do setup/teardown without a full Schedule:
  //   world.run_once([&](World& w){ for (...) w.spawn(...); });
  // Entities created inside are live once run_once returns. Nests safely (only
  // the outermost context flushes, so it is a no-op wrapper inside a schedule).
  template <class Fn>
  void run_once(Fn&& fn) {
    const bool entered = !executing_;
    if (entered) set_executing(true);
    fn(*this);
    if (entered) {
      apply_commands();
      set_executing(false);
    }
  }

  bool alive(Entity e) const {
    return e.index < records_.size() && records_[e.index].alive &&
           records_[e.index].generation == e.generation;
  }

  std::size_t size() const noexcept { return alive_count_; }

  // --- reads ------------------------------------------------------------
  template <class C>
  bool has(Entity e) const {
    return alive(e) &&
           archetypes_[records_[e.index].archetype]->has(component_id<C>);
  }

  // Read a component by value (gathered from its per-field columns).
  template <class C>
  C get(Entity e) const {
    const auto& rec = records_[e.index];
    return archetypes_[rec.archetype]->template column<C>().store.gather(rec.row);
  }

  // --- resources (singletons not owned by any entity) -------------------
  // Set up resources before running a schedule; like structural edits, do not
  // add/remove resources while a schedule is executing.
  template <class T, class... Args>
  T& emplace_resource(Args&&... args) {
    return resources_.emplace<T>(std::forward<Args>(args)...);
  }
  template <class T>
  T& resource() {
    return resources_.get<T>();
  }
  template <class T>
  const T& resource() const {
    return resources_.get<T>();
  }
  template <class T>
  T* try_resource() {
    return resources_.try_get<T>();
  }
  template <class T>
  bool has_resource() const {
    return resources_.contains<T>();
  }
  template <class T>
  void remove_resource() {
    resources_.remove<T>();
  }

  // --- command buffer ---------------------------------------------------
  // Whether a run context is active (a schedule running, or run_once). Mutators
  // require this; reads/resources do not.
  bool executing() const { return executing_.load(std::memory_order_acquire); }

  // The underlying command buffer (advanced: record custom deferred closures).
  CommandBuffer& commands() { return commands_; }
  // Apply all recorded commands now. Driven by the Schedule at each barrier and
  // by run_once; call manually only if you record into commands() directly.
  void apply_commands() { commands_.apply(*this); }

  // --- archetype access (used by queries) -------------------------------
  const std::vector<std::unique_ptr<Archetype>>& archetypes() const {
    return archetypes_;
  }
  std::vector<std::unique_ptr<Archetype>>& archetypes() { return archetypes_; }

  // Internal: locate an entity's archetype/row (used by queries' callbacks).
  std::pair<std::uint32_t, std::uint32_t> locate(Entity e) const {
    const auto& rec = records_[e.index];
    return {rec.archetype, rec.row};
  }

private:
  friend class Schedule;  // toggles deferred mode around system execution

  struct Record {
    std::uint32_t archetype = 0;
    std::uint32_t row = 0;
    std::uint32_t generation = 0;
    bool alive = false;
  };

  void set_executing(bool on) {
    executing_.store(on, std::memory_order_release);
  }

  // Immediate mutation primitives. The public spawn/destroy/add/remove/set
  // dispatch to these when not deferred, and the command closures call them at
  // apply time -- so these always run with exclusive, single-threaded access.
  void destroy_now(Entity e) {
    if (!alive(e)) return;
    auto& rec = records_[e.index];
    remove_row(*archetypes_[rec.archetype], rec.row);
    rec.alive = false;
    ++rec.generation;
    free_.push_back(e.index);
    free_count_.store(free_.size(), std::memory_order_release);
    --alive_count_;
  }

  template <class C>
  void add_now(Entity e, C value) {
    assert(alive(e));
    auto& rec = records_[e.index];
    const ComponentId cid = component_id<C>;
    if (archetypes_[rec.archetype]->has(cid)) {
      archetypes_[rec.archetype]->template column<C>().store.set(rec.row, value);
      return;
    }
    Signature sig = archetypes_[rec.archetype]->signature;
    sig.insert(std::upper_bound(sig.begin(), sig.end(), cid), cid);

    const auto from = rec.archetype;
    const auto to = get_or_create_archetype(sig, [&](Archetype& b) {
      for (auto& [id, col] : archetypes_[from]->columns)
        b.columns.emplace(id, col->clone_empty());
      b.columns.emplace(cid, std::make_unique<Column<C>>());
    });

    relocate(e, to, [&](Archetype& b) {
      b.template column<C>().store.push_back(value);
    });
  }

  template <class C>
  void remove_now(Entity e) {
    assert(alive(e));
    auto& rec = records_[e.index];
    const ComponentId cid = component_id<C>;
    if (!archetypes_[rec.archetype]->has(cid)) return;

    Signature sig;
    sig.reserve(archetypes_[rec.archetype]->signature.size() - 1);
    for (ComponentId id : archetypes_[rec.archetype]->signature)
      if (id != cid) sig.push_back(id);

    const auto from = rec.archetype;
    const auto to = get_or_create_archetype(sig, [&](Archetype& b) {
      for (auto& [id, col] : archetypes_[from]->columns)
        if (id != cid) b.columns.emplace(id, col->clone_empty());
    });

    relocate(e, to, [](Archetype&) {});
  }

  template <class C>
  void set_now(Entity e, const C& value) {
    const auto& rec = records_[e.index];
    archetypes_[rec.archetype]->template column<C>().store.set(rec.row, value);
  }

  // Hand out a fresh entity handle without creating storage. Thread-safe and
  // lock-free on the common (growth) path: a brand-new index is a single atomic
  // fetch_add, so many systems can reserve concurrently while recording spawn
  // commands. Recycling a freed slot takes a short lock; reused indices carry
  // the slot's already-bumped generation.
  Entity reserve() {
    if (free_count_.load(std::memory_order_acquire) > 0) {
      std::lock_guard lock(reserve_mutex_);
      if (!free_.empty()) {
        const std::uint32_t idx = free_.back();
        free_.pop_back();
        free_count_.store(free_.size(), std::memory_order_release);
        return Entity{idx, records_[idx].generation};
      }
    }
    return Entity{reserve_high_.fetch_add(1, std::memory_order_relaxed), 0};
  }

  // Give a reserved entity its storage (a row in the empty archetype). Runs
  // single-threaded -- from create(), or from a spawn command at a barrier.
  // resize() fills any gap with dead default slots that their own materialize()
  // later activates, so out-of-order materialization is safe.
  void materialize(Entity e) {
    if (e.index >= records_.size()) records_.resize(e.index + 1);
    auto& rec = records_[e.index];
    rec.alive = true;
    rec.generation = e.generation;
    rec.archetype = empty_archetype_;
    auto& a = *archetypes_[empty_archetype_];
    rec.row = static_cast<std::uint32_t>(a.entities.size());
    a.entities.push_back(e);
    ++alive_count_;
  }

  template <class Factory>
  std::uint32_t make_archetype(const Signature& sig, Factory&& makeColumns) {
    const auto idx = static_cast<std::uint32_t>(archetypes_.size());
    auto arch = std::make_unique<Archetype>();
    arch->signature = sig;
    makeColumns(*arch);
    sig_index_.emplace(sig, idx);
    archetypes_.push_back(std::move(arch));
    return idx;
  }

  template <class Factory>
  std::uint32_t get_or_create_archetype(const Signature& sig,
                                        Factory&& makeColumns) {
    if (auto it = sig_index_.find(sig); it != sig_index_.end())
      return it->second;
    return make_archetype(sig, std::forward<Factory>(makeColumns));
  }

  // Drop a row from an archetype with swap-and-pop, patching the relocated
  // entity's record. Heap-allocated archetypes keep stable addresses, so
  // references stay valid across archetypes_ growth.
  void remove_row(Archetype& a, std::uint32_t row) {
    const auto last = static_cast<std::uint32_t>(a.entities.size() - 1);
    for (auto& [id, col] : a.columns) col->swap_remove(row);
    if (row != last) {
      a.entities[row] = a.entities[last];
      records_[a.entities[row].index].row = row;
    }
    a.entities.pop_back();
  }

  // Move an entity from its current archetype into `to`, copying shared
  // columns and letting `addExtra` append any newly-added component.
  template <class AddExtra>
  void relocate(Entity e, std::uint32_t to, AddExtra&& addExtra) {
    auto& rec = records_[e.index];
    const auto from = rec.archetype;
    auto& a = *archetypes_[from];
    auto& b = *archetypes_[to];

    for (auto& [id, col] : a.columns) {
      if (auto it = b.columns.find(id); it != b.columns.end())
        col->move_row_to(*it->second, rec.row);
    }
    addExtra(b);

    const auto new_row = static_cast<std::uint32_t>(b.entities.size());
    b.entities.push_back(e);

    remove_row(a, rec.row);
    rec.archetype = to;
    rec.row = new_row;
  }

  CommandBuffer commands_;
  ResourceRegistry resources_;
  std::vector<std::unique_ptr<Archetype>> archetypes_;
  std::unordered_map<Signature, std::uint32_t, detail::SigHash> sig_index_;
  std::vector<Record> records_;
  std::vector<std::uint32_t> free_;
  std::uint32_t empty_archetype_ = 0;
  std::size_t alive_count_ = 0;

  std::mutex reserve_mutex_;             // guards free-list recycling in reserve()
  std::atomic<std::uint32_t> reserve_high_{0}; // next brand-new index; ==
                                               // records_.size() when no
                                               // reservations are outstanding
  std::atomic<std::size_t> free_count_{0};     // free_.size(), for a lock-free
                                               // "anything to recycle?" check
  std::atomic<bool> executing_{false};         // is a run context active?
};

} // namespace ecs
