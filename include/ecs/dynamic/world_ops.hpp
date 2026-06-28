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
        rec.alive      = true;
        rec.generation = e.generation;
        rec.archetype  = w.empty_archetype_;
        auto& a        = *w.archetypes_[w.empty_archetype_];
        rec.row        = static_cast<std::uint32_t>(a.entities.size());
        a.entities.push_back(e);
        ++w.alive_count_;
        return e;
    }

    static bool has(World const& w, Entity e, ComponentId id) {
        return w.alive(e) && w.archetypes_[w.records_[e.index].archetype]->has(id);
    }

    // Add a dynamic component (moving the entity to the matching archetype). If
    // it already has the component, the value is overwritten in place.
    static void add(World& w, Entity e, ComponentId id, void const* blob) {
        if (!w.alive(e))
            return;
        auto& rec = w.records_[e.index];
        if (w.archetypes_[rec.archetype]->has(id)) {
            col(*w.archetypes_[rec.archetype], id).scatter(rec.row, blob);
            return;
        }
        auto sig = w.archetypes_[rec.archetype]->signature;
        sig.insert(std::ranges::upper_bound(sig, id), id);

        auto const from  = rec.archetype;
        auto const& desc = registry().desc(id);
        auto const to    = w.get_or_create_archetype(sig, [&](Archetype& b) {
            for (auto& [cid, column] : w.archetypes_[from]->columns)
                b.columns.emplace(cid, column->clone_empty());
            b.columns.emplace(id, std::make_unique<DynamicColumn>(desc));
        });
        w.relocate(e, to, [&](Archetype& b) { col(b, id).push(blob); });
    }

    // Gather a component's fields into a packed blob. False if absent/dead.
    static bool get(World const& w, Entity e, ComponentId id, void* out) {
        if (!w.alive(e))
            return false;
        auto const& a = *w.archetypes_[w.records_[e.index].archetype];
        if (!a.has(id))
            return false;
        static_cast<DynamicColumn const&>(*a.columns.at(id))
            .gather(w.records_[e.index].row, out);
        return true;
    }

    // Overwrite a component in place. False if absent/dead.
    static bool set(World& w, Entity e, ComponentId id, void const* blob) {
        if (!w.alive(e))
            return false;
        auto& rec = w.records_[e.index];
        if (!w.archetypes_[rec.archetype]->has(id))
            return false;
        col(*w.archetypes_[rec.archetype], id).scatter(rec.row, blob);
        return true;
    }

    static void remove(World& w, Entity e, ComponentId id) {
        if (!w.alive(e))
            return;
        auto& rec = w.records_[e.index];
        if (!w.archetypes_[rec.archetype]->has(id))
            return;
        Signature sig;
        sig.reserve(w.archetypes_[rec.archetype]->signature.size() - 1);
        for (auto cid : w.archetypes_[rec.archetype]->signature)
            if (cid != id)
                sig.push_back(cid);
        auto const from = rec.archetype;
        auto const to   = w.get_or_create_archetype(sig, [&](Archetype& b) {
            for (auto& [cid, column] : w.archetypes_[from]->columns)
                if (cid != id)
                    b.columns.emplace(cid, column->clone_empty());
        });
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
        if (e.index >= w.records_.size())
            w.records_.resize(e.index + 1);
        Signature sig;
        sig.reserve(bundle.size());
        for (auto const& id : bundle | std::views::keys)
            sig.push_back(id);
        std::ranges::sort(sig);
        sig.erase(std::ranges::unique(sig).begin(), sig.end());
        auto const to  = w.get_or_create_archetype(sig, [&](Archetype& b) {
            for (auto const& id : bundle | std::views::keys)
                b.columns.emplace(id,
                                  std::make_unique<DynamicColumn>(registry().desc(id)));
        });
        auto& arch     = *w.archetypes_[to];
        auto& rec      = w.records_[e.index];
        rec.alive      = true;
        rec.generation = e.generation;
        rec.archetype  = to;
        rec.row        = static_cast<std::uint32_t>(arch.entities.size());
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

    // The first archetype that contains `id`, and its DynamicColumn -- the base
    // for a typed-array view. (Per-archetype iteration over all matches is
    // Phase B; for now the host arranges a single matching archetype.)
    static DynamicColumn* column(World& w, ComponentId id) {
        for (auto& arch : w.archetypes_)
            if (arch->has(id))
                return static_cast<DynamicColumn*>(arch->columns.at(id).get());
        return nullptr;
    }

private:
    static DynamicColumn& col(Archetype& a, ComponentId id) {
        return static_cast<DynamicColumn&>(*a.columns.at(id));
    }
};

} // namespace ecs::dynamic
