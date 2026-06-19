// ecs/world.hpp
//
// The World owns all entities and archetypes and drives the dynamic-archetype
// transitions: adding or removing a component moves an entity's data from its
// current archetype table into the one whose signature matches the new set.

#pragma once

#include "archetype.hpp"
#include "command_buffer.hpp"
#include "entity.hpp"
#include "resource.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
#include <ranges>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ecs {

namespace detail {
    struct SigHash {
        std::size_t operator()(Signature const& s) const noexcept {
            return hash_signature(s);
        }
    };

    // True iff the types in the pack are pairwise distinct.
    template <class...>
    struct are_distinct : std::true_type {};
    template <class T, class... Ts>
    struct are_distinct<T, Ts...>
        : std::bool_constant<!std::disjunction_v<std::is_same<T, Ts>...> &&
                             are_distinct<Ts...>::value> {};
    template <class... Ts>
    inline constexpr bool are_distinct_v = are_distinct<Ts...>::value;
} // namespace detail

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
    bool alive(Entity const e) const {
        return e.index < records_.size() && records_[e.index].alive &&
               records_[e.index].generation == e.generation;
    }

    std::size_t size() const noexcept { return alive_count_; }

    // --- reads ------------------------------------------------------------
    template <class C>
    bool has(Entity const e) const {
        return alive(e) && archetypes_[records_[e.index].archetype]->has(component_id<C>);
    }

    // Read a component by value (gathered from its per-field columns).
    // Precondition: has<C>(e). In debug this is asserted; otherwise the missing
    // column lookup throws std::out_of_range.
    template <class C>
    C get(Entity const e) const {
        assert(has<C>(e) && "World::get<C>(e): entity has no component C");
        auto const& rec = records_[e.index];
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
    auto const& archetypes() const { return archetypes_; }
    auto& archetypes() { return archetypes_; }

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
        auto matches = std::vector<std::uint32_t> {};
        for (auto i = 0; i < archetypes_.size(); ++i)
            if (includes_signature(archetypes_[i]->signature, required))
                matches.push_back(i);
        return query_cache_.emplace(required, std::move(matches)).first->second;
    }

private:
    friend class Commands; // the mutation API; records into commands_
    friend class Schedule; // creates Commands and flushes at barriers

    struct Record {
        std::uint32_t archetype  = 0;
        std::uint32_t row        = 0;
        std::uint32_t generation = 0;
        bool alive               = false;
    };

    // Apply all recorded commands; driven by the Schedule at each barrier.
    void apply_commands() { commands_.apply(*this); }

    // Immediate mutation primitives, called only by the command closures at
    // flush time -- so they always run with exclusive, single-threaded access.
    void destroy_now(Entity const e) {
        if (!alive(e))
            return;
        auto& [archetype, row, generation, alive] = records_[e.index];
        remove_row(*archetypes_[archetype], row);
        alive = false;
        ++generation;
        free_.push_back(e.index);
        free_count_.store(free_.size(), std::memory_order_release);
        --alive_count_;
    }

    template <class C>
    void add_now(Entity const e, C value) {
        assert(alive(e));
        auto& rec      = records_[e.index];
        auto const cid = component_id<C>;
        if (archetypes_[rec.archetype]->has(cid)) {
            archetypes_[rec.archetype]->column<C>().store.set(rec.row, value);
            return;
        }
        auto sig = archetypes_[rec.archetype]->signature;
        sig.insert(std::ranges::upper_bound(sig, cid), cid);

        auto const from = rec.archetype;
        auto const to   = get_or_create_archetype(sig, [&](Archetype& b) {
            for (auto& [id, col] : archetypes_[from]->columns)
                b.columns.emplace(id, col->clone_empty());
            b.columns.emplace(cid, std::make_unique<Column<C>>());
        });

        relocate(e, to, [&](Archetype& b) { b.column<C>().store.push_back(value); });
    }

    template <class C>
    void remove_now(Entity e) {
        assert(alive(e));
        auto const& rec = records_[e.index];
        auto const cid  = component_id<C>;
        if (!archetypes_[rec.archetype]->has(cid))
            return;

        auto sig = Signature {};
        sig.reserve(archetypes_[rec.archetype]->signature.size() - 1);
        for (auto id : archetypes_[rec.archetype]->signature)
            if (id != cid)
                sig.push_back(id);

        auto const from = rec.archetype;
        auto const to   = get_or_create_archetype(sig, [&](Archetype& b) {
            for (auto& [id, col] : archetypes_[from]->columns)
                if (id != cid)
                    b.columns.emplace(id, col->clone_empty());
        });

        relocate(e, to, [](Archetype&) {});
    }

    template <class C>
    void set_now(Entity const e, C const& value) {
        auto const& rec = records_[e.index];
        archetypes_[rec.archetype]->column<C>().store.set(rec.row, value);
    }

    // Hand out a fresh entity handle without creating storage. Thread-safe and
    // lock-free on the common (growth) path: a brand-new index is a single
    // atomic fetch_add, so many systems can reserve concurrently while
    // recording spawn commands. Recycling a freed slot takes a short lock;
    // reused indices carry the slot's already-bumped generation.
    Entity reserve() {
        if (free_count_.load(std::memory_order_acquire) > 0) {
            auto lock = std::lock_guard(reserve_mutex_);
            if (!free_.empty()) {
                auto const idx = free_.back();
                free_.pop_back();
                free_count_.store(free_.size(), std::memory_order_release);
                return Entity {idx, records_[idx].generation};
            }
        }
        return Entity {reserve_high_.fetch_add(1, std::memory_order_relaxed), 0};
    }

    // Place a reserved entity directly into its final archetype with all of its
    // components in one step -- no empty-archetype materialization and no
    // per-component relocations (which would also leave empty intermediate
    // archetypes behind). Runs single-threaded, from a spawn command at a
    // barrier. Component types must be distinct.
    template <class... Cs>
    void spawn_now(Entity e, Cs... comps) {
        static_assert(detail::are_distinct_v<Cs...>, "spawn(): duplicate component type");
        auto sig = Signature {component_id<Cs>...};
        std::ranges::sort(sig);
        auto const to = get_or_create_archetype(sig, [](Archetype& b) {
            (b.columns.emplace(component_id<Cs>, std::make_unique<Column<Cs>>()), ...);
        });

        if (e.index >= records_.size())
            records_.resize(e.index + 1);
        auto& [archetype, row, generation, alive] = records_[e.index];

        alive      = true;
        generation = e.generation;
        archetype  = to;

        auto& a = *archetypes_[to];
        row     = a.entities.size();
        a.entities.push_back(e);
        (a.template column<Cs>().store.push_back(comps), ...);
        ++alive_count_;
    }

    template <class Factory>
    auto make_archetype(Signature const& sig, Factory&& makeColumns) -> std::uint32_t {
        auto const idx  = archetypes_.size();
        auto arch       = std::make_unique<Archetype>();
        arch->signature = sig;
        makeColumns(*arch);
        sig_index_.emplace(sig, idx);
        archetypes_.push_back(std::move(arch));
        // Extend any existing query caches the new archetype matches (runs at
        // flush, single-threaded; queries only read caches during a wave).
        auto lock = std::lock_guard(query_cache_mutex_);
        for (auto& [required, list] : query_cache_)
            if (includes_signature(sig, required))
                list.push_back(idx);
        return idx;
    }

    static bool includes_signature(Signature const& super, Signature const& sub) {
        return std::ranges::includes(super, sub);
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
        for (auto const& col : a.columns | std::views::values)
            col->swap_remove(row);
        if (row != last) {
            a.entities[row]                     = a.entities[last];
            records_[a.entities[row].index].row = row;
        }
        a.entities.pop_back();
    }

    // Move an entity from its current archetype into `to`, copying shared
    // columns and letting `addExtra` append any newly-added component.
    template <class AddExtra>
    void relocate(Entity const e, auto const to, AddExtra&& addExtra) {
        auto& rec       = records_[e.index];
        auto const from = rec.archetype;
        auto& a         = *archetypes_[from];
        auto& b         = *archetypes_[to];

        for (auto& [id, col] : a.columns) {
            if (auto const it = b.columns.find(id); it != b.columns.end())
                col->move_row_to(*it->second, rec.row);
        }
        addExtra(b);

        auto const new_row = b.entities.size();
        b.entities.push_back(e);

        remove_row(a, rec.row);
        rec.archetype = to;
        rec.row       = new_row;
    }

    CommandBuffer commands_;
    ResourceRegistry resources_;
    std::vector<std::unique_ptr<Archetype>> archetypes_;
    std::unordered_map<Signature, std::uint32_t, detail::SigHash> sig_index_;
    // required-signature -> matching archetype indices (lazy, append-only).
    mutable std::unordered_map<Signature, std::vector<std::uint32_t>, detail::SigHash>
        query_cache_;
    mutable std::mutex query_cache_mutex_;
    std::vector<Record> records_;
    std::vector<std::uint32_t> free_;
    std::uint32_t empty_archetype_ = 0;
    std::size_t alive_count_       = 0;

    std::mutex reserve_mutex_; // guards free-list recycling in reserve()
    std::atomic<std::uint32_t> reserve_high_ {0}; // next brand-new index; ==
                                                  // records_.size() when no
                                                  // reservations are outstanding
    std::atomic<std::size_t> free_count_ {0};     // free_.size(), for a lock-free
                                                  // "anything to recycle?" check
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
    template <class... Cs>
    Entity spawn(Cs... comps) {
        Entity const e = world_.reserve();
        world_.commands_.record([e, ... comps = std::move(comps)](World& w) mutable {
            w.spawn_now<Cs...>(e, std::move(comps)...);
        });
        return e;
    }

    void destroy(Entity e) {
        world_.commands_.record([e](World& w) { w.destroy_now(e); });
    }

    // Add component C to an entity (moving it to the matching archetype). If
    // the entity already has C, its value is overwritten in place. No-ops if
    // dead.
    template <class C>
    void add(Entity e, C value) {
        world_.commands_.record([e, value = std::move(value)](World& w) mutable {
            if (w.alive(e))
                w.add_now<C>(e, std::move(value));
        });
    }

    template <class C>
    void remove(Entity e) {
        world_.commands_.record([e](World& w) {
            if (w.alive(e))
                w.remove_now<C>(e);
        });
    }

    // Overwrite component C on an entity. No-ops if the entity is dead or does
    // not currently have C (so a stale target never throws at flush time).
    template <class C>
    void set(Entity e, C value) {
        world_.commands_.record([e, value = std::move(value)](World& w) mutable {
            if (w.alive(e) && w.has<C>(e))
                w.set_now<C>(e, std::move(value));
        });
    }

private:
    friend class Schedule; // constructs Commands during run
    explicit Commands(World& w)
        : world_(w) {}
    World& world_;
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
