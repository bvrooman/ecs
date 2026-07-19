// ecs/detail/handle_vector.hpp
//
// A generational handle container: hands out stable Handles {index, generation}
// while storing the payload T contiguously (dense) for cache-friendly iteration.
// A stale handle (its slot recycled) is rejected by a generation mismatch. It is
// the sparse/dense "slot map": a sparse Slot array absorbs churn (a handle's
// index never moves), a dense record array stays packed via swap-and-pop.
//
// Allocation is THREAD-SAFE, split into two phases for the "reserve during a
// wave, materialize at the barrier" pattern:
//
//   reserve()   -- LOCK-FREE, callable concurrently while systems run. Claims a
//                  slot + generation and returns a Handle, WITHOUT touching the
//                  dense arrays. The handle is not alive until commit(), so
//                  get()/is_alive() report false in between.
//   commit()    -- SINGLE-THREADED (barrier). Materializes a reserved handle by
//   destroy()      constructing/removing its record in the dense arrays. Call
//                  only when no reserve() can run concurrently.
//   get()       -- read-only; safe concurrently with reserve() during a wave.
//   is_alive()     (every mutation of the dense arrays happens at the barrier).
//
// create() is a single-threaded convenience: reserve() + commit() in one call,
// for setup outside a wave.

#pragma once

#include "index_recycler.hpp"

#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace ecs::detail {
template <class T>
class HandleVector;

template <class T>
class Handle {
public:
    static constexpr uint32_t invalid_index = std::numeric_limits<uint32_t>::max();

    auto index() const { return index_; }
    auto generation() const { return generation_; }

    static constexpr Handle null() noexcept {
        return Handle {invalid_index, invalid_index};
    }

    // True when this handle is not the null() sentinel.
    [[nodiscard]]
    explicit constexpr operator bool() const noexcept {
        return !(*this == null());
    }

    friend bool operator==(Handle, Handle)  = default;
    friend auto operator<=>(Handle, Handle) = default;

private:
    friend class HandleVector<T>;

    Handle(uint32_t const index, uint32_t const generation)
        : index_(index)
        , generation_(generation) {}

    uint32_t index_;
    uint32_t generation_;
};

template <class T>
class HandleVector {
public:
    static constexpr uint32_t invalid_index = Handle<T>::invalid_index;

    // --- concurrent phase (safe during a wave) -------------------------------

    // Reserve a handle without materializing its record. LOCK-FREE: safe to call
    // concurrently. The record is placed later, at a barrier, by commit(); until
    // then the handle is not alive (get()/is_alive() are false).
    [[nodiscard]]
    Handle<T> reserve() {
        auto const slot_index = slots_alloc_.allocate();
        // A recycled slot carries its bumped generation in slots_; a freshly
        // minted index sits beyond slots_ until commit() grows it -> generation
        // 0. The bound check distinguishes the two (both frozen during a wave).
        auto const generation =
            slot_index < slots_.size() ? slots_[slot_index].generation : 0u;
        return Handle<T> {slot_index, generation};
    }

    // Returns a pointer to the live record, or nullptr for a dead/stale/reserved
    // handle. Read-only; safe concurrently with reserve(). The pointer is
    // invalidated by any subsequent commit()/destroy().
    [[nodiscard]]
    T* get(Handle<T> handle) {
        if (!is_alive(handle)) {
            return nullptr;
        }
        return &records_[slots_[handle.index_].record_index];
    }

    [[nodiscard]]
    T const* get(Handle<T> handle) const {
        if (!is_alive(handle)) {
            return nullptr;
        }
        return &records_[slots_[handle.index_].record_index];
    }

    [[nodiscard]]
    bool is_alive(Handle<T> handle) const {
        if (handle.index_ >= slots_.size()) {
            return false;
        }
        auto const& slot = slots_[handle.index_];
        return slot.alive && slot.generation == handle.generation_;
    }

    // Number of live records.
    [[nodiscard]]
    std::size_t size() const noexcept { return records_.size(); }

    // --- barrier phase (single-threaded, world-quiescent) --------------------

    // Materialize a reserved handle: construct its record in place from `args`.
    // Barrier only. Grows the sparse array to cover a freshly minted slot.
    template <class... Args>
    void commit(Handle<T> handle, Args&&... args) {
        auto const slot_index = handle.index_;
        if (slot_index >= slots_.size()) {
            slots_.resize(slot_index + 1);
        }
        auto& slot        = slots_[slot_index];
        slot.record_index = static_cast<uint32_t>(records_.size());
        slot.generation   = handle.generation_;
        slot.alive        = true;
        handles_.push_back(handle);
        records_.emplace_back(std::forward<Args>(args)...);
    }

    // Single-threaded convenience: reserve() + commit() in one call, for setup
    // outside a wave. Not thread-safe (commit() mutates the dense arrays).
    template <class... Args>
    [[nodiscard]]
    Handle<T> create(Args&&... args) {
        auto const handle = reserve();
        commit(handle, std::forward<Args>(args)...);
        return handle;
    }

    // Remove a live handle, swap-and-popping its record out of the dense arrays.
    // Barrier only. Returns false for a dead/stale handle.
    bool destroy(Handle<T> handle) {
        if (!is_alive(handle)) {
            return false;
        }
        auto& removed_slot       = slots_[handle.index_];
        auto const removed_index = removed_slot.record_index;
        auto const last_index    = static_cast<uint32_t>(records_.size() - 1);
        if (removed_index != last_index) {
            handles_[removed_index]         = std::move(handles_[last_index]);
            records_[removed_index]         = std::move(records_[last_index]);
            auto slot_index                 = handles_[removed_index].index_;
            slots_[slot_index].record_index = removed_index;
        }
        handles_.pop_back();
        records_.pop_back();
        removed_slot.alive        = false;
        removed_slot.record_index = invalid_index;
        ++removed_slot.generation;
        // Reconcile the slots reserve() claimed during the wave before recycling
        // this one -- otherwise free() would resurrect those claimed entries.
        // compact() is idempotent once at rest, so repeated destroys are cheap.
        slots_alloc_.compact();
        slots_alloc_.free(handle.index_);
        return true;
    }

private:
    struct Slot {
        uint32_t record_index = invalid_index;
        uint32_t generation   = 0;
        bool alive            = false;
    };

    std::vector<Handle<T>> handles_;    // Dense index (record index)
    std::vector<T> records_;            // Dense index (record index)
    std::vector<Slot> slots_;           // Sparse index (handle index)
    IndexRecycler slots_alloc_;         // Lock-free sparse-slot allocation
};
} // namespace ecs::detail
