// ecs/detail/container/sparse_handle_vector.hpp
//
// A sparse generational handle map -- the sibling of HandleVector. It stores the
// payload IN the sparse slot, indexed directly by the handle, instead of in a
// separate dense array. Reaching a payload is ONE indirection (handle ->
// slots_[index].value) with no sparse->dense hop, and destroy() is a plain mark
// + recycle (no swap-and-pop, no back-reference patching, no dense compaction).
//
// The trades vs HandleVector:
//   - no dense / contiguous iteration over live payloads (they sit sparsely,
//     interleaved with dead slots);
//   - dead slots retain their slot-sized storage until reused, so a container
//     that has churned to a high water mark holds one payload slot per index
//     ever handed out (heavier for large payloads than the dense form, which
//     stores only live payloads packed).
//
// Use it when payloads are reached by handle (random access) rather than walked
// in bulk -- e.g. World's entity records, looked up by entity and never iterated
// densely (component data lives in the archetype tables). Use HandleVector when
// you iterate the payloads.
//
// The PUBLIC API is identical to HandleVector (same Handle<Tag>, same
// reserve()/commit()/destroy() thread-safety phasing), so a caller can swap
// between the two by changing one type. Allocation is THREAD-SAFE:
//
//   reserve()   -- LOCK-FREE, concurrent during a wave. Claims a slot +
//                  generation without materializing the payload.
//   commit()    -- SINGLE-THREADED (barrier). Constructs the payload in its slot
//   destroy()      / resets it. Call only when no reserve() runs concurrently.
//   get()       -- read-only; safe concurrently with reserve() during a wave.
//   is_alive()

#pragma once

#include <ecs/detail/container/handle.hpp>
#include <ecs/detail/container/index_recycler.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace ecs::detail {

template <class Payload, class Tag>
class SparseHandleVector {
public:
    using handle_type = Handle<Tag>;
    static constexpr uint32_t invalid_index = handle_type::invalid_index;

    // --- concurrent phase (safe during a wave) -------------------------------

    // Reserve a handle without materializing its payload. LOCK-FREE: safe to
    // call concurrently. The payload is placed later, at a barrier, by commit();
    // until then the handle is not alive (get()/is_alive() are false).
    [[nodiscard]]
    handle_type reserve() {
        auto const slot_index = slots_alloc_.allocate();
        // A recycled slot carries its bumped generation; a freshly minted index
        // sits beyond slots_ until commit() grows it -> generation 0. Both are
        // frozen during a wave.
        auto const generation =
            slot_index < slots_.size() ? slots_[slot_index].generation : 0u;
        return handle_type {slot_index, generation};
    }

    // Returns a pointer to the live payload, or nullptr for a dead/stale/reserved
    // handle. Read-only; safe concurrently with reserve().
    [[nodiscard]]
    Payload* get(handle_type handle) {
        if (!is_alive(handle)) {
            return nullptr;
        }
        return &*slots_[handle.index()].value;
    }

    [[nodiscard]]
    Payload const* get(handle_type handle) const {
        if (!is_alive(handle)) {
            return nullptr;
        }
        return &*slots_[handle.index()].value;
    }

    // Unchecked indexed access. The handle MUST be live -- like
    // std::vector::operator[], no liveness check in release; a debug assert
    // catches a dead/stale handle. Use get() when the handle might be dead.
    [[nodiscard]]
    Payload& operator[](handle_type handle) {
        assert(is_alive(handle) && "operator[] on a dead/stale handle");
        return *slots_[handle.index()].value;
    }

    [[nodiscard]]
    Payload const& operator[](handle_type handle) const {
        assert(is_alive(handle) && "operator[] on a dead/stale handle");
        return *slots_[handle.index()].value;
    }

    [[nodiscard]]
    bool is_alive(handle_type handle) const {
        if (handle.index() >= slots_.size()) {
            return false;
        }
        auto const& slot = slots_[handle.index()];
        // An engaged optional is the liveness flag: a reserved-but-uncommitted
        // slot is empty, a destroyed slot was reset -- both read as not alive.
        return slot.value.has_value() && slot.generation == handle.generation();
    }

    // Number of live payloads.
    [[nodiscard]]
    std::size_t size() const noexcept { return live_; }

    // Pre-size the slot storage (capacity hint for bulk setup). Single-threaded.
    void reserve_capacity(std::size_t const n) { slots_.reserve(n); }

    // --- barrier phase (single-threaded, world-quiescent) --------------------

    // Materialize a reserved handle: construct its payload in place from `args`.
    // Barrier only. Grows the slot array to cover a freshly minted index.
    template <class... Args>
    void commit(handle_type handle, Args&&... args) {
        auto const slot_index = handle.index();
        if (slot_index >= slots_.size()) {
            slots_.resize(slot_index + 1);
        }
        auto& slot = slots_[slot_index];
        slot.value.emplace(std::forward<Args>(args)...);
        slot.generation = handle.generation();
        ++live_;
    }

    // Single-threaded convenience: reserve() + commit() in one call, for setup
    // outside a wave. Not thread-safe (commit() mutates a slot).
    template <class... Args>
    [[nodiscard]]
    handle_type create(Args&&... args) {
        auto const handle = reserve();
        commit(handle, std::forward<Args>(args)...);
        return handle;
    }

    // Remove a live handle: reset its payload (releasing its resources), bump the
    // generation, and recycle the slot. Barrier only. No swap-and-pop -- the slot
    // stays put, so no other handle is disturbed. Returns false for a dead handle.
    bool destroy(handle_type handle) {
        if (!is_alive(handle)) {
            return false;
        }
        auto& slot = slots_[handle.index()];
        slot.value.reset();
        ++slot.generation;
        --live_;
        // Reconcile the slots reserve() claimed during the wave before recycling
        // this one (IndexRecycler contract); idempotent once at rest.
        slots_alloc_.compact();
        slots_alloc_.free(handle.index());
        return true;
    }

private:
    struct Slot {
        std::optional<Payload> value; // engaged == live; holds the payload
        uint32_t generation = 0;      // survives across reuse
    };

    std::vector<Slot> slots_;   // sparse, indexed directly by handle index
    IndexRecycler slots_alloc_; // lock-free slot allocation
    std::size_t live_ = 0;      // count of engaged slots
};

} // namespace ecs::detail
