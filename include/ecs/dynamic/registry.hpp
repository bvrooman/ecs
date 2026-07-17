// ecs/dynamic/registry.hpp
//
// Process-wide registry of runtime (JS-defined) component schemas. It hands out
// ids from the *same* counter as the compile-time component_id<T> (so static and
// dynamic component ids never collide and can share archetypes later), and owns
// the ComponentDesc each DynamicColumn refers to by stable pointer.

#pragma once

#include "../entity.hpp" // ComponentId, detail::next_component_id
#include "component_desc.hpp"
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace ecs::dynamic {

class Registry {
public:
    // Register a component schema under a fresh id (a JS-defined component).
    ComponentId define(std::string name,
                       std::vector<std::pair<std::string, FieldType>> const& fields) {
        return define_with_id(ecs::detail::next_component_id(), std::move(name), fields);
    }

    // Register a schema under a *specific* id -- used to describe a native
    // component type T under its component_id<T> (see native.hpp), so the
    // runtime/JS layer can view native columns by name. Offsets/stride are
    // computed by packing the fields in declaration order.
    //
    // Redefinition: registering the SAME schema again is an idempotent no-op
    // (setup code may run twice); registering a DIFFERENT schema under a live
    // id throws -- every existing DynamicColumn holds a pointer to the current
    // desc and sized its field buffers from it, so mutating it in place would
    // corrupt them all.
    ComponentId define_with_id(ComponentId id, std::string name,
                               std::vector<std::pair<std::string, FieldType>> const& fields,
                               StorageKind kind = StorageKind::dynamic_column) {
        ComponentDesc d;
        d.id   = id;
        d.name = std::move(name);
        d.kind = kind;
        std::size_t off = 0;
        for (auto const& [fname, ftype] : fields) {
            auto const sz = field_size(ftype);
            d.fields.push_back(Field {fname, ftype, sz, off});
            off += sz;
        }
        d.stride = off;
        if (auto const it = descs_.find(id); it != descs_.end()) {
            auto const& old = it->second;
            if (old.name == d.name && old.fields == d.fields && old.kind == d.kind)
                return id; // identical re-registration: no-op
            throw std::logic_error("ecs::dynamic: component id already defined "
                                   "with a different schema (\"" +
                                   old.name + "\")");
        }
        descs_.emplace(id, std::move(d));
        return id;
    }

    // True when `id` is described AND stored dynamically (a DynamicColumn). A
    // register_native'd component is described but NOT dynamic -- the blob
    // mutation paths must refuse it (see WorldOps).
    [[nodiscard]]
    bool is_dynamic(ComponentId id) const {
        auto const it = descs_.find(id);
        return it != descs_.end() && it->second.kind == StorageKind::dynamic_column;
    }
    // True when `id` has a schema at all (dynamic or described-native) -- what
    // the view-building paths need.
    [[nodiscard]]
    bool is_described(ComponentId id) const {
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
