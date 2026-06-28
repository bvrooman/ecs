// ecs/dynamic/component_desc.hpp
//
// A ComponentDesc is the runtime analogue of a reflected C++ component struct:
// it describes a JS-defined component's fields (name, scalar type, and byte
// offset within a packed "array of structures" interchange blob). The blob is
// how a host (the JS binding) hands a component's values across the boundary;
// internally DynamicColumn stores those fields structure-of-arrays.

#pragma once

#include "../entity.hpp" // ComponentId
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ecs::dynamic {

// The scalar field types a JS-defined component may use. (Kept to the common
// numeric set for now; widening it is purely additive -- a new enumerator plus
// its size and a marshalling case in the binding.)
enum class FieldType : std::uint8_t { f32, f64, i32, u32 };

constexpr std::size_t field_size(FieldType t) noexcept {
    switch (t) {
    case FieldType::f32: return 4;
    case FieldType::f64: return 8;
    case FieldType::i32: return 4;
    case FieldType::u32: return 4;
    }
    return 0;
}

struct Field {
    std::string name;
    FieldType type;
    std::size_t size;   // element size in bytes (== field_size(type))
    std::size_t offset; // byte offset within the packed AoS interchange blob
};

struct ComponentDesc {
    ComponentId id = 0;
    std::string name;
    std::vector<Field> fields;
    std::size_t stride = 0; // packed blob size == sum of field sizes
};

} // namespace ecs::dynamic
