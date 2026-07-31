// ecs/schedule/access.hpp
//
// The scheduling vocabulary: system identity, the registration options, the
// derived access sets (SystemAccess), and the conflict predicate over them.
// This is the data the wavefront leveling consumes and the visualizer in
// tools/ rebuilds its dependency graph from -- no execution machinery here.

#pragma once

#include <ecs/component_id.hpp> // ComponentId
#include <ecs/resource_id.hpp>  // ResourceId
#include <ecs/system_id.hpp>    // SystemId

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <vector>

namespace ecs {

// Registration options. Every add_* form takes these as a trailing pack: each
// is optional, they may appear in any order, and giving one twice is a compile
// error. Options carry their value, so a runtime one works:
//
//   sched.add_serial("integrate", fn);
//   sched.add_serial("setup",     fn, times {1});
//   sched.add_serial("resort",    fn, phase {-1}, every {64});
//   sched.add_serial("from_cfg",  fn, every {cfg.rate});

// Coarse ordering: systems run in ascending phase order with a barrier between
// phases; within a phase they are leveled by conflicts. Default 0, so
// phase{-1} is a startup phase, phase{1} a teardown/late phase.
struct phase {
    int n = 0;
};

// Cadence: the system is due only on ticks where tick % n == 0. Must be >= 1.
struct every {
    std::uint64_t n = 1;
};

// Run budget: the system retires after it has been due n times. 0 never
// retires; 1 is the one-shot/setup case.
struct times {
    std::uint64_t n = 0;
};

// Work-item granularity for a PARALLEL system: the minimum rows per item,
// replacing the executor's default floor (WavePlan::kMinItemRows). The
// ~64-items-per-system target still caps the count, so this only ever RAISES
// the item count of a small system -- it cannot shatter a large one into
// thousands of items.
//
// The default floor assumes cheap per-row work, where a finer split costs more
// in dispatch than it wins in parallelism. That assumption inverts for a heavy
// kernel over few rows -- a tree-node pass doing thousands of flops per row --
// which the default pins to ONE item, and therefore one lane, however many
// lanes exist. Give those a grain matched to their row count:
//
//   sched.add_parallel("m2l", fn, grain {64});   // 500 rows -> 8 items, not 1
//
// 0 (the default) means "use the executor's floor". Ignored by add_serial
// systems, which are a single opaque item by construction.
struct grain {
    std::size_t n = 0;
};

namespace detail {

    // The resolved option set an add_* hands to its emplace.
    struct AddOptions {
        int phase           = 0;
        std::uint64_t every = 1;
        std::uint64_t times = 0;
        std::size_t grain   = 0; // 0 = the executor's default item-row floor
    };

    template <class T>
    concept AddOption = std::same_as<T, phase> || std::same_as<T, every> ||
                        std::same_as<T, times> || std::same_as<T, grain>;

    template <class Want, class... Opts>
    consteval bool at_most_one() {
        return (std::size_t(std::same_as<Opts, Want>) + ... + 0) <= 1;
    }

    // Store one option into the resolved set. A function template rather than a
    // lambda inside resolve_options: registering with no options is the common
    // case, and a lambda the empty fold never calls is a set-but-unused local at
    // every such call site.
    template <class Opt>
    constexpr void pick(AddOptions& out, Opt const opt) {
        if constexpr (std::same_as<Opt, ecs::phase>)
            out.phase = opt.n;
        else if constexpr (std::same_as<Opt, ecs::every>)
            out.every = opt.n;
        else if constexpr (std::same_as<Opt, ecs::times>)
            out.times = opt.n;
        else if constexpr (std::same_as<Opt, ecs::grain>)
            out.grain = opt.n;
        // anything else already failed resolve_options' static_assert; do nothing
        // here so that assert is the only diagnostic the caller sees.
    }

    // Fold a registration option pack into AddOptions. Unknown or repeated options
    // are rejected here rather than deeper in the registration machinery, so the
    // diagnostic names the call site.
    template <class... Opts>
    constexpr AddOptions resolve_options(Opts... opts) {
        static_assert(
            (AddOption<Opts> && ...),
            "unsupported registration option -- a system may be registered with "
            "phase{n}, every{n}, times{n} and grain{n}");
        static_assert(at_most_one<phase, Opts...>() && at_most_one<every, Opts...>() &&
                          at_most_one<times, Opts...>() && at_most_one<grain, Opts...>(),
                      "each registration option may be given at most once");
        AddOptions out;
        (pick(out, opts), ...);
        // tick % every: a cadence of 0 would divide by zero in WavePlan::prepare.
        assert(out.every >= 1 && "every{n} must be >= 1");
        return out;
    }

} // namespace detail

// A system's derived component/resource access (id sets). `exclusive` means the
// system has unanalyzable access (it took a raw World&) and conflicts with
// every other system. `reads_all` means it reads everything (a WorldView): it
// conflicts only with writers, so read-only systems still run in parallel.
struct SystemAccess {
    std::vector<ComponentId> reads, writes;
    std::vector<ResourceId> res_reads, res_writes;
    bool exclusive = false;
    bool reads_all = false;
    // Purely informational (does not affect conflict analysis): the system took
    // a Commands& and may record deferred structural edits.
    bool commands = false;
};

namespace detail {

    // Access id lists are kept sorted + deduplicated (normalize_access is run
    // at registration), so every conflict check is a linear merge rather than
    // a quadratic scan. Templated over the id type (ComponentId or ResourceId)
    // so a component list can never be merged against a resource list by
    // mistake -- the two spaces are checked independently below.
    inline void normalize_access(SystemAccess& a) {
        auto norm = [](auto& v) {
            std::ranges::sort(v);
            v.erase(std::ranges::unique(v).begin(), v.end());
        };
        norm(a.reads);
        norm(a.writes);
        norm(a.res_reads);
        norm(a.res_writes);
    }

    template <class Id>
    inline bool intersects(std::vector<Id> const& a, std::vector<Id> const& b) {
        auto ia = a.begin();
        auto ib = b.begin();
        while (ia != a.end() && ib != b.end()) {
            if (*ia < *ib)
                ++ia;
            else if (*ib < *ia)
                ++ib;
            else
                return true;
        }
        return false;
    }

    template <class Id>
    inline bool conflicts_on(std::vector<Id> const& aw,
                             std::vector<Id> const& ar,
                             std::vector<Id> const& bw,
                             std::vector<Id> const& br) {
        return intersects(aw, br) || intersects(aw, bw) || intersects(bw, ar);
    }

    inline bool writes_any(SystemAccess const& a) {
        return !a.writes.empty() || !a.res_writes.empty();
    }

} // namespace detail

// Do two access sets conflict? One writes what the other reads or writes
// (read/read never conflicts); an `exclusive` set conflicts with everything,
// a `reads_all` set only with writers. Components and resources are checked
// independently. Public so out-of-tree tooling (e.g. the schedule visualizer
// in tools/) can rebuild the same dependency graph without re-deriving it;
// Schedule::conflicts forwards here.
inline bool conflicts(SystemAccess const& a, SystemAccess const& b) {
    if (a.exclusive || b.exclusive)
        return true;
    if ((a.reads_all && detail::writes_any(b)) || (b.reads_all && detail::writes_any(a)))
        return true;
    return detail::conflicts_on(a.writes, a.reads, b.writes, b.reads) ||
           detail::conflicts_on(a.res_writes, a.res_reads, b.res_writes, b.res_reads);
}

} // namespace ecs
