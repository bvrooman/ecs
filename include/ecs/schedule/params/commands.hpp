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
// item) -- recorded with no lock at all, and the barrier splices the stores
// into the wave's flush in canonical order: due systems in wave order
// (registration order within a wave), and a parallel system's items in
// ordinal (serial-walk) order. Recorded edits therefore APPLY in the same
// order, with the same resulting archetype-row layout, at 1 lane and N.
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

#include <ecs/schedule/params/protocol.hpp>

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

        static void prepare(state& s,
                            World& w,
                            std::span<std::uint32_t const>,
                            WaveContext const&) {
            if (s.world != &w) {
                s.slot  = std::make_unique<command_slot>(w);
                s.world = &w;
            }
            // Normally empty here (spliced and drained at the previous
            // barrier); after an aborted run it may still hold the aborted
            // wave's recordings, which must not leak into this run.
            s.slot->store.discard();
        }

        // Single-threaded, before the wave's flush.
        static void finish(state& s, World& w) {
            if (s.slot->store.size() > 0)
                w.splice_commands(s.slot->store);
        }

        static Commands& bind(state& s, World&, Commands&, WorkItem const&) {
            return s.slot->cmds;
        }
    };

    template <>
    struct parallel_param<Commands&> {
        static constexpr bool allowed = true;
        struct state {
            std::vector<std::unique_ptr<command_slot>> slots;
            std::size_t active = 0;
            World* world       = nullptr; // slots hold World&: rebuilt on change
        };

        static void declare(SystemAccess& a) { a.commands = true; }

        static void prepare(state& s,
                            World& w,
                            std::span<std::uint32_t const> rows,
                            WaveContext const&) {
            if (s.world != &w) {
                s.slots.clear();
                s.world = &w;
            }
            while (s.slots.size() < rows.size())
                s.slots.push_back(std::make_unique<command_slot>(w));
            // As in serial_param<Commands&>: drop an aborted run's leftovers.
            for (auto const& slot : s.slots)
                slot->store.discard();
            s.active = rows.size();
        }

        // Single-threaded, before the wave's flush: splice each item's store
        // into the world's buffer in ordinal order. The flush then drains
        // them in that order, after the thread-sharded commands.
        static void finish(state& s, World& w) {
            for (std::size_t i = 0; i < s.active; ++i)
                if (s.slots[i]->store.size() > 0)
                    w.splice_commands(s.slots[i]->store);
        }

        static Commands& bind(state& s, World&, Commands&, WorkItem const& item) {
            auto ordinal = item.ordinal;
            return s.slots[ordinal]->cmds;
        }
    };

} // namespace detail
} // namespace ecs
