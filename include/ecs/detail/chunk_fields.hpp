// ecs/detail/chunk_fields.hpp
//
// chunk<C>'s backend-selected field base (used by ecs/chunk.hpp).
//
// On the P2996 reflection backend a chunk inherits field_view<C> -- one named
// std::span per field of C (pos.x, pos.y, ...) -- populated from the store's
// columns. On the portable backend there are no reflected field names, so the base
// is an empty struct and column<I>() is the only field access. Selecting the base
// here, behind detail::chunk_fields<C> / make_chunk_fields, keeps chunk itself
// backend-agnostic (no #if in the class body).

#pragma once

#include <ecs/reflection/reflect.hpp> // ECS_USE_P2996, reflect::field_count_v
#include <cstddef>
#include <type_traits>
#include <utility>

#if ECS_USE_P2996

#include <ecs/reflection/field_view.hpp> // field_view<C>, field_name_collides_with_chunk

namespace ecs::detail {

// chunk<C>'s base under P2996: the generated named field spans.
template <class C>
using chunk_fields = field_view<C>;

// Populate the base from this lane's [begin, end) slice of C's columns, so pos.x
// aliases column<0>() and so on. The static_assert keeps a field named like chunk's
// own API (size/empty/column) from silently shadowing it.
template <class C, class Store>
chunk_fields<C> make_chunk_fields(Store& store, std::size_t begin, std::size_t end) {
    using bare = std::remove_const_t<C>;
    static_assert(
        !field_name_collides_with_chunk<bare>(),
        "ecs::chunk: a component field is named size/empty/column, which shadows "
        "chunk's own members; rename the field or read it via column<I>().");
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
        return chunk_fields<C> {
            store.template column<I>().subspan(begin, end - begin)...};
    }(std::make_index_sequence<reflect::field_count_v<bare>> {});
}

} // namespace ecs::detail

#else

namespace ecs::detail {

// No reflected field names: chunk carries no field base; column<I>() is the path.
template <class C>
struct chunk_fields {};

template <class C, class Store>
chunk_fields<C> make_chunk_fields(Store&, std::size_t, std::size_t) {
    return {};
}

} // namespace ecs::detail

#endif
