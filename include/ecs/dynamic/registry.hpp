// ecs/dynamic/registry.hpp
//
// Process-wide registry of runtime (JS-defined) component schemas. It hands out
// ids from the *same* counter as the compile-time component_id<T> (so static and
// dynamic component ids never collide and can share archetypes later), and owns
// the ComponentDesc each DynamicColumn refers to by stable pointer.

#pragma once

#include "../entity.hpp" // ComponentId, detail::next_component_id
#include "component_desc.hpp"
#include <unordered_map>
#include <utility>

namespace ecs::dynamic {

class Registry {
public:
    // Register a component schema and return its fresh id. Field offsets/stride
    // are computed by packing the fields in declaration order.
    ComponentId define(std::string name,
                       std::vector<std::pair<std::string, FieldType>> const& fields) {
        ComponentDesc d;
        d.id   = ecs::detail::next_component_id();
        d.name = std::move(name);
        std::size_t off = 0;
        for (auto const& [fname, ftype] : fields) {
            auto const sz = field_size(ftype);
            d.fields.push_back(Field {fname, ftype, sz, off});
            off += sz;
        }
        d.stride = off;
        return descs_.emplace(d.id, std::move(d)).first->first;
    }

    [[nodiscard]]
    bool is_dynamic(ComponentId id) const {
        return descs_.contains(id);
    }
    [[nodiscard]]
    ComponentDesc const& desc(ComponentId id) const {
        return descs_.at(id);
    }

private:
    // Node-based, so ComponentDesc addresses stay stable as new ones are added
    // (DynamicColumn holds a ComponentDesc const*).
    std::unordered_map<ComponentId, ComponentDesc> descs_;
};

// The shared registry. Global to match the global nature of the component-id
// counter; component ids are already process-wide identity.
inline Registry& registry() {
    static Registry r;
    return r;
}

} // namespace ecs::dynamic
