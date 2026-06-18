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

// Map T -> std::tuple<std::vector<field0>, std::vector<field1>, ...>.
template <class T, std::size_t... I>
auto columns_type(std::index_sequence<I...>)
    -> std::tuple<std::vector<reflect::field_type_t<T, I>>...>;

template <class T>
using columns_t = decltype(columns_type<T>(
    std::make_index_sequence<reflect::field_count_v<T>>{}));

} // namespace detail

template <reflect::Reflectable T>
class soa_storage {
public:
  using value_type = T;
  static constexpr std::size_t field_count = reflect::field_count_v<T>;

  std::size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }

  void reserve(std::size_t n) {
    apply_columns([&](auto&... col) { (col.reserve(n), ...); });
  }

  void clear() noexcept {
    apply_columns([](auto&... col) { (col.clear(), ...); });
    size_ = 0;
  }

  // Scatter the fields of `v` across the per-field columns. Returns the row.
  std::size_t push_back(const T& v) {
    reflect::for_each_field(v, [&](auto Ic, const auto& field) {
      std::get<Ic.value>(columns_).push_back(field);
    });
    return size_++;
  }

  // Reassemble (gather) the element at `row` into a T value.
  T gather(std::size_t row) const {
    T out{};
    reflect::for_each_field(out, [&](auto Ic, auto& field) {
      field = std::get<Ic.value>(columns_)[row];
    });
    return out;
  }

  // Overwrite the element at `row`.
  void set(std::size_t row, const T& v) {
    reflect::for_each_field(v, [&](auto Ic, const auto& field) {
      std::get<Ic.value>(columns_)[row] = field;
    });
  }

  // Remove `row` with swap-and-pop. Returns the source row that was moved into
  // `row` (== new size if the last element was removed), so callers can patch
  // up any external row<->entity bookkeeping.
  std::size_t swap_remove(std::size_t row) {
    const std::size_t last = size_ - 1;
    apply_columns([&](auto&... col) {
      ((col[row] = std::move(col[last])), ...);
      (col.pop_back(), ...);
    });
    --size_;
    return last;
  }

  // Append a default-constructed element and return its row.
  std::size_t emplace_default() { return push_back(T{}); }

  // Contiguous view over the I-th field across all elements: the SoA payoff.
  template <std::size_t I>
  std::span<reflect::field_type_t<T, I>> column() noexcept {
    auto& c = std::get<I>(columns_);
    return {c.data(), c.size()};
  }
  template <std::size_t I>
  std::span<const reflect::field_type_t<T, I>> column() const noexcept {
    const auto& c = std::get<I>(columns_);
    return {c.data(), c.size()};
  }

private:
  template <class F>
  void apply_columns(F&& f) {
    std::apply([&](auto&... col) { f(col...); }, columns_);
  }
  template <class F>
  void apply_columns(F&& f) const {
    std::apply([&](auto&... col) { f(col...); }, columns_);
  }

  detail::columns_t<T> columns_{};
  std::size_t size_ = 0;
};

// A component with zero data members (a "tag"). It still needs a row count but
// stores nothing -- handled as a degenerate SoA with no columns.
template <reflect::Reflectable T>
  requires(reflect::field_count_v<T> == 0)
class soa_storage<T> {
public:
  using value_type = T;
  static constexpr std::size_t field_count = 0;

  std::size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }
  void reserve(std::size_t) noexcept {}
  void clear() noexcept { size_ = 0; }
  std::size_t push_back(const T&) { return size_++; }
  T gather(std::size_t) const { return T{}; }
  void set(std::size_t, const T&) noexcept {}
  std::size_t emplace_default() { return size_++; }
  std::size_t swap_remove(std::size_t) { return --size_; }

private:
  std::size_t size_ = 0;
};

} // namespace ecs
