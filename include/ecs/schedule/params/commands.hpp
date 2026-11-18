// ecs/schedule/params/commands.hpp
//
// Commands& in a PARALLEL system: per-item command recording with canonical
// replay order.
//
// A serial system's Commands records into the world's thread-sharded
// buffer -- safe, but the replay order across shards follows item-to-lane
// timing, so structural edits recorded from a parallel dispatch land in a
// different order run to run. Here each work item instead records into its
// own private CommandStore slot (no lock at all on the hot path), and the
// barrier enqueues the slots into the wave's flush in item-ordinal
// (serial-walk) order. Parallel-recorded edits therefore APPLY in canonical
// world order: the same order, and the same resulting archetype-row layout,
// at 1 lane and N.
//
// The residual nondeterminism, documented rather than hidden: spawn()
// reserves its Entity handle at RECORD time (the API returns a usable handle
// immediately), so the ID a given spawn receives still depends on cross-lane
// reservation timing even though the spawns themselves apply in canonical
// order -- and serial systems' commands keep the sharded path. Canonical
// ID assignment needs record-time reservation to move into the barrier,
// which is an API-visible change tracked in docs/IMPROVEMENTS.md.

#pragma once

#include <ecs/schedule/params/protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace ecs {
namespace detail {

    template <>
    struct parallel_param<Commands&> {
        static constexpr bool allowed = true;
        // Commands is deliberately unmovable, so slots live behind unique_ptr
        // (stable addresses; each slot is its own allocation, so no false
        // sharing between adjacent stores). Stores persist across runs --
        // their arena blocks are the usual zero-alloc-per-tick reuse.
        struct Slot {
            CommandStore store;
            Commands cmds;
            explicit Slot(World& w)
                : cmds(w, store) {}
        };
        struct state {
            std::vector<std::unique_ptr<Slot>> slots;
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
                s.slots.push_back(std::make_unique<Slot>(w));
            // All stores are empty here: drained at the previous barrier, or
            // dropped by abort() if that wave threw. Discarding is a cheap
            // guarantee that nothing recorded earlier leaks into this run.
            for (auto const& slot : s.slots)
                slot->store.discard(w);
            s.active = rows.size();
        }

        // The wave threw before (or while) these stores reached the flush: drop
        // the items' edits now, handing their reserved entities back to `w`, so
        // nothing waits on this system running again.
        static void abort(state& s, World& w) noexcept {
            // Slots still bound to another world hold nothing: they were drained
            // or aborted there, and prepare() rebinds them before any item here
            // records.
            if (s.world != &w)
                return;
            for (auto const& slot : s.slots)
                slot->store.discard(w);
        }

        // Single-threaded, before the wave's flush: enqueue each item's store
        // in ordinal order. The flush then drains them in that order (the
        // replay commands live in the barrier thread's one shard).
        static void finish(state& s, World&) {
            for (std::size_t i = 0; i < s.active; ++i)
                if (s.slots[i]->store.size() > 0)
                    s.slots[i]->cmds.enqueue_local();
        }

        static Commands& bind(state& s, World&, Commands&, WorkItem const& item) {
            auto ordinal = item.ordinal;
            return s.slots[ordinal]->cmds;
        }
    };

} // namespace detail
} // namespace ecs
