// ecs/detail/for_each_row.hpp
//
// Facade for the per-element kernel invocation, mirroring reflection/reflect.hpp.
//
//   detail::for_each_row(fn, ents, chunk<Cs>... cs)
//       invokes the kernel once per row of `ents`, handing it each component by
//       reference -- accessed as p.x -- plus an optional leading Entity.
//
// Two interchangeable backends implement it: for_each_row_p2996.hpp hands
// write-through row<C> proxies; for_each_row_portable.hpp gathers each component
// into a local and scatters the mutable ones back. Because both expose the same
// surface, Query drives it the same way (for_each hands the kernel one row
// at a time) and carries no backend #if of its own.

#pragma once

#include <ecs/reflection/reflect.hpp> // ECS_USE_P2996

#if ECS_USE_P2996
#include <ecs/detail/for_each_row_p2996.hpp>
#else
#include <ecs/detail/for_each_row_portable.hpp>
#endif
