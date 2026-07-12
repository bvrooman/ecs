// ecs/archetype.hpp
//
// An archetype owns the storage for every entity that has exactly one given
// set of component types. Storage is column-oriented twice over:
//
//   archetype
//     |- entities[]                         (row -> Entity)
//     |- column for component A  = soa_storage<A>   (itself SoA over A's
//     fields)
//     |- column for component B  = soa_storage<B>   (itself SoA over B's
//     fields)
//
// Columns are type-erased behind IColumn so an archetype can hold a
// heterogeneous component set, while typed access downcasts back to the
// concrete soa_storage<T>.

#pragma once

#include "entity.hpp"
#include "soa.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace ecs {

// Type-erased component column.
struct IColumn {
    virtual ~IColumn() = default;
    // Pre-grow the column's per-field buffers to hold n elements.
    virtual void reserve(std::size_t n) = 0;
    // Append a default-constructed element; return its row.
    virtual std::size_t emplace_default() = 0;
    // swap-remove a row; return the row that was relocated into it.
    virtual std::size_t swap_remove(std::size_t row) = 0;
    // Reorder rows: new row i holds what was at row perm[i] (perm is a
    // permutation of [0, size)). Bulk maintenance (row sorting); the caller
    // owns any external row bookkeeping (see Archetype::permute_rows).
    virtual void apply_permutation(std::span<std::uint32_t const> perm) = 0;
    // Move element `row` of *this into a fresh row of `dst` (same dynamic
    // type).
    virtual void move_row_to(IColumn& dst, std::size_t row) = 0;
    // A fresh, empty column of the same concrete type.
    [[nodiscard]]
    virtual std::unique_ptr<IColumn> clone_empty() const = 0;
    [[nodiscard]]
    virtual std::size_t size() const = 0;
    // Base pointer of the i-th field's contiguous (SoA) buffer. Lets a host --
    // e.g. a JS system -- read/write a component's fields by index without
    // knowing its C++ type; the field's element type comes from a registered
    // descriptor (see dynamic/). Valid until the column relocates/reallocates.
    [[nodiscard]]
    virtual void* field_base(std::size_t i) = 0;
};

template <class T>
struct Column final : IColumn {
    soa_storage<T> store;

    void reserve(std::size_t n) override { store.reserve(n); }
    std::size_t emplace_default() override { return store.emplace_default(); }
    std::size_t swap_remove(std::size_t row) override { return store.swap_remove(row); }
    void apply_permutation(std::span<std::uint32_t const> perm) override {
        store.apply_permutation(perm);
    }
    void move_row_to(IColumn& dst, std::size_t row) override {
        // A ComponentId bookkeeping bug would make this cast silent UB; catch
        // it loudly in debug builds.
        assert(dynamic_cast<Column*>(&dst) &&
               "Column::move_row_to: destination column has a different type");
        auto& d = static_cast<Column&>(dst);
        // Move, not copy: the source row is swap-removed right after a
        // relocation, so heap-owning fields need not pay a copy per transition.
        d.store.push_back(store.take(row));
    }
    [[nodiscard]]
    std::unique_ptr<IColumn> clone_empty() const override {
        return std::make_unique<Column>();
    }
    [[nodiscard]]
    std::size_t size() const override {
        return store.size();
    }
    [[nodiscard]]
    void* field_base(std::size_t i) override {
        return store.field_base(i);
    }
};

// Sorted list of component ids -- the identity of an archetype.
using Signature = std::vector<ComponentId>;

inline std::size_t hash_signature(Signature const& s) noexcept {
    // Accumulate in an explicit 64-bit type: the FNV-1a offset basis and prime
    // are 64-bit, which would truncate into a 32-bit std::size_t on ILP32 targets
    // (e.g. wasm32). Narrow to size_t only for the returned bucket index.
    std::uint64_t h = 1469598103934665603ull; // FNV-1a
    for (auto const id : s) {
        h ^= id;
        h *= 1099511628211ull;
    }
    return static_cast<std::size_t>(h);
}

struct Archetype {
    static constexpr std::size_t npos = ~std::size_t {0};

    Signature signature;
    std::vector<Entity> entities; // row -> entity
    // One column per signature entry, in signature (sorted-id) order. A flat
    // parallel vector instead of a hash map: lookup is a binary search over a
    // contiguous uint32 array, iteration during relocation is a linear walk,
    // and there are no per-node heap allocations.
    std::vector<std::unique_ptr<IColumn>> columns;

    // Lazily-populated archetype-graph edges: which archetype an entity lands
    // in when component `cid` is added to / removed from this one. Makes the
    // steady-state add/remove transition a single map hit instead of a
    // signature build + hash + global index lookup (World maintains these).
    std::unordered_map<ComponentId, std::uint32_t> add_edge, remove_edge;

    // Index of `id` within signature/columns, or npos.
    [[nodiscard]]
    std::size_t find(ComponentId const id) const noexcept {
        auto const it = std::ranges::lower_bound(signature, id);
        return (it != signature.end() && *it == id)
                   ? static_cast<std::size_t>(it - signature.begin())
                   : npos;
    }

    [[nodiscard]]
    bool has(ComponentId const id) const noexcept {
        return find(id) != npos;
    }

    // Type-erased column for `id`; nullptr when absent.
    [[nodiscard]]
    IColumn* column_for(ComponentId const id) noexcept {
        auto const i = find(id);
        return i == npos ? nullptr : columns[i].get();
    }

    // Type-erased column for `id`. Precondition: has(id). Constness follows
    // `self` (unique_ptr's operator* would otherwise leak a mutable reference
    // out of a const archetype).
    template <class Self>
    auto& column_at(this Self&& self, ComponentId const id) {
        auto const i = self.find(id);
        assert(i != Archetype::npos && "Archetype::column_at: component not present");
        using Col = std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>,
                                       IColumn const,
                                       IColumn>;
        return static_cast<Col&>(*self.columns[i]);
    }

    template <class T, class Self>
    auto& column(this Self&& self) {
        using Col = std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>,
                                       Column<T> const,
                                       Column<T>>;
        return static_cast<Col&>(self.column_at(component_id<T>));
    }

    // Pre-grow every column (and the row->entity table) for n rows.
    void reserve(std::size_t const n) {
        entities.reserve(n);
        for (auto const& col : columns)
            col->reserve(n);
    }

    // Reorder this archetype's rows: new row i takes old row perm[i], in
    // every column and the row->entity table together. Entity RECORDS still
    // name the old rows afterwards -- the caller must patch them
    // (World::sort_rows does), so this is a building block, not an entry
    // point.
    void permute_rows(std::span<std::uint32_t const> perm) {
        assert(perm.size() == entities.size() &&
               "Archetype::permute_rows: perm size != row count");
        std::vector<Entity> next(entities.size());
        for (std::size_t i = 0; i < perm.size(); ++i)
            next[i] = entities[perm[i]];
        entities = std::move(next);
        for (auto const& col : columns)
            col->apply_permutation(perm);
    }

    auto size() const { return entities.size(); }
};

} // namespace ecs
