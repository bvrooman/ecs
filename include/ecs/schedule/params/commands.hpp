// ecs/schedule/params/commands.hpp
//
// Private command recording with canonical replay order, for every system a
// Schedule runs.
//
// No system records into the world's thread-sharded buffer: that is safe,
// but the replay order across shards follows which lane ran what, so
// structural edits would land in a different order run to run. Instead each
// work item gets its own private CommandStore -- one per item for a parallel
// system, one for a serial system (a single work item) -- recorded with no
// lock at all. At prepare each system registers its stores with the wave
// (WaveContext::stores), and the wave drains them at its flush, after the
// thread-sharded commands, in canonical order: due systems in wave order
// (registration order within a wave), and a system's items in ordinal
// (serial-walk) order. Recorded edits therefore APPLY in the same order, with
// the same resulting archetype-row layout, at 1 lane and N. If the wave
// aborts, it discards the stores with the world, so their spawns hand their
// reserved entities back -- no hook of this module's is needed for that.
//
// Typed systems reach their store through the Commands& parameter
// (serial_param / parallel_param below). Dynamic systems
// (add_dynamic_serial / add_dynamic_parallel) have no parameter list, so the
// Schedule owns their command_slots directly and hands each item's Commands
// to the system's run. While a dynamic item runs, the slots also redirect the
// thread's world-level recording (dynamic::WorldOps, called from a JS kernel)
// into that item's store, so those edits are canonical too. Only recording
// done outside any work item -- host code between ticks -- still uses the
// thread-sharded buffer.
//
// The residual nondeterminism, documented rather than hidden: spawn()
// reserves its Entity handle at RECORD time (the API returns a usable handle
// immediately), so the ID a given spawn receives still depends on cross-lane
// reservation timing even though the spawns themselves apply in canonical
// order. Canonical ID assignment needs record-time reservation to move into
// the barrier, which is an API-visible change tracked in docs/IMPROVEMENTS.md.

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

    // A system's recorders, one per work item, indexed by item ordinal. Used
    // by both Commands& parameter policies and, through the Schedule, by
    // dynamic systems.
    class command_slots {
    public:
        // Before the dispatch, single-threaded: one slot per item, each
        // registered with the wave in ordinal order -- the order the wave
        // drains them in at its flush, or discards them in if it aborts.
        void prepare(World& w, std::size_t const items, WaveContext const& ctx) {
            if (world_ != &w) { // slots hold World&: rebuilt on change
                slots_.clear();
                world_ = &w;
            }
            while (slots_.size() < items)
                slots_.push_back(std::make_unique<command_slot>(w));
            for (std::size_t i = 0; i < items; ++i) {
                // Empty already (drained, or discarded with an aborted wave);
                // discarding is a cheap guarantee nothing earlier leaks in.
                slots_[i]->store.discard(w);
                ctx.stores->push_back(&slots_[i]->store);
            }
        }

        [[nodiscard]]
        command_slot& operator[](std::size_t const ordinal) noexcept {
            return *slots_[ordinal];
        }

        // On the lane running `item`: send the thread's world-level
        // CommandBuffer::record calls on `w` to that item's store until the
        // returned scope ends.
        [[nodiscard]]
        RecordingScope redirect(World& w, WorkItem const& item) {
            return RecordingScope(item.system, w.commands_, slots_[item.ordinal]->store);
        }

    private:
        std::vector<std::unique_ptr<command_slot>> slots_;
        World* world_ = nullptr;
    };

    // Commands& is the same parameter for both system kinds: one slot per
    // work item (a serial system has exactly one), registered at prepare.
    struct commands_param {
        static constexpr bool allowed = true;
        using state                   = command_slots;

        static void declare(SystemAccess& a) { a.commands = true; }

        static void prepare(state& s,
                            World& w,
                            std::span<std::uint32_t const> rows,
                            WaveContext const& ctx) {
            s.prepare(w, rows.size(), ctx);
        }

        static void finish(state&, World&) {}

        static Commands& bind(state& s, World&, WorkItem const& item) {
            return s[item.ordinal].cmds;
        }
    };

    template <>
    struct serial_param<Commands&> : commands_param {};
    template <>
    struct parallel_param<Commands&> : commands_param {};

} // namespace detail
} // namespace ecs
