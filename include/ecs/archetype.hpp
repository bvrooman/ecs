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

#include <ecs/archetype_id.hpp>
#include <ecs/column_id.hpp>
#include <ecs/detail/container/id_vector.hpp>
#include <ecs/entity.hpp>
#include <ecs/soa.hpp>

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
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

// Sorted, de-duplicated set of component ids -- the identity of an archetype.
// A component's position in this list is its ColumnId (the archetype's parallel
// `columns` vector is kept in the same order), so the id<->column mapping lives
// here: find(id) yields the column slot. Kept as a flat sorted vector, not a
// hash set -- lookup is a binary search over a contiguous array, membership
// tests are linear merges, and there are no per-node allocations.

class Signature {
public:
    Signature() = default;
    Signature(std::initializer_list<ComponentId> const ids)
        : ids_(ids) {
        normalize();
    }

    // The unmatchable sentinel: a signature holding a private Dummy component id
    // that no real query carries, so it includes() no archetype (an *empty*
    // signature would match every archetype instead).
    static Signature null() noexcept { return Signature {{component_id<Dummy>}}; }

    // Construct from any range of ComponentId (a std::vector, a views::keys of
    // a component bundle, ...). Constrained off Signature itself so it can't
    // shadow the copy/move constructors.
    template <class R>
        requires std::ranges::input_range<R> &&
                 std::convertible_to<std::ranges::range_reference_t<R>, ComponentId> &&
                 (!std::same_as<std::remove_cvref_t<R>, Signature>)
    explicit Signature(R&& ids) {
        if constexpr (std::ranges::sized_range<R>)
            ids_.reserve(std::ranges::size(ids));
        for (ComponentId const id : ids)
            ids_.push_back(id);
        normalize();
    }

    // The column slot of `id` (its position), or nullopt when absent.
    [[nodiscard]]
    std::optional<ColumnId> find(ComponentId const id) const noexcept {
        auto const it = std::ranges::lower_bound(ids_, id);
        if (it != ids_.end() && *it == id)
            return ColumnId {static_cast<ColumnId::type>(it - ids_.begin())};
        return std::nullopt;
    }
    [[nodiscard]]
    bool contains(ComponentId const id) const noexcept {
        return find(id).has_value();
    }
    // Does this signature contain every id in `sub`? (Both are sorted.)
    [[nodiscard]]
    bool includes(Signature const& sub) const noexcept {
        return std::ranges::includes(ids_, sub.ids_);
    }

    // The signature of the archetype reached by adding / removing one component
    // (a no-op if `id` is already present / already absent).
    [[nodiscard]]
    Signature with(ComponentId const id) const {
        Signature s   = *this;
        auto const it = std::ranges::lower_bound(s.ids_, id);
        if (it == s.ids_.end() || *it != id) {
            s.ids_.insert(it, id);
            s.rehash();
        }
        return s;
    }
    [[nodiscard]]
    Signature without(ComponentId const id) const {
        Signature s   = *this;
        auto const it = std::ranges::lower_bound(s.ids_, id);
        if (it != s.ids_.end() && *it == id) {
            s.ids_.erase(it);
            s.rehash();
        }
        return s;
    }

    // The component id occupying column slot `c`.
    [[nodiscard]]
    ComponentId operator[](ColumnId const c) const noexcept {
        return ids_[c.value];
    }

    [[nodiscard]]
    std::size_t size() const noexcept {
        return ids_.size();
    }
    [[nodiscard]]
    bool empty() const noexcept {
        return ids_.empty();
    }
    [[nodiscard]]
    auto begin() const noexcept {
        return ids_.begin();
    }
    [[nodiscard]]
    auto end() const noexcept {
        return ids_.end();
    }

    // The set's hash, computed once at construction and cached (a signature is
    // immutable after construction; with()/without() build a fresh one). Feeds
    // std::hash<Signature> so a Signature is a drop-in unordered_map key.
    [[nodiscard]]
    std::size_t hash() const noexcept {
        return hash_;
    }

    // Equality is element-wise. The cached hash is only a fast reject (unequal
    // hashes => unequal signatures); it is NOT the equality itself -- FNV-1a can
    // collide, and a hash-only compare would silently alias distinct signatures
    // in the archetype maps (a wrong-archetype lookup). So a hash match falls
    // through to the authoritative id compare.
    friend bool operator==(Signature const& a, Signature const& b) noexcept {
        return a.hash_ == b.hash_ && a.ids_ == b.ids_;
    }

private:
    // Dummy component to initialize null (unmatchable) Signature.
    struct Dummy {};

    // FNV-1a over the ids. Accumulate in an explicit 64-bit type: the offset
    // basis and prime are 64-bit, which would truncate into a 32-bit std::size_t
    // on ILP32 targets (e.g. wasm32); narrow to size_t only at the end.
    static constexpr std::uint64_t kFnvBasis = 1469598103934665603ull;
    void rehash() noexcept {
        std::uint64_t h = kFnvBasis;
        for (auto const id : ids_) {
            h ^= id.value;
            h *= 1099511628211ull;
        }
        hash_ = static_cast<std::size_t>(h);
    }
    void normalize() {
        std::ranges::sort(ids_);
        ids_.erase(std::ranges::unique(ids_).begin(), ids_.end());
        rehash();
    }
    std::vector<ComponentId> ids_;                           // sorted, unique
    std::size_t hash_ = static_cast<std::size_t>(kFnvBasis); // empty-set hash
};

struct Archetype {
    Signature signature;
    std::vector<Entity> entities; // row -> entity
    // One column per signature entry, in signature (sorted-id) order, indexed
    // by ColumnId. A flat parallel vector instead of a hash map: lookup is a
    // binary search over the signature, iteration during relocation is a linear
    // walk, and there are no per-node heap allocations.
    detail::IdVector<ColumnId, std::unique_ptr<IColumn>> columns;

    // Lazily-populated archetype-graph edges: which archetype an entity lands
    // in when component `cid` is added to / removed from this one. Makes the
    // steady-state add/remove transition a single map hit instead of a
    // signature build + hash + global index lookup (World maintains these).
    std::unordered_map<ComponentId, ArchetypeId> add_edge, remove_edge;

    [[nodiscard]]
    bool has(ComponentId const id) const noexcept {
        return signature.contains(id);
    }

    // Type-erased column for `id`; nullptr when absent.
    [[nodiscard]]
    IColumn* column_for(ComponentId const id) noexcept {
        auto const c = signature.find(id);
        return c ? columns[*c].get() : nullptr;
    }

    // Type-erased column for `id`. Precondition: has(id). Constness follows
    // `self` (unique_ptr's operator* would otherwise leak a mutable reference
    // out of a const archetype).
    template <class Self>
    auto& column_at(this Self&& self, ComponentId const id) {
        auto const c = self.signature.find(id);
        assert(c && "Archetype::column_at: component not present");
        using Col = std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>,
                                       IColumn const,
                                       IColumn>;
        return static_cast<Col&>(*self.columns[*c]);
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
        std::vector<Entity> next; // Entity (a Handle) is not default-constructible
        next.reserve(entities.size());
        for (auto const old : perm)
            next.push_back(entities[old]);
        entities = std::move(next);
        for (auto const& col : columns)
            col->apply_permutation(perm);
    }

    auto size() const { return entities.size(); }
};

} // namespace ecs

// Signature is a drop-in unordered_map key: its hash is the cached set hash, so
// no bespoke hasher is needed at the use site.
template <>
struct std::hash<ecs::Signature> {
    std::size_t operator()(ecs::Signature const& s) const noexcept { return s.hash(); }
};
