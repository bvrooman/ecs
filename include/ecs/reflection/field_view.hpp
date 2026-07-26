// ecs/reflection/field_view.hpp
//
// Named per-field column views for a component (the "pos.x / pos.y / pos.z"
// ergonomic layer over chunk::column<I>()).
//
// For a component C, field_view<C> is a *generated* aggregate holding one
// std::span per data member of C, each named after that member -- so a kernel can
// read a column by field name instead of by positional index:
//
//   struct Position { float x, y, z; };
//   field_view<Position>        -> struct { std::span<float>       x, y, z; };
//   field_view<Position const>  -> struct { std::span<const float> x, y, z; };
//
// The type is synthesized with P2996 std::meta::define_aggregate from C's
// reflected members, so it exists ONLY on the P2996 reflection backend: recovering
// real member *names* is exactly what the portable (structured-bindings) backend
// cannot do. On that backend this header defines nothing and chunk exposes only
// the positional column<I>() path, which is why column<I>() stays the portable
// contract. chunk<C> inherits field_view<C> under P2996 (see ecs/query.hpp).

#pragma once

#include <ecs/reflection/reflect.hpp> // selects a backend and defines ECS_USE_P2996

#if ECS_USE_P2996

#include <meta>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ecs::detail {

// One std::span member per data member of C, carrying the member's name. The
// span's element type follows C's constness -- span<const F> for a read-only
// (const-marked) query component, span<F> otherwise -- so the generated view
// matches what soa_storage<C>::column<I>() hands back for the same constness.
template <class C>
consteval std::vector<std::meta::info> field_span_specs() {
    using namespace std::meta;
    using Bare = std::remove_const_t<C>;
    std::vector<info> out;
    for (info const m : nonstatic_data_members_of(^^Bare, access_context::current())) {
        info const elem = std::is_const_v<C> ? add_const(type_of(m)) : type_of(m);
        out.push_back(
            data_member_spec(substitute(^^std::span, {elem}), {.name = identifier_of(m)}));
    }
    return out;
}

// The generated aggregate is a *nested* type completed by a sibling consteval
// block: GCC rejects define_aggregate when a class scope intervenes between the
// block and its target, so holder<C> cannot itself be that target.
template <class C>
struct field_view_holder {
    struct type;
    consteval { std::meta::define_aggregate(^^type, field_span_specs<C>()); }
};

// One *reference* member per data member of C, carrying the member's name --
// const-reference when C is const. This is the per-element analogue of
// field_span_specs: where field_view<C> holds a span per field, row<C> holds a
// reference to one element per field, so p.x aliases a single entity's slot in
// the column and writing p.x writes it in place (no gather/scatter).
template <class C>
consteval std::vector<std::meta::info> field_ref_specs() {
    using namespace std::meta;
    using Bare = std::remove_const_t<C>;
    std::vector<info> out;
    for (info const m : nonstatic_data_members_of(^^Bare, access_context::current())) {
        info const elem = std::is_const_v<C> ? add_const(type_of(m)) : type_of(m);
        out.push_back(
            data_member_spec(add_lvalue_reference(elem), {.name = identifier_of(m)}));
    }
    return out;
}

template <class C>
struct row_holder {
    struct type;
    consteval { std::meta::define_aggregate(^^type, field_ref_specs<C>()); }
};

// A field whose name matches one of chunk's own members would be shadowed by it
// (chunk inherits the view, and the derived name wins), silently defeating the
// named accessor. chunk static_asserts on this so the collision is a hard error.
template <class Bare>
consteval bool field_name_collides_with_chunk() {
    using namespace std::meta;
    for (info const m : nonstatic_data_members_of(^^Bare, access_context::current())) {
        std::string_view const n = identifier_of(m);
        if (n == "size" || n == "empty" || n == "column")
            return true;
    }
    return false;
}

} // namespace ecs::detail

namespace ecs {

// Generated view: one named std::span per field of C (see file header).
template <class C>
using field_view = typename detail::field_view_holder<C>::type;

// Generated per-element proxy: one named reference per field of C (const when C
// is const). row<Position> = { float& x, y, z; }; row<Position const> = { const
// float& x, y, z; }. Used by Query::for_each to hand a
// kernel one entity's row with write-through fields (p.x = ...), no gather/scatter.
template <class C>
using row = typename detail::row_holder<C>::type;

} // namespace ecs

#endif // ECS_USE_P2996
