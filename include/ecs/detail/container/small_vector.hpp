// ecs/detail/container/small_vector.hpp
//
// A vector with inline storage for its first N elements, spilling to the heap
// only beyond N. Purpose-built for WorkItem::units: a work item's unit list is
// almost always length 1 (a kernel slice is exactly one archetype row range), so
// the inline case never calls the allocator -- restoring the scheduler's
// zero-steady-state-allocation property, which a plain std::vector<Unit> gave up
// (one malloc per work item per tick, minted fresh each tick as the wave is
// rebuilt).
//
// The representation is a std::variant<std::array<T, N>, std::vector<T>>: the
// array alternative is the inline buffer, the vector alternative the spill. This
// is deliberately NOT a hand-rolled inline buffer + raw pointer -- the variant's
// generated move does the one subtle thing correctly for free: moving the inline
// alternative copies the array (no self-referential pointer to re-seat), moving
// the spilled alternative steals the heap buffer. size_ is the single source of
// truth for length; the vector is used as a sized buffer, not via its own size.
//
// Only the operations WorkItem needs are exposed (push_back, size/empty,
// range-for). It stays noexcept-movable so the wave's std::vector<WorkItem>
// reallocates by move, and copyable (matching the std::vector<Unit> it replaces).

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <variant>
#include <vector>
#include <cstddef>
#include <utility>

namespace ecs::detail {

template <class T, std::uint32_t N>
class SmallVector {
public:
    SmallVector() = default;

    void push_back(T const& value) {
        if (size_ == capacity())
            spill();
        data()[size_++] = value;
    }

    [[nodiscard]]
    std::uint32_t size() const noexcept {
        return size_;
    }
    [[nodiscard]]
    bool empty() const noexcept {
        return size_ == 0;
    }

    T* begin() noexcept { return data(); }
    T* end() noexcept { return data() + size_; }
    T const* begin() const noexcept { return data(); }
    T const* end() const noexcept { return data() + size_; }

private:
    // Inline capacity (index 0) is N; once spilled (index 1) it is the buffer's
    // length, which spill() always grows past size_.
    [[nodiscard]]
    std::uint32_t capacity() const noexcept {
        return store_.index() == 0
                 ? N
                 : static_cast<std::uint32_t>(std::get<1>(store_).size());
    }
    // The live buffer's front, whichever alternative holds it. get_if avoids the
    // throwing check of std::get on this hot access.
    T* data() noexcept {
        if (auto* inline_buf = std::get_if<0>(&store_))
            return inline_buf->data();
        return std::get_if<1>(&store_)->data();
    }
    T const* data() const noexcept {
        if (auto* inline_buf = std::get_if<0>(&store_))
            return inline_buf->data();
        return std::get_if<1>(&store_)->data();
    }
    // Grow onto (or within) the heap: copy the size_ live elements into a fresh
    // buffer of twice the capacity and switch the variant to it. Rare -- only a
    // serial system spanning more than N archetypes reaches this.
    void spill() {
        std::vector<T> buffer(static_cast<std::size_t>(capacity()) * 2);
        std::ranges::copy_n(data(), size_, buffer.begin());
        store_ = std::move(buffer);
    }

    std::variant<std::array<T, N>, std::vector<T>> store_ {};
    std::uint32_t size_ = 0;
};

} // namespace ecs::detail
