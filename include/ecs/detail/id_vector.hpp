// ecs/detail/id_vector.hpp
//
// A std::vector indexed by a strong id (a StrongId over an integer position)
// instead of a raw integer. operator[] accepts ONLY that id type, so the
// container itself enforces the id space -- indexing it with a bare int, a row
// index, or a different id kind is a compile error, not a silent mix-up. The
// single `.value` conversion lives here rather than scattered across every call
// site.
//
// It owns id assignment: push() appends and returns the new element's id, which
// is its position. Ids are assigned once, on append, and the container is
// grow-only (no erase/reorder beyond a pop_back rollback of the last push), so
// an id stays valid for the container's lifetime -- the invariant that makes a
// positional handle a stable one (see ArchetypeId).

#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace ecs::detail {

template <class Id, class T>
class IdVector {
public:
    T& operator[](Id const id) noexcept { return v_[id.value]; }
    T const& operator[](Id const id) const noexcept { return v_[id.value]; }

    [[nodiscard]] std::size_t size() const noexcept { return v_.size(); }
    [[nodiscard]] bool empty() const noexcept { return v_.empty(); }

    // Range-for yields the elements (the ids are their positions); use size()
    // + Id{i} when the id itself is needed while scanning.
    auto begin() noexcept { return v_.begin(); }
    auto end() noexcept { return v_.end(); }
    auto begin() const noexcept { return v_.begin(); }
    auto end() const noexcept { return v_.end(); }

    // Append `value`; its id is its position. Returns that id.
    Id push(T value) {
        Id const id {static_cast<typename Id::type>(v_.size())};
        v_.push_back(std::move(value));
        return id;
    }
    // Undo the most recent push (creation rollback only).
    void pop_back() noexcept { v_.pop_back(); }

private:
    std::vector<T> v_;
};

} // namespace ecs::detail
