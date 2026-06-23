// ecs/reflection/reflect_common.hpp
//
// Shared foundation for the reflection backends.
//
// Both reflect_p2996.hpp and reflect_portable.hpp build on the same two pieces:
// the Reflectable concept (a component must be an aggregate) and the
// detail::unqualified type-name helper. They live here, in a small self-contained
// header that each backend includes, so a backend is a complete translation unit
// on its own -- analyzable in isolation -- rather than only becoming well-formed
// once reflect.hpp has textually composed those declarations ahead of it.
//
// reflect.hpp (the facade) selects and includes exactly one backend and adds the
// backend-independent for_each_field on top of the surface that backend defines.

#pragma once

#include <string_view>
#include <type_traits>

namespace ecs::reflect {

// A component must be an aggregate so that its members can be reflected and
// re-assembled by brace-init / splicing.
template <class T>
concept Reflectable = std::is_aggregate_v<std::remove_cvref_t<T>>;

namespace detail {
    // Strip the outer namespace qualifier from a (possibly templated) type name,
    // keeping any template arguments: "ns::Foo<ns::Bar>" -> "Foo<ns::Bar>". The
    // result is a sub-view of the input, so it shares the input's storage.
    constexpr std::string_view unqualified(std::string_view n) {
        auto const head = n.substr(0, n.find('<'));
        if (auto const p = head.rfind("::"); p != std::string_view::npos)
            n.remove_prefix(p + 2);
        return n;
    }
} // namespace detail

} // namespace ecs::reflect
