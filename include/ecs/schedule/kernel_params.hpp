// ecs/schedule/kernel_params.hpp
//
// Kernel-only system parameters and the slot substrate behind them.
//
// A kernel system's work items run concurrently, so any cross-item state must
// be either read-only or privately owned per item. The substrate: for each
// stateful parameter the system owns an array of per-item SLOTS (indexed by
// the item's ordinal -- its position in generation order: archetypes
// ascending, rows ascending, i.e. the same order a serial query walks), plus
// two barrier-time hooks the executor drives around each wave:
//
//   prepare(state, world, item_rows)   before the dispatch, single-threaded:
//                                      size/reset the slots, pre-size targets
//   finish(state, world)               after the join, before the command
//                                      flush, single-threaded: fold the slots
//                                      into the target in ORDINAL order
//
// Because slot contents are per-item and the fold order is canonical, every
// primitive built on this is deterministic at ANY lane count -- bitwise, even
// for float folds -- which per-row atomics can never give.
//
// Faces implemented here:
//
//   Reduce<T, Op>   fold-shaped state (sums, bounds, grids). Each item gets a
//                   private T partial; at the barrier the partials fold into
//                   the T RESOURCE with Op in ordinal order. The target is
//                   rebuilt every run (reset at prepare), like the serial
//                   rebuild-every-tick reductions it replaces.
//
//   Extract<T>      gather-shaped output (snapshots, render buffers), where
//                   output size == matched rows. The vector-like T resource is
//                   pre-sized once before the dispatch and each item binds a
//                   span over its DISJOINT slice at an executor-computed
//                   offset -- one write per element, no fold at all.
//
// Both declare a resource WRITE on T, so same-wave readers of T level after
// the system exactly as with ResMut -- but the physical writes are either
// per-item-private or disjoint, and the shared-target writes happen only in
// the single-threaded barrier hooks.

#pragma once

#include "../query.hpp" // Query, query_param_traits (slicing)
#include "../world.hpp" // World, Commands, Res/ResMut, resource_id
#include "system.hpp"   // system_param, SystemParam, is_res_mut_v
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace ecs {

namespace detail {
    template <class P>
    struct kernel_param; // defined below; forward for friending
}

// Fold-shaped per-item state for a kernel system. `sum->field` / `*sum`
// reaches THIS ITEM's private partial of T -- no sharing, no atomics. At the
// wave barrier the partials are folded into the T resource, in item-ordinal
// (serial-walk) order, with Op: any stateless callable
//   void operator()(T& into, T& partial) const
// (partial is mutable so Op may move out of it). Precondition, checked at
// bind: the world owns a T resource.
template <class T, class Op>
class Reduce {
public:
    [[nodiscard]]
    T& operator*() const noexcept {
        return *partial_;
    }
    T* operator->() const noexcept { return partial_; }

private:
    template <class P>
    friend struct detail::kernel_param;
    explicit Reduce(T& partial) noexcept
        : partial_(&partial) {}
    T* partial_;
};

// A vector-like reduction/extraction target: contiguous, resizable.
template <class T>
concept ExtractTarget = requires(T& t, std::size_t n) {
    t.resize(n);
    t.data();
    { t.size() } -> std::convertible_to<std::size_t>;
};

// Gather-shaped output for a kernel system: a writable span over THIS ITEM's
// disjoint slice of the pre-sized T resource. Element i corresponds to the
// item's row i (the sliced Query walks the same rows in the same order), so
// the idiomatic body pairs them by index:
//
//   q.for_each_chunk([&](std::span<Entity>, chunk<const Position> p) {
//       auto x = p.column<0>();
//       for (std::size_t i = 0; i < x.size(); ++i)
//           out[i] = x[i];
//   });
template <ExtractTarget T>
class Extract {
    using Elem = std::remove_reference_t<decltype(*std::declval<T&>().data())>;

public:
    [[nodiscard]]
    std::span<Elem> span() const noexcept {
        return span_;
    }
    [[nodiscard]]
    Elem& operator[](std::size_t i) const noexcept {
        return span_[i];
    }
    [[nodiscard]]
    std::size_t size() const noexcept {
        return span_.size();
    }

private:
    template <class P>
    friend struct detail::kernel_param;
    explicit Extract(std::span<Elem> s) noexcept
        : span_(s) {}
    std::span<Elem> span_;
};

// Per-item temporary WORKSPACE for a kernel system: `tmp->...` / `*tmp`
// reaches this item's private T, cleared (capacity retained, when T offers
// clear()) before every run. No declared access, no barrier merge -- it is
// pure scratch, for the temp vector / stack / candidate list a body would
// otherwise allocate per row. Contents do not survive the run.
template <class T>
class Scratch {
public:
    [[nodiscard]]
    T& operator*() const noexcept {
        return *scratch_;
    }
    T* operator->() const noexcept { return scratch_; }

private:
    template <class P>
    friend struct detail::kernel_param;
    explicit Scratch(T& scratch) noexcept
        : scratch_(&scratch) {}
    T* scratch_;
};

// Optional resource seeding every Random parameter in the world's schedules.
// Absent, a fixed default seed is used (runs are then reproducible across
// processes by default).
struct RandomSeed {
    std::uint64_t value = 0x9E37'79B9'7F4A'7C15ull;
};

// Per-item deterministic random stream (PCG32) for a kernel system. The
// stream is derived from (RandomSeed resource, schedule tick, system id, item
// ordinal) -- counter-based, no shared state -- so results are independent of
// lane count and iteration timing: the same world, seed, and tick produce the
// SAME numbers on 1 lane and N, run after run. (A serial ResMut<Rng> system
// can't promise that: its draw order is its iteration order.) Draws stream
// per item; a fresh tick or ordinal is a fresh, decorrelated stream.
class Random {
public:
    // Uniform bits / [0, 1) canonical floats.
    std::uint32_t u32() noexcept {
        std::uint64_t const old = state_;
        state_                  = old * 6364136223846793005ull + inc_;
        auto const xorshifted   = std::uint32_t(((old >> 18u) ^ old) >> 27u);
        auto const rot          = std::uint32_t(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }
    std::uint64_t u64() noexcept {
        return (std::uint64_t(u32()) << 32) | u32();
    }
    float f32() noexcept { return float(u32() >> 8) * 0x1.0p-24f; }
    double f64() noexcept { return double(u64() >> 11) * 0x1.0p-53; }
    // Uniform integer in [0, n) (n > 0). Cheap bounded form (negligible
    // modulo bias for game-sized n).
    std::uint32_t below(std::uint32_t n) noexcept { return u32() % n; }

private:
    template <class P>
    friend struct detail::kernel_param;
    Random(std::uint64_t seed, std::uint64_t seq) noexcept
        : inc_((seq << 1u) | 1u) {
        u32();
        state_ += seed;
        u32();
    }
    std::uint64_t state_ = 0;
    std::uint64_t inc_;
};

namespace detail {

    // SplitMix64 finalizer: decorrelates the (seed, tick, system, ordinal)
    // coordinates into stream seeds.
    inline constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
        x += 0x9E37'79B9'7F4A'7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58'476D'1CE4'E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D0'49BB'1331'11EBull;
        return x ^ (x >> 31);
    }

    // Reset a slot or target for the next run, keeping heap capacity where the
    // type offers it (vectors, maps, strings); assign a fresh value otherwise.
    template <class T>
    void reset_value(T& v) {
        if constexpr (requires { v.clear(); })
            v.clear();
        else
            v = T {};
    }

    template <class P>
    inline constexpr bool is_reduce_v = false;
    template <class T, class Op>
    inline constexpr bool is_reduce_v<Reduce<T, Op>> = true;
    template <class P>
    inline constexpr bool is_extract_v = false;
    template <class T>
    inline constexpr bool is_extract_v<Extract<T>> = true;

    // The kernel-parameter protocol: how each parameter type of a kernel
    // system declares access, owns per-item state, participates in the
    // prepare/finish barrier hooks, and binds for one work item. The primary
    // template covers every ordinary system parameter (stateless; binds
    // through system_param against the shared 1-lane pool). The kernel's
    // single Query parameter is NOT routed through here -- the executor binds
    // it sliced via query_param_traits (see Schedule::add_kernel).
    struct no_state {};

    template <class P>
    struct kernel_param {
        static constexpr bool allowed = SystemParam<P> && !is_res_mut_v<P>;
        using state                   = no_state;
        static void declare(SystemAccess& a) { system_param<P>::declare(a); }
        static void prepare(state&,
                            World&,
                            std::span<std::uint32_t const>,
                            KernelWaveContext const&) {}
        static void finish(state&, World&) {}
        static P bind(state&,
                      World& w,
                      Commands& c,
                      std::uint32_t /*archetype*/,
                      std::size_t /*begin*/,
                      std::size_t /*end*/,
                      std::uint32_t /*ordinal*/) {
            return system_param<P>::bind(w, c, parallel::serial_pool());
        }
    };

    template <class T, class Op>
    struct kernel_param<Reduce<T, Op>> {
        static constexpr bool allowed = true;
        // Slots are cache-line padded: items accumulate into their slot
        // throughout iteration, and adjacent small partials would otherwise
        // false-share across lanes. ~kTargetItemsPerKernel slots per system,
        // so the padding costs a few KB.
        struct alignas(128) Slot {
            T value {};
        };
        struct state {
            std::vector<Slot> slots;
            std::size_t active = 0;
        };

        static void declare(SystemAccess& a) { a.res_writes.push_back(resource_id<T>); }

        static void prepare(state& s,
                            World& w,
                            std::span<std::uint32_t const> rows,
                            KernelWaveContext const&) {
            s.active = rows.size();
            if (s.slots.size() < s.active)
                s.slots.resize(s.active);
            for (std::size_t i = 0; i < s.active; ++i)
                reset_value(s.slots[i].value);
            // The target holds THIS run's reduction: rebuilt every run, like
            // the serial rebuild-every-tick systems Reduce replaces.
            reset_value(w.resource<T>());
        }

        static void finish(state& s, World& w) {
            auto& target = w.resource<T>();
            Op op {};
            for (std::size_t i = 0; i < s.active; ++i)
                op(target, s.slots[i].value);
        }

        static Reduce<T, Op> bind(state& s,
                                  World&,
                                  Commands&,
                                  std::uint32_t,
                                  std::size_t,
                                  std::size_t,
                                  std::uint32_t ordinal) {
            return Reduce<T, Op>(s.slots[ordinal].value);
        }
    };

    template <class T>
    struct kernel_param<Extract<T>> {
        static constexpr bool allowed = true;
        struct state {
            std::vector<std::size_t> offsets; // per-ordinal global row offset
        };

        static void declare(SystemAccess& a) { a.res_writes.push_back(resource_id<T>); }

        static void prepare(state& s,
                            World& w,
                            std::span<std::uint32_t const> rows,
                            KernelWaveContext const&) {
            s.offsets.resize(rows.size());
            std::size_t total = 0;
            for (std::size_t i = 0; i < rows.size(); ++i) {
                s.offsets[i] = total;
                total += rows[i];
            }
            // Pre-size ONCE, before the dispatch: item spans stay stable for
            // the whole wave (vector capacity is retained across ticks, so the
            // steady state allocates nothing).
            w.resource<T>().resize(total);
        }

        static void finish(state&, World&) {} // writes were direct + disjoint

        static Extract<T> bind(state& s,
                               World& w,
                               Commands&,
                               std::uint32_t,
                               std::size_t b,
                               std::size_t e,
                               std::uint32_t ordinal) {
            auto& target = w.resource<T>();
            return Extract<T>(
                std::span(target.data() + s.offsets[ordinal], e - b));
        }
    };

    template <class T>
    struct kernel_param<Scratch<T>> {
        static constexpr bool allowed = true;
        // Padded for the same reason as Reduce slots: scratch is written
        // throughout iteration, and adjacent items run on different lanes.
        struct alignas(128) Slot {
            T value {};
        };
        struct state {
            std::vector<Slot> slots;
        };

        static void declare(SystemAccess&) {} // private state: no access

        static void prepare(state& s,
                            World&,
                            std::span<std::uint32_t const> rows,
                            KernelWaveContext const&) {
            if (s.slots.size() < rows.size())
                s.slots.resize(rows.size());
            for (std::size_t i = 0; i < rows.size(); ++i)
                reset_value(s.slots[i].value);
        }

        static void finish(state&, World&) {}

        static Scratch<T> bind(state& s,
                               World&,
                               Commands&,
                               std::uint32_t,
                               std::size_t,
                               std::size_t,
                               std::uint32_t ordinal) {
            return Scratch<T>(s.slots[ordinal].value);
        }
    };

    template <>
    struct kernel_param<Random> {
        static constexpr bool allowed = true;
        struct state {
            std::uint64_t stream_base = 0;
        };

        // Declares a READ of the RandomSeed resource: a system that (re)seeds
        // the world serializes ahead of every Random user, and Random users
        // still run concurrently with each other.
        static void declare(SystemAccess& a) {
            a.res_reads.push_back(resource_id<RandomSeed>);
        }

        static void prepare(state& s,
                            World& w,
                            std::span<std::uint32_t const>,
                            KernelWaveContext const& ctx) {
            auto const* seed = w.try_resource<RandomSeed>();
            auto const base  = seed ? seed->value : RandomSeed {}.value;
            s.stream_base    = mix64(base) ^ mix64(ctx.tick) ^ mix64(ctx.system);
        }

        static void finish(state&, World&) {}

        static Random bind(state& s,
                           World&,
                           Commands&,
                           std::uint32_t,
                           std::size_t,
                           std::size_t,
                           std::uint32_t ordinal) {
            return Random(mix64(s.stream_base ^ ordinal), ordinal);
        }
    };

    // Pack helpers over a kernel's full parameter tuple.
    template <class Args>
    struct kernel_params_info;
    template <class... A>
    struct kernel_params_info<std::tuple<A...>> {
        static constexpr bool all_allowed =
            ((is_query_param_v<A> || kernel_param<A>::allowed) && ...);
        static constexpr bool any_stateful =
            (!std::is_same_v<typename kernel_param<A>::state, no_state> || ...);
        using states = std::tuple<typename kernel_param<A>::state...>;
    };

    // Bind one kernel-system parameter for a work item: the (single) Query
    // binds restricted to the item's rows; everything else goes through the
    // kernel_param protocol above.
    template <class P, bool Sliced, class State>
    decltype(auto) kernel_bind(State& s,
                               World& w,
                               Commands& c,
                               std::uint32_t archetype,
                               std::size_t b,
                               std::size_t e,
                               std::uint32_t ordinal) {
        if constexpr (Sliced) {
            return query_param_traits<P>::bind_slice(w, archetype, b, e);
        } else {
            return kernel_param<P>::bind(s, w, c, archetype, b, e, ordinal);
        }
    }

    // Whole-parameter-list drivers, folded over the kernel's Args tuple.
    // Plain function templates (not nested generic lambdas): they are called
    // from closures in Schedule::add_kernel, and keeping them ordinary
    // functions keeps the instantiation shape simple.
    template <class A>
    void kernel_declare_one(SystemAccess& a) {
        if constexpr (is_query_param_v<A>)
            system_param<A>::declare(a);
        else
            kernel_param<A>::declare(a);
    }
    template <class Args, std::size_t... I>
    void kernel_declare(SystemAccess& a, std::index_sequence<I...>) {
        (kernel_declare_one<std::tuple_element_t<I, Args>>(a), ...);
    }

    template <class Args, class States, std::size_t... I>
    void kernel_prepare_all(States& st,
                            World& w,
                            std::span<std::uint32_t const> rows,
                            KernelWaveContext const& ctx,
                            std::index_sequence<I...>) {
        (kernel_param<std::tuple_element_t<I, Args>>::prepare(
             std::get<I>(st), w, rows, ctx),
         ...);
    }
    template <class Args, class States, std::size_t... I>
    void kernel_finish_all(States& st, World& w, std::index_sequence<I...>) {
        (kernel_param<std::tuple_element_t<I, Args>>::finish(std::get<I>(st), w), ...);
    }

    template <class Args, std::size_t QI, class Fn, class States, std::size_t... I>
    void kernel_invoke(Fn& fn,
                       States& st,
                       World& w,
                       Commands& c,
                       std::uint32_t archetype,
                       std::size_t b,
                       std::size_t e,
                       std::uint32_t ordinal,
                       std::index_sequence<I...>) {
        fn(kernel_bind<std::tuple_element_t<I, Args>, I == QI>(
            std::get<I>(st), w, c, archetype, b, e, ordinal)...);
    }

} // namespace detail
} // namespace ecs
