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
#include <ranges>
#include <utility>
#include <vector>

namespace ecs::detail {

template <class Id, class T>
class IdVector {
public:
    T& operator[](Id const id) noexcept { return v_[id.value]; }
    T const& operator[](Id const id) const noexcept { return v_[id.value]; }

    [[nodiscard]]
    std::size_t size() const noexcept {
        return v_.size();
    }
    [[nodiscard]]
    bool empty() const noexcept {
        return v_.empty();
    }

    void reserve(std::size_t const n) { v_.reserve(n); }

    // Range-for yields the elements (the ids are their positions); use size()
    // + Id{i} when the id itself is needed while scanning.
    auto begin() noexcept { return v_.begin(); }
    auto end() noexcept { return v_.end(); }
    auto begin() const noexcept { return v_.begin(); }
    auto end() const noexcept { return v_.end(); }

    auto enumerate() noexcept { return enumerate_(*this); }

    auto enumerate() const noexcept { return enumerate_(*this); }

    // Append `value`; its id is its position. Returns that id.
    Id push_back(T value) {
        Id const id {static_cast<Id::type>(v_.size())};
        v_.push_back(std::move(value));
        return id;
    }
    // Undo the most recent push (creation rollback only).
    void pop_back() noexcept { v_.pop_back(); }

    // Erase elements matching the predicate. Erasing elements can invalidate
    // issued IDs.
    template <class Predicate>
    std::size_t erase_if(Predicate predicate) {
        return std::erase_if(v_, std::move(predicate));
    }

private:
    template <class Self>
    static auto enumerate_(Self& self) {
        // std::views::enumerate is C++23 and missing from the libc++ that ships
        // with some Emscripten releases; index with iota + transform (C++20) so
        // the wasm build stays buildable. Yields (Id{position}, element&).
        auto* v = &self.v_;
        return std::views::iota(std::size_t {0}, v->size()) |
               std::views::transform([v](std::size_t const index) {
                   using IdValue          = Id::type;
                   using ElementReference = decltype((*v)[index]);
                   return std::pair<Id, ElementReference> {
                       Id {static_cast<IdValue>(index)},
                       (*v)[index],
                   };
               });
    }

    std::vector<T> v_;
};

template <class Id, class T, typename Predicate>
std::size_t erase_if(ecs::detail::IdVector<Id, T>& c, Predicate pred) {
    return c.erase_if(std::move(pred));
}

} // namespace ecs::detail
