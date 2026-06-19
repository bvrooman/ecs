// ecs/reflect.hpp
//
// Compile-time field reflection facade.
//
// The whole library is written against the small surface defined in
// `namespace ecs::reflect`:
//
//   reflect::field_count_v<T>      -> number of non-static data members of T
//   reflect::get_field<I>(obj)     -> reference to the I-th data member
//   reflect::field_type_t<T, I>    -> type of the I-th data member
//   reflect::field_name<T, I>()    -> name of the I-th data member (best
//   effort) reflect::for_each_field(obj,f) -> invoke f(index, member&) for
//   every member
//
// There are two interchangeable backends:
//
//   * ECS_USE_P2996 == 1 : the *real* C++26 static reflection
//     (P2996 `std::meta`, splicers `[:r:]`, `^^T`). This is the canonical
//     answer to "use C++26 reflection". It requires the experimental
//     clang-p2996 / EDG toolchain.
//
//   * ECS_USE_P2996 == 0 : a portable backend implemented with the aggregate
//     brace-arity probe + structured bindings. It compiles on stock
//     C++20/23 compilers (e.g. Clang 18, GCC 13) so the engine and its tests
//     can be built and run *today*.
//
// Because every higher layer depends only on the facade, switching backends is
// a single flag and changes nothing else.

#pragma once

#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#ifndef ECS_USE_P2996
#if defined(__cpp_reflection) && __has_include(<meta>)
#define ECS_USE_P2996 1
#else
#define ECS_USE_P2996 0
#endif
#endif

namespace ecs::reflect {

// A component must be an aggregate so that its members can be reflected and
// re-assembled by brace-init / splicing.
template <class T>
concept Reflectable = std::is_aggregate_v<std::remove_cvref_t<T>>;

} // namespace ecs::reflect

// ===========================================================================
//  Backend A: real C++26 static reflection (P2996).
// ===========================================================================
#if ECS_USE_P2996

#include <meta>

namespace ecs::reflect {

namespace detail {
    using namespace std::meta;

    template <class T>
    consteval auto members() {
        return nonstatic_data_members_of(^^std::remove_cvref_t<T>,
                                         access_context::current());
    }

    template <class T>
    consteval std::size_t member_count() {
        return members<T>().size();
    }

    template <class T>
    consteval info member_at(std::size_t i) {
        return members<T>()[i];
    }
} // namespace detail

template <Reflectable T>
inline constexpr std::size_t field_count_v = detail::member_count<T>();

// Reference to the I-th non-static data member, via a member splicer.
template <std::size_t I, class T>
constexpr decltype(auto) get_field(T&& obj) noexcept {
    constexpr auto m = detail::member_at<std::remove_cvref_t<T>>(I);
    return (std::forward<T>(obj).[:m:]);
}

template <class T, std::size_t I>
using field_type_t =
    std::remove_cvref_t<decltype(get_field<I>(std::declval<std::remove_cvref_t<T>&>()))>;

template <class T, std::size_t I>
consteval std::string_view field_name() {
    return std::meta::identifier_of(detail::member_at<std::remove_cvref_t<T>>(I));
}

} // namespace ecs::reflect

// ===========================================================================
//  Backend B: portable reflection (aggregate arity + structured bindings).
// ===========================================================================
#else // !ECS_USE_P2996

namespace ecs::reflect {
namespace detail {

    // A value convertible to anything; used to probe how many members an
    // aggregate can be brace-initialized with.

    struct any_field {
        template <class U>
        requires (!std::is_lvalue_reference_v<U>)
        constexpr operator U&&() const noexcept;
        template <class U>
        constexpr operator U&() const noexcept;
    };

    template <class T, class... Args>
    concept brace_constructible = requires { T {Args {}...}; };

    template <class T, class... Args>
    consteval std::size_t count_fields() {
        if constexpr (brace_constructible<T, Args..., any_field>)
            return count_fields<T, Args..., any_field>();
        else
            return sizeof...(Args);
    }

} // namespace detail

template <Reflectable T>
inline constexpr std::size_t field_count_v =
    detail::count_fields<std::remove_cvref_t<T>>();

// Bind every member of `t` to a structured binding and forward them as a tuple
// of *references* (so callers can read and write through them). The ladder is
// generated for up to 32 members, which is far more than a sane component
// needs.
template <class T>
constexpr auto field_tie(T& t) noexcept {
    constexpr std::size_t N = field_count_v<std::remove_cvref_t<T>>;
    static_assert(N <= 32,
                  "ecs portable reflection supports up to 32 fields; "
                  "build with -DECS_USE_P2996=1 for unlimited fields");
    using std::tie;
    if constexpr (N == 0) {
        return tie();
    } else if constexpr (N == 1) {
        auto& [a] = t;
        return tie(a);
    } else if constexpr (N == 2) {
        auto& [a, b] = t;
        return tie(a, b);
    } else if constexpr (N == 3) {
        auto& [a, b, c] = t;
        return tie(a, b, c);
    } else if constexpr (N == 4) {
        auto& [a, b, c, d] = t;
        return tie(a, b, c, d);
    } else if constexpr (N == 5) {
        auto& [a, b, c, d, e] = t;
        return tie(a, b, c, d, e);
    } else if constexpr (N == 6) {
        auto& [a, b, c, d, e, f] = t;
        return tie(a, b, c, d, e, f);
    } else if constexpr (N == 7) {
        auto& [a, b, c, d, e, f, g] = t;
        return tie(a, b, c, d, e, f, g);
    } else if constexpr (N == 8) {
        auto& [a, b, c, d, e, f, g, h] = t;
        return tie(a, b, c, d, e, f, g, h);
    } else if constexpr (N == 9) {
        auto& [a, b, c, d, e, f, g, h, i] = t;
        return tie(a, b, c, d, e, f, g, h, i);
    } else if constexpr (N == 10) {
        auto& [a, b, c, d, e, f, g, h, i, j] = t;
        return tie(a, b, c, d, e, f, g, h, i, j);
    } else if constexpr (N == 11) {
        auto& [a, b, c, d, e, f, g, h, i, j, k] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k);
    } else if constexpr (N == 12) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l);
    } else if constexpr (N == 13) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m);
    } else if constexpr (N == 14) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n);
    } else if constexpr (N == 15) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o);
    } else if constexpr (N == 16) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p);
    } else {
        // 17..32: split into the first 16 plus the remainder.
        auto& [a,
               b,
               c,
               d,
               e,
               f,
               g,
               h,
               i,
               j,
               k,
               l,
               m,
               n,
               o,
               p,
               q,
               r,
               s,
               u,
               v,
               w,
               x,
               y,
               z,
               A,
               B,
               C,
               D,
               E,
               F,
               G] = t;
        if constexpr (N == 17)
            return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q);
        else if constexpr (N == 18)
            return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r);
        else if constexpr (N == 19)
            return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s);
        else if constexpr (N == 20)
            return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u);
        else if constexpr (N == 21)
            return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v);
        else if constexpr (N == 22)
            return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w);
        else if constexpr (N == 23)
            return tie(a,
                       b,
                       c,
                       d,
                       e,
                       f,
                       g,
                       h,
                       i,
                       j,
                       k,
                       l,
                       m,
                       n,
                       o,
                       p,
                       q,
                       r,
                       s,
                       u,
                       v,
                       w,
                       x);
        else if constexpr (N == 24)
            return tie(a,
                       b,
                       c,
                       d,
                       e,
                       f,
                       g,
                       h,
                       i,
                       j,
                       k,
                       l,
                       m,
                       n,
                       o,
                       p,
                       q,
                       r,
                       s,
                       u,
                       v,
                       w,
                       x,
                       y);
        else if constexpr (N == 25)
            return tie(a,
                       b,
                       c,
                       d,
                       e,
                       f,
                       g,
                       h,
                       i,
                       j,
                       k,
                       l,
                       m,
                       n,
                       o,
                       p,
                       q,
                       r,
                       s,
                       u,
                       v,
                       w,
                       x,
                       y,
                       z);
        else if constexpr (N == 26)
            return tie(a,
                       b,
                       c,
                       d,
                       e,
                       f,
                       g,
                       h,
                       i,
                       j,
                       k,
                       l,
                       m,
                       n,
                       o,
                       p,
                       q,
                       r,
                       s,
                       u,
                       v,
                       w,
                       x,
                       y,
                       z,
                       A);
        else if constexpr (N == 27)
            return tie(a,
                       b,
                       c,
                       d,
                       e,
                       f,
                       g,
                       h,
                       i,
                       j,
                       k,
                       l,
                       m,
                       n,
                       o,
                       p,
                       q,
                       r,
                       s,
                       u,
                       v,
                       w,
                       x,
                       y,
                       z,
                       A,
                       B);
        else if constexpr (N == 28)
            return tie(a,
                       b,
                       c,
                       d,
                       e,
                       f,
                       g,
                       h,
                       i,
                       j,
                       k,
                       l,
                       m,
                       n,
                       o,
                       p,
                       q,
                       r,
                       s,
                       u,
                       v,
                       w,
                       x,
                       y,
                       z,
                       A,
                       B,
                       C);
        else if constexpr (N == 29)
            return tie(a,
                       b,
                       c,
                       d,
                       e,
                       f,
                       g,
                       h,
                       i,
                       j,
                       k,
                       l,
                       m,
                       n,
                       o,
                       p,
                       q,
                       r,
                       s,
                       u,
                       v,
                       w,
                       x,
                       y,
                       z,
                       A,
                       B,
                       C,
                       D);
        else if constexpr (N == 30)
            return tie(a,
                       b,
                       c,
                       d,
                       e,
                       f,
                       g,
                       h,
                       i,
                       j,
                       k,
                       l,
                       m,
                       n,
                       o,
                       p,
                       q,
                       r,
                       s,
                       u,
                       v,
                       w,
                       x,
                       y,
                       z,
                       A,
                       B,
                       C,
                       D,
                       E);
        else if constexpr (N == 31)
            return tie(a,
                       b,
                       c,
                       d,
                       e,
                       f,
                       g,
                       h,
                       i,
                       j,
                       k,
                       l,
                       m,
                       n,
                       o,
                       p,
                       q,
                       r,
                       s,
                       u,
                       v,
                       w,
                       x,
                       y,
                       z,
                       A,
                       B,
                       C,
                       D,
                       E,
                       F);
        else
            return tie(a,
                       b,
                       c,
                       d,
                       e,
                       f,
                       g,
                       h,
                       i,
                       j,
                       k,
                       l,
                       m,
                       n,
                       o,
                       p,
                       q,
                       r,
                       s,
                       u,
                       v,
                       w,
                       x,
                       y,
                       z,
                       A,
                       B,
                       C,
                       D,
                       E,
                       F,
                       G);
    }
}

template <std::size_t I, class T>
constexpr decltype(auto) get_field(T&& obj) noexcept {
    return std::get<I>(field_tie(obj));
}

template <class T, std::size_t I>
using field_type_t = std::remove_cvref_t<
    std::tuple_element_t<I,
                         decltype(field_tie(std::declval<std::remove_cvref_t<T>&>()))>>;

// Portable reflection cannot recover member names; provide a stable synthetic
// one. The P2996 backend returns the real identifier.
template <class T, std::size_t I>
constexpr std::string_view field_name() {
    return "field";
}

} // namespace ecs::reflect

#endif // ECS_USE_P2996

// ===========================================================================
//  Backend-independent helpers.
// ===========================================================================
namespace ecs::reflect {

// Invoke f(std::integral_constant<size_t,I>{}, get_field<I>(obj)) for each
// member of obj, in declaration order.
template <class T, class F>
constexpr void for_each_field(T&& obj, F&& f) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (f(std::integral_constant<std::size_t, I> {}, get_field<I>(obj)), ...);
    }(std::make_index_sequence<field_count_v<std::remove_cvref_t<T>>> {});
}

} // namespace ecs::reflect
