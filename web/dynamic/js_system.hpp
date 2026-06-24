// web/dynamic/js_system.hpp
//
// Shared helper for registering a JavaScript-defined system into a Schedule.
// Both the standalone dynamic module (bindings.cpp) and the particle demo
// (particles/main_web.cpp) use this so a JS system is wired identically: declare
// component access { write?: id[], read?: id[] }, and a kernel(count, views)
// that runs once per matching archetype, where views[component][field] is a
// zero-copy typed array over that field's SoA storage (native or dynamic) --
// write-through mutates the world. Runs on the main thread.

#pragma once

#include "ecs/dynamic/registry.hpp"
#include "ecs/schedule.hpp"
#include "ecs/world.hpp"

#include <emscripten/val.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <vector>

namespace ecs::dynamic::web {

// A zero-copy JS typed array aliasing `count` elements of type `t` at `base`.
inline emscripten::val make_view(FieldType t, void* base, std::size_t count) {
    using emscripten::typed_memory_view;
    using emscripten::val;
    switch (t) {
    case FieldType::f32:
        return val(typed_memory_view(count, reinterpret_cast<float*>(base)));
    case FieldType::f64:
        return val(typed_memory_view(count, reinterpret_cast<double*>(base)));
    case FieldType::i32:
        return val(typed_memory_view(count, reinterpret_cast<std::int32_t*>(base)));
    case FieldType::u32:
        return val(typed_memory_view(count, reinterpret_cast<std::uint32_t*>(base)));
    }
    return val::null();
}

// Register a JS system into `schedule`. `spec` is { write?: id[], read?: id[] };
// `kernel` is a JS function (count, views). Returns the system id.
inline SystemId add_js_system(Schedule& schedule,
                              std::string name,
                              emscripten::val spec,
                              emscripten::val kernel) {
    using emscripten::val;
    SystemAccess access;
    std::vector<ComponentId> query;
    auto collect = [&](char const* key, std::vector<std::uint32_t>& into) {
        val arr = spec[key];
        if (arr.isUndefined() || arr.isNull())
            return;
        unsigned const n = arr["length"].as<unsigned>();
        for (unsigned i = 0; i < n; ++i) {
            auto const id = static_cast<ComponentId>(arr[i].as<unsigned>());
            into.push_back(id);
            query.push_back(id);
        }
    };
    collect("write", access.writes);
    collect("read", access.reads);
    std::ranges::sort(query);
    query.erase(std::ranges::unique(query).begin(), query.end()); // sorted+unique -> a Signature

    auto run = [kernel, query](World& w, Commands&, WorkerPool*) {
        Signature const required(query.begin(), query.end());
        for (auto const ai : w.matching_archetypes(required)) {
            auto& arch       = *w.archetypes()[ai];
            auto const count = arch.size();
            if (count == 0)
                continue;
            val views = val::object();
            for (auto const cid : query) {
                auto const& d = registry().desc(cid);
                auto& col     = *arch.columns.at(cid); // IColumn: native or dynamic
                val fields    = val::object();
                for (std::size_t fi = 0; fi < d.fields.size(); ++fi)
                    fields.set(d.fields[fi].name,
                               make_view(d.fields[fi].type, col.field_base(fi), count));
                views.set(d.name, fields);
            }
            kernel(static_cast<unsigned>(count), views);
        }
    };
    return schedule.add_dynamic(std::move(name), std::move(access), std::move(run));
}

} // namespace ecs::dynamic::web
