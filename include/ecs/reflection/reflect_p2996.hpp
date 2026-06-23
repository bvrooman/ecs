// ecs/reflection/reflect_p2996.hpp
//
// Reflection backend A: real C++26 static reflection (P2996).
//
// Implements the ecs::reflect facade surface (field_count_v, get_field,
// field_type_t, field_name, type_name) with std::meta -- the reflection
// operator ^^T, nonstatic_data_members_of, member splicers [:m:], and
// identifier_of. This is the canonical "use C++26 reflection" path and, unlike
// the portable backend, recovers real member *names*. It needs a toolchain with
// P2996 support such as GCC 16 (g++-16, which ships libstdc++).
//
// This is a self-contained header (it includes the shared base and the
// standard headers it uses); reflect.hpp is what selects it, including it only
// when ECS_USE_P2996 == 1.

#pragma once

#include "reflect_common.hpp" // Reflectable, detail::unqualified
#include <cstddef>
#include <meta>
#include <string_view>
#include <type_traits>
#include <utility>

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

// Unqualified name of the type T itself (not a member). The returned view is
// backed by persistent storage, so it is valid for the program lifetime.
// dealias() unwraps the remove_cvref_t alias to the underlying entity; types
// without a plain identifier (e.g. template specializations) have none, so we
// fall back to the display name there.
template <class T>
consteval std::string_view type_name() {
    auto const r = std::meta::dealias(^^std::remove_cvref_t<T>);
    if (std::meta::has_identifier(r))
        return std::meta::identifier_of(r);
    return detail::unqualified(std::meta::display_string_of(r));
}

} // namespace ecs::reflect
