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

// ===========================================================================
//  Commands: the mutation API. A running Schedule creates one bound to the
//  World and passes it to every system, so spawn/destroy/add/remove/set are
//  reachable only from inside a system (or a schedule's setup). All of them are
//  deferred -- recorded into the command buffer and applied at the next flush.
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
    // into storage at flush -- no copies for heap-owning fields.
    template <class... Cs>
    Entity spawn(Cs&&... comps) {
        Entity const e = world_.reserve();
        record([e, ... comps = std::forward<Cs>(comps)](World& w) mutable {
            w.spawn_now<std::decay_t<Cs>...>(e, std::move(comps)...);
        });
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
    friend class Schedule; // constructs Commands during run
    template <class P>
    friend struct detail::kernel_param; // per-item recording for kernel systems
    explicit Commands(World& w)
        : world_(w) {}
    // A kernel work item's Commands records into its own private store (no
    // lock, deterministic replay order) instead of the world's thread-sharded
    // buffer; kernel_param<Commands&> owns the stores and enqueues them at
    // the wave barrier (see schedule/params/commands.hpp).
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
    // Barrier-time (single-threaded): splice this item's recorded commands
    // into the wave's flush by enqueueing one replay command that drains the
    // private store in record order. Enqueued from one thread in ordinal
    // order, so kernel-recorded edits apply in canonical order.
    void enqueue_local() {
        world_.commands_.record([s = local_](World& w) { s->apply(w); });
    }
    World& world_;
    CommandStore* local_ = nullptr;
};

} // namespace ecs
