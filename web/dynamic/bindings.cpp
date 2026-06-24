// web/dynamic/bindings.cpp
//
// Phase A interop spine: an embind facade exposing the runtime component path to
// JavaScript. JS defines components (schemas), creates entities, and adds / gets
// / sets / removes components by id -- all backed by the same World + archetype
// machinery the C++ engine uses, via ecs::dynamic::WorldOps. Component values
// cross the boundary as plain JS number arrays (one per field, declaration
// order); fieldView() hands back a zero-copy typed-array view aliasing a
// component's SoA field buffer -- the Phase B per-chunk kernel foundation.

#include "ecs/dynamic/dynamic_column.hpp"
#include "ecs/dynamic/registry.hpp"
#include "ecs/dynamic/world_ops.hpp"
#include "ecs/world.hpp"

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ecs;
using namespace ecs::dynamic;
using emscripten::val;

namespace {

FieldType parse_type(std::string const& s) {
    if (s == "f32")
        return FieldType::f32;
    if (s == "f64")
        return FieldType::f64;
    if (s == "i32")
        return FieldType::i32;
    if (s == "u32")
        return FieldType::u32;
    throw std::runtime_error("unknown field type: " + s);
}

void write_scalar(std::byte* dst, FieldType t, double v) {
    switch (t) {
    case FieldType::f32: { float f = static_cast<float>(v); std::memcpy(dst, &f, 4); break; }
    case FieldType::f64: { std::memcpy(dst, &v, 8); break; }
    case FieldType::i32: { std::int32_t i = static_cast<std::int32_t>(v); std::memcpy(dst, &i, 4); break; }
    case FieldType::u32: { std::uint32_t u = static_cast<std::uint32_t>(v); std::memcpy(dst, &u, 4); break; }
    }
}

double read_scalar(std::byte const* src, FieldType t) {
    switch (t) {
    case FieldType::f32: { float f; std::memcpy(&f, src, 4); return f; }
    case FieldType::f64: { double d; std::memcpy(&d, src, 8); return d; }
    case FieldType::i32: { std::int32_t i; std::memcpy(&i, src, 4); return i; }
    case FieldType::u32: { std::uint32_t u; std::memcpy(&u, src, 4); return u; }
    }
    return 0;
}

} // namespace

class DynamicWorld {
public:
    // fields: JS array of { name: string, type: 'f32'|'f64'|'i32'|'u32' }.
    int defineComponent(std::string name, val fields) {
        std::vector<std::pair<std::string, FieldType>> fs;
        unsigned const n = fields["length"].as<unsigned>();
        for (unsigned i = 0; i < n; ++i) {
            val f = fields[i];
            fs.emplace_back(f["name"].as<std::string>(), parse_type(f["type"].as<std::string>()));
        }
        return static_cast<int>(registry().define(std::move(name), fs));
    }

    val createEntity() { return to_val(WorldOps::create_entity(world_)); }
    void destroyEntity(val e) { WorldOps::destroy(world_, from_val(e)); }
    bool hasComponent(val e, int id) {
        return WorldOps::has(world_, from_val(e), static_cast<ComponentId>(id));
    }

    void addComponent(val e, int id, val values) {
        auto const blob = pack(static_cast<ComponentId>(id), values);
        WorldOps::add(world_, from_val(e), static_cast<ComponentId>(id), blob.data());
    }
    void setComponent(val e, int id, val values) {
        auto const blob = pack(static_cast<ComponentId>(id), values);
        WorldOps::set(world_, from_val(e), static_cast<ComponentId>(id), blob.data());
    }
    void removeComponent(val e, int id) {
        WorldOps::remove(world_, from_val(e), static_cast<ComponentId>(id));
    }

    // -> JS array of field values, or null if the entity lacks the component.
    val getComponent(val e, int id) {
        auto const& d = registry().desc(static_cast<ComponentId>(id));
        std::vector<std::byte> blob(d.stride);
        if (!WorldOps::get(world_, from_val(e), static_cast<ComponentId>(id), blob.data()))
            return val::null();
        val out = val::array();
        for (std::size_t i = 0; i < d.fields.size(); ++i)
            out.set(i, read_scalar(blob.data() + d.fields[i].offset, d.fields[i].type));
        return out;
    }

    int entityCount() { return static_cast<int>(world_.size()); }

    // Zero-copy typed-array view over field `fieldIndex` of component `id`,
    // across the first archetype that has it (Phase A: arrange one such
    // archetype). NOTE: the view is invalidated by any heap growth or structural
    // edit -- re-acquire it; never cache across those.
    val fieldView(int id, int fieldIndex) {
        auto* column = WorldOps::column(world_, static_cast<ComponentId>(id));
        if (!column)
            return val::null();
        auto const count = column->size();
        void* base       = column->field_base(static_cast<std::size_t>(fieldIndex));
        switch (column->desc().fields[fieldIndex].type) {
        case FieldType::f32:
            return val(emscripten::typed_memory_view(count, reinterpret_cast<float*>(base)));
        case FieldType::f64:
            return val(emscripten::typed_memory_view(count, reinterpret_cast<double*>(base)));
        case FieldType::i32:
            return val(emscripten::typed_memory_view(count, reinterpret_cast<std::int32_t*>(base)));
        case FieldType::u32:
            return val(emscripten::typed_memory_view(count, reinterpret_cast<std::uint32_t*>(base)));
        }
        return val::null();
    }

private:
    std::vector<std::byte> pack(ComponentId id, val values) {
        auto const& d = registry().desc(id);
        std::vector<std::byte> blob(d.stride);
        for (std::size_t i = 0; i < d.fields.size(); ++i)
            write_scalar(blob.data() + d.fields[i].offset, d.fields[i].type,
                         values[i].as<double>());
        return blob;
    }

    static val to_val(Entity e) {
        val o = val::object();
        o.set("index", static_cast<unsigned>(e.index));
        o.set("generation", static_cast<unsigned>(e.generation));
        return o;
    }
    static Entity from_val(val e) {
        return Entity {e["index"].as<unsigned>(), e["generation"].as<unsigned>()};
    }

    World world_;
};

EMSCRIPTEN_BINDINGS(ecs_dynamic) {
    using namespace emscripten;
    class_<DynamicWorld>("DynamicWorld")
        .constructor<>()
        .function("defineComponent", &DynamicWorld::defineComponent)
        .function("createEntity", &DynamicWorld::createEntity)
        .function("destroyEntity", &DynamicWorld::destroyEntity)
        .function("hasComponent", &DynamicWorld::hasComponent)
        .function("addComponent", &DynamicWorld::addComponent)
        .function("setComponent", &DynamicWorld::setComponent)
        .function("removeComponent", &DynamicWorld::removeComponent)
        .function("getComponent", &DynamicWorld::getComponent)
        .function("entityCount", &DynamicWorld::entityCount)
        .function("fieldView", &DynamicWorld::fieldView);
}
