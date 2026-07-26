// ecs/schedule/validate.hpp
//
// The compile-time half of system registration: the self-gating predicates
// Schedule's static_asserts and `if constexpr` build guards are phrased in
// terms of. Pulled out of schedule.hpp because none of them touch a Schedule
// -- they are pure queries over a callable's parameter list.

#pragma once

#include <ecs/schedule/params/protocol.hpp> // detail::params_info
#include <ecs/schedule/system.hpp>           // IntrospectableSystem, query_info, any_res_mut_v

namespace ecs::detail {

// Self-gating checks used by Schedule's static_asserts and `if constexpr`
// build guards. Each returns its DEFERRING value when Fn is not
// introspectable (params_allowed / at_most_one_query -> true, has_res_mut
// -> false), so that case yields the IntrospectableSystem assert alone;
// otherwise it reports the real check over the argument list.
template <template <class> class Param, class Fn>
consteval bool params_allowed() {
    if constexpr (detail::IntrospectableSystem<Fn>)
        return detail::params_info<Param, detail::system_args_t<Fn>>::all_allowed;
    else
        return true;
}
template <class Fn>
consteval bool at_most_one_query() {
    if constexpr (detail::IntrospectableSystem<Fn>)
        return detail::query_info<detail::system_args_t<Fn>>::count <= 1;
    else
        return true;
}
template <class Fn>
consteval bool has_res_mut() {
    if constexpr (detail::IntrospectableSystem<Fn>)
        return detail::any_res_mut_v<detail::system_args_t<Fn>>;
    else
        return false;
}
template <class Fn>
consteval bool has_query() {
    if constexpr (detail::IntrospectableSystem<Fn>)
        return detail::query_info<detail::system_args_t<Fn>>::has_query;
    else
        return true; // defer: let the IntrospectableSystem assert be the lone one
}

} // namespace ecs::detail
