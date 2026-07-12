// ecs/soa.hpp
//
// soa_storage<T>: the "AoS written, SoA stored" container.
//
// You declare a component as a plain struct and push/read it as if it were an
// array-of-structures. Reflection (ecs/reflection/reflect.hpp) decomposes T into its
// fields at compile time and stores one contiguous column per field -- i.e. a
// structure-of-arrays -- so that field-wise access is cache friendly and
// SIMD/vectorizer friendly.
//
//   struct Transform { Vec3 pos; Quat rot; Vec3 scale; };
//   soa_storage<Transform> s;       // stored as 3 separate columns
//   s.push_back({...});             // scatter fields into their columns
//   Transform t = s.gather(i);      // reassemble one element
//   std::span pos = s.column<0>();  // contiguous run of all pos values

#pragma once

#include "reflection/reflect.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace ecs {

namespace detail {
    template <class T, class IndexSequence>
    struct columns_helper;

    template <class T, std::size_t... I>
    struct columns_helper<T, std::index_sequence<I...>> {
        using type = std::tuple<std::vector<reflect::field_type_t<T, I>>...>;
    };

    template <class T>
    using columns_t =
        columns_helper<T, std::make_index_sequence<reflect::field_count_v<T>>>::type;

} // namespace detail

template <reflect::Reflectable T>
class soa_storage {
public:
    using value_type                  = T;
    static constexpr auto field_count = reflect::field_count_v<T>;

    [[nodiscard]]
    auto size() const noexcept {
        return size_;
    }
    [[nodiscard]]
    auto empty() const noexcept {
        return size_ == 0;
    }

    void reserve(std::size_t n) {
        apply_columns([&](auto&... col) { (col.reserve(n), ...); });
    }

    void clear() noexcept {
        apply_columns([](auto&... col) { (col.clear(), ...); });
        size_ = 0;
    }

    // Scatter the fields of `v` across the per-field columns. Returns the row.
    // Transactional: if any field's push throws (growth failure, throwing field
    // copy), the columns already appended to are popped back on unwind --
    // otherwise the columns would be left at different lengths, silently and
    // permanently desynchronizing every row after this one.
    auto push_back(T const& v) {
        return scatter_append(v, [](auto& col, auto const& field) {
            col.push_back(field);
        });
    }

    // Move overload: fields are moved into their columns, so a component with
    // heap-owning fields (std::string, std::vector) pays no copy on spawn or
    // archetype transition. Also the only path a move-only component can take.
    auto push_back(T&& v) {
        return scatter_append(v, [](auto& col, auto& field) {
            col.push_back(std::move(field));
        });
    }

    // Reassemble (gather) the element at `row` into a T value.
    [[nodiscard]]
    T gather(std::size_t row) const {
        T out {};
        reflect::for_each_field(out, [&](auto Ic, auto& field) {
            field = std::get<Ic.value>(columns_)[row];
        });
        return out;
    }

    // Gather the element at `row` by MOVING each field out of its column --
    // for relocation, where the source row is swap-removed immediately after.
    [[nodiscard]]
    T take(std::size_t row) {
        T out {};
        reflect::for_each_field(out, [&](auto Ic, auto& field) {
            field = std::move(std::get<Ic.value>(columns_)[row]);
        });
        return out;
    }

    // Overwrite the element at `row`.
    void set(std::size_t row, T const& v) {
        reflect::for_each_field(v, [&](auto Ic, auto const& field) {
            std::get<Ic.value>(columns_)[row] = field;
        });
    }
    void set(std::size_t row, T&& v) {
        reflect::for_each_field(v, [&](auto Ic, auto& field) {
            std::get<Ic.value>(columns_)[row] = std::move(field);
        });
    }

    // Remove `row` with swap-and-pop. Returns the source row that was moved
    // into `row` (== new size if the last element was removed), so callers can
    // patch up any external row<->entity bookkeeping.
    auto swap_remove(std::size_t row) {
        assert(size_ > 0 && row < size_ && "soa_storage::swap_remove: row out of range");
        auto const last = size_ - 1;
        apply_columns([&](auto&... col) {
            // Skip the move when removing the last row: self-move-assignment is
            // only valid-but-unspecified for well-behaved types and outright
            // wrong for naive user-defined move assignments.
            if (row != last)
                ((col[row] = std::move(col[last])), ...);
            (col.pop_back(), ...);
        });
        --size_;
        return last;
    }

    // Append a default-constructed element and return its row.
    auto emplace_default() { return push_back(T {}); }

    // Reorder the elements: new row i holds what was at row perm[i] (perm is
    // a permutation of [0, size)). One linear gather pass per field column
    // into fresh storage -- a maintenance operation (World::sort_rows), not a
    // hot-path one, so it allocates the buffers it gathers into rather than
    // permuting in place.
    void apply_permutation(std::span<std::uint32_t const> perm) {
        assert(perm.size() == size_ &&
               "soa_storage::apply_permutation: perm size != element count");
        apply_columns([&](auto&... col) { (permute_column(col, perm), ...); });
    }

    // Contiguous view over the I-th field across all elements: the SoA payoff.
    // The explicit object parameter lets one definition serve both const and
    // mutable callers -- the span's element constness follows that of `self`.
    // Constrained to lvalues: a span into a temporary storage would dangle.
    template <std::size_t I, class Self>
        requires std::is_lvalue_reference_v<Self>
    auto column(this Self&& self) noexcept {
        auto& c = std::get<I>(self.columns_);
        return std::span(c.data(), c.size());
    }

    // Runtime (non-templated) base pointer of the i-th field's contiguous buffer
    // -- the type-erased counterpart of column<I>(), for host (JS) access where
    // the field index is only known at run time. Dispatches i through a static
    // table of per-field getters.
    void* field_base(std::size_t i) noexcept {
        assert(i < field_count && "soa_storage::field_base: field index out of range");
        static constexpr std::array<void* (*)(soa_storage&), field_count> getters =
            []<std::size_t... I>(std::index_sequence<I...>) {
                return std::array<void* (*)(soa_storage&), field_count> {
                    +[](soa_storage& s) -> void* { return std::get<I>(s.columns_).data(); }...};
            }(std::make_index_sequence<field_count> {});
        return getters[i](*this);
    }

private:
    template <class Self, class F>
    void apply_columns(this Self&& self, F&& f) {
        std::apply([&](auto&... col) { f(col...); }, self.columns_);
    }

    template <class C>
    static void permute_column(C& col, std::span<std::uint32_t const> perm) {
        C next;
        next.reserve(col.size());
        for (auto const from : perm)
            next.push_back(std::move(col[from]));
        col = std::move(next);
    }

    // The transactional scatter both push_back overloads share: `append`
    // performs one column's push (copying or moving); columns already appended
    // to are popped back on unwind (see push_back).
    template <class V, class Append>
    auto scatter_append(V&& v, Append&& append) {
        std::size_t pushed = 0;
        try {
            reflect::for_each_field(v, [&](auto Ic, auto&& field) {
                append(std::get<Ic.value>(columns_),
                       std::forward<decltype(field)>(field));
                ++pushed;
            });
        } catch (...) {
            std::size_t i = 0;
            apply_columns([&](auto&... col) {
                ((i++ < pushed ? col.pop_back() : void()), ...);
            });
            throw;
        }
        return size_++;
    }

    detail::columns_t<T> columns_ {};
    std::size_t size_ = 0;
};

// A component with zero data members (a "tag"). It still needs a row count but
// stores nothing -- handled as a degenerate SoA with no columns.
template <reflect::Reflectable T>
requires (reflect::field_count_v<T> == 0)
class soa_storage<T> {
public:
    using value_type                         = T;
    static constexpr std::size_t field_count = 0;

    [[nodiscard]]
    auto size() const noexcept {
        return size_;
    }
    [[nodiscard]]
    auto empty() const noexcept {
        return size_ == 0;
    }
    void reserve(std::size_t) noexcept {}
    void clear() noexcept { size_ = 0; }
    auto push_back(T const&) { return size_++; }
    auto push_back(T&&) { return size_++; }
    T gather(std::size_t) const { return T {}; }
    T take(std::size_t) { return T {}; }
    void set(std::size_t, T const&) noexcept {}
    void set(std::size_t, T&&) noexcept {}
    auto emplace_default() { return size_++; }
    auto swap_remove(std::size_t) {
        assert(size_ > 0 && "soa_storage::swap_remove: empty tag column");
        return --size_;
    }
    void apply_permutation(std::span<std::uint32_t const>) noexcept {} // no data
    void* field_base(std::size_t) noexcept { return nullptr; } // no fields

private:
    std::size_t size_ = 0;
};

} // namespace ecs
