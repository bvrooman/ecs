// ecs/world.hpp
//
// The World owns all entities and archetypes and drives the dynamic-archetype
// transitions: adding or removing a component moves an entity's data from its
// current archetype table into the one whose signature matches the new set.

#pragma once

#include "archetype.hpp"
#include "entity.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
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

  // --- entity lifecycle -------------------------------------------------
  Entity create() {
    std::uint32_t idx;
    if (!free_.empty()) {
      idx = free_.back();
      free_.pop_back();
    } else {
      idx = static_cast<std::uint32_t>(records_.size());
      records_.push_back(Record{});
    }
    Record& rec = records_[idx];
    rec.alive = true;
    Entity e{idx, rec.generation};

    Archetype& a = *archetypes_[empty_archetype_];
    rec.archetype = empty_archetype_;
    rec.row = static_cast<std::uint32_t>(a.entities.size());
    a.entities.push_back(e);
    ++alive_count_;
    return e;
  }

  template <class... Cs>
  Entity create_with(Cs... comps) {
    Entity e = create();
    (add<Cs>(e, std::move(comps)), ...);
    return e;
  }

  void destroy(Entity e) {
    if (!alive(e)) return;
    Record& rec = records_[e.index];
    remove_row(*archetypes_[rec.archetype], rec.row);
    rec.alive = false;
    ++rec.generation;
    free_.push_back(e.index);
    --alive_count_;
  }

  bool alive(Entity e) const {
    return e.index < records_.size() && records_[e.index].alive &&
           records_[e.index].generation == e.generation;
  }

  std::size_t size() const noexcept { return alive_count_; }

  // --- components -------------------------------------------------------
  template <class C>
  void add(Entity e, C value) {
    assert(alive(e));
    Record& rec = records_[e.index];
    const ComponentId cid = component_id<C>;
    if (archetypes_[rec.archetype]->has(cid)) {
      archetypes_[rec.archetype]->template column<C>().store.set(rec.row, value);
      return;
    }
    Signature sig = archetypes_[rec.archetype]->signature;
    sig.insert(std::upper_bound(sig.begin(), sig.end(), cid), cid);

    const std::uint32_t from = rec.archetype;
    const std::uint32_t to = get_or_create_archetype(sig, [&](Archetype& b) {
      for (auto& [id, col] : archetypes_[from]->columns)
        b.columns.emplace(id, col->clone_empty());
      b.columns.emplace(cid, std::make_unique<Column<C>>());
    });

    relocate(e, to, [&](Archetype& b) {
      b.template column<C>().store.push_back(value);
    });
  }

  template <class C>
  void remove(Entity e) {
    assert(alive(e));
    Record& rec = records_[e.index];
    const ComponentId cid = component_id<C>;
    if (!archetypes_[rec.archetype]->has(cid)) return;

    Signature sig;
    sig.reserve(archetypes_[rec.archetype]->signature.size() - 1);
    for (ComponentId id : archetypes_[rec.archetype]->signature)
      if (id != cid) sig.push_back(id);

    const std::uint32_t from = rec.archetype;
    const std::uint32_t to = get_or_create_archetype(sig, [&](Archetype& b) {
      for (auto& [id, col] : archetypes_[from]->columns)
        if (id != cid) b.columns.emplace(id, col->clone_empty());
    });

    relocate(e, to, [](Archetype&) {});
  }

  template <class C>
  bool has(Entity e) const {
    return alive(e) &&
           archetypes_[records_[e.index].archetype]->has(component_id<C>);
  }

  // Read a component by value (gathered from its per-field columns).
  template <class C>
  C get(Entity e) const {
    const Record& rec = records_[e.index];
    return archetypes_[rec.archetype]->template column<C>().store.gather(rec.row);
  }

  template <class C>
  void set(Entity e, const C& value) {
    const Record& rec = records_[e.index];
    archetypes_[rec.archetype]->template column<C>().store.set(rec.row, value);
  }

  // --- archetype access (used by queries) -------------------------------
  const std::vector<std::unique_ptr<Archetype>>& archetypes() const {
    return archetypes_;
  }
  std::vector<std::unique_ptr<Archetype>>& archetypes() { return archetypes_; }

  // Internal: locate an entity's archetype/row (used by queries' callbacks).
  std::pair<std::uint32_t, std::uint32_t> locate(Entity e) const {
    const Record& rec = records_[e.index];
    return {rec.archetype, rec.row};
  }

private:
  struct Record {
    std::uint32_t archetype = 0;
    std::uint32_t row = 0;
    std::uint32_t generation = 0;
    bool alive = false;
  };

  template <class Factory>
  std::uint32_t make_archetype(const Signature& sig, Factory&& makeColumns) {
    const std::uint32_t idx = static_cast<std::uint32_t>(archetypes_.size());
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
    const std::uint32_t last = static_cast<std::uint32_t>(a.entities.size() - 1);
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
    Record& rec = records_[e.index];
    const std::uint32_t from = rec.archetype;
    Archetype& a = *archetypes_[from];
    Archetype& b = *archetypes_[to];

    for (auto& [id, col] : a.columns) {
      if (auto it = b.columns.find(id); it != b.columns.end())
        col->move_row_to(*it->second, rec.row);
    }
    addExtra(b);

    const std::uint32_t new_row = static_cast<std::uint32_t>(b.entities.size());
    b.entities.push_back(e);

    remove_row(a, rec.row);
    rec.archetype = to;
    rec.row = new_row;
  }

  std::vector<std::unique_ptr<Archetype>> archetypes_;
  std::unordered_map<Signature, std::uint32_t, detail::SigHash> sig_index_;
  std::vector<Record> records_;
  std::vector<std::uint32_t> free_;
  std::uint32_t empty_archetype_ = 0;
  std::size_t alive_count_ = 0;
};

} // namespace ecs
