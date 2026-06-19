// ecs/soa.hpp
//
// soa_storage<T>: the "AoS written, SoA stored" container.
//
// You declare a component as a plain struct and push/read it as if it were an
// array-of-structures. Reflection (ecs/reflect.hpp) decomposes T into its
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

#include "reflect.hpp"
#include <cstddef>
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
    auto push_back(T const& v) {
        reflect::for_each_field(v, [&](auto Ic, auto const& field) {
            std::get<Ic.value>(columns_).push_back(field);
        });
        return size_++;
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

    // Overwrite the element at `row`.
    void set(std::size_t row, T const& v) {
        reflect::for_each_field(v, [&](auto Ic, auto const& field) {
            std::get<Ic.value>(columns_)[row] = field;
        });
    }

    // Remove `row` with swap-and-pop. Returns the source row that was moved
    // into `row` (== new size if the last element was removed), so callers can
    // patch up any external row<->entity bookkeeping.
    auto swap_remove(std::size_t row) {
        auto const last = size_ - 1;
        apply_columns([&](auto&... col) {
            ((col[row] = std::move(col[last])), ...);
            (col.pop_back(), ...);
        });
        --size_;
        return last;
    }

    // Append a default-constructed element and return its row.
    auto emplace_default() { return push_back(T {}); }

    // Contiguous view over the I-th field across all elements: the SoA payoff.
    // The explicit object parameter lets one definition serve both const and
    // mutable callers -- the span's element constness follows that of `self`.
    template <std::size_t I, class Self>
    auto column(this Self&& self) noexcept {
        auto& c = std::get<I>(self.columns_);
        return std::span(c.data(), c.size());
    }

private:
    template <class Self, class F>
    void apply_columns(this Self&& self, F&& f) {
        std::apply([&](auto&... col) { f(col...); }, self.columns_);
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
    T gather(std::size_t) const { return T {}; }
    void set(std::size_t, T const&) noexcept {}
    auto emplace_default() { return size_++; }
    auto swap_remove(std::size_t) { return --size_; }

private:
    std::size_t size_ = 0;
};

} // namespace ecs
