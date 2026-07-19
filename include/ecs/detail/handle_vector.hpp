#pragma once

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

    // Construct a record in place from `args` and return a handle to it. Accepts
    // an lvalue (copies), an rvalue (moves), or constructor arguments for T.
    template <class... Args>
    [[nodiscard]]
    Handle<T> create(Args&&... args) {
        auto const slot_index = issue();
        auto& slot            = slots_[slot_index];
        slot.record_index     = static_cast<uint32_t>(records_.size());
        slot.alive            = true;
        handles_.push_back({slot_index, slot.generation});
        records_.emplace_back(std::forward<Args>(args)...);
        return handles_.back();
    }

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
        removed_slot.next = head_;
        head_             = handle.index_;
        return true;
    }

    // Returns a pointer to the live record, or nullptr for a dead/stale handle.
    // The pointer is invalidated by any subsequent create()/destroy().
    [[nodiscard]]
    T* get(Handle<T> handle) {
        if (!is_alive(handle)) {
            return nullptr;
        }
        auto const& slot        = slots_[handle.index_];
        auto const record_index = slot.record_index;
        return &records_[record_index];
    }

    [[nodiscard]]
    T const* get(Handle<T> handle) const {
        if (!is_alive(handle)) {
            return nullptr;
        }
        auto const& slot        = slots_[handle.index_];
        auto const record_index = slot.record_index;
        return &records_[record_index];
    }

private:
    struct Slot {
        uint32_t record_index = invalid_index;
        uint32_t generation   = 0;
        uint32_t next         = invalid_index;
        bool alive            = false;
    };

    auto issue() {
        uint32_t index;
        if (head_ != invalid_index) {
            index      = head_;
            auto& slot = slots_[index];
            head_      = slot.next;
            slot.next  = invalid_index;
        } else {
            index = static_cast<uint32_t>(slots_.size());
            slots_.emplace_back();
        }
        return index;
    }

    [[nodiscard]]
    bool is_alive(Handle<T> handle) const {
        if (handle.index_ >= slots_.size()) {
            return false;
        }
        auto& slot = slots_[handle.index_];
        return slot.alive && slot.generation == handle.generation_;
    }

    std::vector<Handle<T>> handles_; // Dense index (record index)
    std::vector<T> records_;         // Dense index (record index)
    std::vector<Slot> slots_;        // Sparse index (handle index)
    uint32_t head_ = invalid_index;
};
}
