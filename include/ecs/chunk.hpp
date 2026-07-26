// ecs/chunk.hpp
//
// chunk<C>: a lane's view of component C over the row range the executor handed
// it. for_each_chunk gives each lane one chunk per component, already scoped to
// that lane's rows. Access this lane's slice of a field either by name -- pos.x,
// pos.y (from the reflected member names, span<const F> when C is const) -- or by
// index, column<I>(); both denote the same std::span. The kernel iterates with
// `for (auto& v : pos.x)` (or column<I>()[i], 0-based within the chunk) and never
// handles a begin/end; because a chunk exposes only its own slice, writing through
// a mutable one cannot touch another lane's rows, which is what keeps the
// deterministic split race-free. Trivially copyable (a storage pointer + the range
// + the field spans), so it is passed by value like a span.
//
// The named accessors are the field spans of a generated field_view<C> base, which
// exists only on the P2996 reflection backend (it alone recovers member names); on
// the portable backend the base is empty and column<I>() is the only path. Which
// base is chosen lives in detail/chunk_fields.hpp, so this class carries no #if.

#pragma once

#include <ecs/detail/chunk_fields.hpp>
#include <ecs/soa.hpp>

#include <cassert>
#include <cstddef>
#include <type_traits>

namespace ecs {

template <class C>
class chunk : public detail::chunk_fields<C> {
    using bare    = std::remove_const_t<C>;
    using store_t = soa_storage<bare>;
    using storage = std::conditional_t<std::is_const_v<C>, store_t const, store_t>;

public:
    chunk(storage& store, std::size_t begin, std::size_t end) noexcept
        : detail::chunk_fields<C>(detail::make_chunk_fields<C>(store, begin, end))
        , store_(store)
        , begin_(begin)
        , end_(end) {
        assert(begin <= end && end <= store.size() && "chunk: row range out of bounds");
    }

    // This lane's contiguous slice of field I (span<const F> if C is const). Same
    // span the named accessor denotes; the portable, always-available form.
    template <std::size_t I>
    [[nodiscard]]
    auto column() const noexcept {
        return store_.template column<I>().subspan(begin_, end_ - begin_);
    }

    [[nodiscard]]
    std::size_t size() const noexcept {
        return end_ - begin_;
    }
    [[nodiscard]]
    bool empty() const noexcept {
        return begin_ == end_;
    }

private:
    storage& store_;
    std::size_t begin_, end_;
};

} // namespace ecs
