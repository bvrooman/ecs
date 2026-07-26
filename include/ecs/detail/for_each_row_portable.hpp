// ecs/detail/for_each_row_portable.hpp
//
// Per-row kernel invocation on the portable backend (see for_each_row.hpp). With no
// reflected field names, gather each component into a local, pass it by reference
// (+ optional Entity), then scatter the mutable ones back. NB the gather copies the
// WHOLE component every row regardless of what the kernel touches, so it scales with
// the field count and is severe for wide/sparsely-used or non-trivial components --
// prefer for_each_chunk there (quantified by benchmarks/gather_bench).

#pragma once

#include <ecs/chunk.hpp>
#include <ecs/entity.hpp>             // Entity
#include <ecs/reflection/reflect.hpp> // reflect::for_each_field
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <cstddef>

namespace ecs::detail {

// Gather component C at row i of chunk c into a bare<C> local (all fields, via
// reflection over the columns).
template <class C>
std::remove_const_t<C> gather_row(chunk<C> c, std::size_t i) {
    std::remove_const_t<C> out {};
    reflect::for_each_field(out, [&](auto Ic, auto& field) {
        field = c.template column<Ic.value>()[i];
    });
    return out;
}

// Scatter a component back to row i; a no-op when C is read-only (const), matching
// the query's write-back rule.
template <class C, class V>
void scatter_row(chunk<C> c, std::size_t i, V const& v) {
    if constexpr (!std::is_const_v<C>)
        reflect::for_each_field(v, [&](auto Ic, auto const& field) {
            c.template column<Ic.value>()[i] = field;
        });
}

// Invoke fn once per row of [ents): gather each component into a local, pass by
// reference (bindable as auto& p) plus the entity if the kernel declares one, then
// scatter the mutable ones back.
template <class F, class... Cs>
void for_each_row(F& fn, std::span<Entity> ents, chunk<Cs>... cs) {
    for (std::size_t i = 0; i < ents.size(); ++i) {
        std::tuple<std::remove_const_t<Cs>...> locals {gather_row<Cs>(cs, i)...};
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            if constexpr (std::is_invocable_v<F&, Entity, Cs&...>)
                fn(ents[i], static_cast<Cs&>(std::get<I>(locals))...);
            else
                fn(static_cast<Cs&>(std::get<I>(locals))...);
            (scatter_row<Cs>(cs, i, std::get<I>(locals)), ...);
        }(std::index_sequence_for<Cs...> {});
    }
}

} // namespace ecs::detail
