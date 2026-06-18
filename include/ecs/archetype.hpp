// ecs/archetype.hpp
//
// An archetype owns the storage for every entity that has exactly one given
// set of component types. Storage is column-oriented twice over:
//
//   archetype
//     |- entities[]                         (row -> Entity)
//     |- column for component A  = soa_storage<A>   (itself SoA over A's fields)
//     |- column for component B  = soa_storage<B>   (itself SoA over B's fields)
//
// Columns are type-erased behind IColumn so an archetype can hold a
// heterogeneous component set, while typed access downcasts back to the
// concrete soa_storage<T>.

#pragma once

#include "entity.hpp"
#include "soa.hpp"

#include <algorithm>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace ecs {

// Type-erased component column.
struct IColumn {
  virtual ~IColumn() = default;
  // Append a default-constructed element; return its row.
  virtual std::size_t emplace_default() = 0;
  // swap-remove a row; return the row that was relocated into it.
  virtual std::size_t swap_remove(std::size_t row) = 0;
  // Move element `row` of *this into a fresh row of `dst` (same dynamic type).
  virtual void move_row_to(IColumn& dst, std::size_t row) = 0;
  // A fresh, empty column of the same concrete type.
  virtual std::unique_ptr<IColumn> clone_empty() const = 0;
  virtual std::size_t size() const = 0;
};

template <class T>
struct Column final : IColumn {
  soa_storage<T> store;

  std::size_t emplace_default() override { return store.emplace_default(); }
  std::size_t swap_remove(std::size_t row) override {
    return store.swap_remove(row);
  }
  void move_row_to(IColumn& dst, std::size_t row) override {
    auto& d = static_cast<Column<T>&>(dst);
    d.store.push_back(store.gather(row));
  }
  std::unique_ptr<IColumn> clone_empty() const override {
    return std::make_unique<Column<T>>();
  }
  std::size_t size() const override { return store.size(); }
};

// Sorted list of component ids -- the identity of an archetype.
using Signature = std::vector<ComponentId>;

inline std::size_t hash_signature(const Signature& s) noexcept {
  std::size_t h = 1469598103934665603ull; // FNV-1a
  for (ComponentId id : s) {
    h ^= id;
    h *= 1099511628211ull;
  }
  return h;
}

struct Archetype {
  Signature signature;
  std::vector<Entity> entities;                          // row -> entity
  std::unordered_map<ComponentId, std::unique_ptr<IColumn>> columns;

  bool has(ComponentId id) const { return columns.contains(id); }

  template <class T>
  Column<T>& column() {
    return static_cast<Column<T>&>(*columns.at(component_id<T>));
  }
  template <class T>
  const Column<T>& column() const {
    return static_cast<const Column<T>&>(*columns.at(component_id<T>));
  }

  std::size_t size() const { return entities.size(); }
};

} // namespace ecs
