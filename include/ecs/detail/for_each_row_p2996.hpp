// ecs/detail/for_each_row_p2996.hpp
//
// Per-row kernel invocation on the P2996 backend (see for_each_row.hpp). Hands the
// kernel a write-through row<C> proxy per component -- its named fields (p.x)
// reference this entity's SoA columns in place, so there is no gather/scatter and
// the loop vectorizes like a hand-written index loop.

#pragma once

#include <ecs/chunk.hpp>
#include <ecs/entity.hpp>                // Entity
#include <ecs/reflection/field_view.hpp> // row<C>
#include <ecs/reflection/reflect.hpp>    // reflect::field_count_v

#include <cstddef>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ecs::detail {

// row<C> proxy referencing row i of chunk c's columns (p.x = column<0>()[i], ...).
// Each reference binds to the SoA storage (span[i], not the temporary span), so
// writing through the proxy writes the column in place.
template <class C>
row<C> row_at(chunk<C> c, std::size_t i) {
    using bare = std::remove_const_t<C>;
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
        return row<C> {c.template column<I>()[i]...};
    }(std::make_index_sequence<reflect::field_count_v<bare>> {});
}

// Invoke fn once per row of [ents): one row<C> proxy per component, passed as
// lvalues (bindable as auto& p), plus the entity if the kernel declares one
// (detected by arity, so both kernel shapes compile).
template <class F, class... Cs>
void for_each_row(F& fn, std::span<Entity> ents, chunk<Cs>... cs) {
    for (std::size_t i = 0; i < ents.size(); ++i) {
        std::tuple<row<Cs>...> rows {row_at<Cs>(cs, i)...};
        std::apply(
            [&](row<Cs>&... r) {
                if constexpr (std::is_invocable_v<F&, Entity, row<Cs>&...>)
                    fn(ents[i], r...); // kernel opted in to the entity
                else
                    fn(r...);
            },
            rows);
    }
}

} // namespace ecs::detail
