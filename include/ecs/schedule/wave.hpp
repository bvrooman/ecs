#pragma once

#include "../detail/id_vector.hpp"
#include "../detail/strong_id.hpp"
#include "executor.hpp"
#include "system.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

namespace ecs {
using WaveId = StrongId<struct WaveIdTag, uint32_t>;

// A kernel system's barrier prepare hook, bound to one system for one tick.
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
            if (it.system != system || it.archetype == detail::kImperative)
                continue;
            if (rows.size() <= it.ordinal)
                rows.resize(it.ordinal + 1);
            rows[it.ordinal] = it.end - it.begin;
        }
        (*fn_)(world, rows, detail::KernelWaveContext {system, tick_});
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

// A kernel system's barrier finish hook, bound to one system.
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

// Per-system wave outcomes, indexed by SystemId.value and sized to the system
// count. reset() clears via assign() (reusing capacity, no reallocation once
// warm), so a steady-state run allocates nothing; a system that produced no
// work items simply keeps its zeroed slot -- no keyed lookup that could miss.
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

    // Execute the item list. A lone item runs inline on the caller with the full
    // pool; several are claimed from an atomic cursor across the pool's lanes,
    // imperative items bound to the shared 1-lane pool. Each item's busy time is
    // rolled up per system into result_.
    void run(SystemVector& systems, World& world, Commands& cmds, WorkerPool& pool) {
        assert(state_ == State::Prepared);
        using clock  = std::chrono::steady_clock;
        auto run_one = [&](WorkItem& item, WorkerPool& p) {
            auto const t0 = clock::now();
            run_work_item(item, systems, world, cmds, p);
            item.busy_us = detail::elapsed_us(t0);
        };
        if (items_.size() == 1) {
            run_one(items_[0], pool);
        } else if (items_.size() > 1) {
            auto next   = std::atomic {0uz};
            auto& inner = parallel::serial_pool(); // 1-lane: dispatch-free
            pool.parallel_for(items_.size(), 2, [&](std::size_t, std::size_t) {
                for (auto i = next.fetch_add(1, std::memory_order_relaxed);
                     i < items_.size();
                     i = next.fetch_add(1, std::memory_order_relaxed))
                    run_one(items_[i], inner);
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
    // (the result vectors are indexed by SystemId.value).
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

// The persistent plan for one wave: its member systems (set once at rebuild),
// the due-this-tick subset (recomputed each tick), and a reused Wave the tick's
// work is compiled into. Nothing here allocates in steady state -- systems_ is
// fixed, due_ and the Wave's buffers retain capacity across ticks.
class WavePlan {
public:
    using System                                = detail::SystemRecord;
    using SystemVector                          = detail::IdVector<SystemId, System>;
    static constexpr auto kMinItemRows          = 1024uz;
    static constexpr auto kTargetItemsPerKernel = 64uz;

    WaveId wave_id = {};

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
    // imperative system, ~kTargetItemsPerKernel row-range items per kernel, plus
    // a prepare/finish context per due system. Sorted longest-first with a
    // deterministic tie-break (a 1-lane run replays identically). Requires
    // prepare() to have populated due_ this tick.
    Wave& build(SystemVector& systems, World const& world, std::uint64_t const tick) {
        wave_.reset(systems.size());
        wave_.id      = wave_id;
        auto& items   = wave_.items_;
        auto& prepare = wave_.prepare_;
        auto& finish  = wave_.finish_;
        for (auto const id : due_) {
            auto& s = systems[id];
            // Every due system gets a prepare/finish context (even without a
            // hook: the context is a no-op), so every due system has a result
            // slot regardless of how many work items it contributes.
            prepare.push_back({s.prepare_items, id, tick});
            finish.push_back({s.finish_items, id});
            if (!s.is_kernel()) {
                items.push_back({id, detail::kImperative, 0, 0, 0});
                continue;
            }
            auto const& matches = s.match.resolve(world, s.query_sig);
            auto total          = 0uz;
            for (auto const ai : matches)
                total += world.archetypes()[ai]->size();
            if (total == 0)
                continue;
            std::size_t const grain =
                std::max<std::size_t>(kMinItemRows, total / kTargetItemsPerKernel);
            std::uint32_t ordinal = 0; // generation order == serial-walk order
            for (auto const ai : matches) {
                auto const n = world.archetypes()[ai]->size();
                for (std::size_t b = 0; b < n; b += grain) {
                    auto const e = std::min(n, b + grain);
                    items.push_back({id,
                                     ai,
                                     static_cast<std::uint32_t>(b),
                                     static_cast<std::uint32_t>(e),
                                     ordinal++});
                }
            }
        }
        // Longest first (imperative items -- unknown cost -- ahead of
        // everything); deterministic tie-break so a 1-lane run replays.
        std::ranges::sort(items, [](WorkItem const& a, WorkItem const& b) {
            bool const ia = a.archetype == detail::kImperative;
            bool const ib = b.archetype == detail::kImperative;
            if (ia != ib)
                return ia;
            auto const ra = a.end - a.begin;
            auto const rb = b.end - b.begin;
            if (ra != rb)
                return ra > rb;
            return std::tie(a.system, a.archetype, a.begin) <
                   std::tie(b.system, b.archetype, b.begin);
        });
        return wave_;
    }

private:
    using WorkItem = detail::WorkItem;

    std::vector<SystemId> systems_; // wave membership (fixed at rebuild)
    std::vector<SystemId> due_;     // due this tick (reused scratch)
    Wave wave_;                     // this tick's compiled wave (reused buffers)
};
} // namespace ecs
