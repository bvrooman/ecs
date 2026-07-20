// ecs/world.hpp
//
// The World owns all entities and archetypes and drives the dynamic-archetype
// transitions: adding or removing a component moves an entity's data from its
// current archetype table into the one whose signature matches the new set.

#pragma once

#include "archetype.hpp"
#include "command_buffer.hpp"
#include "detail/id_vector.hpp"
#include "detail/sparse_handle_vector.hpp"
#include "entity.hpp"
#include "resource.hpp"
#include "world_id.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <memory>
#include <mutex>
#include <numeric>
#include <ranges>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ecs {

namespace detail {

    // True iff the types in the pack are pairwise distinct.
    template <class...>
    struct are_distinct : std::true_type {};
    template <class T, class... Ts>
    struct are_distinct<T, Ts...>
        : std::bool_constant<!std::disjunction_v<std::is_same<T, Ts>...> &&
                             are_distinct<Ts...>::value> {};
    template <class... Ts>
    inline constexpr bool are_distinct_v = are_distinct<Ts...>::value;

    // The kernel-parameter protocol (schedule/params/protocol.hpp), forward-
    // declared so Commands can befriend its Commands& specialization (per-item
    // command recording for kernel systems).
    template <class P>
    struct kernel_param;
} // namespace detail

// Runtime (JS-host-driven) component operations live in dynamic/world_ops.hpp and
// reuse World's generic archetype machinery via friendship -- forward-declared
// here so World can befriend it without depending on the dynamic module.
namespace dynamic {
    struct WorldOps;
}

template <class... Cs>
class Query; // defined in query.hpp; befriended for mutable archetype access

class World {
public:
    World() {
        // Archetype 0 is the empty archetype (entities with no components).
        empty_archetype_ = make_archetype(Signature {}, [](Archetype&) {});
    }

    World(World const&)            = delete;
    World& operator=(World const&) = delete;

    // --- reads & lifecycle queries ----------------------------------------
    // World exposes only reads. All mutation (spawn/destroy/add/remove/set)
    // goes through a `Commands` object, which a running Schedule creates and
    // passes to each system -- so mutation is reachable only inside a system,
    // enforced at compile time rather than by a runtime check.
    bool alive(Entity const e) const { return entities_.is_alive(e); }

    std::size_t size() const noexcept { return alive_count_; }

    // Capacity hint: pre-size the entity record table for n entities. Purely an
    // allocation optimization for bulk setup (per-archetype component storage
    // is pre-grown separately via Archetype::reserve).
    void reserve_entities(std::size_t n) { entities_.reserve_capacity(n); }

    // --- maintenance: row sorting ------------------------------------------
    // Stable-sort the rows of every archetype containing C by key(component
    // value) -- a data-LAYOUT operation. Work items and chunk splits slice
    // contiguous ROW ranges, so making row order correlate with the key a
    // kernel scatters by (e.g. a spatial cell index) is what lets per-item
    // partials compress (see docs/IMPROVEMENTS.md, parallel-primitives
    // section); neighborhood-reading kernels gain cache locality as a bonus.
    //
    // Entity handles remain valid (their records are patched); what changes
    // is iteration/serial-walk order -- deterministically: a stable sort by a
    // canonical key is the same permutation at any lane count, so schedule
    // results stay bitwise reproducible across sorts. Rows drift back out of
    // order via swap-and-pop churn; re-sort every N ticks (key coherence
    // decays slowly, the sort amortizes). Structural, like the *_now
    // primitives: call it OUTSIDE run(), or from inside a system by recording
    // it as a command (Commands::sort) so it applies at the barrier -- never
    // directly mid-wave.
    //
    // `min_disorder` skips archetypes that are still nearly in order: the
    // disorder of an archetype is its fraction of adjacent key DESCENTS
    // (0 = sorted, ~1 = reversed; drift from churn accumulates it slowly),
    // and the sort runs only when disorder > min_disorder. The default 0
    // sorts whenever any pair is out of order.
    //
    // Integral keys with a compact range (a grid-cell index; range up to a
    // few times the row count) take a counting sort -- measured ~60x cheaper
    // than the comparison sort on 40k rows -- and fall back to stable_sort
    // otherwise; both are stable, so both produce the same canonical
    // permutation.
    template <class C, class KeyFn>
    void sort_rows(KeyFn key, double const min_disorder = 0.0) {
        std::vector<std::uint32_t> perm;
        for (auto const& arch_ptr : archetypes_) {
            auto& arch = *arch_ptr;
            if (!arch.has(component_id<C>) || arch.size() < 2)
                continue;
            auto const& store = arch.template column<C>().store;
            auto const n      = arch.size();
            using Key         = decltype(key(store.gather(0)));

            // One pass: gather keys, count adjacent descents, track range.
            std::vector<Key> keys(n);
            std::size_t descents = 0;
            keys[0]              = key(store.gather(0));
            Key lo = keys[0], hi = keys[0];
            for (std::size_t r = 1; r < n; ++r) {
                keys[r] = key(store.gather(r));
                if (keys[r] < keys[r - 1])
                    ++descents;
                lo = std::min(lo, keys[r]);
                hi = std::max(hi, keys[r]);
            }
            if (descents == 0) // already in key order
                continue;
            if (double(descents) / double(n - 1) <= min_disorder)
                continue; // still coherent enough to not be worth a permute

            perm.resize(n);
            bool counted = false;
            if constexpr (std::integral<Key>) {
                // Counting sort when the bucket table stays proportionate to
                // the work (covers the grid-cell case with lots of headroom).
                auto const range = std::uint64_t(hi) - std::uint64_t(lo) + 1;
                if (range <= std::max<std::uint64_t>(4 * n, 1024)) {
                    std::vector<std::uint32_t> starts(range + 1, 0);
                    for (auto const k : keys)
                        ++starts[std::uint64_t(k) - std::uint64_t(lo) + 1];
                    for (std::size_t b = 1; b <= range; ++b)
                        starts[b] += starts[b - 1];
                    for (std::uint32_t r = 0; r < n; ++r)
                        perm[starts[std::uint64_t(keys[r]) - std::uint64_t(lo)]++] =
                            r; // ascending r per bucket: stable
                    counted = true;
                }
            }
            if (!counted) {
                std::ranges::iota(perm, 0u);
                std::ranges::stable_sort(perm, {}, [&](std::uint32_t const r) {
                    return keys[r];
                });
            }
            arch.permute_rows(perm);
            for (std::uint32_t r = 0; r < n; ++r)
                entities_[arch.entities[r]].row = r;
        }
    }

    // --- reads ------------------------------------------------------------
    template <class C>
    bool has(Entity const e) const {
        return alive(e) && archetypes_[entities_[e].archetype]->has(component_id<C>);
    }

    // Read a component by value (gathered from its per-field columns).
    // Precondition: has<C>(e). In debug this is asserted; otherwise the missing
    // column lookup is caught by column_at's assert / yields UB in release like
    // any other precondition violation.
    template <class C>
    C get(Entity const e) const {
        assert(has<C>(e) && "World::get<C>(e): entity has no component C");
        auto const& rec = entities_[e];
        return archetypes_[rec.archetype]->column<C>().store.gather(rec.row);
    }

    // --- resources (singletons not owned by any entity) -------------------
    // Set up resources before running a schedule; like structural edits, do not
    // add/remove resources while a schedule is executing.
    template <class T, class... Args>
    T& emplace_resource(Args&&... args) {
        return resources_.emplace<T>(std::forward<Args>(args)...);
    }
    template <class T>
    T& resource() {
        return resources_.get<T>();
    }
    template <class T>
    T const& resource() const {
        return resources_.get<T>();
    }
    template <class T>
    T* try_resource() {
        return resources_.try_get<T>();
    }
    template <class T>
    bool has_resource() const {
        return resources_.contains<T>();
    }
    template <class T>
    void remove_resource() {
        resources_.remove<T>();
    }

    // --- archetype access (used by queries) -------------------------------
    // Read-only. The mutable view is private: exposing it publicly would let
    // any World& holder structurally mutate mid-wave, bypassing the
    // Commands-only invariant the concurrency model rests on. Queries (which
    // need mutable column access for declared writes) are friends.
    auto const& archetypes() const { return archetypes_; }

    // Bumped whenever an archetype is created (only at flush) -- lets a query
    // memoize its matching_archetypes() list and skip the locked cache lookup
    // while the set of archetypes is unchanged (Query does this per type, the
    // schedule executor per kernel system).
    std::uint64_t archetype_generation() const noexcept {
        return archetype_gen_.load(std::memory_order_acquire);
    }
    // Process-unique id for this World object, so a match-list memo cannot
    // confuse two Worlds (or a new World reusing a destroyed one's address).
    WorldId instance_id() const noexcept { return instance_id_; }

    // Indices of the archetypes whose signature contains all of `required`
    // (sorted). Cached per required-signature and kept current as archetypes
    // are created, so a repeated query does not re-scan every archetype.
    // Thread-safe: parallel systems may build/read caches concurrently. The
    // returned reference stays valid after the lock is released (unordered_map
    // element references survive rehash) and the vector is only appended at
    // flush, never mid-wave.
    auto const& matching_archetypes(Signature const& required) const {
        auto lock = std::lock_guard(query_cache_mutex_);
        if (auto const it = query_cache_.find(required); it != query_cache_.end())
            return it->second;
        auto matches = std::vector<ArchetypeId> {};
        for (std::uint32_t i = 0; i < archetypes_.size(); ++i) {
            ArchetypeId const id {i};
            if (archetypes_[id]->signature.includes(required))
                matches.push_back(id);
        }
        return query_cache_.emplace(required, std::move(matches)).first->second;
    }

private:
    friend class Commands;           // the mutation API; records into commands_
    friend class Schedule;           // creates Commands and flushes at barriers
    friend struct dynamic::WorldOps; // runtime component path (dynamic/world_ops.hpp)
    template <class... Cs>
    friend class Query; // needs mutable columns for its declared writes

    // Mutable archetype access for the friends above.
    auto& archetypes() { return archetypes_; }

    // An entity's location: which archetype table holds it and its row there.
    // Stored as the payload of entities_ (a HandleVector), which owns the
    // liveness flag and generation, so a Record no longer carries either.
    struct Record {
        ArchetypeId archetype {};
        std::uint32_t row = 0;
    };

    // Apply all recorded commands; driven by the Schedule at each barrier.
    void apply_commands(FlushAttrib* attrib = nullptr) { commands_.apply(*this, attrib); }

    // Drop all recorded commands unapplied; driven by the Schedule when a run
    // aborts, so a failed run's edits cannot leak into the next run's flush.
    void discard_commands() noexcept { commands_.discard(); }

    // Immediate mutation primitives, called only by the command closures at
    // flush time -- so they always run with exclusive, single-threaded access.
    void destroy_now(Entity const e) {
        if (!alive(e))
            return;
        auto const& rec = entities_[e];
        remove_row(*archetypes_[rec.archetype], rec.row);
        // entities_.destroy() bumps the generation, recycles the slot, and
        // swap-pops the Record from the dense array -- so use `rec` before it.
        entities_.destroy(e);
        --alive_count_;
    }

    template <class C>
    void add_now(Entity const e, C value) {
        assert(alive(e));
        auto& rec      = entities_[e];
        auto const cid = component_id<C>;
        auto& a        = *archetypes_[rec.archetype]; // heap-stable across growth
        if (a.has(cid)) {
            a.column<C>().store.set(rec.row, std::move(value));
            return;
        }
        // Steady state: the destination is cached on the archetype's add edge,
        // so the transition costs one hash hit -- no signature vector build, no
        // allocation, no global index lookup.
        ArchetypeId to;
        if (auto const it = a.add_edge.find(cid); it != a.add_edge.end()) {
            to = it->second;
        } else {
            auto const from = rec.archetype;
            to = get_or_create_archetype(a.signature.with(cid), [&](Archetype& b) {
                auto& src = *archetypes_[from];
                b.columns.reserve(b.signature.size());
                for (auto const id : b.signature)
                    b.columns.push_back(id == cid ? std::make_unique<Column<C>>()
                                                  : src.column_at(id).clone_empty());
            });
            a.add_edge.emplace(cid, to);
        }
        relocate(e, to, [&](Archetype& b) {
            b.column<C>().store.push_back(std::move(value));
        });
    }

    template <class C>
    void remove_now(Entity e) {
        assert(alive(e));
        auto& rec      = entities_[e];
        auto const cid = component_id<C>;
        auto& a        = *archetypes_[rec.archetype];
        if (!a.has(cid))
            return;

        ArchetypeId to;
        if (auto const it = a.remove_edge.find(cid); it != a.remove_edge.end()) {
            to = it->second;
        } else {
            auto const from = rec.archetype;
            to = get_or_create_archetype(a.signature.without(cid), [&](Archetype& b) {
                auto& src = *archetypes_[from];
                b.columns.reserve(b.signature.size());
                for (auto const id : b.signature)
                    b.columns.push_back(src.column_at(id).clone_empty());
            });
            a.remove_edge.emplace(cid, to);
        }
        relocate(e, to, [](Archetype&) {});
    }

    template <class C>
    void set_now(Entity const e, C value) {
        // A set command can outlive its entity if a destroy was recorded earlier
        // in the same flush; skip a dead/stale handle rather than dereferencing a
        // recycled slot (which reads through an invalidated record index).
        if (!alive(e))
            return;
        auto const& rec = entities_[e];
        archetypes_[rec.archetype]->column<C>().store.set(rec.row, std::move(value));
    }

    // Hand out a fresh entity handle without creating storage. Thread-safe and
    // lock-free: entities_.reserve() claims a slot (recycled with its bumped
    // generation, or freshly minted at generation 0) without touching the dense
    // record table, which spawn_now materializes at the barrier.
    Entity reserve() { return entities_.reserve(); }

    // Place a reserved entity directly into its final archetype with all of its
    // components in one step -- no empty-archetype materialization and no
    // per-component relocations (which would also leave empty intermediate
    // archetypes behind). Runs single-threaded, from a spawn command at a
    // barrier. Component types must be distinct.
    template <class... Cs>
    void spawn_now(Entity e, Cs... comps) {
        static_assert(detail::are_distinct_v<Cs...>, "spawn(): duplicate component type");
        // The signature for a given component set is constant (component ids are
        // stable after first use), so build it once per instantiation rather
        // than allocating a Signature vector on every spawn -- the last
        // per-spawn heap allocation on the steady-state path. Same idiom as
        // Query::required().
        static Signature const sig = Signature {component_id<Cs>...};
        auto const to              = get_or_create_archetype(sig, [](Archetype& b) {
            // One column per component, ordered to match the sorted signature.
            std::array<std::pair<ComponentId, std::unique_ptr<IColumn>>, sizeof...(Cs)>
                cols {std::pair<ComponentId, std::unique_ptr<IColumn>> {
                    component_id<Cs>,
                    std::make_unique<Column<Cs>>()}...};
            std::ranges::sort(cols, {}, [](auto const& p) { return p.first; });
            b.columns.reserve(cols.size());
            for (auto& [id, col] : cols)
                b.columns.push_back(std::move(col));
        });

        // Storage first, record bookkeeping second: if a column push throws,
        // roll the archetype back to a consistent state (every column as long
        // as `entities`) and leave the entity uncommitted (still not alive),
        // rather than registering a half-materialized entity.
        auto& a            = *archetypes_[to];
        auto const new_row = static_cast<std::uint32_t>(a.entities.size());
        try {
            (a.template column<Cs>().store.push_back(std::move(comps)), ...);
            a.entities.push_back(e);
        } catch (...) {
            rollback_overlong_columns(a);
            throw;
        }

        // Materialize the reserved handle: places the Record in the dense table,
        // marks the slot alive, and grows the sparse array to cover a minted
        // index.
        entities_.commit(e, Record {to, new_row});
        ++alive_count_;
    }

    template <class Factory>
    auto make_archetype(Signature const& sig, Factory&& makeColumns) -> ArchetypeId {
        auto arch       = std::make_unique<Archetype>();
        arch->signature = sig;
        makeColumns(*arch);
        // Publish the archetype (push() assigns its id = its position) before
        // indexing it: if the index insertion ran first and the append then
        // threw, sig_index_ would map this signature to an out-of-bounds slot
        // forever.
        auto const idx = archetypes_.push_back(std::move(arch));
        try {
            sig_index_.emplace(sig, idx);
        } catch (...) {
            archetypes_.pop_back();
            throw;
        }
        // Extend any existing query caches the new archetype matches (runs at
        // flush, single-threaded; queries only read caches during a wave).
        {
            auto lock = std::lock_guard(query_cache_mutex_);
            for (auto& [required, list] : query_cache_)
                if (sig.includes(required))
                    list.push_back(idx);
        }
        archetype_gen_.fetch_add(1, std::memory_order_release);
        return idx;
    }

    template <class Factory>
    auto get_or_create_archetype(Signature const& sig, Factory&& makeColumns) {
        if (auto const it = sig_index_.find(sig); it != sig_index_.end())
            return it->second;
        return make_archetype(sig, std::forward<Factory>(makeColumns));
    }

    // Drop a row from an archetype with swap-and-pop, patching the relocated
    // entity's record. Heap-allocated archetypes keep stable addresses, so
    // references stay valid across archetypes_ growth.
    void remove_row(Archetype& a, std::uint32_t const row) {
        auto const last = static_cast<std::uint32_t>(a.entities.size() - 1);
        for (auto const& col : a.columns)
            col->swap_remove(row);
        if (row != last) {
            a.entities[row]                = a.entities[last];
            entities_[a.entities[row]].row = row;
        }
        a.entities.pop_back();
    }

    // Move an entity from its current archetype into `to`, copying shared
    // columns and letting `addExtra` append any newly-added component.
    // Transactional on `b`: nothing is removed from `a` until every column of
    // `b` has the new row, so a mid-move throw rolls `b` back and leaves the
    // entity untouched in `a` (no silent column desynchronization).
    template <class AddExtra>
    void relocate(Entity const e, auto const to, AddExtra&& addExtra) {
        auto& rec       = entities_[e];
        auto const from = rec.archetype;
        auto& a         = *archetypes_[from];
        auto& b         = *archetypes_[to];

        try {
            // Both signatures are sorted: shared columns via two-pointer merge
            // over column slots (a signature position is its ColumnId).
            for (ColumnId ia {0}, ib {0};
                 ia.value < a.signature.size() && ib.value < b.signature.size();) {
                if (a.signature[ia] < b.signature[ib])
                    ++ia;
                else if (b.signature[ib] < a.signature[ia])
                    ++ib;
                else {
                    a.columns[ia]->move_row_to(*b.columns[ib], rec.row);
                    ++ia;
                    ++ib;
                }
            }
            addExtra(b);
            b.entities.push_back(e);
        } catch (...) {
            rollback_overlong_columns(b);
            throw;
        }
        auto const new_row = static_cast<std::uint32_t>(b.entities.size() - 1);

        remove_row(a, rec.row);
        rec.archetype = to;
        rec.row       = new_row;
    }

    // Pop any column row appended beyond `entities.size()` -- the unwind path
    // of spawn_now/relocate, restoring the "every column as long as entities"
    // invariant after a partial append.
    static void rollback_overlong_columns(Archetype& a) noexcept {
        for (auto const& col : a.columns)
            while (col->size() > a.entities.size())
                col->swap_remove(col->size() - 1);
    }

    CommandBuffer commands_;
    ResourceRegistry resources_;
    detail::IdVector<ArchetypeId, std::unique_ptr<Archetype>> archetypes_;
    std::unordered_map<Signature, ArchetypeId> sig_index_;
    // required-signature -> matching archetype indices (lazy, append-only).
    mutable std::unordered_map<Signature, std::vector<ArchetypeId>> query_cache_;
    mutable std::mutex query_cache_mutex_;
    // The entity table: a generational handle container keyed by Entity. It owns
    // slot lifecycle (lock-free reserve() during a wave, commit()/destroy() at
    // the barrier), the alive flag, and the generation; the payload is a Record
    // (archetype + row). Entity is detail::Handle<EntityTag>.
    detail::SparseHandleVector<Record, EntityTag> entities_;
    ArchetypeId empty_archetype_ = {};
    std::size_t alive_count_     = 0;

    WorldId const instance_id_ = WorldId::next();
    std::atomic<std::uint64_t> archetype_gen_ {0};
};

// ===========================================================================
//  Commands: the mutation API. A running Schedule creates one bound to the
//  World and passes it to every system, so spawn/destroy/add/remove/set are
//  reachable only from inside a system (or a schedule's setup). All of them are
//  deferred -- recorded into the command buffer and applied at the next flush.
// ===========================================================================
class Commands {
public:
    Commands(Commands const&)            = delete;
    Commands& operator=(Commands const&) = delete;
    Commands(Commands&&)                 = delete;
    Commands& operator=(Commands&&)      = delete;

    // spawn returns a usable handle immediately (alive after the next flush).
    // At flush the entity is placed directly into its final archetype.
    // Perfect-forwarded: an rvalue component moves into the capture and again
    // into storage at flush -- no copies for heap-owning fields.
    template <class... Cs>
    Entity spawn(Cs&&... comps) {
        Entity const e = world_.reserve();
        record([e, ... comps = std::forward<Cs>(comps)](World& w) mutable {
            w.spawn_now<std::decay_t<Cs>...>(e, std::move(comps)...);
        });
        return e;
    }

    void destroy(Entity e) {
        record([e](World& w) { w.destroy_now(e); });
    }

    // Add component C to an entity (moving it to the matching archetype). If
    // the entity already has C, its value is overwritten in place. No-ops if
    // dead.
    template <class C>
    void add(Entity e, C&& value) {
        record([e, value = std::forward<C>(value)](World& w) mutable {
            if (w.alive(e))
                w.add_now<std::decay_t<C>>(e, std::move(value));
        });
    }

    template <class C>
    void remove(Entity e) {
        record([e](World& w) {
            if (w.alive(e))
                w.remove_now<C>(e);
        });
    }

    // Overwrite component C on an entity. No-ops if the entity is dead or does
    // not currently have C (so a stale target never throws at flush time).
    template <class C>
    void set(Entity e, C&& value) {
        record([e, value = std::forward<C>(value)](World& w) mutable {
            using D = std::decay_t<C>;
            if (w.alive(e) && w.has<D>(e))
                w.set_now<D>(e, std::move(value));
        });
    }

    // Reorder every C-bearing archetype's rows by a canonical key at the next
    // flush -- a deferred World::sort_rows. sort_rows is structural (it permutes
    // rows, like the *_now edits) so it cannot run mid-wave; recording it as a
    // command runs it single-threaded and world-quiescent at the barrier, the
    // same place spawns/despawns apply. That is what lets an ordinary system ask
    // for data-layout upkeep (e.g. re-sort by grid cell every N ticks) without
    // the raw World& sort_rows would otherwise need. Deterministic; see
    // World::sort_rows for the key / min_disorder contract.
    template <class C, class KeyFn>
    void sort(KeyFn key, double min_disorder = 0.0) {
        record([key = std::move(key), min_disorder](World& w) {
            w.sort_rows<C>(key, min_disorder);
        });
    }

private:
    friend class Schedule; // constructs Commands during run
    template <class P>
    friend struct detail::kernel_param; // per-item recording for kernel systems
    explicit Commands(World& w)
        : world_(w) {}
    // A kernel work item's Commands records into its own private store (no
    // lock, deterministic replay order) instead of the world's thread-sharded
    // buffer; kernel_param<Commands&> owns the stores and enqueues them at
    // the wave barrier (see schedule/params/commands.hpp).
    Commands(World& w, CommandStore& local)
        : world_(w)
        , local_(&local) {}
    template <class F>
    void record(F&& f) {
        if (local_)
            local_->record(std::forward<F>(f));
        else
            world_.commands_.record(std::forward<F>(f));
    }
    // Barrier-time (single-threaded): splice this item's recorded commands
    // into the wave's flush by enqueueing one replay command that drains the
    // private store in record order. Enqueued from one thread in ordinal
    // order, so kernel-recorded edits apply in canonical order.
    void enqueue_local() {
        world_.commands_.record([s = local_](World& w) { s->apply(w); });
    }
    World& world_;
    CommandStore* local_ = nullptr;
};

// Typed resource access for system parameters: Res<T> is a read of resource T,
// ResMut<T> a write. They resolve the resource once at construction. The
// scheduler derives a system's declared resource access from these parameter
// types (see schedule.hpp), so the declaration cannot drift from actual use.
template <class T>
class Res {
public:
    explicit Res(World& w)
        : ptr_(&w.resource<T>()) {}
    T const& operator*() const noexcept { return *ptr_; }
    T const* operator->() const noexcept { return ptr_; }
    T const& get() const noexcept { return *ptr_; }

private:
    T const* ptr_;
};

template <class T>
class ResMut {
public:
    explicit ResMut(World& w)
        : ptr_(&w.resource<T>()) {}
    T& operator*() noexcept { return *ptr_; }
    T* operator->() noexcept { return ptr_; }
    T& get() noexcept { return *ptr_; }

private:
    T* ptr_;
};

} // namespace ecs
