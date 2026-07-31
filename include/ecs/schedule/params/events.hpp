// ecs/schedule/params/events.hpp
//
// Frame-buffered gameplay events: an Events<T> channel resource, written
// through EventWriter<T> and read through EventReader<T> parallel parameters.
//
// The channel is double-buffered per schedule run: events emitted during tick
// N become readable during tick N+1 and are dropped after it. Readers
// therefore always see a complete, stable snapshot -- one tick old -- no
// matter where in the tick they run, and writers and readers never touch the
// same buffer. The swap happens at most once per tick, driven by the schedule
// tick in WaveContext, inside the single-threaded prepare hooks.
//
// The write side uses the same slot plumbing as Collect: each work item
// appends into a private partial, and the barrier concatenates partials into
// the channel in item-ordinal order. Cross-SYSTEM order is wave order (two
// writers conflict on the channel, so they land in distinct waves) -- the
// full sequence readers see next tick is deterministic at any lane count.
//
// Events<T> is an ordinary resource, so serial add() systems participate with
// the existing parameters: ResMut<Events<T>> + emit() to write,
// Res<Events<T>> + read() to consume. (event::Emitter is the synchronous
// instrumentation bus; this channel is for gameplay messages.)

#pragma once

#include <ecs/schedule/params/parallel.hpp> // detail::slot_array
#include <ecs/schedule/params/protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

namespace ecs {

// The double-buffered event channel for message type T. Emplace one as a
// resource before running a schedule that uses EventWriter<T> /
// EventReader<T>. One schedule per channel: the swap is keyed on the
// schedule's tick counter.
template <class T>
class Events {
public:
    // Everything emitted during the previous tick, in deterministic order.
    [[nodiscard]]
    std::span<T const> read() const noexcept {
        return {read_.data(), read_.size()};
    }
    // Emit an event, readable next tick. For serial systems (through
    // ResMut<Events<T>>); parallel systems emit through EventWriter<T>.
    void emit(T e) { write_.push_back(std::move(e)); }
    // Swap the buffers for a new tick (idempotent per tick value). Driven
    // automatically by the EventWriter/EventReader prepare hooks; call it
    // directly only when NO parallel system touches the channel (pure
    // ResMut/Res usage), e.g. once per frame outside run().
    void advance_to(std::uint64_t const tick) {
        if (tick == tick_)
            return; // another param already swapped this tick
        tick_ = tick;
        std::swap(read_, write_);
        write_.clear();
    }

private:
    template <class P>
    friend struct detail::parallel_param;
    std::vector<T> read_;
    std::vector<T> write_;
    std::uint64_t tick_ = 0;
};

// Parallel-side write face of Events<T>: emits into THIS ITEM's private
// partial; the barrier moves the partials into the channel in ordinal order.
// Precondition, checked at prepare: the world owns an Events<T> resource.
template <class T>
class EventWriter {
public:
    void emit(T e) { partial_->push_back(std::move(e)); }

private:
    template <class P>
    friend struct detail::parallel_param;
    explicit EventWriter(std::vector<T>& partial) noexcept
        : partial_(&partial) {}
    std::vector<T>* partial_;
};

// Parallel-side read face of Events<T>: a stable view of everything emitted
// during the PREVIOUS tick (same span for every item -- events are not
// per-row). Iterable: `for (auto const& e : events) ...`.
// Precondition, checked at prepare: the world owns an Events<T> resource.
template <class T>
class EventReader {
public:
    [[nodiscard]]
    auto begin() const noexcept {
        return events_.begin();
    }
    [[nodiscard]]
    auto end() const noexcept {
        return events_.end();
    }
    [[nodiscard]]
    std::size_t size() const noexcept {
        return events_.size();
    }
    [[nodiscard]]
    bool empty() const noexcept {
        return events_.empty();
    }
    [[nodiscard]]
    T const& operator[](std::size_t i) const noexcept {
        return events_[i];
    }
    [[nodiscard]]
    std::span<T const> span() const noexcept {
        return events_;
    }

private:
    template <class P>
    friend struct detail::parallel_param;
    explicit EventReader(std::span<T const> events) noexcept
        : events_(events) {}
    std::span<T const> events_;
};

namespace detail {

    template <class T>
    struct parallel_param<EventWriter<T>> {
        static constexpr bool allowed = true;
        struct state {
            slot_array<std::vector<T>> parts;
        };

        // A WRITE on the channel: two writer systems land in distinct waves
        // (deterministic cross-system event order), and serial
        // ResMut<Events<T>> systems level against parallel writers.
        static void declare(SystemAccess& a) {
            a.res_writes.push_back(resource_id<Events<T>>);
        }

        static void prepare(state& s,
                            World& w,
                            std::span<std::uint32_t const> rows,
                            WaveContext const& ctx) {
            w.resource<Events<T>>().advance_to(ctx.tick);
            s.parts.prepare(rows.size());
        }

        static void finish(state& s, World& w, parallel::WorkerPool&) {
            auto& channel = w.resource<Events<T>>();
            for (std::size_t i = 0; i < s.parts.active; ++i) {
                auto& part = s.parts[i];
                channel.write_.insert(channel.write_.end(),
                                      std::make_move_iterator(part.begin()),
                                      std::make_move_iterator(part.end()));
            }
        }

        static EventWriter<T> bind(state& s, World&, Commands&, WorkItem const& item) {
            auto ordinal = item.ordinal;
            return EventWriter<T>(s.parts[ordinal]);
        }
    };

    template <class T>
    struct parallel_param<EventReader<T>> {
        static constexpr bool allowed = true;
        struct state {
            std::span<T const> events;
        };

        static void declare(SystemAccess& a) {
            a.res_reads.push_back(resource_id<Events<T>>);
        }

        // Whichever event param's prepare runs first this tick performs the
        // swap; the read buffer is then stable for the whole tick, so the
        // span cached here outlives every item bind.
        static void prepare(state& s,
                            World& w,
                            std::span<std::uint32_t const>,
                            WaveContext const& ctx) {
            auto& channel = w.resource<Events<T>>();
            channel.advance_to(ctx.tick);
            s.events = channel.read();
        }

        static void finish(state&, World&, parallel::WorkerPool&) {}

        static EventReader<T> bind(state& s, World&, Commands&, WorkItem const&) {
            return EventReader<T>(s.events);
        }
    };

} // namespace detail
} // namespace ecs
