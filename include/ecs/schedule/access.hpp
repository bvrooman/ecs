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

// Work-item granularity for a PARALLEL system: exactly n rows per work item,
// replacing the executor's default sizing entirely.
//
// The default -- a floor of WavePlan::kMinItemRows (1024), relaxed so a large
// system lands near kTargetItemsPerSystem items -- is a policy for CHEAP
// per-row work, where a finer split costs more in dispatch than it wins in
// parallelism. A heavy kernel breaks it in both directions:
//
//   too coarse below the floor   a tree-level pass of 500 thousand-flop rows
//                                becomes ONE item, so it runs on one lane
//                                however many lanes exist:
//                                  grain {64}   // 500 rows -> 8 items, not 1
//   too coarse above the target  a large system with unevenly weighted rows
//                                (an n-body near field, where per-row cost
//                                tracks local density) is capped at the item
//                                target, and its longest item becomes the
//                                straggler every lane waits on:
//                                  grain {256}  // 100k rows -> 391 items, not 65
//
// More items cost more dispatch bookkeeping and re-bind the system's parameters
// once per item, so this is a tuning knob, not a default to lower globally: aim
// for a few items per lane, sized so the heaviest is a small fraction of the
// wave. 0 (the default) means "use the executor's sizing". Ignored by
// add_serial systems, which are a single opaque item by construction.
struct grain {
    std::size_t n = 0;
};

// Partition a PARALLEL system's rows by the value of key component K: one work
// item per distinct key, holding EVERY row carrying that key -- so a group is
// never split across lanes, and the body sees the whole group at once.
//
//   sched.add_parallel("m2l", fn, partition_by<Subtree> {});
//
// grain{n} says how big an item may be; partition_by<K> says what an item MEANS.
// The default slicing is free to cut anywhere, which is exactly right when rows
// are independent and exactly wrong when they are not: a tree traversal, a
// per-cell solve, a per-island constraint batch all have a unit of work that is
// a GROUP of rows, and a group half-processed on one lane while its other half
// runs on another is not a smaller version of the same job -- it is a race, or a
// wrong answer. The usual workaround is to lift the group loop into a serial
// system and fan out by hand from inside it; declaring the partition instead
// keeps the work in the executor's one flat item list, where it is leveled,
// load-balanced against every other system in the wave, and ordered canonically.
//
// What the executor does with it: it scans K down each matched archetype for
// runs of equal key, gathers the runs of one key (they may span archetypes) into
// one item, and emits items in ascending key order -- so the ordinals every
// barrier fold replays in stay canonical. Grouping is by VALUE, not by locality,
// so unsorted rows still yield exactly one item per key; they just make that
// item hold many small runs instead of one, which costs a longer scan at build
// and a scattered walk at run. Sorting the rows by K (World::sort_rows /
// Commands::sort) collapses each group back to a single contiguous range.
//
// K must be a component with exactly one integral field. The system reads it
// implicitly (declared for conflict analysis) and matches only archetypes that
// have it -- a row with no K has no group. Mutually exclusive with grain{n}: a
// partition item is atomic by construction. Ignored by add_serial, which is one
// item already -- so passing it there is a compile error rather than a silent
// no-op.
template <class K>
struct partition_by {};

namespace detail {

    template <class T>
    inline constexpr bool is_partition_by_v = false;
    template <class K>
    inline constexpr bool is_partition_by_v<ecs::partition_by<K>> = true;

    // The key type of the pack's partition_by<K>, or void when it has none.
    // Carried as a TYPE all the way to build_system rather than stored in
    // AddOptions: a key type is not a value, and threading it through the option
    // struct would make access.hpp (deliberately execution-free) depend on
    // Archetype to do anything with it.
    template <class... Opts>
    struct partition_key {
        using type = void;
    };
    template <class K, class... Rest>
    struct partition_key<ecs::partition_by<K>, Rest...> {
        using type = K;
    };
    template <class First, class... Rest>
    struct partition_key<First, Rest...> : partition_key<Rest...> {};

    template <class... Opts>
    using partition_key_t = typename partition_key<Opts...>::type;

    // The resolved option set an add_* hands to its emplace.
    struct AddOptions {
        int phase           = 0;
        std::uint64_t every = 1;
        std::uint64_t times = 0;
        std::size_t grain   = 0; // 0 = the executor's default item-row floor
    };

    template <class T>
    concept AddOption =
        std::same_as<T, phase> || std::same_as<T, every> || std::same_as<T, times> ||
        std::same_as<T, grain> || is_partition_by_v<T>;

    template <class Want, class... Opts>
    consteval bool at_most_one() {
        return (std::size_t(std::same_as<Opts, Want>) + ... + 0) <= 1;
    }

    template <class... Opts>
    consteval bool at_most_one_partition() {
        return (std::size_t(is_partition_by_v<Opts>) + ... + 0) <= 1;
    }

    template <class... Opts>
    consteval bool has_partition() {
        return (is_partition_by_v<Opts> || ... || false);
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
        // partition_by<K> carries no value -- it is read off the pack as a type
        // (partition_key_t), so there is nothing to store. Anything else already
        // failed resolve_options' static_assert; do nothing here so that assert
        // is the only diagnostic the caller sees.
    }

    // Fold a registration option pack into AddOptions. Unknown or repeated options
    // are rejected here rather than deeper in the registration machinery, so the
    // diagnostic names the call site.
    template <class... Opts>
    constexpr AddOptions resolve_options(Opts... opts) {
        static_assert(
            (AddOption<Opts> && ...),
            "unsupported registration option -- a system may be registered with "
            "phase{n}, every{n}, times{n}, grain{n} and partition_by<K>{}");
        static_assert(at_most_one<phase, Opts...>() && at_most_one<every, Opts...>() &&
                          at_most_one<times, Opts...>() &&
                          at_most_one<grain, Opts...>() &&
                          at_most_one_partition<Opts...>(),
                      "each registration option may be given at most once");
        static_assert(!(has_partition<Opts...>() &&
                        (std::same_as<Opts, ecs::grain> || ... || false)),
                      "partition_by<K> and grain{n} are mutually exclusive -- grain "
                      "sizes an item, partition_by defines what one IS, and a "
                      "partition item is atomic (a group split across lanes is the "
                      "thing partition_by exists to prevent)");
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
