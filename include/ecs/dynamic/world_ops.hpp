// ecs/dynamic/world_ops.hpp
//
// Runtime (host-driven) component operations over a World. WorldOps is a friend
// of World so it can reach the *generic* archetype machinery the compile-time
// path also uses -- get_or_create_archetype / relocate / destroy_now / the
// record table -- and add only a runtime column factory (DynamicColumn) plus raw
// byte row access. It introduces no new archetype logic.
//
// These run immediately and single-threaded, like World::emplace_resource: the
// intent is host-side setup/scripting *outside* a running Schedule. (Routing
// them through the deferred Commands buffer so JS systems can mutate mid-tick is
// Phase B.)

#pragma once

#include "../world.hpp"
#include "dynamic_column.hpp"
#include "registry.hpp"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ecs::dynamic {

struct WorldOps {
    // Create a live entity with no components (in the empty archetype).
    static Entity create_entity(World& w) {
        Entity const e = w.reserve();
        if (e.index >= w.records_.size())
            w.records_.resize(e.index + 1);
        auto& rec      = w.records_[e.index];
        rec.generation = e.generation;
        rec.set_archetype(w.empty_archetype_);
        rec.set_alive(true);
        auto& a = *w.archetypes_[w.empty_archetype_];
        rec.row = static_cast<std::uint32_t>(a.entities.size());
        a.entities.push_back(e);
        ++w.alive_count_;
        return e;
    }

    static bool has(World const& w, Entity e, ComponentId id) {
        return w.alive(e) && w.archetypes_[w.records_[e.index].archetype()]->has(id);
    }

    // Add a dynamic component (moving the entity to the matching archetype). If
    // it already has the component, the value is overwritten in place.
    static void add(World& w, Entity e, ComponentId id, void const* blob) {
        require_dynamic(id, "add");
        if (!w.alive(e))
            return;
        auto& rec = w.records_[e.index];
        auto& a   = *w.archetypes_[rec.archetype()];
        if (a.has(id)) {
            col(a, id).scatter(rec.row, blob);
            return;
        }
        ArchetypeId to;
        if (auto const it = a.add_edge.find(id); it != a.add_edge.end()) {
            to = it->second;
        } else {
            auto const from  = rec.archetype();
            auto const& desc = registry().desc(id);
            to = w.get_or_create_archetype(a.signature.with(id), [&](Archetype& b) {
                auto& src = *w.archetypes_[from];
                b.columns.reserve(b.signature.size());
                for (auto const cid : b.signature)
                    b.columns.push(cid == id
                                       ? std::make_unique<DynamicColumn>(desc)
                                       : src.column_at(cid).clone_empty());
            });
            a.add_edge.emplace(id, to);
        }
        w.relocate(e, to, [&](Archetype& b) { col(b, id).push(blob); });
    }

    // Gather a component's fields into a packed blob. False if absent/dead.
    static bool get(World const& w, Entity e, ComponentId id, void* out) {
        require_dynamic(id, "get");
        if (!w.alive(e))
            return false;
        auto const& a = *w.archetypes_[w.records_[e.index].archetype()];
        if (!a.has(id))
            return false;
        static_cast<DynamicColumn const&>(a.column_at(id))
            .gather(w.records_[e.index].row, out);
        return true;
    }

    // Overwrite a component in place. False if absent/dead.
    static bool set(World& w, Entity e, ComponentId id, void const* blob) {
        require_dynamic(id, "set");
        if (!w.alive(e))
            return false;
        auto& rec = w.records_[e.index];
        auto& a   = *w.archetypes_[rec.archetype()];
        if (!a.has(id))
            return false;
        col(a, id).scatter(rec.row, blob);
        return true;
    }

    static void remove(World& w, Entity e, ComponentId id) {
        if (!w.alive(e))
            return;
        auto& rec = w.records_[e.index];
        auto& a   = *w.archetypes_[rec.archetype()];
        if (!a.has(id))
            return;
        ArchetypeId to;
        if (auto const it = a.remove_edge.find(id); it != a.remove_edge.end()) {
            to = it->second;
        } else {
            auto const from = rec.archetype();
            to = w.get_or_create_archetype(a.signature.without(id), [&](Archetype& b) {
                auto& src = *w.archetypes_[from];
                b.columns.reserve(b.signature.size());
                for (auto const cid : b.signature)
                    b.columns.push(src.column_at(cid).clone_empty());
            });
            a.remove_edge.emplace(id, to);
        }
        w.relocate(e, to, [](Archetype&) {});
    }

    static void destroy(World& w, Entity e) { w.destroy_now(e); }

    // --- deferred (in-system) structural mutation -------------------------
    // The immediate ops above are unsafe mid-tick (they relocate storage while a
    // system iterates). These instead RECORD into the world's command buffer --
    // the same one native Commands use -- so they apply at the next Schedule
    // flush (wave barrier). A JS emit/reap kernel reaches them through the
    // binding (DynamicWorld.spawn/destroy).

    // A packed component bundle: (id, AoS bytes) per component, as a JS emitter
    // hands it over.
    using Bundle = std::vector<std::pair<ComponentId, std::vector<std::byte>>>;

    // Place a reserved entity directly into the archetype matching `bundle`'s
    // component set (one transition, not one-per-component) and scatter each
    // component's bytes. Runs at flush, when nothing is iterating.
    static void build_now(World& w, Entity e, Bundle const& bundle) {
        for (auto const& id : bundle | std::views::keys)
            require_dynamic(id, "spawn");
        if (e.index >= w.records_.size())
            w.records_.resize(e.index + 1);
        Signature const sig(bundle | std::views::keys); // ctor sorts + dedups
        auto const to = w.get_or_create_archetype(sig, [&](Archetype& b) {
            b.columns.reserve(b.signature.size());
            for (auto const id : b.signature)
                b.columns.push(
                    std::make_unique<DynamicColumn>(registry().desc(id)));
        });
        auto& arch     = *w.archetypes_[to];
        auto& rec      = w.records_[e.index];
        rec.generation = e.generation;
        rec.set_archetype(to);
        rec.set_alive(true);
        rec.row = static_cast<std::uint32_t>(arch.entities.size());
        arch.entities.push_back(e);
        for (auto const& [id, bytes] : bundle)
            col(arch, id).push(bytes.data());
        ++w.alive_count_;
    }

    // Reserve a handle now, build the entity at the next flush (like
    // Commands::spawn). The returned handle is alive after that flush.
    static Entity spawn_deferred(World& w, Bundle bundle) {
        Entity const e = w.reserve();
        w.commands_.record([e, bundle = std::move(bundle)](World& world) {
            build_now(world, e, bundle);
        });
        return e;
    }

    // Destroy at the next flush (a no-op if the handle is already stale).
    static void destroy_deferred(World& w, Entity e) {
        w.commands_.record([e](World& world) {
            if (world.alive(e))
                world.destroy_now(e);
        });
    }

    // Mutable access to one archetype, for host-side (JS) view construction over
    // matching_archetypes() indices. World's mutable archetype accessor is
    // private -- the dynamic layer reaches it via friendship, keeping the escape
    // hatch scoped to this module rather than the public World API.
    static Archetype& archetype_at(World& w, ArchetypeId i) {
        return *w.archetypes_[i];
    }

    // The first archetype that contains `id`, and its DynamicColumn -- the base
    // for a typed-array view. (Per-archetype iteration over all matches is
    // Phase B; for now the host arranges a single matching archetype.)
    static DynamicColumn* column(World& w, ComponentId id) {
        require_dynamic(id, "column");
        for (auto& arch : w.archetypes_)
            if (auto* c = arch->column_for(id))
                return static_cast<DynamicColumn*>(c);
        return nullptr;
    }

private:
    // The blob paths downcast columns to DynamicColumn. For a native id --
    // register_native describes a Column<T>, and hosts legitimately hold those
    // ids for views -- that downcast would be UB, so refuse loudly instead.
    static void require_dynamic(ComponentId id, char const* op) {
        if (!registry().is_dynamic(id))
            throw std::invalid_argument(
                std::string("ecs::dynamic::WorldOps::") + op +
                ": component id is not a dynamic component (a native component "
                "must be mutated through the C++ API)");
    }

    static DynamicColumn& col(Archetype& a, ComponentId id) {
        return static_cast<DynamicColumn&>(a.column_at(id));
    }
};

} // namespace ecs::dynamic
