// ecs/reflection/reflect_portable.hpp
//
// Reflection backend B: portable (aggregate brace-arity + structured bindings).
//
// Implements the ecs::reflect facade surface (field_count_v, get_field,
// field_type_t, field_name, type_name) without any compiler reflection support:
// the field count comes from an aggregate brace-arity probe and members are
// bound with structured bindings (a ladder generated up to 32 fields). It
// compiles on stock C++20/23 (e.g. Clang 18, GCC 13) so the engine builds, runs,
// and is tested today. Limitations vs P2996: field_name has no real identifier
// (returns a synthetic "field") and a nested aggregate counts as a single field.
//
// This is a self-contained header (it includes the shared base and the
// standard headers it uses); reflect.hpp is what selects it, including it only
// when ECS_USE_P2996 == 0.

#pragma once

#include <ecs/reflection/reflect_common.hpp> // Reflectable, detail::unqualified

#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

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

    // Count of top-level MEMBERS, probed with one braced initializer `{}` per
    // member. Unlike the flat any_field probe above, a braced initializer
    // cannot elide into a C-array member's elements, so for
    //   struct S { float v[3]; };
    // this counts 1 where count_fields() counts 3. The two disagreeing is how
    // an unsupported layout is detected (see the static_assert in field_tie)
    // instead of exploding inside the structured-binding ladder.
    // clang-format off
    template <class T>
    consteval std::size_t count_members_braced() {
        if      constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 32;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 31;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 30;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 29;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 28;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 27;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 26;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 25;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 24;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 23;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 22;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 21;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 20;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 19;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 18;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 17;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 16;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 15;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 14;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 13;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{},{}}; }) return 12;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{},{}}; }) return 11;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{},{}}; }) return 10;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{},{}}; }) return 9;
        else if constexpr (requires { T {{},{},{},{},{},{},{},{}}; }) return 8;
        else if constexpr (requires { T {{},{},{},{},{},{},{}}; }) return 7;
        else if constexpr (requires { T {{},{},{},{},{},{}}; }) return 6;
        else if constexpr (requires { T {{},{},{},{},{}}; }) return 5;
        else if constexpr (requires { T {{},{},{},{}}; }) return 4;
        else if constexpr (requires { T {{},{},{}}; }) return 3;
        else if constexpr (requires { T {{},{}}; }) return 2;
        else if constexpr (requires { T {{}}; }) return 1;
        else return 0;
    }
    // clang-format on

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
    // The flat arity probe counts a C-array member once per ELEMENT (brace
    // elision), while a structured binding binds it as ONE name -- the ladder
    // below would then fail with an inscrutable arity error. Catch the
    // disagreement here with a named diagnostic instead.
    static_assert(N == detail::count_members_braced<std::remove_cvref_t<T>>(),
                  "ecs portable reflection cannot decompose this struct: it has "
                  "a C-array member (wrap it in std::array or a nested struct) "
                  "or a member that is not default-constructible; alternatively "
                  "build with -DECS_USE_P2996=1");
    using std::tie;
    // clang-format off
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
    } else if constexpr (N == 17) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q);
    } else if constexpr (N == 18) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r);
    } else if constexpr (N == 19) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s);
    } else if constexpr (N == 20) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u);
    } else if constexpr (N == 21) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v);
    } else if constexpr (N == 22) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w);
    } else if constexpr (N == 23) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x);
    } else if constexpr (N == 24) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x, y] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x, y);
    } else if constexpr (N == 25) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x, y, z] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x, y, z);
    } else if constexpr (N == 26) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x, y, z, A] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x, y, z, A);
    } else if constexpr (N == 27) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x, y, z, A, B] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x, y, z, A, B);
    } else if constexpr (N == 28) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x, y, z, A, B, C] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x, y, z, A, B, C);
    } else if constexpr (N == 29) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x, y, z, A, B, C, D] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x, y, z, A, B, C, D);
    } else if constexpr (N == 30) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x, y, z, A, B, C, D, E] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x, y, z, A, B, C, D, E);
    } else if constexpr (N == 31) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x, y, z, A, B, C, D, E, F] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x, y, z, A, B, C, D, E, F);
    } else { // N == 32
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x, y, z, A, B, C, D, E, F, G] = t;
        return tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, u, v, w, x, y, z, A, B, C, D, E, F, G);
    }
    // clang-format on
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
// one, unique per index ("field0", "field1", ...) so consumers keying on names
// (the dynamic registry, tooling) do not collide. The P2996 backend returns
// the real identifier.
namespace detail {
    template <std::size_t I>
    struct synthetic_field_name {
        static constexpr auto storage = [] {
            static_assert(I < 100);
            std::array<char, 8> buf {'f', 'i', 'e', 'l', 'd'};
            std::size_t pos = 5;
            if constexpr (I >= 10)
                buf[pos++] = char('0' + I / 10);
            buf[pos++] = char('0' + I % 10);
            return buf;
        }();
        static constexpr std::size_t length = I >= 10 ? 7 : 6;
    };
} // namespace detail

template <class T, std::size_t I>
constexpr std::string_view field_name() {
    return {detail::synthetic_field_name<I>::storage.data(),
            detail::synthetic_field_name<I>::length};
}

// Unqualified name of the type T itself, parsed from the compiler's pretty
// function signature (GCC/Clang). The view points into static storage. The
// P2996 backend returns the real identifier instead.
template <class T>
constexpr std::string_view type_name() {
#if defined(__clang__) || defined(__GNUC__)
    std::string_view sig = __PRETTY_FUNCTION__;
    auto b               = sig.find("T = ");
    if (b == std::string_view::npos)
        return "?";
    b += 4;
    auto const e = sig.find_first_of(";]", b);
    return detail::unqualified(sig.substr(b, e - b));
#else
    return "?";
#endif
}

} // namespace ecs::reflect
