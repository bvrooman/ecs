#pragma once

#include "../detail/id_vector.hpp"
#include "../detail/strong_id.hpp"
#include "executor.hpp"
#include "system.hpp"
#include <cstdint>

namespace ecs {
using WaveId = StrongId<struct WaveIdTag, uint32_t>;

class PrepareContext {
    using WorkItem = detail::WorkItem;
    using Fn       = detail::PrepareItemsFn;

public:
    auto prepare(World& world, std::span<WorkItem> items) {
        if (!fn)
            return false;
        auto filter = std::views::filter([&](auto item) {
            return item.system == system && item.archetype != detail::kImperative;
        });
        for (auto const& it : items | filter) {
            if (rows_scratch_.size() <= it.ordinal)
                rows_scratch_.resize(it.ordinal + 1);
            rows_scratch_[it.ordinal] = it.end - it.begin;
        }
        auto const kernel_context = detail::KernelWaveContext {system, tick_};
        fn(world, rows_scratch_, kernel_context);
        return true;
    }

private:
    friend class WavePlan;
    friend class Wave;

    PrepareContext(Fn& fn, SystemId system, uint64_t tick)
        : fn(fn)
        , system(system)
        , tick_(tick) {}

    Fn& fn;
    SystemId system;
    uint64_t tick_;
    std::vector<uint32_t> rows_scratch_; // per-system ordinal rows
};

class FinishContext {
    using Fn = detail::FinishItemsFn;

public:
    auto finish(World& world) const {
        if (!fn)
            return false;
        detail::recording_source() = system;
        fn(world);
        return true;
    }

private:
    friend class WavePlan;
    friend class Wave;

    FinishContext(Fn& fn, SystemId system)
        : fn(fn)
        , system(system) {}

    Fn& fn;
    SystemId system;
};

struct WaveResult {
    std::unordered_map<SystemId, double> prepare_us;
    std::unordered_map<SystemId, double> finish_us;
    std::unordered_map<SystemId, double> busy_us;
    std::unordered_map<SystemId, uint32_t> item_counts;
};

// Compiled wave plan
class Wave {
public:
    using System       = detail::SystemRecord;
    using SystemVector = detail::IdVector<SystemId, System>;
    using WorkItem     = detail::WorkItem;

    WaveId id = {};

    [[nodiscard]]
    auto size() const {
        return static_cast<uint32_t>(items_.size());
    }

    void prepare(World& world) {
        assert(state == State::New);
        for (auto&& context : prepare_) {
            auto const t0 = detail::sched_clock::now();
            context.prepare(world, items_);
            result_.prepare_us[context.system] = detail::elapsed_us(t0);
        }
        state = State::Prepared;
    }

    void finish(World& world) {
        assert(state == State::Ran);
        for (auto&& context : finish_) {
            auto const t0 = detail::sched_clock::now();
            context.finish(world);
            result_.finish_us[context.system] = detail::elapsed_us(t0);
        }
        state = State::Finished;
    }

    void run(SystemVector& systems, World& world, Commands& cmds, WorkerPool& pool) {
        assert(state == State::Prepared);
        using clock  = std::chrono::steady_clock;
        auto run_one = [&](WorkItem& item, WorkerPool& p) {
            auto const t0 = clock::now();
            run_work_item(item, systems, world, cmds, p);
            item.busy_us = detail::elapsed_us(t0);
        };
        if (items_.empty()) {
            state = State::Ran;
            return;
        }
        if (items_.size() == 1) {
            auto item = items_[0];
            run_one(item, pool);
            result_.busy_us[item.system] += item.busy_us;
            result_.item_counts[item.system] = 1;
            state                            = State::Ran;
            return;
        }
        auto next   = std::atomic {0uz};
        auto& inner = parallel::serial_pool();
        pool.parallel_for(items_.size(), 2, [&](std::size_t, std::size_t) {
            for (auto i = next.fetch_add(1, std::memory_order_relaxed); i < items_.size();
                 i      = next.fetch_add(1, std::memory_order_relaxed))
                run_one(items_[i], inner);
        });
        for (auto&& item : items_) {
            result_.busy_us[item.system] += item.busy_us;
            result_.item_counts[item.system]++;
        }
        state = State::Ran;
    }

    auto result() const -> WaveResult const& {
        assert(state == State::Finished);
        return result_;
    }

private:
    friend class WavePlan;
    Wave(WaveId const id,
         std::vector<WorkItem>&& items,
         std::vector<PrepareContext>&& prepare,
         std::vector<FinishContext>&& finish)
        : id {id}
        , items_ {std::move(items)}
        , prepare_ {std::move(prepare)}
        , finish_ {std::move(finish)} {}

    std::vector<WorkItem> items_;
    std::vector<PrepareContext> prepare_;
    std::vector<FinishContext> finish_;
    WaveResult result_;

    enum class State {
        New,
        Prepared,
        Ran,
        Finished,
    };
    State state = State::New;
};

class WavePlan {
public:
    using System                                = detail::SystemRecord;
    using SystemVector                          = detail::IdVector<SystemId, System>;
    static constexpr auto kMinItemRows          = 1024uz;
    static constexpr auto kTargetItemsPerKernel = 64uz;

    WaveId wave_id = {};

    auto push_back(SystemId const id) { systems_.push_back(id); }

    [[nodiscard]]
    auto size() const {
        return systems_.size();
    }
    [[nodiscard]]
    auto empty() const {
        return systems_.empty();
    }

    auto begin() { return due_.begin(); }
    auto end() { return due_.end(); }
    auto begin() const { return due_.begin(); }
    auto end() const { return due_.end(); }

    auto prepare(SystemVector& systems, uint64_t tick) {
        due_.clear();
        for (auto const id : systems_) {
            auto const& s = systems[id];
            if (tick % s.every == 0)
                due_.push_back(id);
        }
        return due_.size();
    }

    auto build(SystemVector& systems, World const& world, uint64_t tick) const -> Wave {
        auto items   = std::vector<detail::WorkItem>();
        auto prepare = std::vector<PrepareContext>();
        auto finish  = std::vector<FinishContext>();
        for (auto const id : due_) {
            auto& s = systems[id];
            if (tick % s.every != 0)
                continue;
            prepare.push_back({s.prepare_items, id, tick});
            finish.push_back({s.finish_items, id});
            if (!s.is_kernel()) {
                items.emplace_back(id, detail::kImperative, 0, 0, 0);
                continue;
            }
            auto const& matches = s.match.resolve(world, s.query_sig);
            auto total          = 0uz;
            for (auto const ai : matches)
                total += world.archetypes()[ai]->size();
            if (total == 0)
                continue;
            // ~kTargetItemsPerKernel items per kernel system
            std::size_t const grain =
                std::max<std::size_t>(kMinItemRows, total / kTargetItemsPerKernel);
            std::uint32_t ordinal = 0;
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
        // Longest first (imperative items -- unknown cost -- ahead of everything);
        // deterministic tie-break so a 1-lane run replays identically.
        std::ranges::sort(items,
                          [](detail::WorkItem const& a, detail::WorkItem const& b) {
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
        return {wave_id, std::move(items), std::move(prepare), std::move(finish)};
    }

private:
    std::vector<SystemId> systems_;
    std::vector<SystemId> due_;
};
}
