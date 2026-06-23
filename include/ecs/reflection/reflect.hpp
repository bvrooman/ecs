// ecs/reflection/reflect.hpp
//
// Compile-time field reflection facade.
//
// The whole library is written against the small surface defined in
// `namespace ecs::reflect`:
//
//   reflect::field_count_v<T>      -> number of non-static data members of T
//   reflect::get_field<I>(obj)     -> reference to the I-th data member
//   reflect::field_type_t<T, I>    -> type of the I-th data member
//   reflect::field_name<T, I>()    -> name of the I-th data member (best effort)
//   reflect::type_name<T>()        -> unqualified name of T itself
//   reflect::for_each_field(obj,f) -> invoke f(index, member&) for every member
//
// That surface has two interchangeable implementations ("backends"), each in its
// own header and selected below by ECS_USE_P2996:
//
//   * reflect_p2996.hpp    -- the *real* C++26 static reflection (P2996
//     `std::meta`, splicers `[:r:]`, `^^T`). The canonical "use C++26
//     reflection" answer; needs a toolchain with P2996 such as GCC 16 (g++-16,
//     which ships libstdc++).
//
//   * reflect_portable.hpp -- a portable backend (aggregate brace-arity probe +
//     structured bindings) that compiles on stock C++20/23 compilers (e.g.
//     Clang 18, GCC 13), so the engine and its tests build and run *today*.
//
// The foundation both backends rely on -- the Reflectable concept and the
// detail::unqualified helper -- lives in reflect_common.hpp, which each backend
// includes, so a backend is a complete, self-contained translation unit. This
// header is the facade: it selects one backend and, on top of the surface that
// backend defines, adds the backend-independent for_each_field. Because every
// higher layer depends only on this facade, switching backends is a single flag
// and changes nothing else.

#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

#ifndef ECS_USE_P2996
#if defined(__cpp_reflection) && __has_include(<meta>)
#define ECS_USE_P2996 1
#else
#define ECS_USE_P2996 0
#endif
#endif

// ===========================================================================
//  Backend selection. Each backend includes the shared base
//  (reflect_common.hpp) and implements the same facade surface -- field_count_v,
//  get_field, field_type_t, field_name, type_name -- into namespace ecs::reflect.
// ===========================================================================
#if ECS_USE_P2996
#include "reflect_p2996.hpp"
#else
#include "reflect_portable.hpp"
#endif

// ===========================================================================
//  Epilogue: backend-independent helpers, built on the selected backend's
//  surface (so they are defined after it is included).
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
