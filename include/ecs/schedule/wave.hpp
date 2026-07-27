#pragma once

#include <ecs/detail/container/id_vector.hpp>
#include <ecs/detail/work_item.hpp>
#include <ecs/parallel/worker_pool.hpp>
#include <ecs/schedule/system.hpp>
#include <ecs/schedule/wave_id.hpp>
#include <ecs/strong_id.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace ecs {

// A system's barrier prepare hook, bound to one system for one tick. Built
// for every due system -- a serial one taking Local<T> is stateful too;
// a system with no state carries a null fn and the hook no-ops.
// Holds only a pointer + ids, so the vector of these refills each tick (clear +
// push_back) without per-element allocation; the row-count scratch it fills is
// owned by the Wave and shared across contexts -- one reused buffer, not one
// vector per system.
class PrepareContext {
    using WorkItem = detail::WorkItem;
    using Fn       = detail::PrepareItemsFn;

public:
    SystemId system;

    // rows: the Wave's shared scratch, sized here to this system's item count.
    bool prepare(World& world,
                 std::span<WorkItem const> items,
                 std::vector<uint32_t>& rows) const {
        if (!*fn_)
            return false;
        rows.clear();
        for (auto const& it : items) {
            if (it.system != system)
                continue;
            if (rows.size() <= it.ordinal)
                rows.resize(it.ordinal + 1);
            rows[it.ordinal] = it.end - it.begin;
        }
        (*fn_)(world, rows, detail::WaveContext {system, tick_});
        return true;
    }

private:
    friend class WavePlan;
    PrepareContext(Fn& fn, SystemId system, uint64_t tick)
        : system(system)
        , fn_(&fn)
        , tick_(tick) {}

    Fn* fn_;
    uint64_t tick_;
};

// A system's barrier finish hook, bound to one system. Built for every due
// system, like PrepareContext above.
class FinishContext {
    using Fn = detail::FinishItemsFn;

public:
    SystemId system;

    bool finish(World& world) const {
        if (!*fn_)
            return false;
        detail::recording_source() = system;
        (*fn_)(world);
        return true;
    }

private:
    friend class WavePlan;
    FinishContext(Fn& fn, SystemId system)
        : system(system)
        , fn_(&fn) {}

    Fn* fn_;
};

// Per-system wave outcomes, indexed by SystemId and sized to the system count.
// reset() clears via assign() (reusing capacity, no reallocation once warm), so
// a steady-state run allocates nothing; a system that produced no work items
// simply keeps its zeroed slot -- no keyed lookup that could miss.
struct WaveResult {
    detail::IdVector<SystemId, double> prepare_us;
    detail::IdVector<SystemId, double> finish_us;
    detail::IdVector<SystemId, double> busy_us;
    detail::IdVector<SystemId, uint32_t> item_counts;

    void reset(std::size_t const n) {
        prepare_us.assign(n, 0.0);
        finish_us.assign(n, 0.0);
        busy_us.assign(n, 0.0);
        item_counts.assign(n, 0u);
    }
};

// A wave compiled for one tick: its work items, its systems' barrier hooks, and
// their measured outcomes. Owned and reused by its WavePlan across ticks (see
// WavePlan::build), so its buffers retain capacity; reset() drops the previous
// tick's contents without freeing. Lifecycle is a strict New -> Prepared -> Ran
// -> Finished progression (debug-asserted).
class Wave {
public:
    using System       = detail::SystemRecord;
    using SystemVector = detail::IdVector<SystemId, System>;
    using WorkItem     = detail::WorkItem;

    WaveId id = {};

    // Barrier prepare hooks (Reduce targets reset, Extract pre-sizing, ...),
    // single-threaded, before the dispatch. Each hooked system's duration lands
    // in result_.prepare_us[system]; unhooked systems keep their zeroed slot.
    void prepare(World& world) {
        assert(state_ == State::New);
        for (auto const& ctx : prepare_) {
            auto const t0 = detail::sched_clock::now();
            if (ctx.prepare(world, items_, rows_scratch_))
                result_.prepare_us[ctx.system] = detail::elapsed_us(t0);
        }
        state_ = State::Prepared;
    }

    // Execute the item list. A lone item runs inline on the caller; several are
    // claimed from an atomic cursor across the pool's lanes. Each item's busy
    // time is rolled up per system into result_.
    void run(SystemVector& systems, World& world, Commands& cmds, WorkerPool& pool) {
        assert(state_ == State::Prepared);
        using clock  = std::chrono::steady_clock;
        auto run_one = [&](WorkItem& item) {
            auto& system               = systems[item.system];
            detail::recording_source() = system.id;
            auto const t0              = clock::now();
            system.run(world, cmds, item);
            item.busy_us = detail::elapsed_us(t0);
        };
        if (items_.size() == 1) {
            run_one(items_[0]);
        } else if (items_.size() > 1) {
            auto next = std::atomic {0uz};
            pool.parallel_for(items_.size(), 2, [&](std::size_t, std::size_t) {
                for (auto i = next.fetch_add(1, std::memory_order_relaxed);
                     i < items_.size();
                     i = next.fetch_add(1, std::memory_order_relaxed))
                    run_one(items_[i]);
            });
        }
        for (auto const& item : items_) {
            result_.busy_us[item.system] += item.busy_us;
            result_.item_counts[item.system]++;
        }
        state_ = State::Ran;
    }

    // Barrier finish hooks (Reduce folds, ...), single-threaded, after the join
    // and before the command flush.
    void finish(World& world) {
        assert(state_ == State::Ran);
        for (auto const& ctx : finish_) {
            auto const t0 = detail::sched_clock::now();
            if (ctx.finish(world))
                result_.finish_us[ctx.system] = detail::elapsed_us(t0);
        }
        state_ = State::Finished;
    }

    [[nodiscard]]
    auto const& result() const {
        assert(state_ == State::Finished);
        return result_;
    }

private:
    friend class WavePlan;

    // Start a fresh tick: keep buffer capacity, drop the previous tick's
    // contents, and zero the per-system result slots. `n` is the system count
    // (the result IdVectors are sized to hold every SystemId).
    void reset(std::size_t const n) {
        items_.clear();
        prepare_.clear();
        finish_.clear();
        result_.reset(n);
        state_ = State::New;
    }

    std::vector<WorkItem> items_;
    std::vector<PrepareContext> prepare_;
    std::vector<FinishContext> finish_;
    std::vector<std::uint32_t> rows_scratch_; // shared prepare-hook scratch
    WaveResult result_;

    enum class State { New, Prepared, Ran, Finished };
    State state_ = State::New;
};

// The build outcome of a WavePlan for one tick.
struct WavePlanResult {
    double build_us = 0; // the build_parallel/build_serial flatten loop
    double sort_us  = 0; // the LPT sort (disjoint from build_us)
};

// The persistent plan for one wave: its member systems (set once at rebuild),
// the due-this-tick subset (recomputed each tick), and a reused Wave the tick's
// work is compiled into. Nothing here allocates in steady state -- systems_ is
// fixed, due_ and the Wave's buffers retain capacity across ticks.
class WavePlan {
public:
    using System                                = detail::SystemRecord;
    using SystemVector                          = detail::IdVector<SystemId, System>;
    static constexpr auto kMinItemRows          = 1024uz;
    static constexpr auto kTargetItemsPerSystem = 64uz;

    WaveId id = {};

    WavePlan()                               = default;
    WavePlan(WavePlan const&)                = delete;
    WavePlan& operator=(WavePlan const&)     = delete;
    WavePlan(WavePlan&&) noexcept            = default;
    WavePlan& operator=(WavePlan&&) noexcept = default;

    void push_back(SystemId const id) { systems_.push_back(id); }

    // size()/empty() and iteration refer to the systems DUE this tick, as
    // populated by the most recent prepare(); the full membership stays private
    // (systems_). Meaningful only after prepare() has run for the tick.
    [[nodiscard]]
    auto size() const {
        return due_.size();
    }
    [[nodiscard]]
    auto empty() const {
        return due_.empty();
    }
    auto begin() const { return due_.begin(); }
    auto end() const { return due_.end(); }

    // Compute this tick's due subset (tick % every == 0). Returns the due count;
    // due_ is reused, so this allocates nothing in steady state.
    std::size_t prepare(SystemVector& systems, std::uint64_t const tick) {
        due_.clear();
        for (auto const id : systems_)
            if (tick % systems[id].every == 0)
                due_.push_back(id);
        return due_.size();
    }

    // Flatten this tick's due systems into the reused Wave: one opaque item per
    // serial system, ~kTargetItemsPerSystem row-range items per parallel system, plus
    // a prepare/finish context per due system. Sorted longest-first with a
    // deterministic tie-break (a 1-lane run replays identically). Requires
    // prepare() to have populated due_ this tick.
    Wave& build(SystemVector& systems, World const& world, std::uint64_t const tick) {
        wave_.reset(systems.size());
        wave_.id           = id;
        auto& items        = wave_.items_;
        auto& prepare      = wave_.prepare_;
        auto& finish       = wave_.finish_;
        auto const t_build = detail::sched_clock::now();
        for (auto const id : due_) {
            auto& s = systems[id];
            prepare.push_back({s.prepare_items, id, tick});
            finish.push_back({s.finish_items, id});
            if (s.is_parallel) {
                build_parallel(world, s, items);
            } else {
                build_serial(world, s, items);
            }
        }
        result_.build_us = detail::elapsed_us(t_build);
        // Greedy LPT: opaque (serial) items first, then longest-first, with a
        // deterministic tie-break.
        auto const t_sort = detail::sched_clock::now();
        std::ranges::sort(items, [&systems](WorkItem const& a, WorkItem const& b) {
            bool const ia = !systems[a.system].is_parallel;
            bool const ib = !systems[b.system].is_parallel;
            if (ia != ib)
                return ia; // opaque/serial items first
            auto const ra = a.end - a.begin;
            auto const rb = b.end - b.begin;
            if (ra != rb)
                return ra > rb;
            return std::tie(a.system, a.begin) < std::tie(b.system, b.begin);
        });
        result_.sort_us = detail::elapsed_us(t_sort);
        return wave_;
    }

    // The build timings of the most recent build().
    [[nodiscard]]
    WavePlanResult const& result() const noexcept {
        return result_;
    }

private:
    void build_parallel(auto& world, auto& system, auto& items) {
        auto id             = system.id;
        auto const& matches = system.match.resolve(world, system.query_sig);
        auto total          = 0uz;
        for (auto const ai : matches)
            total += world.archetypes()[ai]->size();
        auto const grain      = std::max(kMinItemRows, total / kTargetItemsPerSystem);
        std::uint32_t ordinal = 0;
        for (auto const ai : matches) {
            auto const n = world.archetypes()[ai]->size();
            for (auto b = 0uz; b < n; b += grain) {
                auto const e = std::min(n, b + grain);
                auto units   = detail::SmallVector<detail::Unit, 1> {};
                units.push_back(detail::Unit {
                    ai,
                    static_cast<std::uint32_t>(b),
                    static_cast<std::uint32_t>(e),
                });
                auto item = WorkItem {id,
                                      std::move(units),
                                      static_cast<uint32_t>(b),
                                      static_cast<uint32_t>(e),
                                      ordinal++};
                items.push_back(std::move(item));
            }
        }
    }

    void build_serial(auto& world, auto& system, auto& items) {
        auto id             = system.id;
        auto const& matches = system.match.resolve(world, system.query_sig);
        auto total          = 0uz;
        for (auto const ai : matches)
            total += world.archetypes()[ai]->size();
        auto units = detail::SmallVector<detail::Unit, 1> {};
        for (auto const ai : matches) {
            auto const n    = world.archetypes()[ai]->size();
            auto const unit = detail::Unit {
                ai,
                static_cast<std::uint32_t>(0),
                static_cast<std::uint32_t>(n),
            };
            units.push_back(unit);
        }
        auto item = WorkItem {id,
                              std::move(units),
                              static_cast<uint32_t>(0),
                              static_cast<uint32_t>(total),
                              0};
        items.push_back(std::move(item));
    }

    using WorkItem = detail::WorkItem;
    std::vector<SystemId> systems_; // wave membership (fixed at rebuild)
    std::vector<SystemId> due_;     // due this tick (reused scratch)
    Wave wave_;                     // this tick's compiled wave (reused buffers)
    WavePlanResult result_;         // build timings of the last build()
};
} // namespace ecs
