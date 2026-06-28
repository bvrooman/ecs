// ecs/dynamic/native.hpp
//
// Expose a *native* (compile-time) component type to the runtime/JS layer. A
// native component is stored as ecs::Column<T> with a reflection-derived SoA
// layout; register_native<T>() records a matching ComponentDesc under T's
// component_id<T> so a runtime query or JS system can read & write its columns
// by name -- through the same IColumn::field_base path dynamic components use.
//
// Field *types* come from reflection (field_type_t<T,I>); field *names* are
// supplied explicitly, because the portable reflection backend (the one used
// under Emscripten) carries no member identifiers. T's fields must be the
// supported scalar types.

#pragma once

#include "../entity.hpp" // component_id<T>
#include "../reflection/reflect.hpp"
#include "component_desc.hpp"
#include "registry.hpp"
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ecs::dynamic {

// Map a scalar C++ field type to its runtime FieldType tag.
template <class U>
constexpr FieldType to_field_type() {
    if constexpr (std::is_same_v<U, float>)
        return FieldType::f32;
    else if constexpr (std::is_same_v<U, double>)
        return FieldType::f64;
    else if constexpr (std::is_same_v<U, std::int32_t> || std::is_same_v<U, int>)
        return FieldType::i32;
    else if constexpr (std::is_same_v<U, std::uint32_t> || std::is_same_v<U, unsigned>)
        return FieldType::u32;
    else
        static_assert(sizeof(U) == 0,
                      "register_native: unsupported field type (fields must be "
                      "float/double/int32/uint32 scalars)");
}

// Record native component T's schema (field types via reflection, names supplied)
// under component_id<T>. Returns that id. field_names.size() must equal T's field
// count. Call once during setup, before a JS system queries T.
template <reflect::Reflectable T>
ComponentId register_native(std::string name, std::vector<std::string> field_names) {
    constexpr auto N = reflect::field_count_v<T>;
    std::vector<std::pair<std::string, FieldType>> fields;
    fields.reserve(N);
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (fields.emplace_back(std::move(field_names[I]),
                             to_field_type<reflect::field_type_t<T, I>>()),
         ...);
    }(std::make_index_sequence<N> {});
    registry().define_with_id(component_id<T>, std::move(name), fields);
    return component_id<T>;
}

} // namespace ecs::dynamic
