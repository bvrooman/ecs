// ecs/world/commands.hpp
//
// Commands: the deferred mutation API handed to every system. Split out of
// world.hpp so the World class and the API used to mutate it are separate
// files; include ecs/world.hpp to get both.

#pragma once

#include <ecs/command_buffer.hpp>
#include <ecs/entity.hpp>
#include <ecs/world/world.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace ecs {

namespace detail {
    struct command_slot; // schedule/params/commands.hpp
} // namespace detail

// ===========================================================================
//  Commands: the mutation API, handed to systems by the Schedule, so
//  spawn/destroy/add/remove/set are reachable only from inside a system (or a
//  schedule's setup). All of them are deferred -- recorded, then applied at the
//  next flush. A typed system's Commands& records into a private store its
//  parameter policy owns and the wave drains in canonical order; a
//  dynamic system gets the Schedule's shared one, which records into the
//  world's thread-sharded buffer.
// ===========================================================================
class Commands {
public:
    Commands(Commands const&)            = delete;
    Commands& operator=(Commands const&) = delete;
    Commands(Commands&&)                 = delete;
    Commands& operator=(Commands&&)      = delete;

    // spawn returns a usable handle immediately (alive after the next flush).
    // At flush the entity is placed directly into its final archetype.
    // Perfect-forwarded: an rvalue component moves into the capture and again
    // into storage at flush -- no copies for heap-owning fields. If the spawn
    // never applies (the run aborts first), the handle stays dead and its slot
    // is reused.
    template <class... Cs>
    Entity spawn(Cs&&... comps) {
        Entity const e = world_.reserve();
        auto build     = [... comps = std::forward<Cs>(comps)](World& w,
                                                               Entity self) mutable {
            w.spawn_now<std::decay_t<Cs>...>(self, std::move(comps)...);
        };
        record(detail::ReservedSpawn<decltype(build)> {e, std::move(build)});
        return e;
    }

    void destroy(Entity e) {
        record([e](World& w) { w.destroy_now(e); });
    }

    // Add component C to an entity (moving it to the matching archetype). If
    // the entity already has C, its value is overwritten in place. No-ops if
    // dead.
    template <class C>
    void add(Entity e, C&& value) {
        record([e, value = std::forward<C>(value)](World& w) mutable {
            if (w.alive(e))
                w.add_now<std::decay_t<C>>(e, std::move(value));
        });
    }

    template <class C>
    void remove(Entity e) {
        record([e](World& w) {
            if (w.alive(e))
                w.remove_now<C>(e);
        });
    }

    // Overwrite component C on an entity. No-ops if the entity is dead or does
    // not currently have C (so a stale target never throws at flush time).
    template <class C>
    void set(Entity e, C&& value) {
        record([e, value = std::forward<C>(value)](World& w) mutable {
            using D = std::decay_t<C>;
            if (w.alive(e) && w.has<D>(e))
                w.set_now<D>(e, std::move(value));
        });
    }

    // Reorder every C-bearing archetype's rows by a canonical key at the next
    // flush -- a deferred World::sort_rows. sort_rows is structural (it permutes
    // rows, like the *_now edits) so it cannot run mid-wave; recording it as a
    // command runs it single-threaded and world-quiescent at the barrier, the
    // same place spawns/despawns apply. That is what lets an ordinary system ask
    // for data-layout upkeep (e.g. re-sort by grid cell every N ticks) without
    // the raw World& sort_rows would otherwise need. Deterministic; see
    // World::sort_rows for the key / min_disorder contract.
    template <class C, class KeyFn>
    void sort(KeyFn key, double min_disorder = 0.0) {
        record([key = std::move(key), min_disorder](World& w) {
            w.sort_rows<C>(key, min_disorder);
        });
    }

private:
    friend class Schedule; // constructs the shared Commands for dynamic systems
    friend struct detail::command_slot; // a store-backed Commands (see below)
    explicit Commands(World& w)
        : world_(w) {}
    // A typed system's Commands records into a private store (no lock,
    // deterministic replay order) instead of the world's thread-sharded
    // buffer. The Commands& parameter policies own the stores (each paired
    // with its Commands in a command_slot) and register them with the wave,
    // which drains them at its flush (see schedule/params/commands.hpp).
    Commands(World& w, CommandStore& local)
        : world_(w)
        , local_(&local) {}
    template <class F>
    void record(F&& f) {
        if (local_)
            local_->record(std::forward<F>(f));
        else
            world_.commands_.record(std::forward<F>(f));
    }
    World& world_;
    CommandStore* local_ = nullptr;
};

} // namespace ecs
