// ecs/schedule/params/commands.hpp
//
// Commands& in a typed system: private command recording with canonical
// replay order, for both system kinds.
//
// A system's Commands& does not record into the world's thread-sharded
// buffer: that is safe, but the replay order across shards follows which lane
// ran what, so structural edits land in a different order run to run.
// Instead each recorder gets its own private CommandStore -- one per work
// item for a parallel system, one for a serial system (which is a single work
// item) -- recorded with no lock at all. At prepare each system registers its
// stores with the wave (WaveContext::stores), and the wave drains them at its
// flush, after the thread-sharded commands, in canonical order: due systems in
// wave order (registration order within a wave), and a parallel system's
// items in ordinal (serial-walk) order. Recorded edits therefore APPLY in the
// same order, with the same resulting archetype-row layout, at 1 lane and N.
// If the wave aborts, it discards the stores with the world, so their spawns
// hand their reserved entities back -- no hook of this parameter's is needed.
//
// The residual nondeterminism, documented rather than hidden: spawn()
// reserves its Entity handle at RECORD time (the API returns a usable handle
// immediately), so the ID a given spawn receives still depends on cross-lane
// reservation timing even though the spawns themselves apply in canonical
// order. And dynamic systems (add_dynamic_serial/add_dynamic_parallel), which
// have no parameter list to own a store, still record through the Schedule's
// shared Commands into the thread-sharded buffer, as does dynamic::WorldOps.
// Canonical ID assignment needs record-time reservation to move into the
// barrier, which is an API-visible change tracked in docs/IMPROVEMENTS.md.

#pragma once

#include <ecs/schedule/params/parallel.hpp>
#include <ecs/schedule/params/protocol.hpp>
#include <ecs/schedule/params/serial.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace ecs {
namespace detail {

    // One private recorder: a store and the Commands bound to it. Commands
    // is deliberately unmovable, so slots live behind unique_ptr (stable
    // addresses; each slot is its own allocation, so no false sharing between
    // adjacent stores). Stores persist across runs -- their arena blocks are
    // the usual zero-alloc-per-tick reuse.
    struct command_slot {
        CommandStore store;
        Commands cmds;
        explicit command_slot(World& w)
            : cmds(w, store) {}
    };

    template <>
    struct serial_param<Commands&> {
        static constexpr bool allowed = true;
        struct state {
            std::unique_ptr<command_slot> slot;
            World* world = nullptr; // the slot holds World&: rebuilt on change
        };

        static void declare(SystemAccess& a) { a.commands = true; }

        // Single-threaded, before the dispatch: register this system's store
        // with the wave, which drains it at its flush (or discards it if the
        // wave aborts).
        static void prepare(state& s,
                            World& w,
                            std::span<std::uint32_t const>,
                            WaveContext const& ctx) {
            if (s.world != &w) {
                s.slot  = std::make_unique<command_slot>(w);
                s.world = &w;
            }
            // Empty already (drained, or discarded with the aborted wave);
            // discarding is a cheap guarantee nothing earlier leaks in.
            s.slot->store.discard(w);
            ctx.stores->push_back(&s.slot->store);
        }

        static void finish(state&, World&) {}

        static Commands& bind(state& s, World&, Commands&, WorkItem const&) {
            return s.slot->cmds;
        }
    };

    template <>
    struct parallel_param<Commands&> {
        static constexpr bool allowed = true;
        struct state {
            std::vector<std::unique_ptr<command_slot>> slots;
            World* world = nullptr; // slots hold World&: rebuilt on change
        };

        static void declare(SystemAccess& a) { a.commands = true; }

        // Single-threaded, before the dispatch: one store per work item,
        // registered with the wave in ordinal order -- the order it drains
        // them in.
        static void prepare(state& s,
                            World& w,
                            std::span<std::uint32_t const> rows,
                            WaveContext const& ctx) {
            if (s.world != &w) {
                s.slots.clear();
                s.world = &w;
            }
            while (s.slots.size() < rows.size())
                s.slots.push_back(std::make_unique<command_slot>(w));
            for (std::size_t i = 0; i < rows.size(); ++i) {
                s.slots[i]->store.discard(w); // as in serial_param: empty
                ctx.stores->push_back(&s.slots[i]->store);
            }
        }

        static void finish(state&, World&) {}

        static Commands& bind(state& s, World&, Commands&, WorkItem const& item) {
            return s.slots[item.ordinal]->cmds;
        }
    };

} // namespace detail
} // namespace ecs
